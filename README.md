# Mini Redis — A Lightweight In-Memory Key-Value Store

A from-scratch implementation of a Redis-like in-memory key-value store written in C++. The server supports pipelined requests, non-blocking I/O via `poll()`, and a custom binary protocol. It is built from five core components: a server event loop, a client, a hash map, a dynamic buffer abstraction, and shared utilities.

---

## Project Structure

```
.
├── server.cpp       # Main server: event loop, connection handling, command dispatch
├── client.cpp       # Test client: sends pipelined commands and reads responses
├── hashtable.h      # HMap interface: incremental rehashing hash map
├── hashtable.cpp    # HMap implementation
├── utils.h          # Buffer abstraction and utility declarations
└── utils.cpp        # Buffer, I/O, and utility implementations
```

---

## Architecture Overview

```
Client                          Server
──────                          ──────
send_req(set key1 val)  ──────► handle_read()
send_req(get key1)      ──────►   └─ try_one_request()
send_req(keys)          ──────►         └─ parse_req()
                                         └─ do_request()
                                               ├─ do_set()
                                               ├─ do_get()
                                               ├─ do_del()
                                               └─ do_keys()
read_res()              ◄──────  handle_write()
read_res()              ◄──────
read_res()              ◄──────
```

The server is **single-threaded** and uses `poll()` to multiplex many connections. Each connection is non-blocking, with the server tracking per-connection intent (`want_read`, `want_write`, `want_close`) to decide what events to register with `poll()`.

---

## Wire Protocol

All communication uses a simple **length-prefixed binary protocol**.

### Request Format

```
+--------+--------+--------+-----+--------+--------+
| msglen | nstrs  | len[0] | s0  | len[1] | s1 ... |
| 4 bytes| 4 bytes| 4 bytes| ... | 4 bytes| ...    |
+--------+-----+--+--------+-----+--------+--------+
```

- `msglen` (4 bytes): total byte length of the payload following this header
- `nstrs` (4 bytes): number of string arguments
- For each argument: `len` (4 bytes) + raw string bytes

### Response Format

Responses are also length-prefixed. The payload is a **tagged serialized value**:

| Tag       | Byte | Payload                                 |
|-----------|------|-----------------------------------------|
| `TAG_NIL` | `0`  | (none)                                  |
| `TAG_ERR` | `1`  | `code` (4B) + `msglen` (4B) + msg       |
| `TAG_STR` | `2`  | `len` (4B) + raw string bytes           |
| `TAG_INT` | `3`  | 64-bit signed integer (8B)              |
| `TAG_DBL` | `4`  | 64-bit IEEE double (8B)                 |
| `TAG_ARR` | `5`  | `count` (4B) + `count` serialized items |

All multi-byte integers are little-endian (native).

---

## Component Breakdown

### `server.cpp` — Event Loop & Command Dispatch

**Startup:** Creates a TCP socket, enables `SO_REUSEADDR`, sets it non-blocking, binds to `0.0.0.0:1234`, and calls `listen()`.

**Event Loop (`main`):**

The loop uses a `std::vector<Conn*> fd2conn` array indexed by file descriptor to track all active connections. On each iteration:

1. Build `poll_args` — the listening socket always goes first, followed by connection sockets with `POLLIN`/`POLLOUT` flags set based on each connection's intent.
2. Call `poll()` to block until at least one fd is ready.
3. If the listening socket is readable, call `handle_accept()` to create a new `Conn`.
4. For each connection socket with events, call `handle_read()` or `handle_write()`. If `want_close` is set (or `POLLERR` occurs), clean up and delete the connection.

**`handle_accept(fd)`:**

Calls `accept()`, logs the client IP and port, sets the new fd to non-blocking, and allocates a `Conn` struct with pre-allocated 16KB incoming and outgoing `Buffer`s. Sets `want_read = true` to start reading the first request.

**`handle_read(conn)`:**

Reads up to 64KB from the socket into the incoming buffer. Then calls `try_one_request()` in a loop to process as many complete messages as are buffered. After all requests are processed, if the outgoing buffer has data it flips to write mode and calls `handle_write()` optimistically (to avoid an extra `poll()` round-trip).

**`try_one_request(conn)`:**

1. Checks there are at least 4 bytes (the header) in the incoming buffer.
2. Reads `len` from the header and validates it is within `k_max_msg`.
3. Checks the full `4 + len` bytes are present; if not, returns `false` to wait for more data.
4. Calls `parse_req()` to deserialize the string arguments.
5. Calls `do_request()` to produce a response into the outgoing buffer.
6. Calls `buf_consume()` to remove the processed message from the incoming buffer.
7. Returns `true` so the caller loops and tries the next request (supporting **pipelining**).

**`handle_write(conn)`:**

Calls `write()` on the outgoing buffer. If the write drains the outgoing buffer completely, it switches back to `want_read = true`.

**Command Dispatch (`do_request`):**

Routes the parsed command vector to one of:

| Command           | Handler     | Response         |
|-------------------|-------------|------------------|
| `get <key>`       | `do_get()`  | `TAG_STR` or `TAG_NIL` |
| `set <key> <val>` | `do_set()`  | `TAG_NIL`        |
| `del <key>`       | `do_del()`  | `TAG_INT` (0 or 1) |
| `keys`            | `do_keys()` | `TAG_ARR` of `TAG_STR` |

`do_request()` reserves a 4-byte length placeholder at the start of the response, writes the payload, then patches the placeholder with the actual payload size — so the framing header is always correct without a second pass.

**Storage:**

All key-value pairs are stored in a global `HMap g_data.db`. Each entry is a heap-allocated `Entry` struct embedding an `HNode` for intrusive hash map linking, plus `std::string key` and `std::string val`. Lookups use FNV-1a hashing on the key bytes.

---

### `client.cpp` — Pipelined Test Client

Connects to `127.0.0.1:1234` and demonstrates **request pipelining**:

1. **Sends all requests first** without reading any responses — `set key1`, `set key2`, `get key1`, `get key2`, `keys`.
2. **Then reads all responses** in order.

This works because the server buffers responses internally and the TCP stack buffers the in-flight data. `send_req()` serializes a command vector into the wire format and calls `write_all()`. `read_res()` reads the 4-byte length header, then the payload, and dispatches to `print_response()` which recursively pretty-prints any response type, including nested arrays.

---

### `hashtable.h` / `hashtable.cpp` — Incremental Rehashing Hash Map

The hash map is implemented as `HMap`, which contains two `HTab`s: `newer` and `older`. This two-table design enables **incremental rehashing** — spreading the cost of resizing across many operations rather than doing it all at once.

**`HNode` / `HTab`:**

`HNode` is an intrusive linked-list node embedded directly in the user's struct (e.g., `Entry`). Each slot in `HTab` is a pointer to the head of a chain (`HNode**`). The mask is always `2^n - 1`, so `hcode & mask` gives the slot index.

**Insertion (`hm_insert`):**

- Inserts into `newer`. If `older` is empty and `newer` exceeds `k_max_load_factor` (8) entries per slot on average, triggers rehashing by promoting `newer` → `older` and allocating a new `newer` at double the capacity.
- Every insert also calls `hm_help_rehashing()`.

**Incremental Rehash (`hm_help_rehashing`):**

Moves up to `k_rehashing_work` (128) nodes per call from `older` into `newer` by walking `migrate_pos`. When `older` is drained, its backing array is freed.

**Lookup / Delete:**

Both check `newer` first, then `older`. `h_lookup()` returns a `HNode**` (pointer to the slot pointer) so that `h_detach()` can splice the node out of the chain in O(1) without a separate "previous pointer" scan.

**Iteration (`hm_foreach`):**

Walks all slots in both `newer` and `older`, calling a user callback for each node. Used by the `keys` command.

---

### `utils.h` / `utils.cpp` — Buffer Abstraction & I/O Helpers

**`Buffer`:**

A growable byte buffer with four pointers:

```
buffer_begin        data_begin       data_end         buffer_end
     │                  │                │                 │
     └──────────────────┴────────────────┴─────────────────┘
          (free/consumed)   (live data)       (free space)
```

- `buf_append()`: Writes data after `data_end`. If there is not enough room, first tries to slide live data back to `buffer_begin` (avoiding a realloc). If the buffer is simply too small, `realloc()`s to `2 * capacity + len` and fixes up all pointers.
- `buf_consume()`: Advances `data_begin` by `len`. If the buffer becomes empty, resets both pointers to `buffer_begin` to maximize free space for the next append without any allocation.
- `buf_size()`: `data_end - data_begin`.
- `buf_free_space()`: `buffer_end - data_end`.

**I/O helpers:**

- `read_all()` / `write_all()`: Retry loops around `read()`/`write()` to handle `EINTR` and short reads/writes.
- `fd_set_nonblock()`: Uses `fcntl()` to set `O_NONBLOCK`.
- `die()`: Prints errno and message, then `abort()`s.

---

## Execution Flow (End to End)

```
1. server starts
   └─ socket() → setsockopt() → fd_set_nonblock() → bind() → listen()

2. poll() loop begins
   └─ new connection arrives on listening fd
        └─ handle_accept() → new Conn{want_read=true}

3. client sends: set key1 hello | set key2 world | get key1 | get key2 | keys
   (all five requests sent before reading any response)

4. server poll() wakes on POLLIN for conn fd
   └─ handle_read()
        └─ read() → buf_append() to incoming buffer
        └─ try_one_request() x5 (processes all pipelined requests)
              each call:
                parse_req() → do_request() → buf_append() to outgoing buffer
                buf_consume() from incoming buffer
        └─ outgoing buffer non-empty → want_write=true → handle_write()

5. server poll() wakes on POLLOUT (or optimistic write succeeds)
   └─ handle_write() → write() → buf_consume() outgoing
   └─ outgoing drained → want_read=true

6. client reads 5 responses in order via read_res() / print_response()

7. client closes connection → server read() returns 0 → want_close=true
   └─ close(fd) → buf_destroy() → delete conn
```

---

## Key Design Decisions

**Non-blocking I/O with `poll()`** — The server never blocks on a single connection. All fds are set to `O_NONBLOCK`, and `EAGAIN` is handled gracefully.

**Pipelining** — `try_one_request()` loops until the incoming buffer is exhausted, so multiple requests sent without waiting for responses are all processed in one `handle_read()` call.

**Incremental rehashing** — Avoids latency spikes caused by copying the entire hash table during resize. At most 128 entries migrate per operation.

**Intrusive linked lists** — `HNode` is embedded directly in `Entry`, avoiding a separate heap allocation per node and improving cache locality during chain traversal.

**Optimistic write** — After processing requests, `handle_read()` immediately calls `handle_write()` to attempt flushing the response before returning to `poll()`. This saves a round-trip through the event loop for the common case where the socket is immediately writable.
