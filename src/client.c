/**
 * @file client.c
 * @brief Interactive client for the distributed file system.
 *
 * The client registers with the Name Server under a username and forwards
 * each command typed at the prompt. When the Name Server answers with an SS
 * redirect, the client connects straight to that storage server, sends the
 * request and prints the streamed reply until STOP.
 */

#include "common.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/**
 * @brief Print the commands the client understands.
 */
static void print_help(void)
{
    printf("Commands:\n"
           "  CREATE <file>          create an empty file you own\n"
           "  WRITE <file> <text>    append a line to a file you own\n"
           "  READ <file>            print a file\n"
           "  INFO <file>            show owner, location and status\n"
           "  VIEW [-a]              list files (-a adds storage servers)\n"
           "  DELETE <file>          delete a file you own\n"
           "  HELP                   show this help\n"
           "  EXIT                   quit\n");
}

/**
 * @brief Send a redirected request to a storage server and print its reply.
 * @param ip Storage server IP.
 * @param port Storage server port.
 * @param request Request line to send.
 */
static void handle_storage_server(const char *ip, int port, const char *request)
{
    int fd = connect_tcp(ip, port);
    if (fd < 0)
    {
        printf("ERROR Could not connect to storage server %s:%d\n", ip, port);
        return;
    }
    if (send_line(fd, request) < 0)
    {
        printf("ERROR Could not send request to storage server\n");
        close(fd);
        return;
    }

    LineReader reader;
    reader_init(&reader, fd);
    char line[DFS_LINE_MAX];
    while (read_line(&reader, line, sizeof(line)) >= 0 && strcmp(line, DFS_STOP) != 0)
        printf("%s\n", unstuff_line(line));
    close(fd);
}

/**
 * @brief Run the client.
 *
 * Usage: client [nameserver_ip] [nameserver_port] (default 127.0.0.1 9000).
 * The username is read from the first input line.
 */
int main(int argc, char **argv)
{
    const char *nm_ip = argc >= 2 ? argv[1] : "127.0.0.1";
    int nm_port = argc >= 3 ? atoi(argv[2]) : 9000;
    signal(SIGPIPE, SIG_IGN);

    char username[DFS_USER_MAX];
    printf("Enter username: ");
    fflush(stdout);
    if (!fgets(username, sizeof(username), stdin))
        return 1;
    username[strcspn(username, "\r\n")] = '\0';
    if (!username[0] || strchr(username, ' '))
    {
        printf("Username must be a single non-empty word.\n");
        return 1;
    }

    int nm = connect_tcp(nm_ip, nm_port);
    if (nm < 0)
    {
        printf("Failed to connect to Name Server at %s:%d.\n", nm_ip, nm_port);
        return 1;
    }

    LineReader reader;
    reader_init(&reader, nm);
    char reply[DFS_LINE_MAX];
    send_fmt(nm, "REGISTER_CLIENT %s", username);
    if (read_line(&reader, reply, sizeof(reply)) < 0)
    {
        printf("Name Server closed the connection.\n");
        return 1;
    }
    printf("Name Server: %s\n", reply);
    printf("Type HELP for a list of commands.\n");

    char cmd[DFS_LINE_MAX];
    for (;;)
    {
        printf("client> ");
        fflush(stdout);
        if (!fgets(cmd, sizeof(cmd), stdin))
            break;
        cmd[strcspn(cmd, "\r\n")] = '\0';
        if (!cmd[0])
            continue;
        if (strcmp(cmd, "EXIT") == 0)
            break;
        if (strcmp(cmd, "HELP") == 0)
        {
            print_help();
            continue;
        }

        if (send_line(nm, cmd) < 0 || read_line(&reader, reply, sizeof(reply)) < 0)
        {
            printf("Name Server closed the connection.\n");
            break;
        }

        if (strncmp(reply, "SS ", 3) == 0)
        {
            char ss_ip[DFS_IP_MAX];
            int ss_port, consumed = 0;
            if (sscanf(reply, "SS %63s %d %n", ss_ip, &ss_port, &consumed) == 2 && consumed > 0)
                handle_storage_server(ss_ip, ss_port, reply + consumed);
            else
                printf("ERROR Malformed redirect from Name Server\n");
            continue;
        }

        printf("%s\n", reply);
        if (strcmp(reply, "FILES:") == 0)
        {
            while (read_line(&reader, reply, sizeof(reply)) >= 0 && strcmp(reply, DFS_END) != 0)
                printf("  %s\n", reply);
        }
    }

    close(nm);
    return 0;
}
