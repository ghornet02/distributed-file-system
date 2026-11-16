/**
 * @file common.h
 * @brief Networking, line framing and validation helpers shared by every DFS component.
 *
 * All components speak a line-based protocol over TCP: one request per
 * newline-terminated line. Multi-line replies from storage servers end with a
 * STOP line, and content lines are dot-stuffed (a leading '.' is doubled) so a
 * file line can never be mistaken for the terminator.
 */

#ifndef DFS_COMMON_H
#define DFS_COMMON_H

#include <stddef.h>
#include <sys/types.h>

#define DFS_LINE_MAX 4096
#define DFS_NAME_MAX 128
#define DFS_USER_MAX 64
#define DFS_IP_MAX 64
#define DFS_STOP "STOP"
#define DFS_END "END"

/**
 * @brief Buffered reader that splits a socket stream into lines.
 */
typedef struct
{
    int fd;
    char buf[DFS_LINE_MAX];
    size_t len;
} LineReader;

/**
 * @brief Open a TCP connection.
 * @param ip IPv4 address in dotted-quad form.
 * @param port Port number.
 * @return Connected socket, or -1 on failure.
 */
int connect_tcp(const char *ip, int port);

/**
 * @brief Create a listening TCP socket bound to all interfaces.
 * @param port Port to listen on.
 * @param backlog listen() backlog.
 * @return Listening socket, or -1 on failure.
 */
int listen_tcp(int port, int backlog);

/**
 * @brief Write an entire buffer, retrying on short writes and EINTR.
 * @return 0 on success, -1 on failure.
 */
int send_all(int fd, const char *data, size_t len);

/**
 * @brief Send a line, appending the terminating newline.
 * @return 0 on success, -1 on failure.
 */
int send_line(int fd, const char *line);

/**
 * @brief printf-style variant of send_line().
 * @return 0 on success, -1 on failure.
 */
int send_fmt(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/**
 * @brief Prepare a LineReader for a socket.
 */
void reader_init(LineReader *r, int fd);

/**
 * @brief Read the next line, without its newline.
 *
 * Lines longer than the buffer are returned in pieces.
 * @param r Reader.
 * @param out Output buffer.
 * @param cap Size of out.
 * @return Length of the line, or -1 on end of stream or error.
 */
ssize_t read_line(LineReader *r, char *out, size_t cap);

/**
 * @brief Send one line of file content, dot-stuffed.
 * @return 0 on success, -1 on failure.
 */
int send_content_line(int fd, const char *line);

/**
 * @brief Undo dot-stuffing on a received content line.
 * @return Pointer into line with the stuffing removed.
 */
const char *unstuff_line(const char *line);

/**
 * @brief Check that a file name is safe to use as a single path component.
 *
 * Rejects empty names, names longer than DFS_NAME_MAX - 1, "." and "..", and
 * names containing '/', whitespace or control characters.
 * @return Non-zero if valid.
 */
int valid_filename(const char *name);

#endif
