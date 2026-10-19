# The DuckDB table engine uses only the C API of DuckDB (`duckdb.h`), so it can be linked against the prebuilt
# `libduckdb.so` from https://github.com/duckdb/duckdb/releases (the `libduckdb-linux-*.zip` assets).
#
# The library is not searched for: its location is named explicitly by the environment variable `DUCKDB_ROOT`,
# a directory with `duckdb.h` and `libduckdb.so`, so that a build cannot pick up whatever a machine happens to carry.
#
#   DUCKDB_ROOT=/path/to/unpacked/zip cmake -DENABLE_DUCKDB=1 ...
option(ENABLE_DUCKDB "Enable the DuckDB table engine (links a prebuilt libduckdb.so)" OFF)

if (NOT ENABLE_DUCKDB OR NOT OS_LINUX)
    message(STATUS "Not using DuckDB")
    return()
endif()

set(DUCKDB_DIR "$ENV{DUCKDB_ROOT}")
if (NOT DUCKDB_DIR)
    message(FATAL_ERROR "ENABLE_DUCKDB requires the environment variable DUCKDB_ROOT: a directory with duckdb.h and libduckdb.so")
endif()

set(DUCKDB_HEADER "${DUCKDB_DIR}/duckdb.h")
set(DUCKDB_LIBRARY "${DUCKDB_DIR}/libduckdb.so")
foreach (duckdb_file IN ITEMS "${DUCKDB_HEADER}" "${DUCKDB_LIBRARY}")
    if (NOT EXISTS "${duckdb_file}")
        message(FATAL_ERROR "ENABLE_DUCKDB: ${duckdb_file} does not exist (DUCKDB_ROOT=${DUCKDB_DIR})")
    endif()
endforeach()

add_library(_duckdb SHARED IMPORTED GLOBAL)
set_target_properties(_duckdb PROPERTIES IMPORTED_LOCATION "${DUCKDB_LIBRARY}")
target_include_directories(_duckdb SYSTEM INTERFACE "${DUCKDB_DIR}")
add_library(ch_contrib::duckdb ALIAS _duckdb)

message(STATUS "Using DuckDB: ${DUCKDB_LIBRARY}")
