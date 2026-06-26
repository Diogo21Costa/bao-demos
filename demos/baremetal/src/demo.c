/**
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 */

#include <stdint.h>
#include <stdio.h>
#include <wfi.h>
#include <timer.h>

#define CACHE_LINE_SIZE 64ull
#define BUFFER_SIZE     (2ull * 1024ull * 1024ull)

#define SAMPLE_PERIOD_MS     100ull
#define BENCHMARK_DURATION_S 10ull
#define SAMPLE_COUNT         ((BENCHMARK_DURATION_S * 1000ull) / SAMPLE_PERIOD_MS)

#define CACHE_LINE_COUNT (BUFFER_SIZE / CACHE_LINE_SIZE)
#define TIMER_CHECK_BYTES 0x100000ull
#define PASSES_PER_TIMER_CHECK \
    ((BUFFER_SIZE < TIMER_CHECK_BYTES) ? (TIMER_CHECK_BYTES / BUFFER_SIZE) : 1ull)

#if ((BENCHMARK_DURATION_S * 1000ull) % SAMPLE_PERIOD_MS) != 0
#error "SAMPLE_PERIOD_MS must divide BENCHMARK_DURATION_S * 1000."
#endif

#if ((CACHE_LINE_COUNT & (CACHE_LINE_COUNT - 1ull)) != 0)
#error "CACHE_LINE_COUNT must be a power of two for the pointer-chase sequence."
#endif

static uint64_t buffer[BUFFER_SIZE / sizeof(uint64_t)] __attribute__((aligned(BUFFER_SIZE)));
static volatile uint64_t accesses_per_sample[SAMPLE_COUNT];
static volatile uint64_t elapsed_ticks_per_sample[SAMPLE_COUNT];

static inline void cache_invalidate(uint64_t addr)
{
    asm volatile("DC IVAC, %0" : : "r"(addr));
}

static void init_pointer_chase(void)
{
    const size_t stride = CACHE_LINE_SIZE / sizeof(uint64_t);

    for (size_t line = 0; line < CACHE_LINE_COUNT; line++) {
        size_t next_line = ((line * 5ull) + 1ull) & (CACHE_LINE_COUNT - 1ull);
        buffer[line * stride] = next_line * stride;
    }

    asm volatile("dsb sy" : : : "memory");
}

static void invalidate_buffer(void)
{
    const size_t stride = CACHE_LINE_SIZE / sizeof(uint64_t);

    for (size_t i = 0; i < (BUFFER_SIZE / sizeof(uint64_t)); i += stride) {
        cache_invalidate((uint64_t)&buffer[i]);
    }

    asm volatile("dsb sy" : : : "memory");
}

void main(void)
{
    const size_t stride = CACHE_LINE_SIZE / sizeof(uint64_t);
    const uint64_t accesses_per_pass = CACHE_LINE_COUNT;
    const uint64_t sample_period_ticks = TIME_MS(SAMPLE_PERIOD_MS);
    size_t chase_index = 0;
    uint64_t read_sink = 0;

    init_pointer_chase();

    printf("memory bandwidth benchmark\n");
    printf("buffer=%llu bytes cache_line=%llu bytes duration=%llu s sample=%llu ms\n",
           (unsigned long long)BUFFER_SIZE,
           (unsigned long long)CACHE_LINE_SIZE,
           (unsigned long long)BENCHMARK_DURATION_S,
           (unsigned long long)SAMPLE_PERIOD_MS);

    for (size_t sample = 0; sample < SAMPLE_COUNT; sample++) {
        uint64_t accesses = 0;
        uint64_t start = timer_get();
        uint64_t elapsed;

        do {
            for (size_t pass = 0; pass < PASSES_PER_TIMER_CHECK; pass++) {
                for (size_t access = 0; access < accesses_per_pass; access++) {
                    chase_index = buffer[chase_index];
                    read_sink += chase_index;
                }
                accesses += accesses_per_pass;
                invalidate_buffer();
            }

            elapsed = timer_get() - start;
        } while (elapsed < sample_period_ticks);

        accesses_per_sample[sample] = accesses;
        elapsed_ticks_per_sample[sample] = elapsed;
    }

    for (size_t sample = 0; sample < SAMPLE_COUNT; sample++) {
        uint64_t accesses = accesses_per_sample[sample];
        uint64_t bytes = accesses * CACHE_LINE_SIZE;
        uint64_t elapsed = elapsed_ticks_per_sample[sample];
        uint64_t bytes_per_second = (bytes * TIMER_FREQ) / elapsed;
        uint64_t mib_per_second = bytes_per_second / (1024ull * 1024ull);

        printf("sample[%02zu] accesses=%llu touched_bytes=%llu bw=%llu MiB/s\n",
               sample,
               (unsigned long long)accesses,
               (unsigned long long)bytes,
               (unsigned long long)mib_per_second);
    }

    printf("read sink = %llu\n", (unsigned long long)(read_sink + stride));

    while (1) {
        wfi();
    }
}
