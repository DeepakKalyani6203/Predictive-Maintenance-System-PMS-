#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <math.h>
#include <time.h>
#include <sys/wait.h>

typedef struct { int fd; pid_t pid; char buf[8192]; int len; } Proc;

static FILE  *out;
static double latest_temp = NAN;
static volatile sig_atomic_t running = 1;

static void on_sig(int s) { (void)s; running = 0; }

static int start(Proc *p, const char *path) {
    int pfd[2];
    if (pipe(pfd)) return -1;
    p->pid = fork();
    if (p->pid == 0) {
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[0]); close(pfd[1]);
        execl(path, path, (char *)NULL);
        perror("exec"); _exit(127);
    }
    close(pfd[1]);
    p->fd = pfd[0];
    p->len = 0;
    return 0;
}

static void on_adxl(char *line) {
    double ts; float x, y, z;
    if (sscanf(line, "%lf %f %f %f", &ts, &x, &y, &z) != 4) return;
    time_t sec = (time_t)ts;
    int us = (int)((ts - (double)sec) * 1e6);
    struct tm tm;
    localtime_r(&sec, &tm);
    char tb[32];
    strftime(tb, sizeof tb, "%Y-%m-%d %H:%M:%S", &tm);
    if (isnan(latest_temp))
        fprintf(out, "%s.%06d,%.3f,%.3f,%.3f,\n", tb, us, x, y, z);
    else
        fprintf(out, "%s.%06d,%.3f,%.3f,%.3f,%.2f\n", tb, us, x, y, z, latest_temp);
}

static void on_temp(char *line) {
    float t;
    if (sscanf(line, "%f", &t) == 1) latest_temp = t;
}

static void drain(Proc *p, void (*cb)(char *)) {
    ssize_t n = read(p->fd, p->buf + p->len, sizeof p->buf - p->len - 1);
    if (n <= 0) { close(p->fd); p->fd = -1; return; }
    p->len += n;
    p->buf[p->len] = 0;
    char *s = p->buf, *nl;
    while ((nl = strchr(s, '\n'))) { *nl = 0; cb(s); s = nl + 1; }
    p->len = (int)strlen(s);
    memmove(p->buf, s, p->len + 1);
}

int main(void) {
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    out = fopen("data.csv", "w");
    if (!out) { perror("data.csv"); return 1; }
    static char big[1 << 20];
    setvbuf(out, big, _IOFBF, sizeof big);
    fprintf(out, "Timestamp,ADXL345 X (g),ADXL345 Y (g),ADXL345 Z (g),DS18B20 Temp (°C)\n");

    Proc adxl = {0}, temp = {0};
    if (start(&adxl, "./adxl345") || start(&temp, "./ds18b20")) return 1;

    time_t last_flush = time(NULL);
    while (running && (adxl.fd >= 0 || temp.fd >= 0)) {
        struct pollfd pf[2] = {
            { adxl.fd, POLLIN, 0 },   // negative fd is ignored by poll
            { temp.fd, POLLIN, 0 }
        };
        if (poll(pf, 2, 50) < 0) continue;
        if (pf[1].revents & (POLLIN | POLLHUP)) drain(&temp, on_temp);   // temp first
        if (pf[0].revents & (POLLIN | POLLHUP)) drain(&adxl, on_adxl);
        if (time(NULL) != last_flush) { fflush(out); last_flush = time(NULL); }
    }

    kill(adxl.pid, SIGTERM);
    kill(temp.pid, SIGTERM);
    waitpid(adxl.pid, NULL, 0);
    waitpid(temp.pid, NULL, 0);
    fclose(out);
    return 0;
}
