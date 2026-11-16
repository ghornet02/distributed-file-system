/**
 * @file common.c
 * @brief Implementation of the shared networking and protocol helpers.
 */

#include "common.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int connect_tcp(const char *ip, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1 ||
        connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

int listen_tcp(int port, int backlog)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, backlog) < 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

int send_all(int fd, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len)
    {
        ssize_t w = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (w < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        sent += (size_t)w;
    }
    return 0;
}

int send_line(int fd, const char *line)
{
    if (send_all(fd, line, strlen(line)) < 0)
        return -1;
    return send_all(fd, "\n", 1);
}

int send_fmt(int fd, const char *fmt, ...)
{
    char buf[DFS_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return send_line(fd, buf);
}

void reader_init(LineReader *r, int fd)
{
    r->fd = fd;
    r->len = 0;
}

ssize_t read_line(LineReader *r, char *out, size_t cap)
{
    for (;;)
    {
        char *nl = memchr(r->buf, '\n', r->len);
        if (nl || r->len == sizeof(r->buf))
        {
            size_t line_len = nl ? (size_t)(nl - r->buf) : r->len;
            size_t consumed = nl ? line_len + 1 : line_len;
            size_t copy = line_len < cap - 1 ? line_len : cap - 1;
            memcpy(out, r->buf, copy);
            out[copy] = '\0';
            if (copy > 0 && out[copy - 1] == '\r')
                out[--copy] = '\0';
            memmove(r->buf, r->buf + consumed, r->len - consumed);
            r->len -= consumed;
            return (ssize_t)copy;
        }

        ssize_t n = recv(r->fd, r->buf + r->len, sizeof(r->buf) - r->len, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
        {
            if (r->len == 0)
                return -1;
            size_t copy = r->len < cap - 1 ? r->len : cap - 1;
            memcpy(out, r->buf, copy);
            out[copy] = '\0';
            r->len = 0;
            return (ssize_t)copy;
        }
        r->len += (size_t)n;
    }
}

int send_content_line(int fd, const char *line)
{
    if (line[0] == '.' || strcmp(line, DFS_STOP) == 0)
    {
        if (send_all(fd, ".", 1) < 0)
            return -1;
    }
    return send_line(fd, line);
}

const char *unstuff_line(const char *line)
{
    return line[0] == '.' ? line + 1 : line;
}

int valid_filename(const char *name)
{
    size_t len = strlen(name);
    if (len == 0 || len >= DFS_NAME_MAX)
        return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;
    for (size_t i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)name[i];
        if (c == '/' || isspace(c) || iscntrl(c))
            return 0;
    }
    return 1;
}
