#!/usr/bin/env bash
# Tags: no-fasttest

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

# The extensions are enabled by the server configuration, so this test runs `clickhouse-local` with its own configuration.
# `json` and `parquet` are linked into DuckDB, so loading them needs no network.
WORK_DIR="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}"
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"
DB_FILE="$WORK_DIR/ext.duckdb"
CONFIG_FILE="$WORK_DIR/config.xml"

cat > "$CONFIG_FILE" <<EOL
<clickhouse>
    <duckdb>
        <allow_extensions>1</allow_extensions>
        <allowed_extensions>json, parquet</allowed_extensions>
        <preload_extensions>json</preload_extensions>
        <extension_directory>$WORK_DIR/extensions/</extension_directory>
    </duckdb>
</clickhouse>
EOL

LOCAL="$CLICKHOUSE_LOCAL --config-file $CONFIG_FILE"

# A table keeps its database open, so the loaded extensions are visible to the functions.
# `json` and `parquet` are built into the library, so they are loaded from the start.
$LOCAL --multiquery "
CREATE TABLE t (x Int32) ENGINE = DuckDB('$DB_FILE', 't');
SELECT 'listed', arrayMap(e -> (e.name, e.loaded), arrayFilter(e -> e.name IN ('json', 'parquet'), duckdbExtensions('$DB_FILE')));
SELECT 'load', duckdbLoadExtension('$DB_FILE', 'parquet');
SELECT 'listed after load', arrayMap(e -> (e.name, e.loaded), arrayFilter(e -> e.name IN ('json', 'parquet'), duckdbExtensions('$DB_FILE')));
"

# Only the allowed extensions with a plain name are accepted, and a path is an extension file, which is off.
$LOCAL --query "SELECT duckdbLoadExtension('$DB_FILE', 'httpfs')" 2>&1 | grep -o "BAD_ARGUMENTS" | head -1
$LOCAL --query "SELECT duckdbInstallExtension('httpfs')" 2>&1 | grep -o "BAD_ARGUMENTS" | head -1
$LOCAL --query "SELECT duckdbLoadExtension('$DB_FILE', '../json')" 2>&1 | grep -o "SUPPORT_IS_DISABLED" | head -1

rm -rf "$WORK_DIR"
