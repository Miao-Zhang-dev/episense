#define _GNU_SOURCE
#include "net.h"
#include "db.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_EVENTS 128

typedef struct {
    int fd;
    char request[HTTP_REQUEST_MAX];
    size_t used;
    char *response;
    size_t response_len;
    size_t sent;
} Client;

static void close_client(int epfd, Client *client)
{
    epoll_ctl(epfd, EPOLL_CTL_DEL, client->fd, NULL);
    close(client->fd);
    free(client->response);
    free(client);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    if (db_init("dictionary.db") < 0) {
        fprintf(stderr, "Could not open dictionary.db\n");
        return 1;
    }
    int listenfd = init_tcp_ser();
    if (listenfd < 0) { db_close(); return 1; }
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) { perror("epoll_create1"); close(listenfd); db_close(); return 1; }
    struct epoll_event ev = {.events = EPOLLIN, .data.ptr = NULL};
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &ev) < 0) {
        perror("epoll_ctl"); close(epfd); close(listenfd); db_close(); return 1;
    }
    printf("Sensor server listening on http://0.0.0.0:%d\n", SER_PORT);
    struct epoll_event events[MAX_EVENTS];
    for (;;) {
        int count = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (count < 0) { if (errno == EINTR) continue; perror("epoll_wait"); break; }
        for (int i = 0; i < count; ++i) {
            Client *client = events[i].data.ptr;
            if (!client) {
                for (;;) {
                    int fd = accept4(listenfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (fd < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) perror("accept"); break; }
                    client = calloc(1, sizeof(*client));
                    if (!client) { close(fd); continue; }
                    client->fd = fd;
                    ev.events = EPOLLIN | EPOLLRDHUP;
                    ev.data.ptr = client;
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) close_client(epfd, client);
                }
                continue;
            }
            int dead = 0;
            if (events[i].events & EPOLLIN) {
                for (;;) {
                    if (client->used == sizeof(client->request)) { dead = 1; break; }
                    ssize_t n = recv(client->fd, client->request + client->used, sizeof(client->request) - client->used, 0);
                    if (n > 0) client->used += (size_t)n;
                    else if (n == 0) { dead = 1; break; }
                    else if (errno == EINTR) continue;
                    else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    else { dead = 1; break; }
                    int ready = http_request_ready(client->request, client->used);
                    if (ready < 0) { dead = 1; break; }
                    if (ready > 0) {
                        if (handle_http_request(client->request, client->used, &client->response, &client->response_len) < 0) { dead = 1; break; }
                        ev.events = EPOLLOUT | EPOLLRDHUP;
                        ev.data.ptr = client;
                        if (epoll_ctl(epfd, EPOLL_CTL_MOD, client->fd, &ev) < 0) dead = 1;
                        break;
                    }
                }
            }
            if (!dead && (events[i].events & EPOLLOUT) && client->response) {
                while (client->sent < client->response_len) {
                    ssize_t n = send(client->fd, client->response + client->sent, client->response_len - client->sent, 0);
                    if (n > 0) client->sent += (size_t)n;
                    else if (n < 0 && errno == EINTR) continue;
                    else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                    else { dead = 1; break; }
                }
                if (client->sent == client->response_len) dead = 1;
            }
            if (!dead && (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) && !client->response) dead = 1;
            if (dead) close_client(epfd, client);
        }
    }
    close(epfd); close(listenfd); db_close();
    return 1;
}
