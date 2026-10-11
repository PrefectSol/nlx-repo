#include "core/cpu.h"
#include "core/system_error.h"

#include <sched.h>

namespace nlx::cpu
{

void set_affinity(pthread_t thread, std::initializer_list<int> cpus)
{
    cpu_set_t cpuset{};
    CPU_ZERO(&cpuset);

    for (int cpu : cpus)
    {
        CPU_SET(cpu, &cpuset);
    }

    const int result = ::pthread_setaffinity_np(thread, sizeof(cpuset), &cpuset);
    if (result != 0) [[unlikely]]
    {
        throw SystemError("pthread_setaffinity_np failed");
    }
}

void set_scheduling(pthread_t thread, int policy, int priority)
{
    sched_param param{};
    param.sched_priority = priority;

    const int result = ::pthread_setschedparam(thread, policy, &param);
    if (result != 0) [[unlikely]]
    {
        throw SystemError("pthread_setschedparam failed");
    }
}

} // namespace nlx::cpu
