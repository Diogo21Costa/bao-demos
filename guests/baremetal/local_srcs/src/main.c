/** 
 * Bao, a Lightweight Static Partitioning Hypervisor 
 *
 * Copyright (c) Bao Project (www.bao-project.org), 2019-
 *
 * Authors:
 *      Jose Martins <jose.martins@bao-project.org>
 *      Sandro Pinto <sandro.pinto@bao-project.org>
 *
 * Bao is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License version 2 as published by the Free
 * Software Foundation, with a special exception exempting guest code from such
 * license. See the COPYING file in the top-level directory for details. 
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <cpu.h>
#include <wfi.h>
#include <spinlock.h>
#include <timer.h>

/* Platform-related configurations */
#define CACHE_LINE_SIZE     64

/* Benchmark-related configurations */
#define NUM_CPUS            1
#define INTERF_BUF_SIZE     0x80000 //0x100000
#define ENABLE_READS        0
#define ENABLE_WRITES       1
#define IVAC_CACHE        0
#define CIVAC_CACHE        0

/* Sampling configuration */
#define SAMPLE_PERIOD_MS     100
#define BENCHMARK_DURATION_S 10

#if (((BENCHMARK_DURATION_S) * 1000) % SAMPLE_PERIOD_MS) != 0
#error "SAMPLE_PERIOD_MS must divide BENCHMARK_DURATION_S*1000 exactly."
#endif

#define SAMPLE_COUNT ((BENCHMARK_DURATION_S * 1000) / SAMPLE_PERIOD_MS)

#define CPU_INTERF_BUF_SIZE (INTERF_BUF_SIZE / NUM_CPUS)
volatile uint64_t cpu_interf_buf[NUM_CPUS][CPU_INTERF_BUF_SIZE / sizeof(uint64_t)]
    __attribute__((aligned(INTERF_BUF_SIZE)));
volatile uint64_t accesses_per_sample[NUM_CPUS][SAMPLE_COUNT];
volatile uint64_t sample_elapsed_ticks[NUM_CPUS][SAMPLE_COUNT];

spinlock_t cpu_ticket_lock = SPINLOCK_INITVAL;
spinlock_t print_lock = SPINLOCK_INITVAL;

enum {
    HC_PROFILE_CPU_READY = 9,
};

#define SMCC32_FID_VND_HYP_SRVC 0x86000000

static uint64_t send_hypercall(uint64_t hc_id, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    register uint64_t r0 asm("x0") = SMCC32_FID_VND_HYP_SRVC | hc_id;
    register uint64_t r1 asm("x1") = arg1;
    register uint64_t r2 asm("x2") = arg2;
    register uint64_t r3 asm("x3") = arg3;

    asm volatile("hvc   #0\n"
                 : "=r"(r0)
                 : "r"(r0), "r"(r1), "r"(r2), "r"(r3));
    return r0;
}

static uint64_t profiler_set_cpu_ready(void)
{
    return send_hypercall(HC_PROFILE_CPU_READY, 0, 0, 0);
}

#if (IVAC_CACHE || CIVAC_CACHE)
static inline void cache_clean_and_invalidate(uint64_t addr)
{
    asm volatile("DC CIVAC, %0" : : "r"(addr));
}

static inline void cache_invalidate(uint64_t addr)
{
    asm volatile("DC IVAC, %0" : : "r"(addr));
}
#endif

void main(void)
{
    static volatile size_t buf_index_ticket = 0;
    size_t buf_index;

    spin_lock(&cpu_ticket_lock);
    buf_index = buf_index_ticket;
    buf_index_ticket += 1;
    spin_unlock(&cpu_ticket_lock);

    if (buf_index >= NUM_CPUS) {
        while (1) {
            wfi();
        }
    }

    const size_t bufid = buf_index;
    const size_t range = CPU_INTERF_BUF_SIZE / sizeof(uint64_t);
    const size_t stride = CACHE_LINE_SIZE / sizeof(uint64_t);
    const size_t base = 0;
    const uint64_t sample_period_ticks = TIME_MS(SAMPLE_PERIOD_MS);
#if (ENABLE_READS || ENABLE_WRITES)
    const uint64_t accesses_per_pass = (range + stride - 1) / stride;
#endif

#if ENABLE_READS
    volatile uint64_t read_sink = 0;
#endif

    // profiler_set_cpu_ready();

    for (size_t sample_index = 0; sample_index < SAMPLE_COUNT; sample_index++) {
        uint64_t accesses_in_current_sample = 0;
        uint64_t sample_start = timer_get();
        uint64_t elapsed_ticks = 0;

        do {
#if ENABLE_WRITES
            for (size_t i = base; i < (base + range); i += stride) {
                cpu_interf_buf[bufid][i] = i;
            }
            accesses_in_current_sample += accesses_per_pass;
#endif

#if ENABLE_READS
            for (size_t i = base; i < (base + range); i += stride) {
                read_sink ^= cpu_interf_buf[bufid][i];
            }
            accesses_in_current_sample += accesses_per_pass;
#endif

 #if IVAC_CACHE
            for (size_t i = base; i < (base + range); i += stride) {
                cache_invalidate((uint64_t)&cpu_interf_buf[bufid][i]);
            }
 #endif

 #if CIVAC_CACHE
            for (size_t i = base; i < (base + range); i += stride) {
                cache_clean_and_invalidate((uint64_t)&cpu_interf_buf[bufid][i]);
            }
 #endif

#if (IVAC_CACHE || CIVAC_CACHE)
            asm volatile("dsb sy" : : : "memory");
#endif
            elapsed_ticks = timer_get() - sample_start;
        } while (elapsed_ticks < sample_period_ticks);

        accesses_per_sample[bufid][sample_index] = accesses_in_current_sample;
        sample_elapsed_ticks[bufid][sample_index] = elapsed_ticks;
    }

    spin_lock(&print_lock);
    printf("cpu%zu memory bandwidth samples (%us total, %ums interval)\n",
           bufid, BENCHMARK_DURATION_S, SAMPLE_PERIOD_MS);
    for (size_t i = 0; i < SAMPLE_COUNT; i++) {
        uint64_t accesses = accesses_per_sample[bufid][i];
        uint64_t bytes = accesses * sizeof(uint64_t);
        uint64_t elapsed_ticks = sample_elapsed_ticks[bufid][i];
        uint64_t bytes_per_second = (elapsed_ticks != 0)
            ? ((bytes * TIMER_FREQ) / elapsed_ticks)
            : 0;
        uint64_t mib_per_second = bytes_per_second / (1024ull * 1024ull);

        printf("sample[%02zu] accesses=%llu bytes=%llu bw=%llu MiB/s\n", i,
               (unsigned long long)accesses,
               (unsigned long long)bytes,
               (unsigned long long)mib_per_second);
    }
#if ENABLE_READS
    printf("cpu%zu read sink = %llu\n", bufid, (unsigned long long)read_sink);
#endif
    spin_unlock(&print_lock);

    while (1) {
        wfi();
    }
}
