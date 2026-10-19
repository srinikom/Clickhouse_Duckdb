-- Tags: no-fasttest

DROP TABLE IF EXISTS duck;
DROP TABLE IF EXISTS duck_inferred;
DROP TABLE IF EXISTS duck_strict;

-- CREATE with explicit columns creates the table in the DuckDB file.
CREATE TABLE duck
(
    id Int64,
    name String,
    score Nullable(Float64),
    flag Bool,
    ts DateTime64(6, 'UTC'),
    d Date,
    u UUID,
    amount Decimal(10, 2)
)
ENGINE = DuckDB(currentDatabase() || '.duckdb', 'events');

INSERT INTO duck VALUES
    (1, 'alpha', 1.5, true, '2024-01-02 03:04:05.678901', '2024-01-02', '00000000-0000-0000-0000-000000000001', 12.34),
    (2, 'it''s "quoted"', NULL, false, '1999-12-31 23:59:59.000001', '1999-12-31', '00000000-0000-0000-0000-000000000002', -0.50);

SELECT * FROM duck ORDER BY id;
SELECT name, score IS NULL FROM duck WHERE id = 2;
SELECT count(), sum(id), max(ts) FROM duck;

-- The structure is inferred from the existing DuckDB table.
CREATE TABLE duck_inferred ENGINE = DuckDB(currentDatabase() || '.duckdb', 'events');
DESCRIBE TABLE duck_inferred;
SELECT * FROM duck_inferred ORDER BY id;

-- Writes through one table are visible through the other.
INSERT INTO duck_inferred (id, name, score, flag, ts, d, u, amount)
    VALUES (3, 'gamma', NULL, false, '2000-01-01 00:00:00', '2000-01-01', '00000000-0000-0000-0000-000000000003', 0);
SELECT count() FROM duck;

-- A NULL cannot be read into a non-Nullable column.
CREATE TABLE duck_strict (id Int64, score Float64) ENGINE = DuckDB(currentDatabase() || '.duckdb', 'events');
SELECT id, score FROM duck_strict ORDER BY id; -- { serverError CANNOT_INSERT_NULL_IN_ORDINARY_COLUMN }
SELECT id FROM duck_strict ORDER BY id;

-- An insert is one transaction: nothing of a failed insert is kept (the third row violates NOT NULL of `name` in DuckDB).
INSERT INTO duck_inferred
    SELECT number + 10, if(number = 2, NULL, 'x'), NULL, true, now64(6), today(), generateUUIDv4(), 1 FROM numbers(3); -- { serverError DUCKDB_ENGINE_ERROR }
SELECT count() FROM duck WHERE id >= 10;

-- A path outside of user_files is rejected.
CREATE TABLE duck_bad (id Int64) ENGINE = DuckDB('/etc/duck.duckdb', 'events'); -- { serverError PATH_ACCESS_DENIED }

DROP TABLE duck;
DROP TABLE duck_inferred;
DROP TABLE duck_strict;
