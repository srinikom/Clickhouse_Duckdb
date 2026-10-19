#include <Common/DynamicLoading.h>

namespace DB
{

namespace
{
std::mutex scope_mutex;
std::atomic<bool> allowed{false};
}

bool isDynamicLoadingAllowed() noexcept
{
    return allowed.load(std::memory_order_acquire);
}

DynamicLoadingScope::DynamicLoadingScope()
    : lock(scope_mutex)
{
    allowed.store(true, std::memory_order_release);
}

DynamicLoadingScope::~DynamicLoadingScope()
{
    allowed.store(false, std::memory_order_release);
}

}
