/**
 * @file nameserver.c
 * @brief Name Server: the metadata and coordination node of the distributed file system.
 *
 * Tracks registered storage servers and the file-to-server mapping, serves
 * client metadata requests (CREATE, DELETE, VIEW) and redirects data
 * requests (READ) to the storage server holding the file. Each connection is
 * handled on its own thread, and a monitor thread evicts storage servers
 * that have not re-registered within SS_TIMEOUT seconds.
 *
 * Wire protocol (one newline-terminated line per message):
 *   REGISTER_CLIENT <user> <port>  -> OK ...
 *   REGISTER_SS <ip> <port>        -> OK Registered SS
 *   CREATE <file> <owner>          -> OK File created | ERROR ...
 *   DELETE <file>                  -> OK File deleted | ERROR ...
 *   VIEW [-a]                      -> FILES: / <entry>... / END
 *   READ <file>                    -> SS <ip> <port> <original request>
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <time.h>
#include <errno.h>

#define BACKLOG 10
#define BUF_SZ 4096
#define MAX_NAME 256
#define SS_TIMEOUT 20

/**
 * @brief A registered storage server and the time it last checked in.
 */
typedef struct StorageServer {
    char ip[64];
    int ss_port;
    int nm_fd_port;
    time_t last_seen;
    struct StorageServer *next;
} StorageServer;

/**
 * @brief Metadata for one file: owner, hosting storage server and access time.
 */
typedef struct FileEntry {
    char name[MAX_NAME];
    char owner[64];
    StorageServer *ss;
    int size_bytes;
    time_t last_access;
    struct FileEntry *next;
} FileEntry;

/**
 * @brief Arguments handed to a per-connection handler thread.
 */
typedef struct {
    int client_fd;
    char addr_str[64];
} ClientConn;

StorageServer *ss_list = NULL;
FileEntry *file_list = NULL;
pthread_mutex_t ss_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t file_lock = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Write a string followed by a newline, retrying on short writes.
 * @param fd Socket descriptor.
 * @param s Message without trailing newline.
 * @return Bytes written, or -1 on error.
 */
ssize_t send_line(int fd, const char *s) {
    size_t n = strlen(s);
    char *msg = malloc(n + 1);
    if (!msg) return -1;
    memcpy(msg, s, n);
    msg[n] = '\n';
    size_t sent = 0;
    while (sent < n + 1) {
        ssize_t w = write(fd, msg + sent, n + 1 - sent);
        if (w < 0) {
            if (errno == EINTR) continue;
            free(msg);
            return -1;
        }
        sent += (size_t)w;
    }
    free(msg);
    return (ssize_t)sent;
}

/**
 * @brief Look up a storage server by address.
 * @return The server, or NULL if not registered.
 */
StorageServer *find_ss_by_ipport(const char *ip, int port) {
    pthread_mutex_lock(&ss_lock);
    StorageServer *p = ss_list;
    while (p) {
        if (strcmp(p->ip, ip) == 0 && p->ss_port == port) {
            pthread_mutex_unlock(&ss_lock);
            return p;
        }
        p = p->next;
    }
    pthread_mutex_unlock(&ss_lock);
    return NULL;
}

/**
 * @brief Register a storage server, or refresh its heartbeat if already known.
 * @param ip Server IP address.
 * @param ss_port Port clients use to reach it.
 * @return The server entry.
 */
StorageServer *register_storage_server(const char *ip, int ss_port) {
    pthread_mutex_lock(&ss_lock);
    StorageServer *existing = ss_list;
    while (existing) {
        if (strcmp(existing->ip, ip) == 0 && existing->ss_port == ss_port) {
            existing->last_seen = time(NULL);
            pthread_mutex_unlock(&ss_lock);
            return existing;
        }
        existing = existing->next;
    }

    StorageServer *s = calloc(1, sizeof(StorageServer));
    strncpy(s->ip, ip, sizeof(s->ip)-1);
    s->ss_port = ss_port;
    s->last_seen = time(NULL);
    s->next = ss_list;
    ss_list = s;
    pthread_mutex_unlock(&ss_lock);
    printf("[NM] Registered SS %s:%d\n", ip, ss_port);
    return s;
}

/**
 * @brief Look up a file entry by name.
 * @return The entry, or NULL if it does not exist.
 */
FileEntry *find_file(const char *fname) {
    pthread_mutex_lock(&file_lock);
    FileEntry *p = file_list;
    while (p) {
        if (strcmp(p->name, fname) == 0) {
            pthread_mutex_unlock(&file_lock);
            return p;
        }
        p = p->next;
    }
    pthread_mutex_unlock(&file_lock);
    return NULL;
}

/**
 * @brief Add a file entry assigned to a storage server.
 * @param fname File name.
 * @param owner Owning user.
 * @param ss Storage server that will hold the data.
 * @return 0 on success, -1 if the file already exists.
 */
int create_file(const char *fname, const char *owner, StorageServer *ss) {
    if (find_file(fname)) return -1;
    FileEntry *f = calloc(1, sizeof(FileEntry));
    strncpy(f->name, fname, MAX_NAME-1);
    strncpy(f->owner, owner, sizeof(f->owner)-1);
    f->ss = ss;
    f->size_bytes = 0;
    f->last_access = time(NULL);
    pthread_mutex_lock(&file_lock);
    f->next = file_list;
    file_list = f;
    pthread_mutex_unlock(&file_lock);
    printf("[NM] Created file %s owned by %s on SS %s:%d\n", fname, owner, ss->ip, ss->ss_port);
    return 0;
}

/**
 * @brief Remove a file entry.
 * @return 0 on success, -1 if not found.
 */
int delete_file(const char *fname) {
    pthread_mutex_lock(&file_lock);
    FileEntry *prev = NULL, *p = file_list;
    while (p) {
        if (strcmp(p->name, fname) == 0) {
            if (prev) prev->next = p->next;
            else file_list = p->next;
            free(p);
            pthread_mutex_unlock(&file_lock);
            return 0;
        }
        prev = p;
        p = p->next;
    }
    pthread_mutex_unlock(&file_lock);
    return -1;
}

/**
 * @brief Pick the storage server for a new file.
 * @return A storage server, or NULL if none are registered.
 */
StorageServer *choose_ss_for_new_file(void) {
    pthread_mutex_lock(&ss_lock);
    StorageServer *p = ss_list;
    if (!p) {
        pthread_mutex_unlock(&ss_lock);
        return NULL;
    }

    StorageServer *chosen = p;
    pthread_mutex_unlock(&ss_lock);
    return chosen;
}

/**
 * @brief Send the file listing to a client, terminated by an END line.
 * @param client_fd Client socket.
 * @param list_all Include the hosting storage server for each file when non-zero.
 */
void handle_view(int client_fd, int list_all) {
    char out[BUF_SZ];
    size_t off = 0;
    off += snprintf(out + off, sizeof(out) - off, "FILES:\n");
    pthread_mutex_lock(&file_lock);
    for (FileEntry *p = file_list; p && off < sizeof(out); p = p->next) {
        if (list_all) {
            off += snprintf(out + off, sizeof(out) - off, "%s (owner=%s) SS=%s:%d\n",
                            p->name, p->owner, p->ss ? p->ss->ip : "NONE", p->ss ? p->ss->ss_port : 0);
        } else {
            off += snprintf(out + off, sizeof(out) - off, "%s (owner=%s)\n", p->name, p->owner);
        }
    }
    pthread_mutex_unlock(&file_lock);
    if (off < sizeof(out))
        snprintf(out + off, sizeof(out) - off, "END");
    else
        snprintf(out + sizeof(out) - 5, 5, "\nEND");
    send_line(client_fd, out);
}

/**
 * @brief Redirect a READ to the storage server that holds the file.
 * @param client_fd Client socket.
 * @param filename Requested file.
 * @param original_cmd Request line to forward to the storage server.
 */
void handle_read_request(int client_fd, const char *filename, const char *original_cmd) {
    FileEntry *f = find_file(filename);
    if (!f || !f->ss) {
        send_line(client_fd, "ERROR File not found or no storage server");
        return;
    }

    char line[BUF_SZ];
    snprintf(line, sizeof(line), "SS %s %d %s", f->ss->ip, f->ss->ss_port, original_cmd);
    send_line(client_fd, line);
}

/**
 * @brief Per-connection thread: read request lines and dispatch them until the peer disconnects.
 * @param arg Heap-allocated ClientConn, freed by this function.
 */
void *connection_handler(void *arg) {
    int fd = ((ClientConn *)arg)->client_fd;
    char addr[64];
    snprintf(addr, sizeof(addr), "%s", ((ClientConn *)arg)->addr_str);
    free(arg);

    char buf[BUF_SZ];
    ssize_t r;

    while ((r = read(fd, buf, sizeof(buf)-1)) > 0) {
        buf[r] = '\0';

        char *nl = strchr(buf, '\n');
        if (nl) *nl = '\0';

        if (strncmp(buf, "REGISTER_CLIENT", 15) == 0) {

            char username[64]; int client_port;
            if (sscanf(buf, "REGISTER_CLIENT %63s %d", username, &client_port) >= 1) {

                char reply[128];
                snprintf(reply, sizeof(reply), "OK Registered client %s", username);
                send_line(fd, reply);
                printf("[NM] Client registered %s from %s\n", username, addr);
                continue;
            } else {
                send_line(fd, "ERR bad REGISTER_CLIENT format");
                continue;
            }
        }

        if (strncmp(buf, "REGISTER_SS", 11) == 0) {

            char ip[64]; int ss_port;
            if (sscanf(buf, "REGISTER_SS %63s %d", ip, &ss_port) == 2) {
                register_storage_server(ip, ss_port);
                send_line(fd, "OK Registered SS");
                continue;
            } else {
                send_line(fd, "ERR bad REGISTER_SS format");
                continue;
            }
        }

        if (strncmp(buf, "VIEW", 4) == 0) {
            int all = (strstr(buf, "-a") != NULL);
            handle_view(fd, all);
            continue;
        }

        if (strncmp(buf, "CREATE ", 7) == 0) {
            char filename[256], owner[64];
            if (sscanf(buf, "CREATE %255s %63s", filename, owner) >= 1) {
                StorageServer *ss = choose_ss_for_new_file();
                if (!ss) {
                    send_line(fd, "ERROR No storage servers available");
                } else {
                    if (create_file(filename, owner, ss) == 0)
                        send_line(fd, "OK File created");
                    else
                        send_line(fd, "ERROR File already exists");
                }
            } else {
                send_line(fd, "ERR bad CREATE syntax");
            }
            continue;
        }

        if (strncmp(buf, "DELETE ", 7) == 0) {
            char filename[256];
            if (sscanf(buf, "DELETE %255s", filename) == 1) {
                if (delete_file(filename) == 0) send_line(fd, "OK File deleted");
                else send_line(fd, "ERROR File not found");
            } else {
                send_line(fd, "ERR bad DELETE syntax");
            }
            continue;
        }

        if (strncmp(buf, "READ ", 5) == 0) {
            char filename[256];
            if (sscanf(buf, "READ %255s", filename) == 1) {
                handle_read_request(fd, filename, buf);
            } else {
                send_line(fd, "ERR bad READ syntax");
            }
            continue;
        }

        send_line(fd, "ERR Unknown command");
    }

    if (r < 0) {
        perror("[NM] read");
    }

    close(fd);
    return NULL;
}

/**
 * @brief Background thread that evicts storage servers whose heartbeat is older than SS_TIMEOUT.
 */
void *ss_heartbeat_monitor(void *arg) {
    (void)arg;
    while (1) {
        sleep(SS_TIMEOUT);
        pthread_mutex_lock(&ss_lock);
        StorageServer *prev = NULL, *p = ss_list;
        time_t now = time(NULL);
        while (p) {
            if (now - p->last_seen > SS_TIMEOUT) {
                printf("[NM] SS %s:%d timed out, removing\n", p->ip, p->ss_port);
                StorageServer *tofree = p;
                if (prev) prev->next = p->next;
                else ss_list = p->next;
                p = p->next;
                free(tofree);
            } else {
                prev = p;
                p = p->next;
            }
        }
        pthread_mutex_unlock(&ss_lock);
    }
    return NULL;
}

/**
 * @brief Start the Name Server.
 *
 * Usage: nameserver [port] (default 9000).
 */
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    int listen_port = 9000;
    if (argc >= 2) listen_port = atoi(argv[1]);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); exit(1); }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serv;
    serv.sin_family = AF_INET;
    serv.sin_addr.s_addr = INADDR_ANY;
    serv.sin_port = htons(listen_port);

    if (bind(listen_fd, (struct sockaddr *)&serv, sizeof(serv)) < 0) {
        perror("bind");
        exit(1);
    }
    if (listen(listen_fd, BACKLOG) < 0) {
        perror("listen");
        exit(1);
    }

    printf("[NM] Listening on port %d\n", listen_port);

    pthread_t hb_thread;
    pthread_create(&hb_thread, NULL, ss_heartbeat_monitor, NULL);

    while (1) {
        struct sockaddr_in cliaddr;
        socklen_t clilen = sizeof(cliaddr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&cliaddr, &clilen);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        char addrstr[64];
        inet_ntop(AF_INET, &cliaddr.sin_addr, addrstr, sizeof(addrstr));
        printf("[NM] Connection from %s:%d\n", addrstr, ntohs(cliaddr.sin_port));

        ClientConn *cc = malloc(sizeof(ClientConn));
        cc->client_fd = client_fd;
        snprintf(cc->addr_str, sizeof(cc->addr_str), "%s", addrstr);

        pthread_t t;
        pthread_create(&t, NULL, connection_handler, cc);
        pthread_detach(t);
    }

    close(listen_fd);
    return 0;
}
