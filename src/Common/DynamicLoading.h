#pragma once

#include <atomic>
#include <mutex>

namespace DB
{

/// `dlopen` is forbidden in the `clickhouse` binary (see `programs/main.cpp`), because loading uncontrolled native code is dangerous.
/// The only exception is a scope of `DynamicLoadingScope`, which the code that loads an extension of the embedded DuckDB opens
/// only when the server configuration allows it (`duckdb.allow_extension_loading`) and keeps open only for the time of the `LOAD`.
/// Outside of such a scope `dlopen` fails as before.
bool isDynamicLoadingAllowed() noexcept;

/// Scopes are serialized, so only one extension is being loaded at a time.
class DynamicLoadingScope
{
public:
    DynamicLoadingScope();
    ~DynamicLoadingScope();

    DynamicLoadingScope(const DynamicLoadingScope &) = delete;
    DynamicLoadingScope & operator=(const DynamicLoadingScope &) = delete;

private:
    std::unique_lock<std::mutex> lock;
};

}
