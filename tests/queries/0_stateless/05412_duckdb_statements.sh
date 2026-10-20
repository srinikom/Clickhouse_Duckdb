#!/usr/bin/env bash
# Tags: no-fasttest

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

# Every statement of https://duckdb.org/docs/current/sql/statements/overview, run through the `duckdb` table function.
# The query is wrapped into a subquery of a read-only table function, so only statements that DuckDB accepts as a
# subquery produce rows. All the others are rejected by DuckDB before they run, and must leave the database unchanged.
WORK_DIR="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}"
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"
DB_FILE="$WORK_DIR/stmts.duckdb"

cat > "$WORK_DIR/config.xml" <<EOL
<clickhouse>
    <duckdb>
        <allow_extensions>1</allow_extensions>
        <allowed_extensions>json</allowed_extensions>
        <allow_arbitrary_sql>1</allow_arbitrary_sql>
        <extension_directory>$WORK_DIR/extensions/</extension_directory>
    </duckdb>
</clickhouse>
EOL

CH="$CLICKHOUSE_LOCAL --config-file $WORK_DIR/config.xml"

$CH --multiquery "
CREATE TABLE t (a Int32, b String) ENGINE = DuckDB('$DB_FILE', 't');
INSERT INTO t VALUES (1, 'x'), (2, 'y');
"

# Prints the rows of the statement, or the name of the error code when it is rejected.
stmt()
{
    local label="$1"
    local query="${2//\'/\'\'}"
    local out
    out=$($CH --query "SELECT * FROM duckdb('$DB_FILE', '$query') FORMAT TSV" 2>&1)
    if [ $? -eq 0 ]; then
        echo "$label: ok"
        echo "$out"
    else
        echo "$label: $(echo "$out" | grep -o '([A-Z_]*)' | tail -1)"
    fi
}

echo "-- statements accepted as a subquery"
stmt "SELECT" "SELECT * FROM t ORDER BY a"
stmt "FROM-first SELECT" "FROM t ORDER BY a"
stmt "DESCRIBE" "DESCRIBE t"
stmt "SHOW" "SHOW t"
stmt "SHOW DATABASES" "SHOW DATABASES"
stmt "SHOW TABLES" "SHOW TABLES"
stmt "SUMMARIZE" "SUMMARIZE t"
stmt "UNPIVOT" "UNPIVOT t ON a"
stmt "PIVOT with IN" "PIVOT t ON b IN ('x', 'y') USING count(*)"

echo "-- statements that DuckDB expands into several statements"
stmt "PIVOT without IN" "PIVOT t ON b USING count(*)"

echo "-- statements that are not queries"
stmt "ANALYZE" "ANALYZE"
stmt "ALTER TABLE" "ALTER TABLE t ADD COLUMN z INT"
stmt "ALTER VIEW" "ALTER VIEW v RENAME TO v2"
stmt "ATTACH" "ATTACH ':memory:' AS m2"
stmt "DETACH" "DETACH m2"
stmt "CALL" "CALL pragma_version()"
stmt "CHECKPOINT" "CHECKPOINT"
stmt "COMMENT ON" "COMMENT ON TABLE t IS 'x'"
stmt "COPY" "COPY t TO '$WORK_DIR/o.csv'"
stmt "CREATE INDEX" "CREATE INDEX i ON t(a)"
stmt "CREATE MACRO" "CREATE MACRO m(x) AS x + 1"
stmt "CREATE SCHEMA" "CREATE SCHEMA s"
stmt "CREATE SECRET" "CREATE SECRET (TYPE s3)"
stmt "CREATE SEQUENCE" "CREATE SEQUENCE sq"
stmt "CREATE TABLE" "CREATE TABLE t2 (a INT)"
stmt "CREATE VIEW" "CREATE VIEW v AS SELECT 1"
stmt "CREATE TYPE" "CREATE TYPE mood AS ENUM ('a')"
stmt "DELETE" "DELETE FROM t"
stmt "DROP" "DROP TABLE t"
stmt "EXPORT DATABASE" "EXPORT DATABASE '$WORK_DIR/exp'"
stmt "IMPORT DATABASE" "IMPORT DATABASE '$WORK_DIR/exp'"
stmt "INSERT" "INSERT INTO t VALUES (9, 'z')"
stmt "LOAD" "LOAD json"
stmt "INSTALL" "INSTALL json"
stmt "MERGE INTO" "MERGE INTO t USING t AS s ON t.a = s.a WHEN MATCHED THEN DELETE"
stmt "PRAGMA" "PRAGMA enable_profiling"
stmt "PREPARE" "PREPARE p AS SELECT 1"
stmt "EXECUTE" "EXECUTE p"
stmt "DEALLOCATE" "DEALLOCATE p"
stmt "SET" "SET threads = 1"
stmt "RESET" "RESET threads"
stmt "SET VARIABLE" "SET VARIABLE x = 1"
stmt "BEGIN" "BEGIN"
stmt "COMMIT" "COMMIT"
stmt "UPDATE" "UPDATE t SET a = 5"
stmt "USE" "USE memory"
stmt "VACUUM" "VACUUM"
stmt "EXPLAIN" "EXPLAIN SELECT * FROM t"

echo "-- the rejected statements changed nothing"
stmt "rows" "SELECT a, b FROM t ORDER BY a"
stmt "tables" "SELECT table_name FROM duckdb_tables() ORDER BY table_name"
stmt "views and schemas" "SELECT count() FROM duckdb_views() WHERE NOT internal"
test -e "$WORK_DIR/o.csv" && echo "COPY wrote a file" || echo "no file written"

# The statements that ClickHouse supports for the table engine map to DuckDB statements.
echo "-- ClickHouse statements on the engine"
$CH --multiquery "
CREATE TABLE e (id Int32, v String) ENGINE = DuckDB('$DB_FILE', 'e');
INSERT INTO e VALUES (1, 'a'), (2, 'b'), (3, 'c');
SELECT count(), sum(id) FROM e;
DESCRIBE TABLE e;
SELECT name FROM duckdb('$DB_FILE', 'SHOW TABLES') ORDER BY name;
DROP TABLE e;
SELECT name FROM duckdb('$DB_FILE', 'SHOW TABLES') ORDER BY name;
" 2>&1

for q in \
    "ALTER TABLE t ADD COLUMN z Int32" \
    "ALTER TABLE t UPDATE a = 5 WHERE a = 1" \
    "ALTER TABLE t DELETE WHERE a = 1" \
    "DELETE FROM t WHERE a = 1" \
    "UPDATE t SET a = 5 WHERE a = 1" \
    "TRUNCATE TABLE t"
do
    echo "$q: $($CH --multiquery "CREATE TABLE t (a Int32, b String) ENGINE = DuckDB('$DB_FILE', 't'); $q" 2>&1 | grep -o '([A-Z_]*)' | tail -1)"
done

rm -rf "$WORK_DIR"
