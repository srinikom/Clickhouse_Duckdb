-- Tags: no-fasttest

SELECT length(duckdbVersion()) > 0;

-- Extensions are off unless the server configuration enables them.
SELECT duckdbExtensions(); -- { serverError SUPPORT_IS_DISABLED }
SELECT duckdbExtensions('any.duckdb'); -- { serverError SUPPORT_IS_DISABLED }
SELECT duckdbInstallExtension('json'); -- { serverError SUPPORT_IS_DISABLED }
SELECT duckdbLoadExtension('any.duckdb', 'json'); -- { serverError SUPPORT_IS_DISABLED }
