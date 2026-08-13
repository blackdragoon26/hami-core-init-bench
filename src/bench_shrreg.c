/*
 * bench_shrreg.c — concurrent shared-region initialisation benchmark for HAMi-core.
 *
 * Links against HAMi-core's own multiprocess_memory_limit.o and calls the real
 * ensure_initialized(). Nothing is reimplemented.
 *
 * Protocol: fork N workers, each blocks reading a shared pipe. Parent closes the
 * write end to release all workers at the same instant. Each worker times exactly
 * one ensure_initialized() call and reports the duration back over a result pipe.
 * The shared-region cache file is removed before each round so every round
 * measures cold first-touch.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>

extern void ensure_initialized(void);

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double pct(double *sorted, int n, double p) {
    if (n <= 0) return 0.0;
    int idx = (int)(p * (n - 1) + 0.5);
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    return sorted[idx];
}

int main(int argc, char **argv) {
    int nproc = (argc > 1) ? atoi(argv[1]) : 8;
    if (nproc < 1) nproc = 1;

    int gate[2], results[2], hold[2];
    if (pipe(gate) != 0 || pipe(results) != 0 || pipe(hold) != 0) {
        perror("pipe");
        return 1;
    }

    for (int i = 0; i < nproc; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }
        if (pid == 0) {
            /* worker */
            close(gate[1]);
            close(results[0]);
            close(hold[1]);

            /* block until the parent closes the write end: releases all at once */
            char c;
            while (read(gate[0], &c, 1) > 0) { }
            close(gate[0]);

            double t0 = now_ms();
            ensure_initialized();
            double dt = now_ms() - t0;

            if (write(results[1], &dt, sizeof(dt)) != sizeof(dt)) {
                _exit(2);
            }
            close(results[1]);

            /* Stay resident until every worker has registered.
             * HAMi-core registers atexit(exit_handler), which frees this
             * process's slot on exit. If workers exited immediately the slot
             * table would drain as fast as it filled and later joiners would
             * scan an almost-empty table -- which is not the scenario this
             * benchmark is about (hundreds of processes concurrently resident).
             */
            while (read(hold[0], &c, 1) > 0) { }
            close(hold[0]);
            _exit(0);
        }
    }

    close(gate[0]);
    close(results[1]);
    close(hold[0]);

    /* let all workers reach the read() before releasing */
    struct timespec settle = {0, 200 * 1000 * 1000};
    nanosleep(&settle, NULL);

    double wall0 = now_ms();
    close(gate[1]);              /* release every worker simultaneously */

    double *samples = calloc(nproc, sizeof(double));
    int got = 0;
    while (got < nproc) {
        double d;
        ssize_t r = read(results[0], &d, sizeof(d));
        if (r == 0) break;
        if (r != sizeof(d)) continue;
        samples[got++] = d;
    }
    double wall = now_ms() - wall0;

    /* all workers have registered; now let them exit */
    close(hold[1]);

    int failed = 0;
    for (int i = 0; i < nproc; i++) {
        int st = 0;
        wait(&st);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) failed++;
    }
    close(results[0]);

    qsort(samples, got, sizeof(double), cmp_double);
    printf("{\"nproc\":%d,\"reported\":%d,\"failed\":%d,"
           "\"wall_ms\":%.3f,\"p50_ms\":%.3f,\"p95_ms\":%.3f,\"max_ms\":%.3f}\n",
           nproc, got, failed, wall,
           pct(samples, got, 0.50), pct(samples, got, 0.95),
           got ? samples[got - 1] : 0.0);
    free(samples);
    return 0;
}
