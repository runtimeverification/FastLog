#include <err.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#define NTIMES 1000
#define LENGTH (1000 * 1000)

static inline uint64_t
rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a" (lo), "=d" (hi));
    return ((uint64_t)hi << 32) | lo;
}

void
run(volatile int64_t *array, int length)
{
    for (int i = 0; i < length; i++) {
        volatile int64_t *addr = &array[i];
        *addr = i;
    }
}

int
main(int argc, char **argv)
{
    volatile int64_t *array;
    int i, length = LENGTH, ntimes = NTIMES;
    if (argc == 2) {
        length = atoi(argv[1]);
    }
    printf("length = %d, ntimes = %d\n", length, ntimes);

    array = malloc(sizeof(*array) * length);
    if (array == NULL)
        err(EXIT_FAILURE, "%s: malloc", __func__);
    uint64_t start = rdtsc();
    for (i = 0; i < ntimes; i++)
        run(array, length);
    uint64_t elapsed = rdtsc() - start;

    printf("%" PRIu64 " cycles, %g cycles/iteration\n",
            elapsed, (double)elapsed / ((double)ntimes * length));
    return EXIT_SUCCESS;
}