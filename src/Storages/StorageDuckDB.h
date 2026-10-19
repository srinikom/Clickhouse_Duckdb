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

/// The functions below back the SQL functions `duckdbVersion`, `duckdbExtensions`, `duckdbInstallExtension` and
/// `duckdbLoadExtension`. Extension management is off unless `duckdb.allow_extensions` is set in the server configuration,
/// and only the names in `duckdb.allowed_extensions` can be installed or loaded.
String getDuckDBLibraryVersion();

/// An empty `database_path` lists the extensions of a private in-memory database, i.e. what is installed in the extension directory.
std::vector<DuckDBExtensionInfo> listDuckDBExtensions(const String & database_path, ContextPtr context);
void installDuckDBExtension(const String & name, ContextPtr context);
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
///   The `duckdb` section of the server configuration (`allow_extensions`, `allowed_extensions`, `preload_extensions`,
///   `extension_directory`) turns on extensions, which need `enable_external_access`; the extension names are validated and allowlisted.
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
        bool create_remote_table);

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

    String database_path;
    String remote_table_name;

    std::mutex instance_mutex;
    std::shared_ptr<DuckDBInstance> instance;
};

}

#endif
