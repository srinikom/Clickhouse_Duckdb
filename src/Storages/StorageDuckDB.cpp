#include <Storages/StorageDuckDB.h>

#if USE_DUCKDB

#include <duckdb.h>

#include <Columns/ColumnNullable.h>
#include <Columns/ColumnString.h>
#include <Core/Settings.h>
#include <DataTypes/DataTypeFactory.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypesDecimal.h>
#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/IDataType.h>
#include <DataTypes/Serializations/ISerialization.h>
#include <Formats/FormatFactory.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromString.h>
#include <Interpreters/Context.h>
#include <Interpreters/convertFieldToType.h>
#include <Interpreters/evaluateConstantExpression.h>
#include <Processors/ISource.h>
#include <Processors/Sinks/SinkToStorage.h>
#include <QueryPipeline/Pipe.h>
#include <Storages/StorageFactory.h>
#include <Storages/checkAndGetLiteralArgument.h>
#include <Common/DynamicLoading.h>
#include <Common/Exception.h>
#include <Common/StringUtils.h>
#include <Common/filesystemHelpers.h>
#include <Common/logger_useful.h>

#include <Poco/Util/AbstractConfiguration.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>

namespace fs = std::filesystem;

namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int CANNOT_INSERT_NULL_IN_ORDINARY_COLUMN;
    extern const int DUCKDB_ENGINE_ERROR;
    extern const int NOT_IMPLEMENTED;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int PATH_ACCESS_DENIED;
    extern const int SUPPORT_IS_DISABLED;
}

struct DuckDBInstance
{
    duckdb_database database = nullptr;

    /// The functions that appeared when an extension was loaded into this database, by extension name.
    std::mutex extension_functions_mutex;
    std::map<String, std::set<std::pair<String, String>>> extension_functions;

    DuckDBInstance() = default;
    DuckDBInstance(const DuckDBInstance &) = delete;
    DuckDBInstance & operator=(const DuckDBInstance &) = delete;

    ~DuckDBInstance()
    {
        if (database)
            duckdb_close(&database);
    }
};

namespace
{

[[noreturn]] void throwDuckDBError(const String & what, const char * message)
{
    throw Exception(ErrorCodes::DUCKDB_ENGINE_ERROR, "DuckDB: {}: {}", what, message ? message : "unknown error");
}

/// DuckDB quotes identifiers with double quotes, which are doubled inside the identifier.
String quoteDuckDBIdentifier(std::string_view identifier)
{
    String res = "\"";
    for (char c : identifier)
    {
        if (c == '"')
            res += '"';
        res += c;
    }
    res += '"';
    return res;
}

/// Two storages over the same file share one instance: a DuckDB file cannot be opened twice in one process
/// with an independent database instance.
std::mutex instances_mutex;
std::unordered_map<String, std::weak_ptr<DuckDBInstance>> opened_instances;

/// The path is resolved against `user_files_path` and has to stay inside it, like for the `SQLite` engine.
String validateDatabasePath(const String & path, ContextPtr context)
{
    const bool need_check = context->getApplicationType() != Context::ApplicationType::LOCAL;
    const String user_files_path = context->getUserFilesPath();

    String absolute_path = fs::absolute(path).lexically_normal();
    if (fs::path(path).is_relative())
        absolute_path = fs::absolute(fs::path(user_files_path) / path).lexically_normal();

    const String absolute_user_files_path = fs::absolute(user_files_path).lexically_normal();
    if (need_check && !fileOrSymlinkPathStartsWith(absolute_path, absolute_user_files_path))
        throw Exception(ErrorCodes::PATH_ACCESS_DENIED, "DuckDB file path '{}' must be inside 'user_files' directory", path);

    return absolute_path;
}

/// The `duckdb` section of the server configuration. It is read when a database is opened, so a change applies to
/// the databases opened afterwards. Extensions are off by default: an extension is native code, and installing one
/// needs the network and the file system.
struct ExtensionSettings
{
    bool enabled = false;
    /// Any plain extension name is allowed, `*` in `allowed_extensions`.
    bool allow_any = false;
    std::set<String> allowed;
    std::vector<String> preload;
    String directory;
    /// Extensions that are not built into the library are native code: loading one needs `dlopen`, which is forbidden in the `clickhouse` binary.
    bool allow_loading = false;
    bool allow_unsigned = false;
    bool allow_community = false;
    bool allow_files = false;
    String custom_repository;
};

std::vector<String> splitList(const String & list)
{
    std::vector<String> result;
    size_t begin = 0;
    while (begin <= list.size())
    {
        size_t end = list.find(',', begin);
        if (end == String::npos)
            end = list.size();
        String item = list.substr(begin, end - begin);
        const auto first = item.find_first_not_of(" \t\n");
        if (first != String::npos)
            result.push_back(item.substr(first, item.find_last_not_of(" \t\n") - first + 1));
        begin = end + 1;
    }
    return result;
}

/// An extension name ends up in `INSTALL`/`LOAD` statements: only plain names are accepted, never paths or URLs.
void validateExtensionName(const String & name)
{
    if (name.empty() || !std::all_of(name.begin(), name.end(), [](char c) { return isAlphaNumericASCII(c) || c == '_'; }))
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Invalid DuckDB extension name '{}'", name);
}

ExtensionSettings readExtensionSettings(ContextPtr context)
{
    ExtensionSettings settings;
    const auto & config = context->getConfigRef();
    settings.enabled = config.getBool("duckdb.allow_extensions", false);
    if (!settings.enabled)
        return settings;

    for (auto & name : splitList(config.getString("duckdb.allowed_extensions", "")))
    {
        if (name == "*")
        {
            settings.allow_any = true;
            continue;
        }
        validateExtensionName(name);
        settings.allowed.insert(std::move(name));
    }

    for (auto & name : splitList(config.getString("duckdb.preload_extensions", "")))
    {
        validateExtensionName(name);
        if (!settings.allow_any && !settings.allowed.contains(name))
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "DuckDB extension '{}' is in 'duckdb.preload_extensions' but not in 'duckdb.allowed_extensions'", name);
        settings.preload.push_back(std::move(name));
    }

    settings.directory = config.getString("duckdb.extension_directory", fs::path(context->getPath()) / "duckdb_extensions" / "");
    settings.allow_loading = config.getBool("duckdb.allow_extension_loading", false);
    settings.allow_unsigned = config.getBool("duckdb.allow_unsigned_extensions", false);
    settings.allow_community = config.getBool("duckdb.allow_community_extensions", false);
    settings.allow_files = config.getBool("duckdb.allow_extension_files", false);
    settings.custom_repository = config.getString("duckdb.custom_extension_repository", "");
    return settings;
}

/// An extension is a name of the repository or, if `allow_extension_files` is set, a file of `user_files`.
struct ExtensionRef
{
    String name;
    String file;
    bool isFile() const { return !file.empty(); }
};

String quoteDuckDBLiteral(std::string_view value)
{
    String res = "'";
    for (char c : value)
    {
        if (c == '\'')
            res += '\'';
        res += c;
    }
    res += '\'';
    return res;
}

ExtensionRef resolveExtension(const ExtensionSettings & settings, const String & reference, ContextPtr context)
{
    if (!settings.enabled)
        throw Exception(ErrorCodes::SUPPORT_IS_DISABLED, "DuckDB extensions are disabled, set 'duckdb.allow_extensions' in the server configuration");

    const bool is_file = reference.find('/') != String::npos || reference.ends_with(".duckdb_extension");
    if (!is_file)
    {
        validateExtensionName(reference);
        if (!settings.allow_any && !settings.allowed.contains(reference))
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "DuckDB extension '{}' is not in 'duckdb.allowed_extensions'", reference);
        return {reference, {}};
    }

    if (!settings.allow_files)
        throw Exception(ErrorCodes::SUPPORT_IS_DISABLED, "Extension files are disabled, set 'duckdb.allow_extension_files' in the server configuration");

    /// A file is read from `user_files`, like a database file.
    String file = validateDatabasePath(reference, context);
    if (!fs::is_regular_file(file))
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "DuckDB extension file '{}' does not exist", reference);

    String name = fs::path(file).stem().string();
    validateExtensionName(name);
    return {name, file};
}

/// An empty path opens a private in-memory database, which is not shared.
std::shared_ptr<DuckDBInstance> openRawInstance(const String & path, bool create_if_missing, const ExtensionSettings & extensions)
{
    std::lock_guard lock(instances_mutex);

    if (!path.empty())
        if (auto it = opened_instances.find(path); it != opened_instances.end())
            if (auto existing = it->second.lock())
                return existing;

    if (!path.empty() && !create_if_missing && !fs::exists(path))
        throw Exception(ErrorCodes::PATH_ACCESS_DENIED, "DuckDB database file '{}' does not exist", path);

    duckdb_config config = nullptr;
    if (duckdb_create_config(&config) != DuckDBSuccess)
        throw Exception(ErrorCodes::DUCKDB_ENGINE_ERROR, "DuckDB: cannot create the configuration");

    /// The engine runs only the statements it generates. Still, do not let an embedded database reach the network or
    /// the file system, or install extensions on its own. Installing an extension needs both, so the external access
    /// is enabled only if the extensions are, and then the extension directory is pinned and nothing is installed or
    /// loaded implicitly.
    const std::array<std::pair<const char *, String>, 5> options
    {{
        {"enable_external_access", extensions.enabled ? "true" : "false"},
        {"autoinstall_known_extensions", "false"},
        {"autoload_known_extensions", "false"},
        {"allow_unsigned_extensions", extensions.allow_unsigned ? "true" : "false"},
        {"allow_community_extensions", extensions.allow_community ? "true" : "false"},
    }};

    auto set_option = [&](const char * name, const String & value)
    {
        if (duckdb_set_config(config, name, value.c_str()) != DuckDBSuccess)
        {
            duckdb_destroy_config(&config);
            throw Exception(ErrorCodes::DUCKDB_ENGINE_ERROR, "DuckDB: cannot set the option '{}'", name);
        }
    };

    for (const auto & [name, value] : options)
        set_option(name, value);

    if (extensions.enabled)
    {
        fs::create_directories(extensions.directory);
        set_option("extension_directory", extensions.directory);
        if (!extensions.custom_repository.empty())
            set_option("custom_extension_repository", extensions.custom_repository);
    }

    auto result = std::make_shared<DuckDBInstance>();
    char * error = nullptr;
    const auto state = duckdb_open_ext(path.empty() ? nullptr : path.c_str(), &result->database, config, &error);
    duckdb_destroy_config(&config);
    if (state != DuckDBSuccess)
    {
        String message = error ? error : "unknown error";
        duckdb_free(error);
        throw Exception(ErrorCodes::DUCKDB_ENGINE_ERROR, "DuckDB: cannot open the database '{}': {}", path, message);
    }

    if (!path.empty())
        opened_instances[path] = result;
    return result;
}

/// A connection is cheap in DuckDB, and every query uses its own, so that cancelling one query never interrupts another.
class Connection
{
public:
    explicit Connection(std::shared_ptr<DuckDBInstance> instance_)
        : instance(std::move(instance_))
    {
        if (duckdb_connect(instance->database, &connection) != DuckDBSuccess)
            throw Exception(ErrorCodes::DUCKDB_ENGINE_ERROR, "DuckDB: cannot connect to the database");
    }

    ~Connection() { duckdb_disconnect(&connection); }

    Connection(const Connection &) = delete;
    Connection & operator=(const Connection &) = delete;

    duckdb_connection get() const { return connection; }

    void execute(const String & query)
    {
        duckdb_result result;
        if (duckdb_query(connection, query.c_str(), &result) != DuckDBSuccess)
        {
            String message = duckdb_result_error(&result) ? duckdb_result_error(&result) : "unknown error";
            duckdb_destroy_result(&result);
            throw Exception(ErrorCodes::DUCKDB_ENGINE_ERROR, "DuckDB: query '{}' failed: {}", query, message);
        }
        duckdb_destroy_result(&result);
    }

private:
    std::shared_ptr<DuckDBInstance> instance;
    duckdb_connection connection = nullptr;
};

/// Runs a query and returns its cells as text.
std::vector<std::vector<String>> queryRows(duckdb_connection connection, const String & query)
{
    duckdb_result result;
    if (duckdb_query(connection, query.c_str(), &result) != DuckDBSuccess)
    {
        String message = duckdb_result_error(&result) ? duckdb_result_error(&result) : "unknown error";
        duckdb_destroy_result(&result);
        throw Exception(ErrorCodes::DUCKDB_ENGINE_ERROR, "DuckDB: query '{}' failed: {}", query, message);
    }

    std::vector<std::vector<String>> rows;
    for (idx_t row = 0; row < duckdb_row_count(&result); ++row)
    {
        auto & cells = rows.emplace_back();
        for (idx_t column = 0; column < duckdb_column_count(&result); ++column)
        {
            char * value = duckdb_value_varchar(&result, column, row);
            cells.emplace_back(value ? value : "");
            duckdb_free(value);
        }
    }
    duckdb_destroy_result(&result);
    return rows;
}

/// The (name, type) pairs of the functions of a database, e.g. `('read_parquet', 'table')`.
std::set<std::pair<String, String>> queryFunctions(duckdb_connection connection)
{
    std::set<std::pair<String, String>> functions;
    for (const auto & row : queryRows(connection, "SELECT DISTINCT function_name, function_type FROM duckdb_functions()"))
        functions.emplace(row[0], row[1]);
    return functions;
}

/// DuckDB does not tell which extension a function belongs to, so the functions are registered by comparing
/// the functions of the database before and after the extension is loaded.
void loadExtension(const std::shared_ptr<DuckDBInstance> & instance, const ExtensionRef & extension, const ExtensionSettings & settings)
{
    Connection connection(instance);

    /// The extensions linked into the library and the ones already loaded need no `dlopen`.
    bool needs_dynamic_loading = true;
    if (!extension.isFile())
    {
        const auto rows = queryRows(
            connection.get(),
            "SELECT install_mode, loaded FROM duckdb_extensions() WHERE extension_name = " + quoteDuckDBLiteral(extension.name));
        needs_dynamic_loading = rows.empty() || (rows[0][0] != "STATICALLY_LINKED" && rows[0][1] != "true");
    }

    std::optional<DynamicLoadingScope> scope;
    if (needs_dynamic_loading)
    {
        if (!settings.allow_loading)
            throw Exception(
                ErrorCodes::SUPPORT_IS_DISABLED,
                "The DuckDB extension '{}' is not built into the library, and loading it needs native code: set 'duckdb.allow_extension_loading' in the server configuration",
                extension.name);
        scope.emplace();
    }

    const auto before = queryFunctions(connection.get());
    connection.execute(extension.isFile() ? "LOAD " + quoteDuckDBLiteral(extension.file) : "LOAD " + extension.name);
    scope.reset();
    const auto after = queryFunctions(connection.get());

    std::lock_guard lock(instance->extension_functions_mutex);
    auto & registered = instance->extension_functions[extension.name];
    for (const auto & function : after)
        if (!before.contains(function))
            registered.insert(function);
}

std::shared_ptr<DuckDBInstance> openInstance(const String & path, bool create_if_missing, ContextPtr context)
{
    const auto extensions = readExtensionSettings(context);
    auto result = openRawInstance(path, create_if_missing, extensions);
    for (const auto & name : extensions.preload)
        loadExtension(result, {name, {}}, extensions);
    return result;
}

class PreparedStatement
{
public:
    PreparedStatement(duckdb_connection connection, const String & query_)
        : query(query_)
    {
        if (duckdb_prepare(connection, query.c_str(), &statement) != DuckDBSuccess)
        {
            String message = duckdb_prepare_error(statement) ? duckdb_prepare_error(statement) : "unknown error";
            duckdb_destroy_prepare(&statement);
            throw Exception(ErrorCodes::DUCKDB_ENGINE_ERROR, "DuckDB: cannot prepare '{}': {}", query, message);
        }
    }

    ~PreparedStatement() { duckdb_destroy_prepare(&statement); }

    PreparedStatement(const PreparedStatement &) = delete;
    PreparedStatement & operator=(const PreparedStatement &) = delete;

    duckdb_prepared_statement get() const { return statement; }
    const String & getQuery() const { return query; }

private:
    String query;
    duckdb_prepared_statement statement = nullptr;
};

class QueryResult
{
public:
    QueryResult() = default;
    ~QueryResult() { duckdb_destroy_result(&result); }

    QueryResult(const QueryResult &) = delete;
    QueryResult & operator=(const QueryResult &) = delete;

    duckdb_result result{};
};

DataTypePtr fromDuckDBType(duckdb_type type, UInt8 decimal_width, UInt8 decimal_scale, const String & column_name)
{
    auto & factory = DataTypeFactory::instance();
    switch (type)
    {
        case DUCKDB_TYPE_BOOLEAN: return factory.get("Bool");
        case DUCKDB_TYPE_TINYINT: return std::make_shared<DataTypeInt8>();
        case DUCKDB_TYPE_SMALLINT: return std::make_shared<DataTypeInt16>();
        case DUCKDB_TYPE_INTEGER: return std::make_shared<DataTypeInt32>();
        case DUCKDB_TYPE_BIGINT: return std::make_shared<DataTypeInt64>();
        case DUCKDB_TYPE_UTINYINT: return std::make_shared<DataTypeUInt8>();
        case DUCKDB_TYPE_USMALLINT: return std::make_shared<DataTypeUInt16>();
        case DUCKDB_TYPE_UINTEGER: return std::make_shared<DataTypeUInt32>();
        case DUCKDB_TYPE_UBIGINT: return std::make_shared<DataTypeUInt64>();
        case DUCKDB_TYPE_HUGEINT: return factory.get("Int128");
        case DUCKDB_TYPE_UHUGEINT: return factory.get("UInt128");
        case DUCKDB_TYPE_FLOAT: return std::make_shared<DataTypeFloat32>();
        case DUCKDB_TYPE_DOUBLE: return std::make_shared<DataTypeFloat64>();
        case DUCKDB_TYPE_VARCHAR:
        case DUCKDB_TYPE_BLOB: return factory.get("String");
        case DUCKDB_TYPE_DATE: return factory.get("Date32");
        case DUCKDB_TYPE_TIMESTAMP_S: return factory.get("DateTime64(0)");
        case DUCKDB_TYPE_TIMESTAMP_MS: return factory.get("DateTime64(3)");
        case DUCKDB_TYPE_TIMESTAMP: return factory.get("DateTime64(6)");
        case DUCKDB_TYPE_TIMESTAMP_NS: return factory.get("DateTime64(9)");
        case DUCKDB_TYPE_UUID: return factory.get("UUID");
        case DUCKDB_TYPE_DECIMAL: return factory.get(fmt::format("Decimal({}, {})", UInt32(decimal_width), UInt32(decimal_scale)));
        default:
            throw Exception(
                ErrorCodes::NOT_IMPLEMENTED,
                "The type (id {}) of the DuckDB column '{}' is not supported by the DuckDB table engine",
                static_cast<int>(type), column_name);
    }
}

/// The DuckDB type used when the engine creates the remote table for a `CREATE TABLE` with an explicit column list.
String toDuckDBType(const DataTypePtr & type, const String & column_name)
{
    const auto nested = removeNullable(type);
    const WhichDataType which(nested);

    if (isBool(nested)) return "BOOLEAN";
    if (which.isInt8()) return "TINYINT";
    if (which.isInt16()) return "SMALLINT";
    if (which.isInt32()) return "INTEGER";
    if (which.isInt64()) return "BIGINT";
    if (which.isInt128()) return "HUGEINT";
    if (which.isUInt8()) return "UTINYINT";
    if (which.isUInt16()) return "USMALLINT";
    if (which.isUInt32()) return "UINTEGER";
    if (which.isUInt64()) return "UBIGINT";
    if (which.isUInt128()) return "UHUGEINT";
    if (which.isFloat32()) return "FLOAT";
    if (which.isFloat64()) return "DOUBLE";
    if (which.isString()) return "VARCHAR";
    if (which.isDate() || which.isDate32()) return "DATE";
    if (which.isDateTime()) return "TIMESTAMP";
    if (which.isDateTime64())
        return getDecimalScale(*nested) > 6 ? "TIMESTAMP_NS" : "TIMESTAMP";
    if (which.isUUID()) return "UUID";
    if (which.isDecimal())
    {
        const auto precision = getDecimalPrecision(*nested);
        if (precision > 38)
            throw Exception(ErrorCodes::NOT_IMPLEMENTED, "The column '{}' has a decimal precision above 38, which DuckDB does not support", column_name);
        return fmt::format("DECIMAL({}, {})", precision, getDecimalScale(*nested));
    }

    throw Exception(
        ErrorCodes::NOT_IMPLEMENTED, "The type {} of the column '{}' is not supported by the DuckDB table engine", type->getName(), column_name);
}

/// `table_expression` is a quoted table name or a parenthesized query with an alias. The statement is only prepared, never executed:
/// DuckDB refuses to prepare several statements, so a query of the `duckdb` table function cannot smuggle in a second one here.
ColumnsDescription getTableStructure(Connection & connection, const String & table_expression)
{
    PreparedStatement statement(connection.get(), "SELECT * FROM " + table_expression);

    NamesAndTypesList columns;
    const idx_t column_count = duckdb_prepared_statement_column_count(statement.get());
    for (idx_t i = 0; i < column_count; ++i)
    {
        const String name = duckdb_prepared_statement_column_name(statement.get(), i);
        auto logical_type = duckdb_prepared_statement_column_logical_type(statement.get(), i);
        const auto type_id = duckdb_get_type_id(logical_type);
        UInt8 width = 0;
        UInt8 scale = 0;
        if (type_id == DUCKDB_TYPE_DECIMAL)
        {
            width = duckdb_decimal_width(logical_type);
            scale = duckdb_decimal_scale(logical_type);
        }
        duckdb_destroy_logical_type(&logical_type);

        /// The result of a query does not carry the nullability of a table column.
        columns.emplace_back(name, makeNullable(fromDuckDBType(type_id, width, scale, name)));
    }
    return ColumnsDescription{columns};
}

/// How a column is transferred from DuckDB.
enum class Decode
{
    Signed, /// A DuckDB integer, read as `Int64`.
    Unsigned, /// A DuckDB unsigned integer or `BOOLEAN`, read as `UInt64`.
    Float, /// A DuckDB `FLOAT` or `DOUBLE`.
    String, /// A DuckDB `VARCHAR` or `BLOB` into a `String`.
    Text, /// Any other combination: the value is cast to `VARCHAR` by DuckDB and parsed by the ClickHouse type.
};

struct ColumnDecoder
{
    DataTypePtr nested_type; /// Without `Nullable`.
    SerializationPtr serialization;
    bool nullable = false;
    Decode decode = Decode::Text;
    duckdb_type source_type = DUCKDB_TYPE_INVALID;
    String name;
};

Decode chooseDecode(duckdb_type source, const DataTypePtr & target)
{
    const WhichDataType which(target);
    const bool target_is_integer = which.isNativeInt() || which.isNativeUInt() || isBool(target);
    const bool target_is_float = which.isFloat();

    switch (source)
    {
        case DUCKDB_TYPE_TINYINT:
        case DUCKDB_TYPE_SMALLINT:
        case DUCKDB_TYPE_INTEGER:
        case DUCKDB_TYPE_BIGINT:
            return (target_is_integer || target_is_float) ? Decode::Signed : Decode::Text;
        case DUCKDB_TYPE_BOOLEAN:
        case DUCKDB_TYPE_UTINYINT:
        case DUCKDB_TYPE_USMALLINT:
        case DUCKDB_TYPE_UINTEGER:
        case DUCKDB_TYPE_UBIGINT:
            return (target_is_integer || target_is_float) ? Decode::Unsigned : Decode::Text;
        case DUCKDB_TYPE_FLOAT:
        case DUCKDB_TYPE_DOUBLE:
            return target_is_float ? Decode::Float : Decode::Text;
        case DUCKDB_TYPE_VARCHAR:
        case DUCKDB_TYPE_BLOB:
            return which.isString() ? Decode::String : Decode::Text;
        default:
            return Decode::Text;
    }
}

class DuckDBSource final : public ISource
{
public:
    DuckDBSource(
        std::shared_ptr<DuckDBInstance> instance_,
        String query_,
        std::vector<ColumnDecoder> decoders_,
        SharedHeader header,
        UInt64 max_block_size_,
        FormatSettings format_settings_)
        : ISource(header)
        , connection(std::move(instance_))
        , query(std::move(query_))
        , decoders(std::move(decoders_))
        , max_block_size(max_block_size_)
        , format_settings(std::move(format_settings_))
    {
    }

    ~DuckDBSource() override
    {
        if (chunk_in_hand)
            duckdb_destroy_data_chunk(&chunk_in_hand);
    }

    String getName() const override { return "DuckDB"; }

protected:
    Chunk generate() override
    {
        if (reached_end)
            return {};

        if (!statement)
        {
            statement = std::make_unique<PreparedStatement>(connection.get(), query);
            if (duckdb_execute_prepared_streaming(statement->get(), &query_result.result) != DuckDBSuccess)
                throwDuckDBError("cannot execute '" + query + "'", duckdb_result_error(&query_result.result));
        }

        MutableColumns columns = getPort().getHeader().cloneEmptyColumns();
        size_t rows = 0;

        while (rows < max_block_size)
        {
            duckdb_data_chunk data_chunk = duckdb_fetch_chunk(query_result.result);
            if (!data_chunk)
            {
                reached_end = true;
                break;
            }
            chunk_in_hand = data_chunk;

            const idx_t chunk_size = duckdb_data_chunk_get_size(data_chunk);
            for (size_t col = 0; col < decoders.size(); ++col)
                decodeColumn(decoders[col], duckdb_data_chunk_get_vector(data_chunk, col), chunk_size, *columns[col]);
            rows += chunk_size;

            duckdb_destroy_data_chunk(&chunk_in_hand);
            chunk_in_hand = nullptr;
        }

        if (rows == 0)
            return {};
        return Chunk(std::move(columns), rows);
    }

    void onCancel() noexcept override
    {
        /// `duckdb_interrupt` is thread-safe and affects only the dedicated connection of this source.
        duckdb_interrupt(connection.get());
    }

private:
    void decodeColumn(const ColumnDecoder & decoder, duckdb_vector vector, idx_t size, IColumn & column) const
    {
        uint64_t * validity = duckdb_vector_get_validity(vector);
        void * data = duckdb_vector_get_data(vector);

        IColumn * nested_column = &column;
        ColumnNullable * nullable_column = nullptr;
        if (decoder.nullable)
        {
            nullable_column = &assert_cast<ColumnNullable &>(column);
            nested_column = &nullable_column->getNestedColumn();
        }

        for (idx_t row = 0; row < size; ++row)
        {
            if (validity && !duckdb_validity_row_is_valid(validity, row))
            {
                if (!nullable_column)
                    throw Exception(
                        ErrorCodes::CANNOT_INSERT_NULL_IN_ORDINARY_COLUMN,
                        "Received NULL from DuckDB for the non-Nullable column '{}'", decoder.name);
                nullable_column->insertDefault();
                continue;
            }

            insertCell(decoder, data, row, *nested_column);
            if (nullable_column)
                nullable_column->getNullMapData().push_back(static_cast<UInt8>(0));
        }
    }

    void insertCell(const ColumnDecoder & decoder, void * data, idx_t row, IColumn & column) const
    {
        switch (decoder.decode)
        {
            case Decode::Signed:
            {
                Int64 value = 0;
                switch (decoder.source_type)
                {
                    case DUCKDB_TYPE_TINYINT: value = static_cast<int8_t *>(data)[row]; break;
                    case DUCKDB_TYPE_SMALLINT: value = static_cast<int16_t *>(data)[row]; break;
                    case DUCKDB_TYPE_INTEGER: value = static_cast<int32_t *>(data)[row]; break;
                    default: value = static_cast<int64_t *>(data)[row]; break;
                }
                insertField(decoder, Field(value), column);
                break;
            }
            case Decode::Unsigned:
            {
                UInt64 value = 0;
                switch (decoder.source_type)
                {
                    case DUCKDB_TYPE_BOOLEAN: value = static_cast<bool *>(data)[row]; break;
                    case DUCKDB_TYPE_UTINYINT: value = static_cast<uint8_t *>(data)[row]; break;
                    case DUCKDB_TYPE_USMALLINT: value = static_cast<uint16_t *>(data)[row]; break;
                    case DUCKDB_TYPE_UINTEGER: value = static_cast<uint32_t *>(data)[row]; break;
                    default: value = static_cast<uint64_t *>(data)[row]; break;
                }
                insertField(decoder, Field(value), column);
                break;
            }
            case Decode::Float:
            {
                const Float64 value = decoder.source_type == DUCKDB_TYPE_FLOAT ? static_cast<Float64>(static_cast<float *>(data)[row]) : static_cast<double *>(data)[row];
                insertField(decoder, Field(value), column);
                break;
            }
            case Decode::String:
            {
                auto & cell = static_cast<duckdb_string_t *>(data)[row];
                column.insertData(duckdb_string_t_data(&cell), duckdb_string_t_length(cell));
                break;
            }
            case Decode::Text:
            {
                auto & cell = static_cast<duckdb_string_t *>(data)[row];
                ReadBufferFromMemory buf(duckdb_string_t_data(&cell), duckdb_string_t_length(cell));
                decoder.serialization->deserializeWholeText(column, buf, format_settings);
                break;
            }
        }
    }

    /// An out of range value (a `BIGINT` that does not fit into a `UInt8` column) is an error, not a silent wrap-around.
    static void insertField(const ColumnDecoder & decoder, const Field & field, IColumn & column)
    {
        Field converted = convertFieldToType(field, *decoder.nested_type);
        if (converted.isNull())
            throw Exception(
                ErrorCodes::DUCKDB_ENGINE_ERROR,
                "The value of the DuckDB column '{}' is out of range of the type {}", decoder.name, decoder.nested_type->getName());
        column.insert(converted);
    }

    Connection connection;
    String query;
    std::vector<ColumnDecoder> decoders;
    UInt64 max_block_size;
    FormatSettings format_settings;

    std::unique_ptr<PreparedStatement> statement;
    QueryResult query_result;
    duckdb_data_chunk chunk_in_hand = nullptr;
    bool reached_end = false;
};

/// Which text forms of the ClickHouse types are accepted by DuckDB when they are bound as `VARCHAR` to a typed parameter.
bool isTextBindable(const DataTypePtr & nested)
{
    const WhichDataType which(nested);
    return which.isDate() || which.isDate32() || which.isDateTime() || which.isDateTime64() || which.isUUID() || which.isDecimal()
        || which.isInt128() || which.isUInt128();
}

class DuckDBSink final : public SinkToStorage
{
public:
    DuckDBSink(
        std::shared_ptr<DuckDBInstance> instance,
        const StorageMetadataPtr & metadata_snapshot,
        const String & table,
        FormatSettings format_settings_)
        : SinkToStorage(std::make_shared<const Block>(metadata_snapshot->getSampleBlock()))
        , connection(std::move(instance))
        , format_settings(std::move(format_settings_))
    {
        String columns_list;
        String placeholders;
        for (const auto & column : getHeader())
        {
            const auto nested = removeNullable(column.type);
            const WhichDataType which(nested);
            if (!(isBool(nested) || which.isNativeInt() || which.isNativeUInt() || which.isFloat() || which.isString() || isTextBindable(nested)))
                throw Exception(
                    ErrorCodes::NOT_IMPLEMENTED,
                    "Cannot insert the column '{}' of type {} into a DuckDB table", column.name, column.type->getName());

            if (!columns_list.empty())
            {
                columns_list += ", ";
                placeholders += ", ";
            }
            columns_list += quoteDuckDBIdentifier(column.name);
            placeholders += "?";
            serializations.push_back(nested->getDefaultSerialization());
        }

        statement = std::make_unique<PreparedStatement>(
            connection.get(), "INSERT INTO " + quoteDuckDBIdentifier(table) + " (" + columns_list + ") VALUES (" + placeholders + ")");
    }

    ~DuckDBSink() override
    {
        /// Rolls back what was written if the insert did not finish.
        if (transaction_open)
        {
            duckdb_result result;
            duckdb_query(connection.get(), "ROLLBACK", &result);
            duckdb_destroy_result(&result);
        }
    }

    String getName() const override { return "DuckDBSink"; }

protected:
    void consume(Chunk & chunk) override
    {
        if (!transaction_open)
        {
            connection.execute("BEGIN TRANSACTION");
            transaction_open = true;
        }

        const auto & header = getHeader();
        const auto columns = chunk.getColumns();
        const size_t rows = chunk.getNumRows();

        for (size_t row = 0; row < rows; ++row)
        {
            for (size_t col = 0; col < columns.size(); ++col)
                bindValue(col, header.getByPosition(col), *columns[col], row);

            QueryResult result;
            if (duckdb_execute_prepared(statement->get(), &result.result) != DuckDBSuccess)
                throwDuckDBError("cannot execute '" + statement->getQuery() + "'", duckdb_result_error(&result.result));
        }
    }

    void onFinish() override
    {
        if (!transaction_open)
            return;
        connection.execute("COMMIT");
        transaction_open = false;
    }

private:
    void bindValue(size_t col, const ColumnWithTypeAndName & header_column, const IColumn & column, size_t row)
    {
        const idx_t index = col + 1;
        duckdb_state state = DuckDBSuccess;

        if (header_column.type->isNullable() && column.isNullAt(row))
        {
            state = duckdb_bind_null(statement->get(), index);
        }
        else
        {
            const auto nested = removeNullable(header_column.type);
            const WhichDataType which(nested);
            const IColumn * value_column = &column;
            if (const auto * nullable = typeid_cast<const ColumnNullable *>(&column))
                value_column = &nullable->getNestedColumn();

            if (isBool(nested))
                state = duckdb_bind_boolean(statement->get(), index, value_column->getUInt(row) != 0);
            else if (which.isNativeUInt())
                state = duckdb_bind_uint64(statement->get(), index, value_column->getUInt(row));
            else if (which.isNativeInt())
                state = duckdb_bind_int64(statement->get(), index, value_column->getInt(row));
            else if (which.isFloat())
                state = duckdb_bind_double(statement->get(), index, value_column->getFloat64(row));
            else if (which.isString())
            {
                const auto data = value_column->getDataAt(row);
                state = duckdb_bind_varchar_length(statement->get(), index, data.data(), data.size());
            }
            else
            {
                /// DuckDB casts the text to the type of the column it is inserted into.
                WriteBufferFromOwnString buf;
                serializations[col]->serializeText(*value_column, row, buf, format_settings);
                const String text = buf.str();
                state = duckdb_bind_varchar_length(statement->get(), index, text.data(), text.size());
            }
        }

        if (state != DuckDBSuccess)
            throwDuckDBError("cannot bind the column '" + header_column.name + "'", duckdb_prepare_error(statement->get()));
    }

    Connection connection;
    FormatSettings format_settings;
    std::unique_ptr<PreparedStatement> statement;
    std::vector<SerializationPtr> serializations;
    bool transaction_open = false;
};

}


void checkDuckDBArbitrarySQLAllowed(ContextPtr context)
{
    if (!context->getConfigRef().getBool("duckdb.allow_arbitrary_sql", false))
        throw Exception(
            ErrorCodes::SUPPORT_IS_DISABLED,
            "Running DuckDB queries is disabled, set 'duckdb.allow_arbitrary_sql' in the server configuration");
}

String resolveDuckDBDatabasePath(const String & path, ContextPtr context, bool allow_in_memory)
{
    if (path.empty() && allow_in_memory)
        return {};
    return validateDatabasePath(path, context);
}

namespace
{

/// The functions that a built-in extension provides. DuckDB does not report this, and a built-in extension is loaded
/// from the start, so there is no moment to compare the functions. A name is reported only if the database has it.
const std::map<String, std::vector<String>> builtin_extension_functions
{
    {"parquet", {"read_parquet", "parquet_scan", "parquet_metadata", "parquet_schema", "parquet_file_metadata", "parquet_kv_metadata", "parquet_bloom_probe"}},
    {"json", {"read_json", "read_json_auto", "read_json_objects", "read_json_objects_auto", "read_ndjson", "read_ndjson_auto", "read_ndjson_objects",
              "json_extract", "json_extract_string", "json_exists", "json_valid", "json_type", "json_keys", "json_structure", "json_array_length",
              "json_contains", "json_transform", "json_group_array", "json_group_object", "json_object", "json_array", "json_serialize_sql",
              "json_deserialize_sql", "to_json", "from_json"}},
    {"autocomplete", {"sql_auto_complete"}},
    {"icu", {"icu_sort_key", "icu_calendar_names", "pg_timezone_names"}},
};

}

std::vector<DuckDBExtensionFunction> listDuckDBExtensionFunctions(const String & database_path, ContextPtr context)
{
    const auto extensions = readExtensionSettings(context);
    if (!extensions.enabled)
        throw Exception(ErrorCodes::SUPPORT_IS_DISABLED, "DuckDB extensions are disabled, set 'duckdb.allow_extensions' in the server configuration");

    const String path = database_path.empty() ? String{} : validateDatabasePath(database_path, context);
    auto instance = openInstance(path, /* create_if_missing */ false, context);
    Connection connection(instance);

    const auto functions = queryFunctions(connection.get());
    std::set<std::tuple<String, String, String>> found;
    for (const auto & row : queryRows(connection.get(), "SELECT extension_name FROM duckdb_extensions() WHERE loaded"))
    {
        const String & extension = row[0];

        if (auto it = builtin_extension_functions.find(extension); it != builtin_extension_functions.end())
            for (const auto & function : functions)
                if (std::find(it->second.begin(), it->second.end(), function.first) != it->second.end())
                    found.emplace(extension, function.first, function.second);

        std::lock_guard lock(instance->extension_functions_mutex);
        if (auto it = instance->extension_functions.find(extension); it != instance->extension_functions.end())
            for (const auto & function : it->second)
                found.emplace(extension, function.first, function.second);
    }

    std::vector<DuckDBExtensionFunction> res;
    for (const auto & [extension, name, type] : found)
        res.push_back({extension, name, type});
    return res;
}

String getDuckDBLibraryVersion()
{
    return duckdb_library_version();
}

std::vector<DuckDBExtensionInfo> listDuckDBExtensions(const String & database_path, ContextPtr context)
{
    const auto extensions = readExtensionSettings(context);
    if (!extensions.enabled)
        throw Exception(ErrorCodes::SUPPORT_IS_DISABLED, "DuckDB extensions are disabled, set 'duckdb.allow_extensions' in the server configuration");

    const String path = database_path.empty() ? String{} : validateDatabasePath(database_path, context);
    Connection connection(openInstance(path, /* create_if_missing */ false, context));

    duckdb_result result;
    const char * query = "SELECT extension_name, installed, loaded, coalesce(extension_version, ''), coalesce(description, '') "
                         "FROM duckdb_extensions() ORDER BY extension_name";
    if (duckdb_query(connection.get(), query, &result) != DuckDBSuccess)
    {
        String message = duckdb_result_error(&result) ? duckdb_result_error(&result) : "unknown error";
        duckdb_destroy_result(&result);
        throw Exception(ErrorCodes::DUCKDB_ENGINE_ERROR, "DuckDB: cannot list the extensions: {}", message);
    }

    auto text = [&](idx_t column, idx_t row)
    {
        char * value = duckdb_value_varchar(&result, column, row);
        String res = value ? value : "";
        duckdb_free(value);
        return res;
    };

    std::vector<DuckDBExtensionInfo> res;
    for (idx_t row = 0; row < duckdb_row_count(&result); ++row)
        res.push_back({text(0, row), duckdb_value_boolean(&result, 1, row), duckdb_value_boolean(&result, 2, row), text(3, row), text(4, row)});
    duckdb_destroy_result(&result);
    return res;
}

void installDuckDBExtension(const String & name, const String & repository, ContextPtr context)
{
    const auto extensions = readExtensionSettings(context);
    const auto extension = resolveExtension(extensions, name, context);

    String statement;
    if (extension.isFile())
    {
        statement = "INSTALL " + quoteDuckDBLiteral(extension.file);
    }
    else
    {
        statement = "INSTALL " + extension.name;
        if (!repository.empty())
        {
            if (repository == "community" && !extensions.allow_community)
                throw Exception(ErrorCodes::SUPPORT_IS_DISABLED, "Community extensions are disabled, set 'duckdb.allow_community_extensions' in the server configuration");
            if (repository != "community" && repository != "core" && repository != "core_nightly")
                throw Exception(ErrorCodes::BAD_ARGUMENTS, "Unknown DuckDB extension repository '{}', expected 'core', 'core_nightly' or 'community'", repository);
            statement += " FROM " + repository;
        }
    }

    /// The installation goes to the extension directory shared by all databases, so a private in-memory database is enough.
    Connection connection(openInstance({}, /* create_if_missing */ true, context));
    connection.execute(statement);
}

void loadDuckDBExtension(const String & database_path, const String & name, ContextPtr context)
{
    const auto extensions = readExtensionSettings(context);
    const auto extension = resolveExtension(extensions, name, context);

    loadExtension(openInstance(validateDatabasePath(database_path, context), /* create_if_missing */ false, context), extension, extensions);
}

StorageDuckDB::StorageDuckDB(
    const StorageID & table_id_,
    const String & database_path_,
    const String & remote_table_name_,
    const ColumnsDescription & columns_,
    const ConstraintsDescription & constraints_,
    const String & comment,
    ContextPtr context_,
    bool create_remote_table,
    const String & remote_query_)
    : StorageWithCommonVirtualColumns(table_id_)
    , WithContext(context_->getGlobalContext())
    , database_path(database_path_)
    , remote_table_name(remote_table_name_)
    , remote_query(remote_query_)
{
    if (!remote_query.empty())
        checkDuckDBArbitrarySQLAllowed(context_);

    StorageInMemoryMetadata storage_metadata;

    if (columns_.empty())
    {
        Connection connection(getInstance(/* create_if_missing */ false));
        storage_metadata.setColumns(getTableStructure(connection, getTableExpression()));
    }
    else
    {
        if (create_remote_table)
        {
            String definition;
            for (const auto & column : columns_.getAllPhysical())
            {
                if (!definition.empty())
                    definition += ", ";
                definition += quoteDuckDBIdentifier(column.name) + " " + toDuckDBType(column.type, column.name);
                if (!column.type->isNullable())
                    definition += " NOT NULL";
            }

            Connection connection(getInstance(/* create_if_missing */ true));
            connection.execute("CREATE TABLE IF NOT EXISTS " + quoteDuckDBIdentifier(remote_table_name) + " (" + definition + ")");
        }
        storage_metadata.setColumns(columns_);
    }

    storage_metadata.setConstraints(constraints_);
    storage_metadata.setComment(comment);
    setInMemoryMetadata(storage_metadata);
}

StorageDuckDB::~StorageDuckDB() = default;

String StorageDuckDB::getTableExpression() const
{
    if (remote_query.empty())
        return quoteDuckDBIdentifier(remote_table_name);
    return "(" + remote_query + ") AS duckdb_query";
}

std::shared_ptr<DuckDBInstance> StorageDuckDB::getInstance(bool create_if_missing)
{
    std::lock_guard lock(instance_mutex);
    if (!instance)
        instance = openInstance(database_path, create_if_missing, getContext());
    return instance;
}

Pipe StorageDuckDB::read(
    const Names & column_names,
    const StorageSnapshotPtr & storage_snapshot,
    SelectQueryInfo & /*query_info*/,
    ContextPtr context_,
    QueryProcessingStage::Enum /*processed_stage*/,
    size_t max_block_size,
    size_t /*num_streams*/)
{
    storage_snapshot->check(column_names);

    auto shared_instance = getInstance(/* create_if_missing */ false);

    /// Find out the DuckDB types of the columns to choose how every column is transferred.
    String plain_columns;
    for (const auto & name : column_names)
    {
        if (!plain_columns.empty())
            plain_columns += ", ";
        plain_columns += quoteDuckDBIdentifier(name);
    }
    const String table = getTableExpression();

    Block sample_block;
    for (const auto & name : column_names)
        sample_block.insert({storage_snapshot->metadata->getColumns().getPhysical(name).type->createColumn(),
                             storage_snapshot->metadata->getColumns().getPhysical(name).type, name});

    std::vector<ColumnDecoder> decoders;
    String select_list;
    {
        Connection probe_connection(shared_instance);
        PreparedStatement probe(probe_connection.get(), "SELECT " + plain_columns + " FROM " + table);
        if (duckdb_prepared_statement_column_count(probe.get()) != column_names.size())
            throw Exception(ErrorCodes::DUCKDB_ENGINE_ERROR, "DuckDB returned an unexpected number of columns for {}", remote_table_name);

        for (size_t i = 0; i < column_names.size(); ++i)
        {
            const auto & column = sample_block.getByPosition(i);

            ColumnDecoder decoder;
            decoder.name = column.name;
            decoder.nullable = column.type->isNullable();
            decoder.nested_type = removeNullable(column.type);
            decoder.serialization = decoder.nested_type->getDefaultSerialization();
            decoder.source_type = duckdb_prepared_statement_column_type(probe.get(), i);
            decoder.decode = chooseDecode(decoder.source_type, decoder.nested_type);

            if (!select_list.empty())
                select_list += ", ";
            const String quoted = quoteDuckDBIdentifier(column.name);
            select_list += decoder.decode == Decode::Text ? "CAST(" + quoted + " AS VARCHAR)" : quoted;
            decoders.push_back(std::move(decoder));
        }
    }

    auto header = std::make_shared<const Block>(sample_block);
    return Pipe(std::make_shared<DuckDBSource>(
        shared_instance,
        "SELECT " + select_list + " FROM " + table,
        std::move(decoders),
        header,
        max_block_size,
        getFormatSettings(context_)));
}

SinkToStoragePtr StorageDuckDB::write(
    const ASTPtr & /*query*/, const StorageMetadataPtr & metadata_snapshot, ContextPtr context_, bool /*async_insert*/)
{
    if (!remote_query.empty())
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Cannot INSERT into the 'duckdb' table function: it is the result of a query");

    return std::make_shared<DuckDBSink>(
        getInstance(/* create_if_missing */ false), metadata_snapshot, remote_table_name, getFormatSettings(context_));
}


void registerStorageDuckDB(StorageFactory & factory);
void registerStorageDuckDB(StorageFactory & factory)
{
    factory.registerStorage("DuckDB", [](const StorageFactory::Arguments & args) -> StoragePtr
    {
        ASTs & engine_args = args.engine_args;

        if (engine_args.size() != 2)
            throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH, "DuckDB engine requires 2 arguments: database path, table name");

        for (auto & engine_arg : engine_args)
            engine_arg = evaluateConstantExpressionOrIdentifierAsLiteral(engine_arg, args.getLocalContext());

        const auto database_path = validateDatabasePath(checkAndGetLiteralArgument<String>(engine_args[0], "database_path"), args.getContext());
        const auto table_name = checkAndGetLiteralArgument<String>(engine_args[1], "table_name");

        /// Only a genuine `CREATE` with an explicit column list creates the remote table (and the file, if it is missing).
        /// An `ATTACH`, e.g. replaying the stored definition on server start, never writes to the database.
        const bool create_remote_table = args.mode <= LoadingStrictnessLevel::CREATE && !args.columns.empty();

        return std::make_shared<StorageDuckDB>(
            args.table_id, database_path, table_name, args.columns, args.constraints, args.comment, args.getContext(), create_remote_table);
    },
    SecretArgumentsSpec{},
    {
        .supports_schema_inference = true,
        .source_access_type = AccessTypeObjects::Source::FILE,
    },
    Documentation{
        .description = R"DOCS_MD(
The engine reads and writes a table of an embedded [DuckDB](https://duckdb.org) database file. It keeps transactional, row-oriented data in a
database file next to the ClickHouse data and makes it queryable with ClickHouse SQL.

The engine is available only in Linux builds configured with `ENABLE_DUCKDB`, which link a prebuilt `libduckdb.so`.

## Creating a table {#creating-a-table}

```sql
CREATE TABLE [IF NOT EXISTS] [db.]table_name
(
    name1 [type1],
    name2 [type2], ...
) ENGINE = DuckDB('database_path', 'table')
```

**Engine Parameters**

- `database_path` — path to the DuckDB database file. It is resolved against, and must stay inside, the `user_files` directory.
- `table` — name of a table in the DuckDB database.

`CREATE TABLE` with an explicit column list creates the table (and the file, if it is missing) in DuckDB, unless the table already exists.
`CREATE TABLE` without a column list infers the structure from the existing DuckDB table; every inferred column is `Nullable`.
`ATTACH TABLE`, including the one that replays the stored definition when the server starts, never modifies the database and never creates the file.
The database is opened on the first query, so a temporarily unavailable file does not prevent the server from starting.

## Usage example {#usage-example}

```sql
CREATE TABLE duck (id Int64, name String, ts DateTime64(6)) ENGINE = DuckDB('analytics.duckdb', 'events');
INSERT INTO duck VALUES (1, 'first', now64(6));
SELECT * FROM duck;
```

## Data types {#data-types}

Integers, `Float32`, `Float64`, `Bool` and `String` are transferred in binary form. A value that does not fit the ClickHouse column type, for example a `BIGINT` read into `UInt8`, raises an exception instead of wrapping around.
Every other type (`Date`, `Date32`, `DateTime`, `DateTime64`, `UUID`, `Decimal`, `Int128`, `UInt128`) is transferred as text: DuckDB casts the value to `VARCHAR` on reads, and casts the text back to the column type on writes.

| ClickHouse type | DuckDB type created by `CREATE TABLE` |
|---|---|
| `Int8`, `Int16`, `Int32`, `Int64`, `Int128` | `TINYINT`, `SMALLINT`, `INTEGER`, `BIGINT`, `HUGEINT` |
| `UInt8`, `UInt16`, `UInt32`, `UInt64`, `UInt128` | `UTINYINT`, `USMALLINT`, `UINTEGER`, `UBIGINT`, `UHUGEINT` |
| `Float32`, `Float64` | `FLOAT`, `DOUBLE` |
| `Bool` | `BOOLEAN` |
| `String` | `VARCHAR` |
| `Date`, `Date32` | `DATE` |
| `DateTime`, `DateTime64(p)` | `TIMESTAMP` (`TIMESTAMP_NS` for `p` above 6) |
| `UUID` | `UUID` |
| `Decimal(P, S)` | `DECIMAL(P, S)`, with `P` up to 38 |

A column that is not `Nullable` is created `NOT NULL`. A `NULL` read into a non-`Nullable` column raises `CANNOT_INSERT_NULL_IN_ORDINARY_COLUMN`. Other types are not supported and raise `NOT_IMPLEMENTED`.

## Transactions and concurrency {#transactions-and-concurrency}

Every `INSERT` runs in one DuckDB transaction that is committed when the insert finishes and rolled back if it fails or is cancelled.
Every query uses its own DuckDB connection, so cancelling a query interrupts only that query.
Tables over the same file in one server share one DuckDB database instance, which stays open as long as one of them exists. A DuckDB file can be opened by only one process at a time, so it cannot be used by the `duckdb` command line while the server holds it.
DuckDB is opened with external file access, automatic extension installation and extension autoloading disabled.

## Extensions {#extensions}

Extensions are managed by the `duckdb` section of the server configuration, in XML or in YAML. They are off by default, and the settings apply to the databases opened afterwards.

```xml
<duckdb>
    <allow_extensions>1</allow_extensions>
    <allowed_extensions>json, parquet, inet</allowed_extensions>
    <preload_extensions>json</preload_extensions>
    <extension_directory>/var/lib/clickhouse/duckdb_extensions/</extension_directory>
    <allow_extension_loading>1</allow_extension_loading>
    <allow_unsigned_extensions>0</allow_unsigned_extensions>
    <allow_community_extensions>0</allow_community_extensions>
    <allow_extension_files>0</allow_extension_files>
    <custom_extension_repository></custom_extension_repository>
    <allow_arbitrary_sql>1</allow_arbitrary_sql>
</duckdb>
```

```yaml
duckdb:
    allow_extensions: 1
    allowed_extensions: json, parquet, inet
    preload_extensions: json
    extension_directory: /var/lib/clickhouse/duckdb_extensions/
    allow_extension_loading: 1
    allow_unsigned_extensions: 0
    allow_community_extensions: 0
    allow_extension_files: 0
    custom_extension_repository: ''
    allow_arbitrary_sql: 1
```

- `allow_extensions`: the main switch. It enables the extension functions and opens databases with external access, which installing an extension needs. Default `0`.
- `allowed_extensions`: comma-separated names of the extensions that can be installed or loaded, or `*` for any. Nothing is allowed by default.
- `preload_extensions`: names, allowed ones, loaded when a database is opened.
- `extension_directory`: where extensions are installed. Default: `duckdb_extensions` in the server data directory.
- `allow_extension_loading`: allows loading the extensions that are not built into the library. It is native code, so `dlopen` is allowed only while an extension is being loaded. Default `0`.
- `allow_unsigned_extensions`: allows extensions that are not signed by DuckDB. Default `0`.
- `allow_community_extensions`: allows `duckdbInstallExtension('name', 'community')`. Default `0`.
- `allow_extension_files`: allows an extension file of `user_files` as the extension. Default `0`.
- `custom_extension_repository`: the repository that extensions are downloaded from instead of the official one.
- `allow_arbitrary_sql`: enables the `duckdb` table function, which runs a DuckDB query as is. Default `0`.

The functions `duckdbVersion`, `duckdbExtensions`, `duckdbExtensionFunctions`, `duckdbInstallExtension` and `duckdbLoadExtension` list, install and load extensions. They need the `WRITE` grant on the `FILE` source. `duckdbExtensionFunctions` maps the loaded extensions to the functions they provide, e.g. `parquet` to `read_parquet`.
The functions of an extension are used in the queries of the `duckdb` table function: `SELECT * FROM duckdb('', 'SELECT * FROM read_parquet(''data.parquet'')')`. The query can do everything DuckDB can, so it is off unless `allow_arbitrary_sql` is set, and it needs the `READ` grant on the `FILE` source.
Extensions linked into the DuckDB library (`json`, `parquet`, `icu`, `core_functions`, `autocomplete`) are always loaded. Any other extension is installed and loaded when `allow_extension_loading` is set.

## Limitations {#limitations}

- `WHERE`, `LIMIT` and aggregations are not pushed down to DuckDB; the whole requested set of columns is read and filtered in ClickHouse.
- `ALTER ... UPDATE`, `ALTER ... DELETE` and `TRUNCATE` are not supported. `DROP TABLE` drops only the ClickHouse table, not the DuckDB one.
- The `duckdb` table function is read-only and needs `duckdb.allow_arbitrary_sql`.
)DOCS_MD",
        .syntax = "ENGINE = DuckDB('path_to_database_file', 'table')",
        .related = {"SQLite", "PostgreSQL"}});
}

}

#endif
