#ifndef NET_H
#define NET_H

#include <stddef.h>

#define SER_PORT 8080
#define HTTP_REQUEST_MAX 16384
#define HTTP_BODY_MAX 8192

int init_tcp_ser(void);
int http_request_ready(const char *request, size_t length);
int handle_http_request(const char *request, size_t length, char **response, size_t *response_length);

#endif
