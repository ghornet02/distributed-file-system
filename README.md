# Distributed File System

The coordination layer of a distributed file system in C. A multithreaded **Name Server** keeps file metadata and tracks the storage servers that are alive. **Clients** talk to the Name Server for metadata, and are redirected to the right storage server to read data.

> **Status:** the Name Server and client are complete. The storage server is not part of this repository yet. READ requests are redirected correctly, but they need a storage server listening on the advertised port.

## Architecture

```
            metadata (CREATE / DELETE / VIEW / READ)
  Client  ───────────────────────────────────────▶  Name Server
    │                                                   ▲
    │  SS <ip> <port> <request>  (redirect)              │ REGISTER_SS (heartbeat)
    ▼                                                   │
  Storage Server  ◀─────────────────────────────────────┘
        data stream ... STOP
```

- **Thread per connection:** each client and storage-server connection runs on its own `pthread`.
- **Thread-safe metadata:** the storage-server list and the file table are each guarded by a mutex.
- **Liveness tracking:** a monitor thread evicts storage servers that have not re-registered within 20 seconds.
- **Line-based protocol:** messages are newline-framed over TCP, so you can debug with `nc`.

## Protocol

| Request | Response |
|---|---|
| `REGISTER_CLIENT <user> <port>` | `OK Registered client <user>` |
| `REGISTER_SS <ip> <port>` | `OK Registered SS` |
| `CREATE <file> <owner>` | `OK File created` or `ERROR ...` |
| `DELETE <file>` | `OK File deleted` or `ERROR File not found` |
| `VIEW [-a]` | `FILES:`, one line per file, then `END` (`-a` adds the hosting server) |
| `READ <file>` | `SS <ip> <port> <original request>`, which redirects the client |

## Build and run

```sh
make
./nameserver 9000                # terminal 1
./client 127.0.0.1 9000          # terminal 2
```

Example session (with a storage server registered):

```
Enter username: bob
Name Server: OK Registered client bob
client> CREATE notes.txt bob
OK File created
client> VIEW -a
FILES:
  notes.txt (owner=bob) SS=127.0.0.1:9600
client> EXIT
```

## Project structure

| File | Purpose |
|---|---|
| `src/nameserver.c` | Name Server: registry, file table, request dispatch, heartbeat monitor |
| `src/client.c` | Interactive client with storage-server redirect handling |
