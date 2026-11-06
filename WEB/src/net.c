#define _GNU_SOURCE
#include "net.h"
#include "db.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

int init_tcp_ser(void)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("socket"); return -1; }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(SER_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 128) < 0) {
        perror("bind/listen"); close(fd); return -1;
    }
    return fd;
}

int http_request_ready(const char *request, size_t length)
{
    const char *end = NULL;
    for (size_t i = 3; i < length; ++i)
        if (!memcmp(request + i - 3, "\r\n\r\n", 4)) { end = request + i + 1; break; }
    if (!end) return length > HTTP_BODY_MAX ? -1 : 0;
    size_t header_len = (size_t)(end - request);
    size_t body_len = 0;
    const char *line = memmem(request, header_len, "\r\n", 2);
    if (!line) return -1;
    line += 2;
    while (line < end - 2) {
        const char *next = memmem(line, (size_t)(end - line), "\r\n", 2);
        if (!next || next == line) break;
        if ((size_t)(next - line) > 15 && !strncasecmp(line, "Content-Length:", 15)) {
            char number[32]; const char *value = line + 15; size_t n = (size_t)(next - value);
            while (n && isspace((unsigned char)*value)) { ++value; --n; }
            if (n >= sizeof(number)) return -1;
            memcpy(number, value, n); number[n] = '\0';
            char *tail; errno = 0; unsigned long parsed = strtoul(number, &tail, 10);
            while (isspace((unsigned char)*tail)) ++tail;
            if (errno || *tail || parsed > HTTP_BODY_MAX) return -1;
            body_len = parsed;
        }
        line = next + 2;
    }
    if (header_len + body_len > HTTP_REQUEST_MAX) return -1;
    return length >= header_len + body_len ? 1 : 0;
}

static int url_decode(char *out, size_t cap, const char *src, size_t length)
{
    size_t j = 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c == '+') c = ' ';
        else if (c == '%') {
            if (i + 2 >= length || !isxdigit((unsigned char)src[i + 1]) || !isxdigit((unsigned char)src[i + 2])) return -1;
            char hex[3] = {src[i + 1], src[i + 2], 0};
            c = (unsigned char)strtoul(hex, NULL, 16); i += 2;
            if (!c) return -1;
        }
        if (j + 1 >= cap) return -1;
        out[j++] = (char)c;
    }
    out[j] = '\0';
    return 0;
}

static int form_value(const char *body, const char *key, char *out, size_t cap)
{
    size_t keylen = strlen(key);
    const char *p = body;
    while (p && *p) {
        const char *end = strchr(p, '&'); if (!end) end = p + strlen(p);
        const char *eq = memchr(p, '=', (size_t)(end - p));
        if (eq && (size_t)(eq - p) == keylen && !memcmp(p, key, keylen)) {
            return url_decode(out, cap, eq + 1, (size_t)(end - eq - 1)) == 0;
        }
        p = *end ? end + 1 : NULL;
    }
    if (cap) out[0] = '\0';
    return 0;
}

static int query_value(const char *query, const char *key, char *out, size_t cap)
{
    return form_value(query, key, out, cap);
}

static int parse_number(const char *text, double *value)
{
    char *end;
    if (!text || !*text) return -1;
    *value = strtod(text, &end);
    return end == text || *end || !isfinite(*value) ? -1 : 0;
}

static void json_error(char *out, size_t cap, const char *message)
{
    snprintf(out, cap, "{\"error\":\"%s\"}", message);
}

static int respond(int status, const char *type, const char *extra, const char *body, size_t body_len, char **out, size_t *out_len)
{
    const char *reason = status == 200 ? "OK" : status == 201 ? "Created" : status == 400 ? "Bad Request" : status == 401 ? "Unauthorized" : status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed" : status == 409 ? "Conflict" : status == 413 ? "Payload Too Large" : "Internal Server Error";
    int head = snprintf(NULL, 0, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n%s\r\n", status, reason, type, body_len, extra ? extra : "");
    if (head < 0) return -1;
    *out = malloc((size_t)head + body_len + 1);
    if (!*out) return -1;
    int n = snprintf(*out, (size_t)head + 1, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n%s\r\n", status, reason, type, body_len, extra ? extra : "");
    memcpy(*out + n, body, body_len);
    *out_len = (size_t)n + body_len;
    return 0;
}

static int static_file(const char *path, char **body, size_t *length, const char **type)
{
    const char *file = NULL;
    if (!strcmp(path, "/") || !strcmp(path, "/1.html")) { file = "../source/1.html"; *type = "text/html; charset=utf-8"; }
    else if (!strcmp(path, "/2.html")) { file = "../source/2.html"; *type = "text/html; charset=utf-8"; }
    else if (!strcmp(path, "/3.html")) { file = "../source/3.html"; *type = "text/html; charset=utf-8"; }
    else {
        static const char *images[] = {"/images/back.jpg", "/images/houyi.jpg", "/images/kai.jpg", "/images/liyuanfang.jpg", "/images/sesefadou.jpg", "/images/sunshangxiang.jpg", "/images/wangzhaojun.jpg", "/images/yase.jpg"};
        for (size_t i = 0; i < sizeof(images) / sizeof(images[0]); ++i) {
            if (!strcmp(path, images[i])) { file = malloc(128); if (!file) return -1; snprintf((char *)file, 128, "../source%s", path); *type = "image/jpeg"; break; }
        }
        if (!file) return 0;
    }
    FILE *fp = fopen(file, "rb");
    if (strncmp(path, "/images/", 8) == 0) free((void *)file);
    if (!fp) return 0;
    if (fseek(fp, 0, SEEK_END) || (*length = (size_t)ftell(fp)) > 512 * 1024 || fseek(fp, 0, SEEK_SET)) { fclose(fp); return -1; }
    *body = malloc(*length ? *length : 1);
    if (!*body) { fclose(fp); return -1; }
    int ok = fread(*body, 1, *length, fp) == *length;
    fclose(fp);
    return ok ? 1 : -1;
}

int handle_http_request(const char *request, size_t length, char **response, size_t *response_length)
{
    char req[HTTP_REQUEST_MAX + 1];
    memcpy(req, request, length); req[length] = '\0';
    char method[8] = {0}, target[512] = {0};
    if (sscanf(req, "%7s %511s", method, target) != 2) return -1;
    char *headers = strstr(req, "\r\n");
    char *body = strstr(req, "\r\n\r\n");
    if (!headers || !body) return -1;
    body += 4;
    *headers = '\0';
    char *query = strchr(target, '?');
    if (query) *query++ = '\0';
    char cookie[128] = {0};
    char *h = headers + 2;
    while (*h) {
        char *next = strstr(h, "\r\n"); if (!next) break;
        if (!strncasecmp(h, "Cookie:", 7)) {
            char *s = strstr(h, "session=");
            if (s && s < next) { s += 8; size_t n = strcspn(s, "; \r\n"); if (n < sizeof(cookie)) { memcpy(cookie, s, n); cookie[n] = 0; } }
        }
        h = next + 2;
    }
    char content[65536] = {0}, extra[256] = {0};
    size_t content_len = 0;
    const char *type = "application/json; charset=utf-8";
    int status = 200, user_id = 0;

    if (!strcmp(target, "/api/auth/register") && !strcmp(method, "POST")) {
        char name[65], password[129];
        if (!form_value(body, "username", name, sizeof(name)) || !form_value(body, "password", password, sizeof(password)) || strlen(password) < 6) { status = 400; json_error(content, sizeof(content), "用户名不能为空，密码至少需要 6 位"); }
        else {
            user_id = db_register_user(name, password);
            if (user_id == 0) { status = 409; json_error(content, sizeof(content), "用户名已存在"); }
            else if (user_id < 0) { status = 500; json_error(content, sizeof(content), "注册失败"); }
            else {
                char token[65];
                if (db_create_session(user_id, token) < 0) { status = 500; json_error(content, sizeof(content), "会话创建失败"); }
                else { snprintf(extra, sizeof(extra), "Set-Cookie: session=%s; HttpOnly; SameSite=Lax; Path=/; Max-Age=86400\r\n", token); snprintf(content, sizeof(content), "{\"ok\":true}"); }
            }
        }
    } else if (!strcmp(target, "/api/auth/login") && !strcmp(method, "POST")) {
        char name[65], password[129];
        if (!form_value(body, "username", name, sizeof(name)) || !form_value(body, "password", password, sizeof(password))) { status = 400; json_error(content, sizeof(content), "用户名和密码不能为空"); }
        else user_id = db_login_user(name, password);
        if (user_id < 0) { status = 500; json_error(content, sizeof(content), "登录失败"); }
        else if (status == 200 && !user_id) { status = 401; json_error(content, sizeof(content), "用户名或密码错误"); }
        else if (status == 200) {
            char token[65];
            if (db_create_session(user_id, token) < 0) { status = 500; json_error(content, sizeof(content), "会话创建失败"); }
            else { snprintf(extra, sizeof(extra), "Set-Cookie: session=%s; HttpOnly; SameSite=Lax; Path=/; Max-Age=86400\r\n", token); snprintf(content, sizeof(content), "{\"ok\":true}"); }
        }
    } else if (!strcmp(target, "/api/auth/logout") && !strcmp(method, "POST")) {
        db_delete_session(cookie);
        snprintf(extra, sizeof(extra), "Set-Cookie: session=; HttpOnly; SameSite=Lax; Path=/; Max-Age=0\r\n");
        snprintf(content, sizeof(content), "{\"ok\":true}");
    } else if (!strncmp(target, "/api/", 5)) {
        user_id = db_session_user(cookie);
        if (!user_id) { status = 401; json_error(content, sizeof(content), "请先登录"); }
        else if (!strcmp(target, "/api/sensors") && !strcmp(method, "GET")) {
            char filter[128] = {0}; if (query) query_value(query, "q", filter, sizeof(filter));
            int n = db_list_sensors(filter, content, sizeof(content));
            if (n < 0) { status = 500; json_error(content, sizeof(content), "读取传感器失败"); }
            else content_len = (size_t)n;
        } else if (!strcmp(target, "/api/sensors") && !strcmp(method, "POST")) {
            char name[81], location[121], temp[32], humidity[32];
            if (!form_value(body, "name", name, sizeof(name)) || !form_value(body, "location", location, sizeof(location)) || !form_value(body, "temperature", temp, sizeof(temp)) || !form_value(body, "humidity", humidity, sizeof(humidity))) { status = 400; json_error(content, sizeof(content), "传感器字段不完整"); }
            else {
                double t, hu; int bad = parse_number(temp, &t) < 0 || parse_number(humidity, &hu) < 0;
                int result = bad ? -1 : db_save_sensor(0, name, location, t, hu);
                if (result < 0) { status = 400; json_error(content, sizeof(content), "数据无效或传感器名称重复"); }
                else { status = 201; snprintf(content, sizeof(content), "{\"id\":%d}", result); }
            }
        } else if (!strcmp(target, "/api/sensors") && (!strcmp(method, "PUT") || !strcmp(method, "DELETE"))) {
            char id_text[16] = {0}; query_value(query, "id", id_text, sizeof(id_text));
            char *id_end; long parsed_id = strtol(id_text, &id_end, 10); int id = *id_text && !*id_end && parsed_id > 0 && parsed_id <= 2147483647 ? (int)parsed_id : 0;
            if (id <= 0) { status = 400; json_error(content, sizeof(content), "缺少有效的传感器 id"); }
            else if (!strcmp(method, "DELETE")) {
                int result = db_delete_sensor(id);
                if (result <= 0) { status = result < 0 ? 500 : 404; json_error(content, sizeof(content), "传感器不存在或删除失败"); }
                else snprintf(content, sizeof(content), "{\"ok\":true}");
            } else {
                char name[81], location[121], temp[32], humidity[32];
                form_value(body, "name", name, sizeof(name)); form_value(body, "location", location, sizeof(location));
                form_value(body, "temperature", temp, sizeof(temp)); form_value(body, "humidity", humidity, sizeof(humidity));
                double t = 0, hu = 0; int bad = !*name || !*location || parse_number(temp, &t) < 0 || parse_number(humidity, &hu) < 0;
                int result = bad ? -1 : db_save_sensor(id, name, location, t, hu);
                if (result < 0) { status = 400; json_error(content, sizeof(content), "数据无效或传感器名称重复"); }
                else if (!result) { status = 404; json_error(content, sizeof(content), "传感器不存在"); }
                else snprintf(content, sizeof(content), "{\"ok\":true}");
            }
        } else { status = 404; json_error(content, sizeof(content), "接口不存在"); }
    } else if (!strcmp(method, "GET")) {
        char *file_body = NULL; size_t file_len = 0; const char *file_type = "application/octet-stream";
        int found = static_file(target, &file_body, &file_len, &file_type);
        if (found <= 0) { status = found < 0 ? 500 : 404; json_error(content, sizeof(content), "页面或资源不存在"); }
        else {
            int rc = respond(200, file_type, "", file_body, file_len, response, response_length);
            free(file_body); return rc;
        }
    } else { status = 405; json_error(content, sizeof(content), "请求方法不支持"); }

    if (!content_len) content_len = strlen(content);
    return respond(status, type, extra, content, content_len, response, response_length);
}
