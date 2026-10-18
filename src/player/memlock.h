/* src/player/memlock.h: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef AUDIOLAZ_MEMLOCK_H
#define AUDIOLAZ_MEMLOCK_H
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdlib.h>

static inline size_t mem_keep(int *refused) {
    *refused = 0;
    const size_t pg = (size_t)getpagesize();
    size_t locked = 0;
    unsigned char *vec = NULL; size_t vcap = 0;
    mach_vm_address_t a = 0;
    for (;;) {
        mach_vm_size_t sz = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS) break;
        /* writable, private (the code: macOS refuses to lock it, EPERM), at most 2 GB */
        if ((info.protection & VM_PROT_WRITE) && !info.shared && sz <= (2ull << 30)) {
            const size_t np = (size_t)(sz / pg);
            if (np > vcap) { free(vec); vcap = np; vec = malloc(vcap); }
            if (vec && mincore((void *)(uintptr_t)a, (size_t)sz, (char *)vec) == 0) {
                for (size_t i = 0; i < np;) {
                    if (!(vec[i] & MINCORE_INCORE)) { i++; continue; }
                    size_t j = i;
                    while (j < np && (vec[j] & MINCORE_INCORE)) j++;
                    if (mlock((void *)(uintptr_t)(a + i * pg), (j - i) * pg) == 0) locked += (j - i) * pg;
                    else (*refused)++;
                    i = j;
                }
            }
        }
        a += sz;
    }
    free(vec);
    return locked;
}
#else
static inline size_t mem_keep(int *refused) {
    *refused = 0;
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) { *refused = 1; return 0; }
    struct rusage u;
    return getrusage(RUSAGE_SELF, &u) == 0 ? (size_t)u.ru_maxrss * 1024 : 1;
}
#endif
/* RLIMIT_MEMLOCK as said in the log (bytes; SIZE_MAX: unlimited) */
static inline size_t mem_keep_limit(void) {
    struct rlimit r;
    if (getrlimit(RLIMIT_MEMLOCK, &r)) return 0;
    return r.rlim_cur == RLIM_INFINITY ? SIZE_MAX : (size_t)r.rlim_cur;
}
#endif
