/*
 * slop-open -- launch a viewer program on a file and wait for it.
 * Used by the file manager so opening a file blocks until the viewer exits,
 * keeping the screen owned by exactly one process at a time.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: slop-open <viewer> <file>\n"); return 2; }
    const char *viewer = argv[1];
    const char *file = argv[2];
    char p1[256], p2[256];
    snprintf(p1, sizeof(p1), "/usr/bin/%s", viewer);
    snprintf(p2, sizeof(p2), "/bin/%s", viewer);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl(p1, viewer, file, (char *)NULL);
        execl(p2, viewer, file, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) { int st = 0; waitpid(pid, &st, 0); return WEXITSTATUS(st); }
    return 1;
}
