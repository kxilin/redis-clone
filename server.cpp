#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <vector>

#include "hashtable.h"
#include "utils.h"

#define container_of(ptr, T, member) ((T*)((char*)ptr - offsetof(T, member)))

constexpr size_t k_max_args = 200 * 1000;

struct Conn {
  int fd = -1;
  // application's intention used by the event loop
  bool want_read = false;
  bool want_write = false;
  bool want_close = false;
  // buffered input and output
  struct Buffer incoming;
  struct Buffer outgoing;
};

enum {
  RES_OK = 0,
  RES_ERR = 1,  // error
  RES_NX = 2,   // not found
};

static Conn* handle_accept(int fd) {
  // accept
  struct sockaddr_in client_addr = {};
  socklen_t addrlen = sizeof(client_addr);
  int connfd = accept(fd, (struct sockaddr*)&client_addr, &addrlen);
  if (connfd < 0) {
    return NULL;
  }
  char ip_str[INET_ADDRSTRLEN];  // Buffer to hold the string
                                 // (usually 16 bytes)
  inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
  fprintf(stderr, "new client from %s:%u\n", ip_str,
          ntohs(client_addr.sin_port));
  // set the new connection fd to nonblocking mode
  fd_set_nonblock(connfd);
  // create Conn struct to track state
  Conn* conn = new Conn();
  conn->fd = connfd;
  conn->want_read = true;  // read the first request

  buf_init(&conn->incoming, 16 * 1024);
  buf_init(&conn->outgoing, 16 * 1024);

  return conn;
}

static bool read_u32(const uint8_t*& cur, const uint8_t* end, uint32_t& out) {
  if (cur + k_header_size > end) {
    return false;
  }
  memcpy(&out, cur, k_header_size);
  cur += 4;
  return true;
}

static bool read_str(const uint8_t*& cur, const uint8_t* end, size_t len,
                     std::string& out) {
  if (cur + len > end) {
    return false;
  }

  out.assign(cur, cur + len);
  cur += len;
  return true;
}

// +------+-----+------+-----+------+-----+-----+------+
// | nstr | len | str1 | len | str2 | ... | len | strn |
// +------+-----+------+-----+------+-----+-----+------+

static int32_t parse_req(const uint8_t* data, size_t size,
                         std::vector<std::string>& out) {
  const uint8_t* end = data + size;
  uint32_t nstr = 0;
  if (!read_u32(data, end, nstr)) {
    return -1;
  }
  if (nstr > k_max_args) {
    return -1;
  }

  while (out.size() < nstr) {
    uint32_t len = 0;
    if (!read_u32(data, end, len)) {
      return -1;
    }
    out.push_back(std::string());
    if (!read_str(data, end, len, out.back())) {
      return -1;
    }
  }

  if (data != end) {
    return -1;  // trailing garbage
  }

  return 0;
}

// error code for TAG_ERR
enum {
  ERR_UNKNOWN = 1,  // unknown command
  ERR_TOO_BIG = 2,  // response too big
};

// data types of serialized data
enum {
  TAG_NIL = 0,  // nil
  TAG_ERR = 1,  // error code + msg
  TAG_STR = 2,  // string
  TAG_INT = 3,  // int64
  TAG_DBL = 4,  // double
  TAG_ARR = 5,  // array
};

// serialization helper functions
static void buf_append_u8(Buffer* buf, uint8_t data) {
  buf_append(buf, (const uint8_t*)&data, 1);
}
static void buf_append_u32(Buffer* buf, uint32_t data) {
  buf_append(buf, (const uint8_t*)&data, 4);
}
static void buf_append_i64(Buffer* buf, int64_t data) {
  buf_append(buf, (const uint8_t*)&data, 8);
}
static void buf_append_dbl(Buffer* buf, double data) {
  buf_append(buf, (const uint8_t*)&data, 8);
}

// append serialized data types to the back
static void out_nil(Buffer* out) { buf_append_u8(out, TAG_NIL); }
static void out_str(Buffer* out, const char* s, size_t size) {
  buf_append_u8(out, TAG_STR);
  buf_append_u32(out, (uint32_t)size);
  buf_append(out, (const uint8_t*)s, size);
}
static void out_int(Buffer* out, int64_t val) {
  buf_append_u8(out, TAG_INT);
  buf_append_i64(out, val);
}
static void out_dbl(Buffer* out, double val) {
  buf_append_u8(out, TAG_DBL);
  buf_append_dbl(out, val);
}
static void out_err(Buffer* out, uint32_t code, const std::string& msg) {
  buf_append_u8(out, TAG_ERR);
  buf_append_u32(out, code);
  buf_append_u32(out, (uint32_t)msg.size());
  buf_append(out, (const uint8_t*)msg.data(), msg.size());
}
static void out_arr(Buffer* out, uint32_t n) {
  buf_append_u8(out, TAG_ARR);
  buf_append_u32(out, n);
}

static struct {
  HMap db;
} g_data;

struct Entry {
  struct HNode node;
  std::string key;
  std::string val;
};

static bool entry_eq(HNode* lhs, HNode* rhs) {
  struct Entry* le = container_of(lhs, struct Entry, node);
  struct Entry* re = container_of(rhs, struct Entry, node);
  return le->key == re->key;
}

// FNV hash
static uint64_t str_hash(const uint8_t* data, size_t len) {
  uint32_t h = 0x811C9DC5;
  for (size_t i = 0; i < len; i++) {
    h = (h + data[i]) * 0x01000193;
  }
  return h;
}

static void do_get(std::vector<std::string>& cmd, struct Buffer& out) {
  Entry key;
  key.key.swap(cmd[1]);
  key.node.hcode = str_hash((uint8_t*)key.key.data(), key.key.size());

  HNode* node = hm_lookup(&g_data.db, &key.node, &entry_eq);
  if (!node) {
    return out_nil(&out);
  }

  const std::string& val = container_of(node, Entry, node)->val;
  return out_str(&out, val.data(), val.size());
}

static void do_set(std::vector<std::string>& cmd, struct Buffer& out) {
  Entry key;
  key.key.swap(cmd[1]);
  key.node.hcode = str_hash((uint8_t*)key.key.data(), key.key.size());

  HNode* node = hm_lookup(&g_data.db, &key.node, &entry_eq);
  if (node) {
    container_of(node, Entry, node)->val.swap(cmd[2]);
  } else {
    Entry* ent = new Entry();
    ent->key.swap(key.key);
    ent->node.hcode = key.node.hcode;
    ent->val.swap(cmd[2]);
    hm_insert(&g_data.db, &ent->node);
  }
  return out_nil(&out);
}

static void do_del(std::vector<std::string>& cmd, struct Buffer& out) {
  Entry key;
  key.key.swap(cmd[1]);
  key.node.hcode = str_hash((uint8_t*)key.key.data(), key.key.size());

  HNode* node = hm_delete(&g_data.db, &key.node, &entry_eq);
  if (node) {
    delete container_of(node, Entry, node);
  }

  return out_int(&out, node ? 1 : 0);
}

static bool cb_keys(HNode* node, void* arg) {
  Buffer* out = (Buffer*)arg;
  const std::string& key = container_of(node, Entry, node)->key;
  out_str(out, key.data(), key.size());
  return true;
}

static void do_keys(std::vector<std::string>& cmd, Buffer& out) {
  out_arr(&out, (uint32_t)hm_size(&g_data.db));
  hm_foreach(&g_data.db, &cb_keys, (void*)&out);
}

static void do_request(std::vector<std::string>& cmd, struct Buffer& out) {
  // remember where the msg starts to leave room for the length header
  size_t header_idx = buf_size(&out);

  // append placeholder
  uint32_t placeholder = 0;
  buf_append(&out, (const uint8_t*)&placeholder, k_header_size);

  // Route the command
  if (cmd.size() == 2 && cmd[0] == "get") {
    do_get(cmd, out);
  } else if (cmd.size() == 3 && cmd[0] == "set") {
    do_set(cmd, out);
  } else if (cmd.size() == 2 && cmd[0] == "del") {
    do_del(cmd, out);
  } else if (cmd.size() == 1 && cmd[0] == "keys") {
    do_keys(cmd, out);
  } else {
    out_err(&out, ERR_UNKNOWN, "unknown command.");
  }

  // size is current - initial - k_header_size
  uint32_t payload_size =
      (uint32_t)(buf_size(&out) - header_idx - k_header_size);

  // patch into placeholder
  memcpy(out.data_begin + header_idx, &payload_size, k_header_size);
}

static bool try_one_request(Conn* conn) {
  // 3. try to parse the buffer
  // protocol message header
  if (buf_size(&conn->incoming) < k_header_size) {
    return false;  // continue to want read
  }
  uint32_t len = 0;
  memcpy(&len, conn->incoming.data_begin, k_header_size);
  if (len > k_max_msg) {
    conn->want_close = true;  // protocol error
    return false;
  }
  // protocol message body
  if (k_header_size + len > buf_size(&conn->incoming)) {
    return false;  // continue to want read
  }

  const uint8_t* request = conn->incoming.data_begin + k_header_size;

  std::vector<std::string> cmd;
  if (parse_req(request, len, cmd) < 0) {
    conn->want_close = true;
    return false;
  }

  do_request(cmd, conn->outgoing);

  buf_consume(&conn->incoming, k_header_size + len);
  return true;
}

static void handle_write(Conn* conn) {
  ssize_t rv =
      write(conn->fd, conn->outgoing.data_begin, buf_size(&conn->outgoing));
  if (rv < 0 && errno == EAGAIN) {
    return;  // not ready
  }
  if (rv < 0) {
    conn->want_close = true;
    return;  // error
  }
  // remove written data from outgoing
  buf_consume(&conn->outgoing, (size_t)rv);
  // update readiness intention
  if (buf_size(&conn->outgoing) == 0) {
    conn->want_write = false;
    conn->want_read = true;
  }
}

static void handle_read(Conn* conn) {
  // 1. do a nonblocking read
  uint8_t buf[64 * 1024];
  ssize_t bytes_read = read(conn->fd, buf, sizeof(buf));
  if (bytes_read <= 0) {
    conn->want_close = true;
    return;
  }
  // 2. add new data to conn incoming buffer
  buf_append(&conn->incoming, buf, (size_t)bytes_read);
  // 3. try to parse the buffer
  // 4. process the parsed message
  // 5. remove the message from conn incoming buffer
  while (try_one_request(conn)) {
  }

  // update readiness intention
  if (buf_size(&conn->outgoing) > 0) {
    conn->want_read = false;
    conn->want_write = true;
    // optimistic write
    return handle_write(conn);
  }
}

int main() {
  int fd = socket(AF_INET, SOCK_STREAM, 0);  // get socket fd
  if (fd < 0) die("socket()");
  int val = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val,
                 sizeof(val)) < 0) {  // socket option to allow same ip:port
                                      // after restart
    die("setsockopt()");
  }
  fd_set_nonblock(fd);

  struct sockaddr_in addr = {};  // address to bind to the socket that the
                                 // server will listen on
  addr.sin_family = AF_INET;
  addr.sin_port = htons(1234);
  addr.sin_addr.s_addr = htonl(0);
  if (bind(fd, (const struct sockaddr*)&addr,
           sizeof(addr)) < 0) {  // // binds the socket to this address
    die("bind()");
  }

  if (listen(fd, SOMAXCONN) < 0) {  // server turns on and starts listening for
                                    // incoming connections
    die("listen()");
  }

  // mapping of fd to Conn
  std::vector<Conn*> fd2conn;
  // event loop
  std::vector<struct pollfd> poll_args;
  while (true) {
    // prepare poll args
    poll_args.clear();
    // put the listening socket in first position
    struct pollfd pfd = {fd, POLLIN, 0};
    poll_args.push_back(pfd);
    // the rest are connection sockets
    for (Conn* conn : fd2conn) {
      if (!conn) continue;
      struct pollfd pfd = {conn->fd, 0, 0};
      // poll() flags depending on application intent
      if (conn->want_read) {
        pfd.events |= POLLIN;
      }
      if (conn->want_write) {
        pfd.events |= POLLOUT;
      }
      poll_args.push_back(pfd);
    }

    // wait for readiness
    int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), -1);
    if (rv < 0 && errno == EINTR) continue;
    if (rv < 0) die("poll()");

    // handle listening socket
    if (poll_args[0].revents & POLLIN) {
      if (Conn* conn = handle_accept(fd)) {
        // add into mapping of fd to Conn
        if (fd2conn.size() <= (size_t)conn->fd) {
          fd2conn.resize(2 * conn->fd);
        }
        fd2conn[conn->fd] = conn;
      }
    }

    // handle connection sockets
    for (size_t i = 1; i < poll_args.size(); i++) {
      uint32_t ready = poll_args[i].revents;
      Conn* conn = fd2conn[poll_args[i].fd];
      if (ready & POLLIN) {
        handle_read(conn);
      }
      if (ready & POLLOUT) {
        handle_write(conn);
      }
      if (ready & POLLERR || conn->want_close) {
        close(conn->fd);
        buf_destroy(&conn->incoming);
        buf_destroy(&conn->outgoing);
        fd2conn[conn->fd] = NULL;
        delete conn;
      }
    }
  }

  return 0;
}
