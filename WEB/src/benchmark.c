#define _GNU_SOURCE
#include <sys/epoll.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/timerfd.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#define ROUNDS 50

static double seconds(struct timespec t) { return t.tv_sec + t.tv_nsec / 1000000000.0; }

static int wait_select(int fd)
{
    fd_set set; FD_ZERO(&set); FD_SET(fd, &set);
    return select(fd + 1, &set, NULL, NULL, NULL);
}

static int wait_poll(int fd)
{
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    return poll(&pfd, 1, -1);
}

static int wait_epoll(int epfd)
{
    struct epoll_event event;
    return epoll_wait(epfd, &event, 1, -1);
}

static int measure(const char *name, int mode)
{
    int timer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (timer < 0) return -1;
    struct itimerspec interval = {.it_value = {0, 10000000}, .it_interval = {0, 10000000}};
    if (timerfd_settime(timer, 0, &interval, NULL) < 0) { close(timer); return -1; }
    int epfd = -1;
    if (mode == 2) {
        epfd = epoll_create1(EPOLL_CLOEXEC);
        struct epoll_event event = {.events = EPOLLIN, .data.fd = timer};
        if (epfd < 0 || epoll_ctl(epfd, EPOLL_CTL_ADD, timer, &event) < 0) { if (epfd >= 0) close(epfd); close(timer); return -1; }
    }
    struct timespec wall_start, wall_end, cpu_start, cpu_end;
    clock_gettime(CLOCK_MONOTONIC, &wall_start);
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_start);
    for (int i = 0; i < ROUNDS; ++i) {
        int rc = mode == 0 ? wait_select(timer) : mode == 1 ? wait_poll(timer) : wait_epoll(epfd);
        if (rc < 0) { perror(name); if (epfd >= 0) close(epfd); close(timer); return -1; }
        uint64_t expirations;
        if (read(timer, &expirations, sizeof(expirations)) != sizeof(expirations)) { perror("read timerfd"); if (epfd >= 0) close(epfd); close(timer); return -1; }
    }
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_end);
    clock_gettime(CLOCK_MONOTONIC, &wall_end);
    printf("%-8s wall %.3fs, process CPU %.6fs\n", name, seconds(wall_end) - seconds(wall_start), seconds(cpu_end) - seconds(cpu_start));
    if (epfd >= 0) close(epfd);
    close(timer);
    return 0;
}

int main(void)
{
    puts("Blocking wait benchmark (50 timerfd wakeups at 10 ms intervals):");
    if (measure("select", 0) < 0 || measure("poll", 1) < 0 || measure("epoll", 2) < 0) return 1;
    puts("For idle waits, all three block instead of busy-spinning. epoll is intended to scale to larger descriptor sets.");
    return 0;
}
