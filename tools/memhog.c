/* memhog MB [hold_seconds]  - allocate and touch MB mebibytes, 1 MiB at a time (test workload). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    long mb = argc > 1 ? atol(argv[1]) : 100;
    int hold = argc > 2 ? atoi(argv[2]) : 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    for (long i = 1; i <= mb; i++) {
        char *p = malloc(1 << 20);
        if (!p) { printf("malloc failed at %ld MiB\n", i); return 3; }
        memset(p, 0xA5, 1 << 20); /* touch every page so it is really charged to the cgroup */
        if (i % 10 == 0 || i == mb) printf("allocated %ld MiB\n", i);
    }
    printf("done allocating %ld MiB\n", mb);
    if (hold > 0) sleep(hold);
    return 0;
}
