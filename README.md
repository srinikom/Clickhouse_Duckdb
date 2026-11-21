# ClickHouse with an embedded DuckDB

This is an unofficial fork of [ClickHouse](https://github.com/ClickHouse/ClickHouse). It adds a **`DuckDB` table engine** for OLTP-style workloads. Its tables live in a single [DuckDB](https://duckdb.org) file next to the server, accept transactional inserts, and sit alongside ClickHouse's own analytical engines.

ClickHouse® is a registered trademark of ClickHouse, Inc. This project is not affiliated with or endorsed by ClickHouse, Inc. or the DuckDB Foundation. ClickHouse is licensed under Apache 2.0 and DuckDB under MIT.

> **Status: experimental.** The integration is Linux only, off in a default build, and not part of upstream ClickHouse. Unlike ClickHouse, which links its third-party libraries statically, it links DuckDB dynamically through the prebuilt `libduckdb.so`. It was built and tested in a Linux Docker container (ClickHouse 26.11, DuckDB v1.5.6). ClickHouse's standard CI jobs do not run its tests, because they need a build with `libduckdb.so`.

## What you get

| Part | Purpose |
| --- | --- |
| `DuckDB` table engine | A ClickHouse table backed by a table of a DuckDB file. `SELECT` and transactional `INSERT`. |
| `duckdb` table function | Runs a DuckDB query and returns the result, so DuckDB functions such as `read_parquet` can be used. Read-only. |
| `duckdbVersion`, `duckdbExtensions`, `duckdbExtensionFunctions` | The DuckDB version, the extensions with their state, and the functions each loaded extension provides. |
| `duckdbInstallExtension`, `duckdbLoadExtension` | Install and load a DuckDB extension, including community, unsigned and file-based ones. |
| `duckdb` section of the server configuration | Turns on and limits all of the above. XML or YAML. |

DuckDB is reached through its C API and the prebuilt `libduckdb.so`, which is a run-time dependency of the `clickhouse` binary.

## Architecture

```text
  clients: clickhouse-client, HTTP, drivers
        |
        |  SQL (port 9000 native, 8123 HTTP)
        v
+-- one Linux host or container -----------------------------------------------+
|                                                                              |
|  clickhouse server (built with -DENABLE_DUCKDB=1)                            |
|  +------------------------------------------------------------------------+  |
|  |                                                                        |  |
|  |   DuckDB table engine ---> materialized view ---> MergeTree            |  |
|  |   (OLTP: INSERT, SELECT)   (copies each INSERT)   (analytics)          |  |
|  |   |                        (through ClickHouse only)                   |  |
|  |   |                                                                    |  |
|  |   +<--- duckdb table function (read-only) and extension functions      |  |
|  |   |                                                                    |  |
|  |   v  C API                                                             |  |
|  |   +--------------------------+                                         |  |
|  |   | libduckdb.so (v1.5.6)    |                                         |  |
|  |   | in-process, found via    |                                         |  |
|  |   | LD_LIBRARY_PATH          |                                         |  |
|  |   +--------------------------+                                         |  |
|  +------------------------------------------------------------------------+  |
|                                                                              |
|  On disk:                                                                    |
|   user_files/app.duckdb (+ .wal)   DuckDB tables; one process can open it    |
|   user_files/*.parquet             files for read_parquet                    |
|   duckdb_extensions/               installed extensions                      |
|   data/                            MergeTree parts                           |
|   config.d/duckdb.xml              duckdb section, the security gates        |
+------------------------------------------------------------------------------+
        |
        v  optional, only if the config allows it
  DuckDB extension repository (internet)
```

- One process holds the DuckDB file, so the `duckdb` command line cannot open `app.duckdb` while the server runs.
- `libduckdb.so` runs inside the server process. There is no separate DuckDB service.
- The materialized view copies only inserts made through ClickHouse, not rows that other tools write to the DuckDB file.

## Quick start

### Build

1. Download the `libduckdb-linux-*.zip` of a [DuckDB release](https://github.com/duckdb/duckdb/releases) and unpack it. It contains `duckdb.h` and `libduckdb.so`.
2. Get the code, either as a fresh clone of this fork:

```bash
git clone --recursive https://github.com/srinikom/Clickhouse_Duckdb.git && cd Clickhouse_Duckdb
git checkout duckdb-table-engine
```

or, if you already have a ClickHouse clone, fetch the branch from this fork into it:

```bash
git fetch https://github.com/srinikom/Clickhouse_Duckdb.git duckdb-table-engine:duckdb-table-engine
git checkout duckdb-table-engine
```

3. Build ClickHouse with the integration on:

```bash
mkdir build && cd build
DUCKDB_ROOT=/path/to/unpacked/zip cmake -DENABLE_DUCKDB=1 ..
ninja clickhouse
```

4. From the build directory, run it with the library visible:

```bash
export LD_LIBRARY_PATH=/path/to/unpacked/zip
./programs/clickhouse server
```

```sql
SELECT value FROM system.build_options WHERE name = 'USE_DUCKDB';
SELECT duckdbVersion();
```

For the general build requirements, see the [ClickHouse build instructions](https://clickhouse.com/docs/development/build).

### Configure

The table engine works without configuration. The table function and the extensions are off by default and are controlled only by the server configuration, which is a security boundary: a query cannot turn them on. Put this into a file in `config.d`:

```xml
<clickhouse>
    <duckdb>
        <allow_extensions>1</allow_extensions>
        <allowed_extensions>json, parquet</allowed_extensions>
        <allow_arbitrary_sql>1</allow_arbitrary_sql>
    </duckdb>
</clickhouse>
```

The same in YAML:

```yaml
duckdb:
    allow_extensions: 1
    allowed_extensions: json, parquet
    allow_arbitrary_sql: 1
```

| Key | Default | Meaning |
| --- | --- | --- |
| `allow_extensions` | `0` | Main switch for the extension functions. When off, DuckDB has no file system or network access. |
| `allowed_extensions` | empty | Names that can be installed or loaded. `*` allows any. |
| `preload_extensions` | empty | Loaded whenever a database is opened. |
| `extension_directory` | `duckdb_extensions` in the data directory | Where extensions are installed. |
| `allow_extension_loading` | `0` | Allows loading extensions that are not built into `libduckdb.so`. They are native code. |
| `allow_unsigned_extensions` | `0` | Allows extensions not signed by DuckDB. |
| `allow_community_extensions` | `0` | Allows installing from the DuckDB community repository. |
| `allow_extension_files` | `0` | Allows an extension file inside `user_files`. |
| `custom_extension_repository` | empty | Download extensions from another repository. |
| `allow_arbitrary_sql` | `0` | Enables the `duckdb` table function. |

> **Warning.** `allowed_extensions: "*"` together with `allow_extension_loading` and `allow_unsigned_extensions` lets any user with the `WRITE` grant on the `FILE` source run native code inside the server process. List the extensions by name and prefer the signed ones.

### Use

```sql
-- A DuckDB table, stored in user_files/app.duckdb
CREATE TABLE orders (id Int64, customer String, amount Decimal(10, 2))
ENGINE = DuckDB('app.duckdb', 'orders');

INSERT INTO orders VALUES (1, 'alice', 10.50), (2, 'bob', 20.00);
SELECT * FROM orders;

-- Run DuckDB SQL, with its extension functions (single quotes are doubled)
SELECT * FROM duckdb('', 'SELECT * FROM read_parquet(''/var/lib/clickhouse/user_files/data.parquet'')');

-- Which extension provides which function
SELECT arrayJoin(duckdbExtensionFunctions()) AS f WHERE f.name = 'read_parquet';
```

## Ingest into DuckDB, analyze in MergeTree

Writes go to a DuckDB table, and a materialized view copies each insert into a native `MergeTree` table:

```sql
CREATE TABLE orders_oltp (order_id Int64, customer String, amount Decimal(10, 2), created_at DateTime64(3, 'UTC'))
ENGINE = DuckDB('shop.duckdb', 'orders');

CREATE TABLE orders_mt AS orders_oltp ENGINE = MergeTree ORDER BY (created_at, order_id);

INSERT INTO orders_mt SELECT * FROM orders_oltp;                                        -- rows that exist now
CREATE MATERIALIZED VIEW orders_to_mt TO orders_mt AS SELECT * FROM orders_oltp;       -- each later insert

INSERT INTO orders_oltp VALUES (1, 'alice', 10.50, now());
SELECT customer, sum(amount) FROM orders_mt GROUP BY customer;
```

The view copies only inserts made through ClickHouse. The [guide](docs/guides/oss/duckdb.mdx) has the full example with a rollup, a catch-up query for rows written elsewhere, and a refreshable-view alternative.

## Transactions

Each `INSERT` is one DuckDB transaction: its rows stay invisible until it commits, and it is rolled back if it fails or is cancelled. Two inserts into one table can run at the same time. You cannot run multi-statement transactions. The copy into a `MergeTree` table by a materialized view is a separate step, outside the insert's transaction. Durability relies on DuckDB's write-ahead log and has not been tested against a server crash.

## Performance

Measured on 2026-11-19 in a Linux container on the machine below, with one insert thread and 1,000,000 rows.

| Part | Specification |
| --- | --- |
| Machine | Intel Core i5-7600 at 3.5 GHz, 4 cores (4 threads), 40 GB of RAM, macOS 13.7.8 |
| Linux VM | Colima 0.10.3 with the Docker 29.5.2 runtime, `x86_64`, 4 CPUs, 8 GB of RAM, Linux 6.8 |
| Software | ClickHouse 26.11, DuckDB v1.5.6 |
| Disk | The DuckDB file is on the container's file system inside the VM. A synchronous 4 KiB write there takes about 1.8 ms, and the rate of small inserts depends on it (see [Small inserts](#small-inserts)). |

| Table | Rows per second |
| --- | --- |
| 2 integer columns | about 3,100,000 |
| 9 mixed columns (`Int64`, `String`, `Float64`, `Date`, `DateTime64(6)`, `UUID`, `Decimal`, `Nullable(Int32)`, `Bool`) | about 500,000 |

Inserts use the DuckDB appender. Insert in batches, because every insert is a transaction with a fixed commit cost: about 66 rows per second at one row per insert, 6,300 at 100 rows, and 361,000 at 10,000 rows. The DuckDB settings `threads`, `ROW_GROUP_SIZE`, `preserve_insertion_order` and `COMPRESS` did not change the insert rate.

### Small inserts

Measured on 2026-11-21 on the same machine, a one-row insert took about 14 ms on a server without the full `config.xml`. The time splits into three parts:

| Part | Time per insert |
| --- | --- |
| ClickHouse's own work for the query, measured with a `Null` table | 7.6 ms |
| DuckDB's commit: one `fsync` of its write-ahead log | about 2 ms |
| DuckDB's statements and the engine's setup for each `INSERT` | about 5 ms |

ClickHouse's own work is the same with any table engine. Because of it, an insert of 100 rows took about as long as an insert of one row. Batching is the main remedy.

The sync depends on the disk. On a disk with slow syncs it becomes the larger part, so use storage with a low sync latency for many small transactions.

DuckDB has no setting that skips or delays the sync at commit. `checkpoint_threshold` (also called `wal_autocheckpoint`) and `immediate_transaction_mode` did not change the one-row rate. A very small `checkpoint_threshold` made it slower.

The server's full `config.xml`, with its system log tables and a trace-level log file, added about 2 ms to every query: 9.4 ms instead of 7.6 ms.

### Comparison with the `SQLite` and `PostgreSQL` engines

Measured on 2026-11-21 on the machine above, with 1,000,000 rows. Each figure is the median of three alternating rounds.

This build does not include the `SQLite` and `PostgreSQL` engines, so they ran in the official ClickHouse 26.12 build. PostgreSQL 17.11 ran with default settings in a container on the same VM, reached over local TCP. The SQLite file used default settings, with a rollback journal.

| Rows per second | `DuckDB` | `SQLite` | `PostgreSQL` |
| --- | --- | --- | --- |
| Insert, 2 integer columns | **2,800,000** | 1,570,000 | 1,320,000 |
| Insert, 9 mixed columns | **489,000** | 312,000 | 234,000 |
| Engine's own time for the 9-column insert | **1.77 s** | 3.03 s | 4.11 s |
| 1 row per insert | 61 | 56 | **94** |
| 100 rows per insert | 6,320 | 6,010 | **9,350** |
| 10,000 rows per insert | 381,000 | 328,000 | 399,000 |
| Scan, 2 integer columns | 2,270,000 | **3,460,000** | 2,420,000 |
| Scan, 9 columns with `count`, `sum` and `length` | **2,870,000** | 2,510,000 | 1,170,000 |
| Scan, all 9 columns | **906,000** | 791,000 | 614,000 |

The engine's own time is the 9-column insert minus an insert of the same rows into a `Null` table. The official build does ClickHouse's own bulk work faster: 5.9 million rows per second into `Null`, against 3.8 million for this build. The insert rows therefore understate the `DuckDB` engine. The engine's own time removes that difference.

The `DuckDB` figures come from a server with the full `config.xml`. Without it, as for the other two, the one-row rate was 70.

For a one-row insert, the time each engine adds to ClickHouse's own work is shown below. Both servers ran without `config.xml`, and the disk calls were counted with `strace`.

| Per one-row insert | `DuckDB` | `SQLite` | `PostgreSQL` |
| --- | --- | --- | --- |
| Total | 14.3 ms | 17.7 ms | 10.6 ms |
| ClickHouse's own work (`Null` table) | 7.6 ms | 7.8 ms | 7.8 ms |
| Engine and database | 6.7 ms | 9.9 ms | 2.8 ms |
| Disk calls | 1 `fsync` of a growing log file | 4 `fsync`, plus creating and deleting a journal file | 1 write and 1 `fdatasync` in the PostgreSQL server |

- **`PostgreSQL`** has the cheapest commit, even though it runs as a separate server. It syncs its log with `fdatasync` into files that already have their full size, which took 0.7 to 0.9 ms here, and the engine reuses pooled connections.
- **`SQLite`** syncs four times per commit with its default rollback journal.
- **`DuckDB`** syncs once, but with `fsync` on a log file that grows. That also writes the file system's metadata and took 1.8 to 2.2 ms here. The rest of its time is DuckDB's statement work and the engine's setup for each `INSERT`.
- **Scans:** no engine pushes aggregation down, so every row is copied into ClickHouse. PostgreSQL computed the same aggregates by itself about 5 times faster, using parallel workers.
- **Without ClickHouse:** PostgreSQL alone, through `psql`, did 625 one-row inserts per second.

## Supported statements

The `duckdb` table function runs the DuckDB statements that are valid as a subquery: `SELECT`, `FROM`-first queries, `DESCRIBE`, `SHOW`, `SHOW DATABASES`, `SHOW TABLES`, `SUMMARIZE`, `UNPIVOT` and `PIVOT` with an `IN` list. The other statements of the [DuckDB statement list](https://duckdb.org/docs/current/sql/statements/overview) are rejected before they run and change nothing. `CREATE TABLE`, `INSERT`, `SELECT` and `DROP TABLE` are available through the table engine, and `LOAD` and `INSTALL` through `duckdbLoadExtension` and `duckdbInstallExtension`.

## Limitations

- Linux only. `libduckdb.so` is a run-time dependency, and an extension must match its DuckDB version and platform.
- No `UPDATE`, `DELETE`, `TRUNCATE` or `ALTER` of the table engine. `DROP TABLE` drops only the ClickHouse table, not the DuckDB one.
- `WHERE`, `LIMIT` and aggregations are not pushed down into DuckDB for the table engine. Use the `duckdb` table function to filter inside DuckDB.
- A DuckDB file can be opened by one process only, so the `duckdb` command line cannot use it while the server holds it.
- A loaded extension is native code in the server process. It is not sandboxed and is not unloaded until the database is closed.

## Tests

The tests are stateless tests in `tests/queries/0_stateless/`, `05406` to `05413`: the engine, transactions, the functions, extensions and their settings, the table function, every DuckDB statement, and bulk inserts. Run them with a build that has `ENABLE_DUCKDB`, with `LD_LIBRARY_PATH` set as above and a server running:

```bash
python3 tests/clickhouse-test --binary ./clickhouse --queries tests/queries --no-stateful --no-shard \
    05406_storage_duckdb 05407_storage_duckdb_transactions 05408_duckdb_functions \
    05409_duckdb_extensions 05410_duckdb_table_function 05411_duckdb_extension_settings \
    05412_duckdb_statements 05413_storage_duckdb_bulk_insert
```

`05408_duckdb_functions` expects a server without the `duckdb` section. Installing and loading a real external extension needs network access and is a manual test, described in the guide.

## Documentation

The full guide is [`docs/guides/oss/duckdb.mdx`](docs/guides/oss/duckdb.mdx). It covers the build, every setting in XML and YAML, the extension model, ten manual test procedures, troubleshooting and the numbers above. Implementation notes for the build are in [`contrib/duckdb-cmake/README.md`](contrib/duckdb-cmake/README.md).

## Upstream

Everything else is ClickHouse. For installing and using it, see the [ClickHouse documentation](https://clickhouse.com/docs/), and to contribute to ClickHouse itself, go to the [upstream repository](https://github.com/ClickHouse/ClickHouse).
