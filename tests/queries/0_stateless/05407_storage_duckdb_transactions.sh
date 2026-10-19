#!/usr/bin/env bash
# Tags: no-fasttest

CURDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CURDIR"/../shell_config.sh

DB_FILE="${CLICKHOUSE_DATABASE}_transactions.duckdb"
rm -f "${USER_FILES_PATH:?}/${DB_FILE}"*

$CLICKHOUSE_CLIENT -m -q "
DROP TABLE IF EXISTS tx;
DROP TABLE IF EXISTS tx_nullable;
CREATE TABLE tx (id Int64, writer String) ENGINE = DuckDB('$DB_FILE', 'tx');
-- The same DuckDB table with Nullable columns, which can send a NULL into the NOT NULL column of the table.
CREATE TABLE tx_nullable ENGINE = DuckDB('$DB_FILE', 'tx');
"

# An insert that is slowed down to one row per 50 ms, so that it can be observed while it is running.
function slow_insert()
{
    local table=$1 query_id=$2 first_id=$3 writer_expr=$4
    $CLICKHOUSE_CLIENT --query_id "${CLICKHOUSE_DATABASE}_$query_id" -q "
        INSERT INTO $table SELECT $first_id + number, $writer_expr FROM numbers(30) WHERE sleepEachRow(0.05) = 0
        SETTINGS max_block_size = 1, max_threads = 1" 2>&1
}

# Waits until the insert has read at least the given number of rows, i.e. some of its rows are already written to DuckDB.
function wait_for_rows()
{
    local query_id="${CLICKHOUSE_DATABASE}_$1" rows=$2
    while [ "$($CLICKHOUSE_CLIENT -q "SELECT count() FROM system.processes WHERE query_id = '$query_id' AND read_rows >= $rows")" != 1 ]
    do
        if [ "$($CLICKHOUSE_CLIENT -q "SELECT count() FROM system.processes WHERE query_id = '$query_id'")" == 0 ]
        then
            echo "The insert $1 finished before it was observed"
            return 1
        fi
        sleep 0.1
    done
}

echo "--- the rows of a running insert are not visible, and are visible after it commits"
slow_insert tx insert_visibility 0 "'a'" > /dev/null &
wait_for_rows insert_visibility 5
$CLICKHOUSE_CLIENT -q "SELECT 'while running', count() FROM tx"
wait
$CLICKHOUSE_CLIENT -q "SELECT 'after commit', count() FROM tx"

echo "--- a cancelled insert is rolled back"
slow_insert tx insert_cancel 100 "'b'" > /dev/null &
wait_for_rows insert_cancel 5
$CLICKHOUSE_CLIENT -q "KILL QUERY WHERE query_id = '${CLICKHOUSE_DATABASE}_insert_cancel' SYNC FORMAT Null"
wait
$CLICKHOUSE_CLIENT -q "SELECT 'after kill', count(), countIf(writer = 'b') FROM tx"

echo "--- two writers at the same time"
slow_insert tx writer_1 1000 "'w1'" > /dev/null &
slow_insert tx writer_2 2000 "'w2'" > /dev/null &
wait_for_rows writer_1 5
wait_for_rows writer_2 5
$CLICKHOUSE_CLIENT -q "SELECT 'while running', count() FROM tx"
wait
$CLICKHOUSE_CLIENT -q "SELECT 'after commit', writer, count(), sum(id) FROM tx GROUP BY writer ORDER BY writer"

echo "--- a failing writer does not affect a concurrent writer"
slow_insert tx writer_3 3000 "'w3'" > /dev/null &
# The row 20 of this insert sends a NULL into a NOT NULL column of DuckDB, which fails the insert.
slow_insert tx_nullable writer_4 4000 "if(number = 20, NULL, 'w4')" | grep -o "DUCKDB_ENGINE_ERROR" | head -1
wait
$CLICKHOUSE_CLIENT -q "SELECT 'after both', writer, count() FROM tx WHERE writer IN ('w3', 'w4') GROUP BY writer ORDER BY writer"
$CLICKHOUSE_CLIENT -q "SELECT 'total', count() FROM tx"

$CLICKHOUSE_CLIENT -m -q "
DROP TABLE tx;
DROP TABLE tx_nullable;
"
rm -f "${USER_FILES_PATH:?}/${DB_FILE}"*
