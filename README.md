## 1. System Philosophy
The system is designed around the **Single-Threaded Event Loop** model. Rather than utilizing threads for concurrency, it leverages non-blocking I/O to handle thousands of simultaneous connections without the context-switching overhead or locking complexities of multi-threaded designs.

## 2. Core Components

### 2.1 Networking & I/O Multiplexing
The server uses the `poll()` system call to manage file descriptors (FDs).
* **Non-Blocking Mode:** All client sockets are set to `O_NONBLOCK`. This ensures that `read` or `write` operations return immediately if the network buffer is empty or full, respectively.
* **Connection State (`Conn` Struct):** Each connection tracks its own state, including:
    * **Application Intent:** `want_read` and `want_write` flags tell the event loop which events to listen for.
    * **Buffers:** Separate `incoming` and `outgoing` buffers to handle partial data transfers.
* **The Loop:** The server polls all active FDs. When an FD is ready, it triggers `handle_read` or `handle_write`. If a command is completed, the server processes it and moves the result to the `outgoing` buffer.

### 2.2 Dynamic Buffer Management
To handle the "streaming" nature of TCP, the `Buffer` structure manages memory dynamically:
* **Manual Memory Tracking:** It uses four pointers (`buffer_begin`, `buffer_end`, `data_begin`, `data_end`) to track allocated space versus actual data.
* **Zero-Copy Consumption:** When data is processed, `data_begin` is simply incremented. This "consumes" the data without moving memory.
* **Automatic Resizing:** If the buffer fills up, it attempts to `memmove` data back to the start. If still insufficient, it uses `realloc` to double the capacity.

### 2.3 The Hash Table (`HMap`)
The database uses a custom-built hash map designed for **predictable latency**.
* **Intrusive Design:** The `HNode` (containing the `next` pointer and hash code) is embedded directly inside the `Entry` struct. This means a lookup returns a pointer that is already part of the data, reducing memory allocations.
* **Progressive Rehashing:** To avoid "Stop-the-World" pauses during resizing:
    1. Two tables are maintained: `newer` and `older`.
    2. When the load factor is too high, a new larger table is created.
    3. Every `GET`, `SET`, or `DEL` operation moves a small constant amount of data (`k_rehashing_work`) from the old table to the new one.
    4. Eventually, the old table is empty and deleted.

---

## 3. Communication Protocol
The system uses a custom binary protocol to avoid the overhead of text parsing (like JSON or RESP).

### Request Format
| Field | Size | Description |
| :--- | :--- | :--- |
| **Length** | 4 Bytes | Total size of the following message |
| **N-Args** | 4 Bytes | Number of strings in the command |
| **Arglen** | 4 Bytes | Length of the first string |
| **Data** | Variable | Raw bytes of the first string |
| **...** | ... | Repeated for each argument |

### Response Format
| Field | Size | Description |
| :--- | :--- | :--- |
| **Length** | 4 Bytes | Total size of the payload |
| **Status** | 4 Bytes | 0 (OK), 1 (ERR), 2 (NX/Not Found) |
| **Data** | Variable | The returned value (for GET) |

---

## 4. Client & Pipelining
The test client demonstrates **Pipelining**, a technique to maximize throughput.
1. **Bulk Send:** The client writes multiple requests to the server's socket in one large batch.
2. **Bulk Receive:** It then enters a loop to read the responses.
3. **Efficiency:** This reduces the number of "Round Trip Times" (RTT). Instead of waiting for the network for every single command, the client only waits for the total batch to process.

---

## 5. Summary of Operations
1. **SET:** Hashes the key, inserts/updates the `Entry` in the `HMap`.
2. **GET:** Hashes the key, performs a lookup in `newer` (and `older` if rehashing is active).
3. **DEL:** Locates the node, detaches it from the linked list, and frees the `Entry` memory.
