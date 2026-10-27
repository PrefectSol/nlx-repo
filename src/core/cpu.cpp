#include "core/cpu.h"

#include <cerrno>
#include <sched.h>

namespace nlx::cpu
{

bool set_affinity(pthread_t thread, std::initializer_list<int> cpus) noexcept
{
    cpu_set_t cpuset{};
    CPU_ZERO(&cpuset);

    for (int cpu : cpus)
    {
        if (cpu < 0 || cpu >= CPU_SETSIZE) [[unlikely]]
        {
            errno = EINVAL;
            return false;
        }

        CPU_SET(cpu, &cpuset);
    }

    const int result = ::pthread_setaffinity_np(thread, sizeof(cpuset), &cpuset);
    if (result != 0) [[unlikely]]
    {
        errno = result;
        return false;
    }

    return true;
}

bool set_scheduling(pthread_t thread, int policy, int priority) noexcept
{
    sched_param param{};
    param.sched_priority = priority;

    const int result = ::pthread_setschedparam(thread, policy, &param);
    if (result != 0) [[unlikely]]
    {
        errno = result;
        return false;
    }

    return true;
}

bool set_name(pthread_t thread, const char *name) noexcept
{
    if (name == nullptr) [[unlikely]]
    {
        errno = EINVAL;
        return false;
    }

    const int result = ::pthread_setname_np(thread, name);
    if (result != 0) [[unlikely]]
    {
        errno = result;
        return false;
    }

    return true;
}

pthread_t this_native_handle() noexcept
{
    return ::pthread_self();
}

} // namespace nlx::cpu
