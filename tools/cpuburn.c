/* cpuburn CPU_SECONDS  - spin until the process has used that much CPU time; print wall vs cpu time. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now(clockid_t c) { struct timespec t; clock_gettime(c, &t); return t.tv_sec + t.tv_nsec / 1e9; }

int main(int argc, char **argv) {
    double target = argc > 1 ? atof(argv[1]) : 2.0;
    double w0 = now(CLOCK_MONOTONIC);
    volatile double x = 1.0;
    while (now(CLOCK_PROCESS_CPUTIME_ID) < target)
        for (int i = 0; i < 200000; i++) x = x * 1.0000001 + 0.5;
    double wall = now(CLOCK_MONOTONIC) - w0, cpu = now(CLOCK_PROCESS_CPUTIME_ID);
    printf("wall=%.3f cpu=%.3f ratio=%.3f\n", wall, cpu, cpu / wall);
    return 0;
}
