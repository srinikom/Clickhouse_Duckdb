#!/usr/bin/env bash
# Tags: no-fasttest

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

# The `duckdb` table function and `duckdbExtensionFunctions` depend on the server configuration, so this test runs
# `clickhouse-local` with its own configurations.
WORK_DIR="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}"
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"
DB_FILE="$WORK_DIR/tf.duckdb"

write_config()
{
    cat > "$WORK_DIR/$1.xml" <<EOL
<clickhouse>
    <duckdb>
        <allow_extensions>1</allow_extensions>
        <allowed_extensions>json, parquet</allowed_extensions>
        <allow_arbitrary_sql>$2</allow_arbitrary_sql>
        <extension_directory>$WORK_DIR/extensions/</extension_directory>
    </duckdb>
</clickhouse>
EOL
}

write_config on 1
write_config off 0

ON="$CLICKHOUSE_LOCAL --config-file $WORK_DIR/on.xml"
OFF="$CLICKHOUSE_LOCAL --config-file $WORK_DIR/off.xml"

# A query, an in-memory database and a function of an extension (`json_extract_string` of `json`).
$ON --query "SELECT * FROM duckdb('', 'SELECT 42 AS answer, ''x'' AS s')"
$ON --query "SELECT v FROM duckdb('', 'SELECT json_extract_string(''{\"a\": \"b\"}'', ''\$.a'') AS v')"

# A database file.
$ON --multiquery "
CREATE TABLE t (x Int32, s String) ENGINE = DuckDB('$DB_FILE', 't');
INSERT INTO t VALUES (1, 'a'), (2, 'b'), (3, 'c');
SELECT sum(x), groupArray(s) FROM duckdb('$DB_FILE', 'SELECT * FROM t WHERE x > 1 ORDER BY x');
"

# The functions of the loaded extensions, mapped to their extensions.
$ON --query "SELECT arrayJoin(duckdbExtensionFunctions()) AS f WHERE f.name IN ('read_parquet', 'json_extract') ORDER BY f.name FORMAT TSV"

# The query is read-only and runs as a single statement.
$ON --query "INSERT INTO TABLE FUNCTION duckdb('', 'SELECT 1') VALUES (1)" 2>&1 | grep -o "NOT_IMPLEMENTED" | head -1
$ON --query "SELECT * FROM duckdb('$DB_FILE', 'SELECT 1 AS a) AS q; CREATE TABLE evil(x INT); SELECT * FROM (SELECT 1 AS a')" 2>&1 | grep -o "DUCKDB_ENGINE_ERROR" | head -1
$ON --query "SELECT count() FROM duckdb('$DB_FILE', 'SELECT table_name FROM duckdb_tables() WHERE table_name = ''evil''')"

# It is off by default.
$OFF --query "SELECT * FROM duckdb('', 'SELECT 1')" 2>&1 | grep -o "SUPPORT_IS_DISABLED" | head -1

rm -rf "$WORK_DIR"
