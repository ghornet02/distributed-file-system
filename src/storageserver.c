/**
 * @file storageserver.c
 * @brief Storage Server: holds file data on local disk and serves it to clients.
 *
 * A storage server keeps its files in a directory of its own. It registers
 * with the Name Server and refreshes that registration every
 * HEARTBEAT_INTERVAL seconds over a persistent connection. If the Name Server
 * is unreachable it keeps retrying, so either side can be restarted.
 *
 * Requests (one line each; every reply ends with a STOP line):
 *   CREATE <file>         create an empty file      (sent by the Name Server)
 *   DELETE <file>         remove a file             (sent by the Name Server)
 *   READ <file>           stream the file's lines   (sent by clients)
 *   WRITE <file> <text>   append a line to the file (sent by clients)
 *
 * Readers take a shared flock() and writers an exclusive one, so concurrent
 * clients never see a partially written line.
 */

#define _GNU_SOURCE
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define BACKLOG 16
#define HEARTBEAT_INTERVAL 5
#define RECONNECT_DELAY 3

static char storage_dir[PATH_MAX];
static char nm_ip[DFS_IP_MAX] = "127.0.0.1";
static int nm_port = 9000;
static char advertise_ip[DFS_IP_MAX] = "127.0.0.1";
static int listen_port;

/**
 * @brief Build the on-disk path for a file name.
 * @return 0 on success, -1 if the name is invalid or the path too long.
 */
static int build_path(const char *name, char *out, size_t cap)
{
    if (!valid_filename(name))
        return -1;
    int n = snprintf(out, cap, "%s/%s", storage_dir, name);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

/**
 * @brief Write an entire buffer to a file descriptor, retrying on short writes.
 * @return 0 on success, -1 on failure.
 */
static int write_full(int fd, const char *data, size_t len)
{
    while (len > 0)
    {
        ssize_t w = write(fd, data, len);
        if (w < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        data += w;
        len -= (size_t)w;
    }
    return 0;
}

/**
 * @brief Send a single-line reply followed by STOP.
 */
static void reply(int fd, const char *msg)
{
    send_line(fd, msg);
    send_line(fd, DFS_STOP);
}

/**
 * @brief Handle CREATE: create an empty file, failing if it already exists.
 */
static void handle_create(int fd, const char *name)
{
    char path[PATH_MAX];
    if (build_path(name, path, sizeof(path)) < 0)
    {
        reply(fd, "ERROR Invalid file name");
        return;
    }
    int file = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (file < 0)
    {
        reply(fd, errno == EEXIST ? "ERROR File already exists" : "ERROR Could not create file");
        return;
    }
    close(file);
    printf("[SS:%d] Created %s\n", listen_port, name);
    reply(fd, "OK File created");
}

/**
 * @brief Handle DELETE: remove a file.
 */
static void handle_delete(int fd, const char *name)
{
    char path[PATH_MAX];
    if (build_path(name, path, sizeof(path)) < 0)
    {
        reply(fd, "ERROR Invalid file name");
        return;
    }
    if (unlink(path) < 0)
    {
        reply(fd, errno == ENOENT ? "ERROR File not found" : "ERROR Could not delete file");
        return;
    }
    printf("[SS:%d] Deleted %s\n", listen_port, name);
    reply(fd, "OK File deleted");
}

/**
 * @brief Handle READ: stream every line of the file under a shared lock.
 */
static void handle_read(int fd, const char *name)
{
    char path[PATH_MAX];
    if (build_path(name, path, sizeof(path)) < 0)
    {
        reply(fd, "ERROR Invalid file name");
        return;
    }
    FILE *f = fopen(path, "r");
    if (!f)
    {
        reply(fd, "ERROR File not found");
        return;
    }
    flock(fileno(f), LOCK_SH);

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, f)) != -1)
    {
        if (n > 0 && line[n - 1] == '\n')
            line[n - 1] = '\0';
        if (send_content_line(fd, line) < 0)
            break;
    }
    free(line);
    flock(fileno(f), LOCK_UN);
    fclose(f);
    send_line(fd, DFS_STOP);
}

/**
 * @brief Handle WRITE: append one line to an existing file under an exclusive lock.
 */
static void handle_write(int fd, const char *name, const char *text)
{
    char path[PATH_MAX];
    if (build_path(name, path, sizeof(path)) < 0)
    {
        reply(fd, "ERROR Invalid file name");
        return;
    }
    int file = open(path, O_WRONLY | O_APPEND);
    if (file < 0)
    {
        reply(fd, "ERROR File not found");
        return;
    }
    flock(file, LOCK_EX);
    size_t len = strlen(text);
    int ok = write_full(file, text, len) == 0 && write_full(file, "\n", 1) == 0;
    flock(file, LOCK_UN);
    close(file);

    char msg[64];
    snprintf(msg, sizeof(msg), ok ? "OK Wrote %zu bytes" : "ERROR Write failed", len + 1);
    reply(fd, msg);
}

/**
 * @brief Per-connection thread: parse and execute requests until the peer disconnects.
 * @param arg Heap-allocated socket descriptor, freed by this function.
 */
static void *connection_handler(void *arg)
{
    int fd = *(int *)arg;
    free(arg);

    LineReader reader;
    reader_init(&reader, fd);
    char line[DFS_LINE_MAX];

    while (read_line(&reader, line, sizeof(line)) >= 0)
    {
        char cmd[16] = "", name[DFS_NAME_MAX] = "";
        int consumed = 0;
        if (sscanf(line, "%15s %127s %n", cmd, name, &consumed) < 2)
        {
            reply(fd, "ERROR Bad request");
            continue;
        }

        if (strcmp(cmd, "CREATE") == 0)
            handle_create(fd, name);
        else if (strcmp(cmd, "DELETE") == 0)
            handle_delete(fd, name);
        else if (strcmp(cmd, "READ") == 0)
            handle_read(fd, name);
        else if (strcmp(cmd, "WRITE") == 0)
            handle_write(fd, name, consumed ? line + consumed : "");
        else
            reply(fd, "ERROR Unknown command");
    }

    close(fd);
    return NULL;
}

/**
 * @brief Background thread that keeps this server registered with the Name Server.
 *
 * Sends REGISTER_SS every HEARTBEAT_INTERVAL seconds and reconnects whenever
 * the connection drops.
 */
static void *heartbeat_thread(void *arg)
{
    (void)arg;
    int warned = 0;
    for (;;)
    {
        int fd = connect_tcp(nm_ip, nm_port);
        if (fd < 0)
        {
            if (!warned)
                printf("[SS:%d] Name Server %s:%d unreachable, retrying\n", listen_port, nm_ip, nm_port);
            warned = 1;
            sleep(RECONNECT_DELAY);
            continue;
        }
        warned = 0;

        LineReader reader;
        reader_init(&reader, fd);
        char ack[DFS_LINE_MAX];
        int first = 1;
        while (send_fmt(fd, "REGISTER_SS %s %d", advertise_ip, listen_port) == 0 &&
               read_line(&reader, ack, sizeof(ack)) >= 0)
        {
            if (first || strncmp(ack, "OK", 2) != 0)
                printf("[SS:%d] Name Server: %s\n", listen_port, ack);
            first = 0;
            sleep(HEARTBEAT_INTERVAL);
        }
        close(fd);
        printf("[SS:%d] Lost connection to Name Server, reconnecting\n", listen_port);
        sleep(RECONNECT_DELAY);
    }
    return NULL;
}

/**
 * @brief Start a storage server.
 *
 * Usage: storageserver <port> <storage_dir> [nameserver_ip] [nameserver_port] [advertise_ip]
 */
int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "Usage: %s <port> <storage_dir> [nameserver_ip] [nameserver_port] [advertise_ip]\n", argv[0]);
        return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    listen_port = atoi(argv[1]);
    snprintf(storage_dir, sizeof(storage_dir), "%s", argv[2]);
    if (argc >= 4)
        snprintf(nm_ip, sizeof(nm_ip), "%s", argv[3]);
    if (argc >= 5)
        nm_port = atoi(argv[4]);
    if (argc >= 6)
        snprintf(advertise_ip, sizeof(advertise_ip), "%s", argv[5]);

    if (mkdir(storage_dir, 0755) < 0 && errno != EEXIST)
    {
        perror("mkdir");
        return 1;
    }

    int listen_fd = listen_tcp(listen_port, BACKLOG);
    if (listen_fd < 0)
    {
        perror("listen");
        return 1;
    }
    printf("[SS:%d] Serving %s, Name Server at %s:%d\n", listen_port, storage_dir, nm_ip, nm_port);

    pthread_t hb;
    pthread_create(&hb, NULL, heartbeat_thread, NULL);
    pthread_detach(hb);

    for (;;)
    {
        int client = accept(listen_fd, NULL, NULL);
        if (client < 0)
        {
            if (errno != EINTR)
                perror("accept");
            continue;
        }
        int *arg = malloc(sizeof(int));
        if (!arg)
        {
            close(client);
            continue;
        }
        *arg = client;
        pthread_t t;
        if (pthread_create(&t, NULL, connection_handler, arg) != 0)
        {
            close(client);
            free(arg);
            continue;
        }
        pthread_detach(t);
    }
}
