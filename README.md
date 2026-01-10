# redis-clone

## Milestone 1: Socket API Setup
This stage covers the basic handshake between the server and the client.

### Server Setup
1. **`socket()`**: Create the file descriptor.
2. **`setsockopt()`**: Enable `SO_REUSEADDR` to allow immediate restarts.
3. **`bind()`**: Associate the socket with an IP and Port.
4. **`listen()`**: Wait for incoming connection requests.
5. **`accept()`**: Block until a client connects and return a new client-specific descriptor.

### Client Setup
1. **`socket()`**: Create the file descriptor.
2. **`connect()`**: Connect to the server's IP and Port.

### Communication
* **`read()` / `write()`**: Exchange data between descriptors.
* **`close()`**: Release the file descriptors.

---

## Milestone 2: Protocol Framing & The Byte Stream

This milestone addresses the transition from raw byte streams to a reliable **Application Protocol**.

### 1. TCP is a Byte Stream, Not a Message Stream
* **The Problem:** TCP does not preserve message boundaries. A single `write()` from a client might be split across multiple `read()` calls, or multiple `write()` calls might be merged into one.
* **The Solution:** We implement **Protocol Framing** using a length-prefix.

### 2. Reliable I/O with `read_full` and `write_all`
Standard syscalls can return "short" (fewer than requested bytes) even without an error.
* **`read_full`**: Loops until the exact number of bytes requested is filled.
* **`write_all`**: Loops until the entire buffer is sent to the kernel.

### 3. Length-Prefixed Protocol
To parse messages efficiently, we use a binary header:
* **Header (4 Bytes):** A little-endian integer representing the length of the message.
* **Payload (Variable):** The actual command or data.
