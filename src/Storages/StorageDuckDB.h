#pragma once

#include "config.h"

#if USE_DUCKDB
#include <Storages/StorageWithCommonVirtualColumns.h>

#include <memory>
#include <mutex>
#include <vector>

namespace DB
{

/// An opened DuckDB database file. Defined in `StorageDuckDB.cpp` to keep `duckdb.h` out of the headers.
struct DuckDBInstance;

/// One row of `duckdb_extensions`.
struct DuckDBExtensionInfo
{
    String name;
    bool installed = false;
    bool loaded = false;
    String version;
    String description;
};

/// A function that an extension adds to DuckDB, e.g. `('parquet', 'read_parquet', 'table')`.
struct DuckDBExtensionFunction
{
    String extension;
    String name;
    String type;
};

/// The functions below back the SQL functions `duckdbVersion`, `duckdbExtensions`, `duckdbInstallExtension` and
/// `duckdbLoadExtension`. Extension management is off unless `duckdb.allow_extensions` is set in the server configuration,
/// and only the names in `duckdb.allowed_extensions` can be installed or loaded.
String getDuckDBLibraryVersion();

/// An empty `database_path` lists the extensions of a private in-memory database, i.e. what is installed in the extension directory.
std::vector<DuckDBExtensionInfo> listDuckDBExtensions(const String & database_path, ContextPtr context);

/// The functions of the loaded extensions, for `duckdbExtensionFunctions`: the built-in extensions have a known list, and for the
/// others the functions that appeared when they were loaded.
std::vector<DuckDBExtensionFunction> listDuckDBExtensionFunctions(const String & database_path, ContextPtr context);

/// The table function `duckdb` runs any DuckDB query, which is allowed only by `duckdb.allow_arbitrary_sql`.
void checkDuckDBArbitrarySQLAllowed(ContextPtr context);

/// Resolves a database path against `user_files`. An empty path is an in-memory database if `allow_in_memory`, and an empty result.
String resolveDuckDBDatabasePath(const String & path, ContextPtr context, bool allow_in_memory);

/// `name` is an extension name, or a path of an extension file inside `user_files` if `duckdb.allow_extension_files` is set.
/// `repository` is empty, `core`, `core_nightly` or `community`.
void installDuckDBExtension(const String & name, const String & repository, ContextPtr context);
void loadDuckDBExtension(const String & database_path, const String & name, ContextPtr context);

/// The `DuckDB` table engine: a table of an embedded DuckDB database file, read and written through the DuckDB C API.
/// `ENGINE = DuckDB('path_to_database_file', 'table')`
///
/// Implementation notes:
/// - Only the C API (`duckdb.h`) is used, so there is no C++ ABI dependency between ClickHouse (`libc++`) and the prebuilt
///   `libduckdb.so` (`libstdc++`). The header is included only in `StorageDuckDB.cpp`.
/// - Reading. `read` prepares `SELECT columns FROM table` to learn the DuckDB type of every column, then picks a decoding per
///   column: integers, floats, `Bool` and `String` are read from the DuckDB vectors directly (a value out of range of the ClickHouse type
///   is an exception, never a wrap-around); everything else is selected as `CAST(column AS VARCHAR)` and parsed by the serialization of
///   the ClickHouse type. The query is executed in streaming mode on a connection owned by the source, and `onCancel` calls `duckdb_interrupt`.
/// - Writing. The sink prepares one `INSERT INTO table (columns) VALUES (?, ...)`, binds every row, and runs it inside one transaction opened on the first
///   chunk, committed in `onFinish` and rolled back in the destructor if the insert did not finish. Non-numeric values other than `String`
///   are bound as text and cast by DuckDB to the column type.
/// - Instances. A DuckDB file cannot be opened twice in one process as two databases, so a process-wide registry of weak pointers shares
///   one `DuckDBInstance` per absolute path. The storage keeps its instance open from the first query until it is destroyed.
/// - Security. The path is validated against `user_files` (except in `clickhouse-local`), and the database is opened with
///   `enable_external_access`, `autoinstall_known_extensions` and `autoload_known_extensions` set to `false`.
///   The `duckdb` section of the server configuration turns on extensions (`allow_extensions`, which needs `enable_external_access`),
///   allowlists them (`allowed_extensions`), and allows native code (`allow_extension_loading`, see `DynamicLoadingScope`), unsigned and
///   community extensions and extension files. The extension names are validated, and a file is read from `user_files` only.
/// - Not implemented: pushdown of `WHERE` and `LIMIT`, mutations, `TRUNCATE`, a table function.
class StorageDuckDB final : public StorageWithCommonVirtualColumns, public WithContext
{
public:
    StorageDuckDB(
        const StorageID & table_id_,
        const String & database_path_,
        const String & remote_table_name_,
        const ColumnsDescription & columns_,
        const ConstraintsDescription & constraints_,
        const String & comment,
        ContextPtr context_,
        bool create_remote_table,
        /// If not empty, the table is the result of this DuckDB query (the table function `duckdb`), and it is read-only.
        const String & remote_query = {});

    ~StorageDuckDB() override;

    std::string getName() const override { return "DuckDB"; }

    using StorageWithCommonVirtualColumns::read;

    Pipe read(
        const Names & column_names,
        const StorageSnapshotPtr & storage_snapshot,
        SelectQueryInfo & query_info,
        ContextPtr context,
        QueryProcessingStage::Enum processed_stage,
        size_t max_block_size,
        size_t num_streams) override;

    SinkToStoragePtr write(const ASTPtr & query, const StorageMetadataPtr & metadata_snapshot, ContextPtr context, bool async_insert) override;

private:
    /// The database is opened on first use (not in the constructor), so that attaching a table whose file is
    /// temporarily unavailable does not fail the server start, and a read never silently creates a new file.
    std::shared_ptr<DuckDBInstance> getInstance(bool create_if_missing);

    String getTableExpression() const;

    String database_path;
    String remote_table_name;
    String remote_query;

    std::mutex instance_mutex;
    std::shared_ptr<DuckDBInstance> instance;
};

}

#endif
