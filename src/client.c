/**
 * @file client.c
 * @brief Interactive client for the distributed file system.
 *
 * Registers with the Name Server, forwards each command typed at the prompt
 * and, when the Name Server answers with an SS redirect, connects directly
 * to that storage server and streams the response until STOP.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define MAX_CMD 2048
#define BUF_SIZE 4096

/**
 * @brief Open a TCP connection.
 * @param ip Server IPv4 address.
 * @param port Server port.
 * @return Socket descriptor, or -1 on failure.
 */
int connect_to_server(char *ip, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return -1; }

    struct sockaddr_in serv;
    serv.sin_family = AF_INET;
    serv.sin_port = htons(port);
    inet_pton(AF_INET, ip, &serv.sin_addr);

    if (connect(sock, (struct sockaddr *)&serv, sizeof(serv)) < 0) {
        perror("connect");
        close(sock);
        return -1;
    }

    return sock;
}

/**
 * @brief Write a whole message to a socket, retrying on short writes.
 */
void send_line(int sock, const char *msg) {
    size_t len = strlen(msg), sent = 0;
    while (sent < len) {
        ssize_t w = write(sock, msg + sent, len - sent);
        if (w <= 0) return;
        sent += (size_t)w;
    }
}

/**
 * @brief Read one newline-terminated line from a socket.
 * @param sock Socket descriptor.
 * @param buffer Output buffer of BUF_SIZE bytes; includes the newline if present.
 * @return Bytes read, or -1 if the connection closed before any data.
 */
int recv_line(int sock, char *buffer) {
    int n = 0;
    char c;
    while (n < BUF_SIZE - 1) {
        ssize_t r = read(sock, &c, 1);
        if (r <= 0) {
            if (n == 0) return -1;
            break;
        }
        buffer[n++] = c;
        if (c == '\n') break;
    }
    buffer[n] = '\0';
    return n;
}

/**
 * @brief Connect to a storage server, send the forwarded request and print the streamed reply.
 * @param ss_ip Storage server IP.
 * @param ss_port Storage server port.
 * @param nm_response Request line to send.
 */
void handle_storage_server(char *ss_ip, int ss_port, char *nm_response) {
    int ss = connect_to_server(ss_ip, ss_port);
    if (ss < 0) {
        printf("ERROR: Could not connect to Storage Server\n");
        return;
    }

    send_line(ss, nm_response);

    char buf[BUF_SIZE];

    while (1) {
        int n = recv_line(ss, buf);
        if (n <= 0) break;

        if (strncmp(buf, "STOP", 4) == 0) break;

        printf("%s", buf);
        fflush(stdout);
    }

    close(ss);
}

/**
 * @brief Run the client.
 *
 * Usage: client [nameserver_ip] [nameserver_port] (default 127.0.0.1 9000).
 */
int main(int argc, char *argv[]) {
    char username[64];
    char nm_ip[64] = "127.0.0.1";
    int nm_port = 9000;

    if (argc >= 2) {
        strncpy(nm_ip, argv[1], sizeof(nm_ip) - 1);
        nm_ip[sizeof(nm_ip) - 1] = '\0';
    }
    if (argc >= 3) {
        nm_port = atoi(argv[2]);
    }
    printf("Enter username: ");
    if (scanf("%63s", username) != 1) return 1;

    int nm_sock = connect_to_server(nm_ip, nm_port);
    if (nm_sock < 0) {
        printf("Failed to connect to Name Server.\n");
        exit(1);
    }

    char reg_msg[128];
    snprintf(reg_msg, sizeof(reg_msg), "REGISTER_CLIENT %s 0\n", username);
    send_line(nm_sock, reg_msg);

    char buf[BUF_SIZE];
    if (recv_line(nm_sock, buf) < 0) {
        printf("Name Server closed the connection.\n");
        exit(1);
    }
    buf[strcspn(buf, "\n")] = '\0';
    printf("Name Server: %s\n", buf);

    getchar();

    while (1) {
        char cmd[MAX_CMD];
        printf("client> ");
        fflush(stdout);

        if (!fgets(cmd, MAX_CMD, stdin)) break;

        if (strncmp(cmd, "EXIT", 4) == 0) break;

        send_line(nm_sock, cmd);

        if (recv_line(nm_sock, buf) < 0) {
            printf("Name Server closed the connection.\n");
            break;
        }
        buf[strcspn(buf, "\n")] = '\0';

        if (strncmp(buf, "SS ", 3) == 0) {

            char ss_ip[64], original_req[MAX_CMD];
            int ss_port;

            sscanf(buf, "SS %s %d %[^\n]", ss_ip, &ss_port, original_req);

            handle_storage_server(ss_ip, ss_port, original_req);
            continue;
        }

        if (strcmp(buf, "FILES:") == 0) {
            printf("%s\n", buf);
            while (recv_line(nm_sock, buf) > 0) {
                buf[strcspn(buf, "\n")] = '\0';
                if (strcmp(buf, "END") == 0) break;
                printf("  %s\n", buf);
            }
            continue;
        }

        printf("%s\n", buf);
    }

    close(nm_sock);
    return 0;
}
