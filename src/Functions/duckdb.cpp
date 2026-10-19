#include "config.h"

#if USE_DUCKDB

#include <Access/Common/AccessFlags.h>
#include <Access/ContextAccess.h>
#include <Columns/ColumnString.h>
#include <Core/Field.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypeTuple.h>
#include <DataTypes/DataTypesNumber.h>
#include <Functions/FunctionFactory.h>
#include <Functions/FunctionHelpers.h>
#include <Functions/IFunction.h>
#include <Interpreters/Context.h>
#include <Storages/StorageDuckDB.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int ILLEGAL_COLUMN;
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
}

namespace
{

/// The commands manage the extensions of the embedded DuckDB, which is off by default, see `duckdb.allow_extensions`.
/// They touch the file system and the network, so they need the `WRITE` grant on the `FILE` source, like writing a file.
void checkDuckDBAccess(const ContextPtr & context)
{
    context->getAccess()->checkAccessWithFilter(AccessType::WRITE, toStringSource(AccessTypeObjects::Source::FILE), /* filter */ "");
}

String getConstantString(const ColumnsWithTypeAndName & arguments, size_t index, const String & function_name)
{
    const auto * column = checkAndGetColumnConstData<ColumnString>(arguments[index].column.get());
    if (!column)
        throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Argument {} of function {} must be a constant String", index + 1, function_name);
    return String(column->getDataAt(0));
}

void checkStringArguments(const DataTypes & arguments, const String & function_name)
{
    for (size_t i = 0; i < arguments.size(); ++i)
        if (!isString(arguments[i]))
            throw Exception(
                ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                "Illegal type {} of argument {} of function {}, must be String",
                arguments[i]->getName(),
                i + 1,
                function_name);
}

/// Base of the functions of this file: all of them have constant arguments, so the work is done once per block.
class FunctionDuckDBBase : public IFunction
{
public:
    bool isDeterministic() const override { return false; }
    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo & /*arguments*/) const override { return false; }
    bool useDefaultImplementationForConstants() const override { return false; }
};

class FunctionDuckDBVersion final : public FunctionDuckDBBase
{
public:
    static constexpr auto name = "duckdbVersion";
    static FunctionPtr create(ContextPtr) { return std::make_shared<FunctionDuckDBVersion>(); }

    String getName() const override { return name; }
    size_t getNumberOfArguments() const override { return 0; }
    bool isDeterministic() const override { return true; }

    DataTypePtr getReturnTypeImpl(const DataTypes &) const override { return std::make_shared<DataTypeString>(); }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName &, const DataTypePtr & result_type, size_t input_rows_count) const override
    {
        return result_type->createColumnConst(input_rows_count, getDuckDBLibraryVersion());
    }
};

class FunctionDuckDBExtensions final : public FunctionDuckDBBase
{
public:
    static constexpr auto name = "duckdbExtensions";

    static FunctionPtr create(ContextPtr context)
    {
        checkDuckDBAccess(context);
        return std::make_shared<FunctionDuckDBExtensions>(context);
    }

    explicit FunctionDuckDBExtensions(ContextPtr context_) : context(context_) {}

    String getName() const override { return name; }
    bool isVariadic() const override { return true; }
    size_t getNumberOfArguments() const override { return 0; }

    DataTypePtr getReturnTypeImpl(const DataTypes & arguments) const override
    {
        if (arguments.size() > 1)
            throw Exception(
                ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH, "Function {} takes at most 1 argument: the database path", getName());
        checkStringArguments(arguments, getName());

        return std::make_shared<DataTypeArray>(std::make_shared<DataTypeTuple>(
            DataTypes{
                std::make_shared<DataTypeString>(),
                std::make_shared<DataTypeUInt8>(),
                std::make_shared<DataTypeUInt8>(),
                std::make_shared<DataTypeString>(),
                std::make_shared<DataTypeString>()},
            Strings{"name", "installed", "loaded", "version", "description"}));
    }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr & result_type, size_t input_rows_count) const override
    {
        const String database_path = arguments.empty() ? String{} : getConstantString(arguments, 0, getName());

        Array rows;
        for (const auto & extension : listDuckDBExtensions(database_path, context))
            rows.push_back(Tuple{extension.name, UInt64(extension.installed), UInt64(extension.loaded), extension.version, extension.description});
        return result_type->createColumnConst(input_rows_count, rows);
    }

private:
    ContextPtr context;
};

class FunctionDuckDBInstallExtension final : public FunctionDuckDBBase
{
public:
    static constexpr auto name = "duckdbInstallExtension";

    static FunctionPtr create(ContextPtr context)
    {
        checkDuckDBAccess(context);
        return std::make_shared<FunctionDuckDBInstallExtension>(context);
    }

    explicit FunctionDuckDBInstallExtension(ContextPtr context_) : context(context_) {}

    String getName() const override { return name; }
    size_t getNumberOfArguments() const override { return 1; }

    DataTypePtr getReturnTypeImpl(const DataTypes & arguments) const override
    {
        checkStringArguments(arguments, getName());
        return std::make_shared<DataTypeUInt8>();
    }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr & result_type, size_t input_rows_count) const override
    {
        installDuckDBExtension(getConstantString(arguments, 0, getName()), context);
        return result_type->createColumnConst(input_rows_count, 1u);
    }

private:
    ContextPtr context;
};

class FunctionDuckDBLoadExtension final : public FunctionDuckDBBase
{
public:
    static constexpr auto name = "duckdbLoadExtension";

    static FunctionPtr create(ContextPtr context)
    {
        checkDuckDBAccess(context);
        return std::make_shared<FunctionDuckDBLoadExtension>(context);
    }

    explicit FunctionDuckDBLoadExtension(ContextPtr context_) : context(context_) {}

    String getName() const override { return name; }
    size_t getNumberOfArguments() const override { return 2; }

    DataTypePtr getReturnTypeImpl(const DataTypes & arguments) const override
    {
        checkStringArguments(arguments, getName());
        return std::make_shared<DataTypeUInt8>();
    }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr & result_type, size_t input_rows_count) const override
    {
        loadDuckDBExtension(getConstantString(arguments, 0, getName()), getConstantString(arguments, 1, getName()), context);
        return result_type->createColumnConst(input_rows_count, 1u);
    }

private:
    ContextPtr context;
};

}

REGISTER_FUNCTION(DuckDB)
{
    factory.registerFunction<FunctionDuckDBVersion>(FunctionDocumentation{
        .description = "Returns the version of the embedded DuckDB library.",
        .syntax = "duckdbVersion()",
        .arguments = {},
        .returned_value = {"The version of the DuckDB library.", {"String"}},
        .examples = {{"Usage", "SELECT duckdbVersion()", "v1.5.6"}},
        .introduced_in = {26, 11},
        .category = FunctionDocumentation::Category::Introspection});

    factory.registerFunction<FunctionDuckDBExtensions>(FunctionDocumentation{
        .description = R"(
Lists the extensions known to the embedded DuckDB: whether each is installed in the extension directory and, if a database path is given, whether it is loaded in that database.
Requires `duckdb.allow_extensions` in the server configuration and the `WRITE` grant on the `FILE` source.
)",
        .syntax = "duckdbExtensions([database_path])",
        .arguments = {{"database_path", "The path of a DuckDB database file inside `user_files`. Optional.", {"const String"}}},
        .returned_value = {"The extensions with their state.", {"Array(Tuple(name String, installed UInt8, loaded UInt8, version String, description String))"}},
        .examples = {{"Usage", "SELECT arrayJoin(duckdbExtensions()).name", "httpfs"}},
        .introduced_in = {26, 11},
        .category = FunctionDocumentation::Category::Introspection});

    factory.registerFunction<FunctionDuckDBInstallExtension>(FunctionDocumentation{
        .description = R"(
Installs a DuckDB extension into the extension directory (`duckdb.extension_directory`), which downloads it. The extension has to be in `duckdb.allowed_extensions`.
Requires `duckdb.allow_extensions` in the server configuration and the `WRITE` grant on the `FILE` source.
)",
        .syntax = "duckdbInstallExtension(name)",
        .arguments = {{"name", "The name of the extension.", {"const String"}}},
        .returned_value = {"`1` on success, otherwise an exception is thrown.", {"UInt8"}},
        .examples = {{"Usage", "SELECT duckdbInstallExtension('httpfs')", "1"}},
        .introduced_in = {26, 11},
        .category = FunctionDocumentation::Category::Other});

    factory.registerFunction<FunctionDuckDBLoadExtension>(FunctionDocumentation{
        .description = R"(
Loads an installed DuckDB extension into an opened DuckDB database. The extension has to be in `duckdb.allowed_extensions`, and the database has to exist.
The extension stays loaded while the database is open, i.e. while a table of the `DuckDB` engine uses it. Use `duckdb.preload_extensions` to load an extension in every database.
Requires `duckdb.allow_extensions` in the server configuration and the `WRITE` grant on the `FILE` source.
)",
        .syntax = "duckdbLoadExtension(database_path, name)",
        .arguments
        = {{"database_path", "The path of a DuckDB database file inside `user_files`.", {"const String"}},
           {"name", "The name of the extension.", {"const String"}}},
        .returned_value = {"`1` on success, otherwise an exception is thrown.", {"UInt8"}},
        .examples = {{"Usage", "SELECT duckdbLoadExtension('app.duckdb', 'json')", "1"}},
        .introduced_in = {26, 11},
        .category = FunctionDocumentation::Category::Other});
}

}

#endif
