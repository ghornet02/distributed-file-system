/**
 * @file nameserver.c
 * @brief Name Server: the metadata and coordination node of the distributed file system.
 *
 * The Name Server tracks which storage servers are alive and which server
 * holds each file, together with the file's owner. Clients send every request
 * here. Metadata operations (CREATE, DELETE, VIEW, INFO) are answered
 * directly, with CREATE and DELETE forwarded to the storage server. Data
 * operations (READ, WRITE) get an SS redirect so the client can talk to the
 * storage server directly.
 *
 * Each connection runs on its own thread. A monitor thread evicts storage
 * servers whose heartbeat is older than the timeout (15 seconds by default). Their files stay
 * in the table and become available again when the server re-registers.
 *
 * Client protocol (one line per request):
 *   REGISTER_CLIENT <user> [port]  -> OK Registered client <user>
 *   CREATE <file>                  -> OK File created on <ip>:<port> | ERROR ...
 *   DELETE <file>                  -> OK File deleted | ERROR ...   (owner only)
 *   VIEW [-a]                      -> FILES: / <entry>... / END
 *   INFO <file>                    -> INFO ... | ERROR ...
 *   READ <file>                    -> SS <ip> <port> READ <file>
 *   WRITE <file> <text>            -> SS <ip> <port> WRITE <file> <text>  (owner only)
 *
 * Storage-server protocol:
 *   REGISTER_SS <ip> <port>        -> OK Registered SS   (repeated as a heartbeat)
 */

#define _GNU_SOURCE
#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BACKLOG 32
#define DEFAULT_SS_TIMEOUT 15

/**
 * @brief A registered storage server and the time of its last heartbeat.
 */
typedef struct StorageServer
{
    char ip[DFS_IP_MAX];
    int port;
    time_t last_seen;
    struct StorageServer *next;
} StorageServer;

/**
 * @brief Metadata for one file. The hosting server is stored by address, not by pointer.
 */
typedef struct FileEntry
{
    char name[DFS_NAME_MAX];
    char owner[DFS_USER_MAX];
    char ss_ip[DFS_IP_MAX];
    int ss_port;
    time_t created;
    struct FileEntry *next;
} FileEntry;

static StorageServer *ss_list = NULL;
static FileEntry *file_list = NULL;
static pthread_mutex_t ss_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t file_lock = PTHREAD_MUTEX_INITIALIZER;
static int ss_timeout = DEFAULT_SS_TIMEOUT;

/**
 * @brief Register a storage server, or refresh its heartbeat if it is already known.
 */
static void register_storage_server(const char *ip, int port)
{
    pthread_mutex_lock(&ss_lock);
    for (StorageServer *s = ss_list; s; s = s->next)
    {
        if (strcmp(s->ip, ip) == 0 && s->port == port)
        {
            s->last_seen = time(NULL);
            pthread_mutex_unlock(&ss_lock);
            return;
        }
    }
    StorageServer *s = calloc(1, sizeof(StorageServer));
    if (s)
    {
        snprintf(s->ip, sizeof(s->ip), "%s", ip);
        s->port = port;
        s->last_seen = time(NULL);
        s->next = ss_list;
        ss_list = s;
    }
    pthread_mutex_unlock(&ss_lock);
    if (s)
        printf("[NM] Storage server %s:%d online\n", ip, port);
}

/**
 * @brief Check whether a storage server is currently registered.
 * @return Non-zero if alive.
 */
static int ss_is_alive(const char *ip, int port)
{
    int alive = 0;
    pthread_mutex_lock(&ss_lock);
    for (StorageServer *s = ss_list; s && !alive; s = s->next)
        alive = strcmp(s->ip, ip) == 0 && s->port == port;
    pthread_mutex_unlock(&ss_lock);
    return alive;
}

/**
 * @brief Pick the live storage server that holds the fewest files.
 * @param ip Receives the chosen server's IP.
 * @param port Receives the chosen server's port.
 * @return 0 on success, -1 if no storage server is registered.
 */
static int choose_storage_server(char *ip, int *port)
{
    int best = -1;
    pthread_mutex_lock(&ss_lock);
    pthread_mutex_lock(&file_lock);
    for (StorageServer *s = ss_list; s; s = s->next)
    {
        int count = 0;
        for (FileEntry *f = file_list; f; f = f->next)
            count += strcmp(f->ss_ip, s->ip) == 0 && f->ss_port == s->port;
        if (best < 0 || count < best)
        {
            best = count;
            snprintf(ip, DFS_IP_MAX, "%s", s->ip);
            *port = s->port;
        }
    }
    pthread_mutex_unlock(&file_lock);
    pthread_mutex_unlock(&ss_lock);
    return best < 0 ? -1 : 0;
}

/**
 * @brief Copy a file's metadata out of the table.
 * @param name File name.
 * @param out Receives a copy of the entry (its next pointer is cleared).
 * @return 0 if found, -1 otherwise.
 */
static int file_lookup(const char *name, FileEntry *out)
{
    int found = -1;
    pthread_mutex_lock(&file_lock);
    for (FileEntry *f = file_list; f; f = f->next)
    {
        if (strcmp(f->name, name) == 0)
        {
            *out = *f;
            out->next = NULL;
            found = 0;
            break;
        }
    }
    pthread_mutex_unlock(&file_lock);
    return found;
}

/**
 * @brief Insert a file entry; the existence check and the insert are atomic.
 * @return 0 on success, -1 if the name is already taken or memory runs out.
 */
static int file_insert(const char *name, const char *owner, const char *ip, int port)
{
    pthread_mutex_lock(&file_lock);
    for (FileEntry *f = file_list; f; f = f->next)
    {
        if (strcmp(f->name, name) == 0)
        {
            pthread_mutex_unlock(&file_lock);
            return -1;
        }
    }
    FileEntry *f = calloc(1, sizeof(FileEntry));
    if (!f)
    {
        pthread_mutex_unlock(&file_lock);
        return -1;
    }
    snprintf(f->name, sizeof(f->name), "%s", name);
    snprintf(f->owner, sizeof(f->owner), "%s", owner);
    snprintf(f->ss_ip, sizeof(f->ss_ip), "%s", ip);
    f->ss_port = port;
    f->created = time(NULL);
    f->next = file_list;
    file_list = f;
    pthread_mutex_unlock(&file_lock);
    return 0;
}

/**
 * @brief Remove a file entry.
 * @return 0 on success, -1 if not found.
 */
static int file_remove(const char *name)
{
    pthread_mutex_lock(&file_lock);
    for (FileEntry **pp = &file_list; *pp; pp = &(*pp)->next)
    {
        if (strcmp((*pp)->name, name) == 0)
        {
            FileEntry *dead = *pp;
            *pp = dead->next;
            free(dead);
            pthread_mutex_unlock(&file_lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&file_lock);
    return -1;
}

/**
 * @brief Send a command to a storage server and collect its first reply line.
 * @param ip Storage server IP.
 * @param port Storage server port.
 * @param cmd Request line.
 * @param reply_buf Receives the first line of the reply.
 * @param cap Size of reply_buf.
 * @return 0 if the reply starts with OK, -1 otherwise.
 */
static int ss_command(const char *ip, int port, const char *cmd, char *reply_buf, size_t cap)
{
    snprintf(reply_buf, cap, "ERROR Storage server unreachable");
    int fd = connect_tcp(ip, port);
    if (fd < 0)
        return -1;

    LineReader reader;
    reader_init(&reader, fd);
    char line[DFS_LINE_MAX];
    int first = 1;
    if (send_line(fd, cmd) == 0)
    {
        while (read_line(&reader, line, sizeof(line)) >= 0 && strcmp(line, DFS_STOP) != 0)
        {
            if (first)
                snprintf(reply_buf, cap, "%s", line);
            first = 0;
        }
    }
    close(fd);
    return strncmp(reply_buf, "OK", 2) == 0 ? 0 : -1;
}

/**
 * @brief Handle CREATE: place a new file on the least-loaded storage server.
 */
static void handle_create(int fd, const char *user, const char *name)
{
    FileEntry existing;
    if (!valid_filename(name))
    {
        send_line(fd, "ERROR Invalid file name");
        return;
    }
    if (file_lookup(name, &existing) == 0)
    {
        send_line(fd, "ERROR File already exists");
        return;
    }

    char ip[DFS_IP_MAX];
    int port;
    if (choose_storage_server(ip, &port) < 0)
    {
        send_line(fd, "ERROR No storage servers available");
        return;
    }

    char cmd[DFS_LINE_MAX], reply_buf[DFS_LINE_MAX];
    snprintf(cmd, sizeof(cmd), "CREATE %s", name);
    if (ss_command(ip, port, cmd, reply_buf, sizeof(reply_buf)) < 0)
    {
        send_line(fd, reply_buf);
        return;
    }

    if (file_insert(name, user, ip, port) < 0)
    {
        snprintf(cmd, sizeof(cmd), "DELETE %s", name);
        ss_command(ip, port, cmd, reply_buf, sizeof(reply_buf));
        send_line(fd, "ERROR File already exists");
        return;
    }
    printf("[NM] %s created %s on %s:%d\n", user, name, ip, port);
    send_fmt(fd, "OK File created on %s:%d", ip, port);
}

/**
 * @brief Handle DELETE: remove a file from its storage server and the table (owner only).
 */
static void handle_delete(int fd, const char *user, const char *name)
{
    FileEntry f;
    if (file_lookup(name, &f) < 0)
    {
        send_line(fd, "ERROR File not found");
        return;
    }
    if (strcmp(f.owner, user) != 0)
    {
        send_line(fd, "ERROR Permission denied: only the owner can delete this file");
        return;
    }
    if (!ss_is_alive(f.ss_ip, f.ss_port))
    {
        send_line(fd, "ERROR Storage server offline");
        return;
    }

    char cmd[DFS_LINE_MAX], reply_buf[DFS_LINE_MAX];
    snprintf(cmd, sizeof(cmd), "DELETE %s", name);
    if (ss_command(f.ss_ip, f.ss_port, cmd, reply_buf, sizeof(reply_buf)) < 0 &&
        strcmp(reply_buf, "ERROR File not found") != 0)
    {
        send_line(fd, reply_buf);
        return;
    }
    file_remove(name);
    printf("[NM] %s deleted %s\n", user, name);
    send_line(fd, "OK File deleted");
}

/**
 * @brief Handle VIEW: list files, one per line, followed by END.
 * @param list_all When non-zero, include each file's storage server and its status.
 */
static void handle_view(int fd, int list_all)
{
    FileEntry *snapshot = NULL;
    size_t count = 0;

    pthread_mutex_lock(&file_lock);
    for (FileEntry *f = file_list; f; f = f->next)
        count++;
    snapshot = count ? calloc(count, sizeof(FileEntry)) : NULL;
    size_t i = 0;
    for (FileEntry *f = file_list; f && snapshot; f = f->next)
        snapshot[i++] = *f;
    pthread_mutex_unlock(&file_lock);

    send_line(fd, "FILES:");
    for (i = 0; snapshot && i < count; i++)
    {
        FileEntry *f = &snapshot[i];
        if (list_all)
            send_fmt(fd, "%s (owner=%s) SS=%s:%d [%s]", f->name, f->owner, f->ss_ip, f->ss_port,
                     ss_is_alive(f->ss_ip, f->ss_port) ? "online" : "offline");
        else
            send_fmt(fd, "%s (owner=%s)", f->name, f->owner);
    }
    send_line(fd, DFS_END);
    free(snapshot);
}

/**
 * @brief Handle INFO: report a file's owner, location, availability and creation time.
 */
static void handle_info(int fd, const char *name)
{
    FileEntry f;
    if (file_lookup(name, &f) < 0)
    {
        send_line(fd, "ERROR File not found");
        return;
    }
    char when[32];
    struct tm tm_buf;
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime_r(&f.created, &tm_buf));
    send_fmt(fd, "INFO %s owner=%s ss=%s:%d status=%s created=%s", f.name, f.owner, f.ss_ip, f.ss_port,
             ss_is_alive(f.ss_ip, f.ss_port) ? "online" : "offline", when);
}

/**
 * @brief Handle READ and WRITE by redirecting the client to the file's storage server.
 * @param fd Client socket.
 * @param user Requesting user.
 * @param name File name.
 * @param request Original request line, forwarded unchanged.
 * @param owner_only When non-zero, only the file's owner may proceed.
 */
static void handle_redirect(int fd, const char *user, const char *name, const char *request, int owner_only)
{
    FileEntry f;
    if (file_lookup(name, &f) < 0)
    {
        send_line(fd, "ERROR File not found");
        return;
    }
    if (owner_only && strcmp(f.owner, user) != 0)
    {
        send_line(fd, "ERROR Permission denied: only the owner can write to this file");
        return;
    }
    if (!ss_is_alive(f.ss_ip, f.ss_port))
    {
        send_line(fd, "ERROR Storage server offline");
        return;
    }
    send_fmt(fd, "SS %s %d %s", f.ss_ip, f.ss_port, request);
}

/**
 * @brief Arguments handed to a connection thread.
 */
typedef struct
{
    int fd;
    char addr[DFS_IP_MAX];
} ClientConn;

/**
 * @brief Return non-zero if a command takes a file name argument.
 */
static int needs_file(const char *cmd)
{
    return strcmp(cmd, "CREATE") == 0 || strcmp(cmd, "DELETE") == 0 || strcmp(cmd, "INFO") == 0 ||
           strcmp(cmd, "READ") == 0 || strcmp(cmd, "WRITE") == 0;
}

/**
 * @brief Per-connection thread: read request lines and dispatch them until the peer disconnects.
 *
 * File commands are only accepted after REGISTER_CLIENT, which fixes the
 * identity used for ownership checks.
 * @param arg Heap-allocated ClientConn, freed by this function.
 */
static void *connection_handler(void *arg)
{
    ClientConn conn = *(ClientConn *)arg;
    free(arg);

    LineReader reader;
    reader_init(&reader, conn.fd);
    char line[DFS_LINE_MAX];
    char user[DFS_USER_MAX] = "";

    while (read_line(&reader, line, sizeof(line)) >= 0)
    {
        char cmd[32] = "", name[DFS_NAME_MAX] = "";
        sscanf(line, "%31s %127s", cmd, name);

        if (strcmp(cmd, "REGISTER_SS") == 0)
        {
            char ip[DFS_IP_MAX];
            int port;
            if (sscanf(line, "REGISTER_SS %63s %d", ip, &port) == 2 && port > 0)
            {
                register_storage_server(ip, port);
                send_line(conn.fd, "OK Registered SS");
            }
            else
                send_line(conn.fd, "ERROR Usage: REGISTER_SS <ip> <port>");
            continue;
        }

        if (strcmp(cmd, "REGISTER_CLIENT") == 0)
        {
            if (name[0] && strlen(name) < DFS_USER_MAX)
            {
                snprintf(user, sizeof(user), "%s", name);
                send_fmt(conn.fd, "OK Registered client %s", user);
                printf("[NM] Client %s connected from %s\n", user, conn.addr);
            }
            else
                send_line(conn.fd, "ERROR Usage: REGISTER_CLIENT <user>");
            continue;
        }

        if (cmd[0] == '\0')
            continue;
        if (user[0] == '\0')
        {
            send_line(conn.fd, "ERROR Register first with REGISTER_CLIENT <user>");
            continue;
        }

        if (strcmp(cmd, "VIEW") == 0)
            handle_view(conn.fd, strstr(line, "-a") != NULL);
        else if (needs_file(cmd) && !name[0])
            send_fmt(conn.fd, "ERROR Usage: %s <file>%s", cmd, strcmp(cmd, "WRITE") == 0 ? " <text>" : "");
        else if (strcmp(cmd, "CREATE") == 0)
            handle_create(conn.fd, user, name);
        else if (strcmp(cmd, "DELETE") == 0)
            handle_delete(conn.fd, user, name);
        else if (strcmp(cmd, "INFO") == 0)
            handle_info(conn.fd, name);
        else if (strcmp(cmd, "READ") == 0)
            handle_redirect(conn.fd, user, name, line, 0);
        else if (strcmp(cmd, "WRITE") == 0)
            handle_redirect(conn.fd, user, name, line, 1);
        else
            send_line(conn.fd, "ERROR Unknown command");
    }

    close(conn.fd);
    return NULL;
}

/**
 * @brief Background thread that evicts storage servers whose heartbeat has expired.
 */
static void *ss_heartbeat_monitor(void *arg)
{
    (void)arg;
    for (;;)
    {
        sleep(1);
        time_t now = time(NULL);
        pthread_mutex_lock(&ss_lock);
        for (StorageServer **pp = &ss_list; *pp;)
        {
            StorageServer *s = *pp;
            if (now - s->last_seen > ss_timeout)
            {
                printf("[NM] Storage server %s:%d timed out\n", s->ip, s->port);
                *pp = s->next;
                free(s);
            }
            else
                pp = &s->next;
        }
        pthread_mutex_unlock(&ss_lock);
    }
    return NULL;
}

/**
 * @brief Start the Name Server.
 *
 * Usage: nameserver [port] [ss_timeout_seconds] (defaults 9000 and 15).
 */
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    int port = argc >= 2 ? atoi(argv[1]) : 9000;
    if (argc >= 3 && atoi(argv[2]) > 0)
        ss_timeout = atoi(argv[2]);
    int listen_fd = listen_tcp(port, BACKLOG);
    if (listen_fd < 0)
    {
        perror("listen");
        return 1;
    }
    printf("[NM] Listening on port %d\n", port);

    pthread_t monitor;
    pthread_create(&monitor, NULL, ss_heartbeat_monitor, NULL);
    pthread_detach(monitor);

    for (;;)
    {
        struct sockaddr_in peer;
        socklen_t len = sizeof(peer);
        int fd = accept(listen_fd, (struct sockaddr *)&peer, &len);
        if (fd < 0)
        {
            if (errno != EINTR)
                perror("accept");
            continue;
        }

        ClientConn *conn = malloc(sizeof(ClientConn));
        if (!conn)
        {
            close(fd);
            continue;
        }
        conn->fd = fd;
        inet_ntop(AF_INET, &peer.sin_addr, conn->addr, sizeof(conn->addr));

        pthread_t t;
        if (pthread_create(&t, NULL, connection_handler, conn) != 0)
        {
            close(fd);
            free(conn);
            continue;
        }
        pthread_detach(t);
    }
}
