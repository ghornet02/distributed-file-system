# Distributed File System

A distributed file system written in C with POSIX sockets and pthreads. A central **Name Server** manages metadata and coordinates any number of **Storage Servers**, which hold the file data. **Clients** send metadata requests to the Name Server and read and write data directly on the storage servers.

## Architecture

```
                 CREATE / DELETE / VIEW / INFO / READ / WRITE
   Client  ─────────────────────────────────────────────────▶  Name Server
     │   ◀──── SS <ip> <port> <request>  (redirect) ─────────    │    ▲
     │                                                          │    │ REGISTER_SS
     │          READ / WRITE                     CREATE / DELETE│    │ (heartbeat every 5 s)
     ▼                                                          ▼    │
   Storage Server 1 … N  ◀────────────────────────────────────────────┘
     (files on local disk)
```

- **Metadata/data split:** the Name Server never touches file contents. READ and WRITE requests get a redirect to the storage server that holds the file, so data moves directly between that server and the client.
- **Load balancing:** each new file goes to the live storage server that holds the fewest files.
- **Failure detection and recovery:** storage servers send a heartbeat over a persistent connection. The Name Server marks a server offline when its heartbeat stops. Its files stay listed and become available again as soon as the server re-registers. Storage servers retry automatically if the Name Server restarts.
- **Access control:** every file has an owner. Anyone can read a file, but only its owner can write to it or delete it.
- **Concurrency:** both servers handle each connection on its own thread. The Name Server's tables are protected by mutexes, and lookups return copies, so no pointer is used after its lock is released. Storage servers use `flock()` so that readers and writers never see a partly written line.
- **Robust framing:** the protocol is newline-framed with a buffered line reader. Multi-line replies end with `STOP`, and content lines are dot-stuffed, so a file can contain any text, including the line `STOP`.
- **Safe file names:** names with `/`, `..`, whitespace or control characters are rejected, which blocks path traversal on the storage servers.

## Build and run

```sh
make
./nameserver 9000                                   # terminal 1
./storageserver 9101 data/ss1 127.0.0.1 9000        # terminal 2
./storageserver 9102 data/ss2 127.0.0.1 9000        # terminal 3
./client 127.0.0.1 9000                             # terminal 4
```

Usage:

```
nameserver    [port] [ss_timeout_seconds]
storageserver <port> <storage_dir> [nameserver_ip] [nameserver_port] [advertise_ip]
client        [nameserver_ip] [nameserver_port]
```

## Example session

```
Enter username: alice
Name Server: OK Registered client alice
client> CREATE notes.txt
OK File created on 127.0.0.1:9101
client> WRITE notes.txt hello world
OK Wrote 12 bytes
client> READ notes.txt
hello world
client> VIEW -a
FILES:
  notes.txt (owner=alice) SS=127.0.0.1:9101 [online]
client> INFO notes.txt
INFO notes.txt owner=alice ss=127.0.0.1:9101 status=online created=2026-11-16 23:40:12
client> DELETE notes.txt
OK File deleted
```

## Commands

| Command | Handled by | Description |
|---|---|---|
| `CREATE <file>` | Name Server, then storage server | Create an empty file owned by you |
| `WRITE <file> <text>` | Storage server (redirect) | Append a line (owner only) |
| `READ <file>` | Storage server (redirect) | Print the file |
| `INFO <file>` | Name Server | Owner, location, online status, creation time |
| `VIEW [-a]` | Name Server | List files (`-a` adds the storage server and its status) |
| `DELETE <file>` | Name Server, then storage server | Delete a file (owner only) |

## Tests

```sh
make test
```

`tests/e2e.sh` starts a Name Server, two storage servers and several clients on random ports. It runs 21 end-to-end checks:
- file creation and load balancing
- duplicate and path-traversal names
- reads and writes, including edge cases in content framing
- `VIEW` and `INFO`
- owner-only permissions
- deletion on disk
- a storage server failing and recovering

## Project structure

| File | Purpose |
|---|---|
| `src/nameserver.c` | Metadata, storage-server registry, request routing, heartbeat monitor |
| `src/storageserver.c` | File storage, locking, heartbeat to the Name Server |
| `src/client.c` | Interactive client with redirect handling |
| `src/common.c` / `src/common.h` | Sockets, line framing, dot-stuffing, file-name validation |
| `tests/e2e.sh` | End-to-end test suite |
