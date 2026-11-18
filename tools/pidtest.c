/* pidtest N  - try to fork N sleeping children (bounded, safe); report how many succeeded. */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 50, ok = 0, err = 0;
    pid_t *kids = calloc(n, sizeof *kids);
    setvbuf(stdout, NULL, _IONBF, 0);
    for (int i = 0; i < n; i++) {
        pid_t p = fork();
        if (p == 0) { pause(); _exit(0); }
        if (p < 0) { err = errno; break; }
        kids[ok++] = p;
    }
    printf("forked=%d requested=%d %s%s\n", ok, n, err ? "failed: " : "", err ? strerror(err) : "");
    for (int i = 0; i < ok; i++) { kill(kids[i], SIGKILL); waitpid(kids[i], NULL, 0); }
    return 0;
}
