#ifndef NLX_CORE_CPU_H
#define NLX_CORE_CPU_H

#include <initializer_list>
#include <sched.h>
#include <pthread.h>

namespace nlx::cpu
{

void set_affinity(pthread_t thread, std::initializer_list<int> cpus);
void set_scheduling(pthread_t thread, int policy, int priority);

} // namespace nlx::cpu
#endif // !NLX_CORE_CPU_H

