-- Tags: no-fasttest

DROP TABLE IF EXISTS bulk;
DROP TABLE IF EXISTS bulk_reordered;
DROP TABLE IF EXISTS narrow;
DROP TABLE IF EXISTS narrow_wide;

-- An insert bigger than a few vectors of the DuckDB appender, with every column kind.
CREATE TABLE bulk
(
    id Int64,
    u8 UInt8,
    s String,
    f Float64,
    d Date,
    ts DateTime64(6, 'UTC'),
    u UUID,
    a Decimal(10, 2),
    n Nullable(Int32),
    b Bool
)
ENGINE = DuckDB(currentDatabase() || '.duckdb', 'bulk');

INSERT INTO bulk
SELECT
    number,
    number % 256,
    toString(number),
    number / 4,
    toDate('2026-01-01') + number % 365,
    toDateTime64('2026-01-01 00:00:00', 6, 'UTC') + number,
    reinterpretAsUUID(unhex(lpad(hex(number), 32, '0'))),
    toDecimal32(number % 100000 / 100, 2),
    if(number % 7 = 0, NULL, toInt32(number)),
    number % 2 = 0
FROM numbers(300000);

SELECT count(), sum(id), sum(u8), uniqExact(s), sum(f), uniqExact(d), uniqExact(ts), uniqExact(u), sum(a), countIf(n IS NULL), sum(n), countIf(b) FROM bulk;

-- The values keep their exact form, including the UUID bytes.
SELECT id, u8, s, f, d, ts, u, a, n, b FROM bulk WHERE id IN (0, 7, 255, 65536, 299999) ORDER BY id;
SELECT count() FROM bulk WHERE u = reinterpretAsUUID(unhex(lpad(hex(id), 32, '0')));

-- The columns of the ClickHouse table can be in another order than in the DuckDB table.
CREATE TABLE bulk_reordered (b Bool, n Nullable(Int32), a Decimal(10, 2), u UUID, ts DateTime64(6, 'UTC'), d Date, f Float64, s String, u8 UInt8, id Int64)
ENGINE = DuckDB(currentDatabase() || '.duckdb', 'bulk');
INSERT INTO bulk_reordered VALUES (true, NULL, 1.25, '00000000-0000-0000-0000-0000000000ff', '2030-01-02 03:04:05.678901', '2030-01-02', 2.5, 'reordered', 7, -1);
SELECT id, u8, s, f, d, ts, u, a, n, b FROM bulk WHERE id = -1;

-- A failure in the middle of an insert leaves nothing of the statement.
CREATE TABLE narrow (v Int8) ENGINE = DuckDB(currentDatabase() || '.duckdb', 'narrow');
INSERT INTO narrow VALUES (1);
CREATE TABLE narrow_wide (v Int64) ENGINE = DuckDB(currentDatabase() || '.duckdb', 'narrow');
INSERT INTO narrow_wide SELECT number FROM numbers(300000); -- { serverError DUCKDB_ENGINE_ERROR }
SELECT count() FROM narrow;
INSERT INTO narrow_wide SELECT number % 100 FROM numbers(10);
SELECT count() FROM narrow;

DROP TABLE bulk;
DROP TABLE bulk_reordered;
DROP TABLE narrow;
DROP TABLE narrow_wide;
