# DuckDB for the `DuckDB` table engine

The engine (`src/Storages/StorageDuckDB.cpp`) uses only the DuckDB C API, and DuckDB is linked as a prebuilt shared library
instead of being compiled from source. This is Linux only, and it is off by default.

## Building

1. Download and unpack the `libduckdb-linux-amd64.zip` (or `-arm64`) asset of a release
   from <https://github.com/duckdb/duckdb/releases>. It contains `duckdb.h` and `libduckdb.so`. The engine was tested with v1.5.6.
2. Name the directory with the environment variable `DUCKDB_ROOT` (the library is deliberately not searched for)
   and enable the option:

```bash
DUCKDB_ROOT=/path/to/unpacked/zip cmake -DENABLE_DUCKDB=1 ...
ninja clickhouse
```

3. `libduckdb.so` is a run-time dependency of the `clickhouse` binary: put its directory into `LD_LIBRARY_PATH` or install the
   library next to the binary.

The tests are in `tests/queries/0_stateless/05406_storage_duckdb.sql`. They need a build with `ENABLE_DUCKDB`.

## Why a shared library

It avoids compiling DuckDB (hundreds of translation units) and keeping a fork of its CMake build. The cost is that the binary is no longer
self-contained, that the library is not built with the ClickHouse toolchain and sanitizers, and that two C++ runtimes
(`libc++` and `libstdc++`) live in one process. Building DuckDB from source as a regular `contrib` module would remove these
costs and is the way to go for an upstream-ready change.

## Extensions

The `duckdb` section of the server configuration (`allow_extensions`, `allowed_extensions`, `preload_extensions`,
`extension_directory`) and the functions `duckdbVersion`, `duckdbExtensions`, `duckdbInstallExtension`, `duckdbLoadExtension`
manage DuckDB extensions, see the documentation of the engine. The tests are `05408_duckdb_functions` and `05409_duckdb_extensions`.
Only the extensions linked into `libduckdb.so` can be loaded: the `clickhouse` binary does not allow `dlopen`.
