#!/usr/bin/env bash
# Tags: no-fasttest

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

# The settings of the extensions of the embedded DuckDB, written in YAML, and the checks that need no network.
WORK_DIR="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}"
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"
DB_FILE="$WORK_DIR/ext.duckdb"

# Not an extension, only a file with this name: DuckDB has to reject its contents, ClickHouse only has to let the load through.
FAKE_EXTENSION="$WORK_DIR/fake.duckdb_extension"
echo "not an extension" > "$FAKE_EXTENSION"

write_config()
{
    cat > "$WORK_DIR/$1.yaml" <<EOL
duckdb:
    allow_extensions: 1
    allowed_extensions: json, parquet
    allow_arbitrary_sql: 1
    allow_extension_files: $2
    allow_extension_loading: $3
    allow_unsigned_extensions: $4
    allow_community_extensions: $4
    custom_extension_repository: https://example.invalid/extensions
    extension_directory: $WORK_DIR/extensions/
EOL
}

write_config closed 1 0 0
write_config open 1 1 1
write_config nofiles 0 0 0

run()
{
    local config=$1
    shift
    $CLICKHOUSE_LOCAL --config-file "$WORK_DIR/$config.yaml" --multiquery --query "$@" 2>&1
}

# The settings reach DuckDB.
SETTINGS="SELECT current_setting(''allow_unsigned_extensions'')::VARCHAR AS unsigned, current_setting(''allow_community_extensions'')::VARCHAR AS community, current_setting(''custom_extension_repository'')::VARCHAR AS repository"
run closed "SELECT * FROM duckdb('', '$SETTINGS')"
run open "SELECT * FROM duckdb('', '$SETTINGS')"

# A table keeps the database open.
CREATE="CREATE TABLE t (x Int32) ENGINE = DuckDB('$DB_FILE', 't')"

# An extension file is refused unless files are allowed.
run nofiles "$CREATE; SELECT duckdbLoadExtension('$DB_FILE', '$FAKE_EXTENSION')" | grep -o "SUPPORT_IS_DISABLED" | head -1

# Loading native code is refused unless it is allowed.
run closed "$CREATE; SELECT duckdbLoadExtension('$DB_FILE', '$FAKE_EXTENSION')" | grep -o "SUPPORT_IS_DISABLED" | head -1

# When it is allowed, the call is passed to DuckDB, which rejects the file.
run open "$CREATE; SELECT duckdbLoadExtension('$DB_FILE', '$FAKE_EXTENSION')" | grep -o "DUCKDB_ENGINE_ERROR" | head -1

# A missing file is rejected.
run open "$CREATE; SELECT duckdbLoadExtension('$DB_FILE', '$WORK_DIR/missing.duckdb_extension')" | grep -o "BAD_ARGUMENTS" | head -1

# Repositories.
run closed "SELECT duckdbInstallExtension('json', 'community')" | grep -o "SUPPORT_IS_DISABLED" | head -1
run open "SELECT duckdbInstallExtension('json', 'unknown')" | grep -o "BAD_ARGUMENTS" | head -1

# The allowlist is still checked.
run open "SELECT duckdbInstallExtension('httpfs')" | grep -o "BAD_ARGUMENTS" | head -1

rm -rf "$WORK_DIR"
