#include "config.h"

#if USE_DUCKDB

#include <Common/Exception.h>
#include <Interpreters/evaluateConstantExpression.h>
#include <Parsers/ASTFunction.h>
#include <Storages/StorageDuckDB.h>
#include <Storages/checkAndGetLiteralArgument.h>
#include <TableFunctions/ITableFunction.h>
#include <TableFunctions/TableFunctionFactory.h>
#include <TableFunctions/registerTableFunctions.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
}

namespace
{

class TableFunctionDuckDB : public ITableFunction
{
public:
    static constexpr auto name = "duckdb";
    std::string getName() const override { return name; }

private:
    StoragePtr executeImpl(
        const ASTPtr & ast_function, ContextPtr context, const std::string & table_name, ColumnsDescription cached_columns, bool is_insert_query) const override;

    const char * getStorageEngineName() const override { return "DuckDB"; }

    ColumnsDescription getActualTableStructure(ContextPtr context, bool is_insert_query) const override;
    void parseArguments(const ASTPtr & ast_function, ContextPtr context) override;

    std::shared_ptr<StorageDuckDB> createStorage(const String & table_name, ContextPtr context) const
    {
        return std::make_shared<StorageDuckDB>(
            StorageID(getDatabaseName(), table_name),
            database_path,
            "query",
            ColumnsDescription{},
            ConstraintsDescription{},
            /* comment = */ "",
            context,
            /* create_remote_table = */ false,
            query);
    }

    String database_path;
    String query;
};

StoragePtr TableFunctionDuckDB::executeImpl(
    const ASTPtr & /*ast_function*/, ContextPtr context, const std::string & table_name, ColumnsDescription /*cached_columns*/, bool /*is_insert_query*/) const
{
    auto storage = createStorage(table_name, context);
    storage->startup();
    return storage;
}

ColumnsDescription TableFunctionDuckDB::getActualTableStructure(ContextPtr context, bool /*is_insert_query*/) const
{
    auto storage = createStorage("duckdb_query", context);
    const auto metadata = storage->getInMemoryMetadataPtr(context, /* bypass_metadata_cache */ true);
    return metadata->getColumns();
}

void TableFunctionDuckDB::parseArguments(const ASTPtr & ast_function, ContextPtr context)
{
    const auto & func_args = ast_function->as<ASTFunction &>();
    if (!func_args.arguments)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Table function 'duckdb' must have arguments.");

    ASTs & args = func_args.arguments->children;
    if (args.size() != 2)
        throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH, "Table function 'duckdb' requires 2 arguments: database path, query");

    checkDuckDBArbitrarySQLAllowed(context);

    for (auto & arg : args)
        arg = evaluateConstantExpressionOrIdentifierAsLiteral(arg, context);

    database_path = resolveDuckDBDatabasePath(checkAndGetLiteralArgument<String>(args[0], "database_path"), context, /* allow_in_memory */ true);
    query = checkAndGetLiteralArgument<String>(args[1], "query");
}

}

void registerTableFunctionDuckDB(TableFunctionFactory & factory);
void registerTableFunctionDuckDB(TableFunctionFactory & factory)
{
    factory.registerFunction<TableFunctionDuckDB>({.description = R"DOCS_MD(
Runs a query in an embedded [DuckDB](https://duckdb.org) database and returns its result. It makes the functions of the DuckDB extensions, such as `read_parquet`, available.

The function is disabled unless `duckdb.allow_arbitrary_sql` is set in the server configuration, because the query is passed to DuckDB as is and can do everything DuckDB can, including reading files and writing to the database.
It needs the `READ` grant on the `FILE` source. See the [`DuckDB`](/reference/engines/table-engines/integrations/duckdb) table engine for the configuration.

## Syntax {#syntax}

```sql
duckdb('database_path', 'query')
```

## Arguments {#arguments}

- `database_path` — Path to a DuckDB database file inside `user_files`, or an empty string for an empty in-memory database. [String](/reference/data-types/string).
- `query` — A DuckDB `SELECT` query. It must not end with a semicolon. [String](/reference/data-types/string).

## Returned value {#returned-value}

- A read-only table with the columns of the query. The columns are `Nullable`.

## Example {#example}

```sql title="Query"
SELECT * FROM duckdb('', 'SELECT * FROM read_parquet(''/var/lib/clickhouse/user_files/data.parquet'')');
```

Use `duckdbExtensionFunctions` to list the functions that the loaded extensions provide.
)DOCS_MD", .category = FunctionDocumentation::Category::TableFunction}, SecretArgumentsSpec{});
}

}

#endif
