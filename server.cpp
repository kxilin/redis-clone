#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include "common.h"
#include "hashtable.h"
#include "heap.h"
#include "list.h"
#include "utils.h"
#include "zset.h"

constexpr size_t k_max_args = 200 * 1000;

static uint64_t get_monotonic_msec() {
  struct timespec tv = {0, 0};  // (seconds, nanoseconds)
  clock_gettime(CLOCK_MONOTONIC, &tv);
  return uint64_t(tv.tv_sec) * 1000 + tv.tv_nsec / 1000 / 1000;
}

struct Conn {
  int fd = -1;
  // application's intention used by the event loop
  bool want_read = false;
  bool want_write = false;
  bool want_close = false;
  // buffered input and output
  struct Buffer incoming;
  struct Buffer outgoing;
  // timer
  uint64_t last_active_ms = 0;
  DList timer_node;
};

static struct {
  DList idle_list;
  DList io_list;
  std::vector<HeapItem> heap;
  HMap db;
  std::vector<Conn*> fd2conn;
} g_data;

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
  conn->last_active_ms = get_monotonic_msec();
  dlist_insert_before(&g_data.idle_list, &conn->timer_node);

  buf_init(&conn->incoming, 16 * 1024);
  buf_init(&conn->outgoing, 16 * 1024);

  return conn;
}

static void conn_destroy(Conn* conn) {
  (void)close(conn->fd);
  buf_destroy(&conn->incoming);
  buf_destroy(&conn->outgoing);
  g_data.fd2conn[conn->fd] = NULL;
  dlist_detach(&conn->timer_node);
  delete conn;
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
  ERR_BAD_TYP = 3,  // unexpected value type
  ERR_BAD_ARG = 4,  // bad arguments
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
static size_t out_begin_arr(Buffer* out) {
  buf_append_u8(out, TAG_ARR);
  buf_append_u32(out, 0);    // filled by out_end_arr()
  return buf_size(out) - 4;  // the `ctx` arg
}
static void out_end_arr(Buffer* out, size_t ctx, uint32_t n) {
  memcpy(out->data_begin + ctx, &n, 4);
}

enum {
  T_INIT = 0,
  T_STR = 1,   // string
  T_ZSET = 2,  // sorted set
};

struct Entry {
  struct HNode node;  // hashtable node
  std::string key;
  // for TTL
  size_t heap_idx = -1;
  // value
  uint32_t type = 0;
  // one of the following
  std::string str;
  ZSet zset;
};

static Entry* entry_new(uint32_t type) {
  Entry* ent = new Entry();
  ent->type = type;
  return ent;
}

static void entry_set_ttl(Entry* ent, int64_t ttl_ms);

static void entry_del(Entry* ent) {
  if (ent->type == T_ZSET) {
    zset_clear(&ent->zset);
  }
  entry_set_ttl(ent, -1);
  delete ent;
}

struct LookupKey {
  struct HNode node;
  std::string key;
};

static bool entry_eq(HNode* node, HNode* key) {
  struct Entry* ent = container_of(node, struct Entry, node);
  struct LookupKey* keydata = container_of(key, struct LookupKey, node);
  return ent->key == keydata->key;
}

static void do_get(std::vector<std::string>& cmd, struct Buffer& out) {
  LookupKey key;
  key.key.swap(cmd[1]);
  key.node.hcode = str_hash((uint8_t*)key.key.data(), key.key.size());

  HNode* node = hm_lookup(&g_data.db, &key.node, &entry_eq);
  if (!node) {
    return out_nil(&out);
  }

  Entry* ent = container_of(node, Entry, node);
  if (ent->type != T_STR) {
    return out_err(&out, ERR_BAD_TYP, "not a string value");
  }
  return out_str(&out, ent->str.data(), ent->str.size());
}

static void do_set(std::vector<std::string>& cmd, struct Buffer& out) {
  LookupKey key;
  key.key.swap(cmd[1]);
  key.node.hcode = str_hash((uint8_t*)key.key.data(), key.key.size());

  HNode* node = hm_lookup(&g_data.db, &key.node, &entry_eq);
  if (node) {
    Entry* ent = container_of(node, Entry, node);
    if (ent->type != T_STR) {
      return out_err(&out, ERR_BAD_TYP, "a non-string value exists");
    }
    ent->str.swap(cmd[2]);
  } else {
    Entry* ent = entry_new(T_STR);
    ent->key.swap(key.key);
    ent->node.hcode = key.node.hcode;
    ent->str.swap(cmd[2]);
    hm_insert(&g_data.db, &ent->node);
  }
  return out_nil(&out);
}

static void do_del(std::vector<std::string>& cmd, struct Buffer& out) {
  LookupKey key;
  key.key.swap(cmd[1]);
  key.node.hcode = str_hash((uint8_t*)key.key.data(), key.key.size());

  HNode* node = hm_delete(&g_data.db, &key.node, &entry_eq);
  if (node) {
    entry_del(container_of(node, Entry, node));
  }

  return out_int(&out, node ? 1 : 0);
}

static void heap_upsert(std::vector<HeapItem>& a, size_t pos, HeapItem t) {
  if (pos < a.size()) {
    a[pos] = t;
  } else {
    pos = a.size();
    a.push_back(t);
  }
  heap_update(a.data(), pos, a.size());
}

static void heap_delete(std::vector<HeapItem>& a, size_t pos) {
  a[pos] = a.back();
  a.pop_back();
  if (pos < a.size()) {
    heap_update(a.data(), pos, a.size());
  }
}

static void entry_set_ttl(Entry* ent, int64_t ttl_ms) {
  if (ttl_ms < 0 && ent->heap_idx != (size_t)-1) {
    heap_delete(g_data.heap, ent->heap_idx);
    ent->heap_idx = -1;
  } else if (ttl_ms >= 0) {
    uint64_t expire_at = get_monotonic_msec() + (uint64_t)ttl_ms;
    HeapItem item = {expire_at, &ent->heap_idx};
    heap_upsert(g_data.heap, ent->heap_idx, item);
  }
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

static bool str2dbl(const std::string& s, double& out) {
  char* endp = NULL;
  out = strtod(s.c_str(), &endp);
  return endp == s.c_str() + s.size() && !isnan(out);
}

static bool str2int(const std::string& s, int64_t& out) {
  char* endp = NULL;
  out = strtoll(s.c_str(), &endp, 10);
  return endp == s.c_str() + s.size();
}

// zadd zset score name
static void do_zadd(std::vector<std::string>& cmd, Buffer& out) {
  double score = 0;
  if (!str2dbl(cmd[2], score)) {
    return out_err(&out, ERR_BAD_ARG, "expect float");
  }

  // look up or create the zset
  LookupKey key;
  key.key.swap(cmd[1]);
  key.node.hcode = str_hash((uint8_t*)key.key.data(), key.key.size());
  HNode* hnode = hm_lookup(&g_data.db, &key.node, &entry_eq);

  Entry* ent = NULL;
  if (!hnode) {  // insert a new key
    ent = entry_new(T_ZSET);
    ent->key.swap(key.key);
    ent->node.hcode = key.node.hcode;
    hm_insert(&g_data.db, &ent->node);
  } else {  // check the existing key
    ent = container_of(hnode, Entry, node);
    if (ent->type != T_ZSET) {
      return out_err(&out, ERR_BAD_TYP, "expect zset");
    }
  }

  // add or update the tuple
  const std::string& name = cmd[3];
  bool added = zset_insert(&ent->zset, name.data(), name.size(), score);
  return out_int(&out, (int64_t)added);
}

static const ZSet k_empty_zset;

static ZSet* expect_zset(std::string& s) {
  LookupKey key;
  key.key.swap(s);
  key.node.hcode = str_hash((uint8_t*)key.key.data(), key.key.size());
  HNode* hnode = hm_lookup(&g_data.db, &key.node, &entry_eq);
  if (!hnode) {  // a non-existent key is treated as an empty zset
    return (ZSet*)&k_empty_zset;
  }
  Entry* ent = container_of(hnode, Entry, node);
  return ent->type == T_ZSET ? &ent->zset : NULL;
}

// zrem zset name
static void do_zrem(std::vector<std::string>& cmd, Buffer& out) {
  ZSet* zset = expect_zset(cmd[1]);
  if (!zset) {
    return out_err(&out, ERR_BAD_TYP, "expect zset");
  }

  const std::string& name = cmd[2];
  ZNode* znode = zset_lookup(zset, name.data(), name.size());
  if (znode) {
    zset_delete(zset, znode);
  }
  return out_int(&out, znode ? 1 : 0);
}

// zscore zset name
static void do_zscore(std::vector<std::string>& cmd, Buffer& out) {
  ZSet* zset = expect_zset(cmd[1]);
  if (!zset) {
    return out_err(&out, ERR_BAD_TYP, "expect zset");
  }

  const std::string& name = cmd[2];
  ZNode* znode = zset_lookup(zset, name.data(), name.size());
  return znode ? out_dbl(&out, znode->score) : out_nil(&out);
}

// zquery zset score name offset limit
static void do_zquery(std::vector<std::string>& cmd, Buffer& out) {
  // parse args
  double score = 0;
  if (!str2dbl(cmd[2], score)) {
    return out_err(&out, ERR_BAD_ARG, "expect fp number");
  }
  const std::string& name = cmd[3];
  int64_t offset = 0, limit = 0;
  if (!str2int(cmd[4], offset) || !str2int(cmd[5], limit)) {
    return out_err(&out, ERR_BAD_ARG, "expect int");
  }

  // get the zset
  ZSet* zset = expect_zset(cmd[1]);
  if (!zset) {
    return out_err(&out, ERR_BAD_TYP, "expect zset");
  }

  // seek to the key
  if (limit <= 0) {
    return out_arr(&out, 0);
  }
  ZNode* znode = zset_seekge(zset, score, name.data(), name.size());
  znode = znode_offset(znode, offset);

  // output
  size_t ctx = out_begin_arr(&out);
  int64_t n = 0;
  while (znode && n < limit) {
    out_str(&out, znode->name, znode->len);
    out_dbl(&out, znode->score);
    znode = znode_offset(znode, +1);
    n += 2;
  }
  out_end_arr(&out, ctx, (uint32_t)n);
}

// zqueryr zset score name offset limit
static void do_zqueryr(std::vector<std::string>& cmd, Buffer& out) {
  // parse args
  double score = 0;
  if (!str2dbl(cmd[2], score)) {
    return out_err(&out, ERR_BAD_ARG, "expect fp number");
  }
  const std::string& name = cmd[3];
  int64_t offset = 0, limit = 0;
  if (!str2int(cmd[4], offset) || !str2int(cmd[5], limit)) {
    return out_err(&out, ERR_BAD_ARG, "expect int");
  }

  // get the zset
  ZSet* zset = expect_zset(cmd[1]);
  if (!zset) {
    return out_err(&out, ERR_BAD_TYP, "expect zset");
  }

  // seek to the key
  if (limit <= 0) {
    return out_arr(&out, 0);
  }
  ZNode* znode = zset_seekle(zset, score, name.data(), name.size());
  znode = znode_offset(znode, -offset);

  // output
  size_t ctx = out_begin_arr(&out);
  int64_t n = 0;
  while (znode && n < limit) {
    out_str(&out, znode->name, znode->len);
    out_dbl(&out, znode->score);
    znode = znode_offset(znode, -1);
    n += 2;
  }
  out_end_arr(&out, ctx, (uint32_t)n);
}

// zcount zset lo_score lo_name hi_score hi_name
static void do_zcount(std::vector<std::string>& cmd, Buffer& out) {
  // parse args
  double lo_score = 0, hi_score = 0;
  if (!str2dbl(cmd[2], lo_score)) {
    return out_err(&out, ERR_BAD_ARG, "expect float");
  }
  const std::string& lo_name = cmd[3];
  if (!str2dbl(cmd[4], hi_score)) {
    return out_err(&out, ERR_BAD_ARG, "expect float");
  }
  const std::string& hi_name = cmd[5];

  // get the zset
  ZSet* zset = expect_zset(cmd[1]);
  if (!zset) {
    return out_err(&out, ERR_BAD_TYP, "expect zset");
  }

  int64_t count = zset_count(zset, lo_score, lo_name.data(), lo_name.size(),
                             hi_score, hi_name.data(), hi_name.size());
  return out_int(&out, count);
}

// zrank zset name
static void do_zrank(std::vector<std::string>& cmd, Buffer& out) {
  ZSet* zset = expect_zset(cmd[1]);
  if (!zset) {
    return out_err(&out, ERR_BAD_TYP, "expect zset");
  }

  const std::string& name = cmd[2];
  ZNode* znode = zset_lookup(zset, name.data(), name.size());
  if (!znode) {
    return out_nil(&out);  // name not found
  }
  return out_int(&out, avl_rank(&znode->tree));
}

// PEXPIRE key ttl_ms
static void do_expire(std::vector<std::string>& cmd, Buffer& out) {
  int64_t ttl_ms = 0;
  if (!str2int(cmd[2], ttl_ms)) {
    return out_err(&out, ERR_BAD_ARG, "expect int64");
  }

  LookupKey key;
  key.key.swap(cmd[1]);
  key.node.hcode = str_hash((uint8_t*)key.key.data(), key.key.size());
  HNode* node = hm_lookup(&g_data.db, &key.node, &entry_eq);
  if (node) {
    Entry* ent = container_of(node, Entry, node);
    entry_set_ttl(ent, ttl_ms);
  }
  return out_int(&out, node ? 1 : 0);
}

// PTTL key
static void do_ttl(std::vector<std::string>& cmd, Buffer& out) {
  LookupKey key;
  key.key.swap(cmd[1]);
  key.node.hcode = str_hash((uint8_t*)key.key.data(), key.key.size());

  HNode* node = hm_lookup(&g_data.db, &key.node, &entry_eq);
  if (!node) {
    return out_int(&out, -2);  // not found
  }

  Entry* ent = container_of(node, Entry, node);
  if (ent->heap_idx == (size_t)-1) {
    return out_int(&out, -1);  // no TTL
  }

  uint64_t expire_at = g_data.heap[ent->heap_idx].val;
  uint64_t now_ms = get_monotonic_msec();
  return out_int(&out, expire_at > now_ms ? (expire_at - now_ms) : 0);
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
  } else if (cmd.size() == 4 && cmd[0] == "zadd") {
    do_zadd(cmd, out);
  } else if (cmd.size() == 3 && cmd[0] == "zrem") {
    do_zrem(cmd, out);
  } else if (cmd.size() == 3 && cmd[0] == "zscore") {
    do_zscore(cmd, out);
  } else if (cmd.size() == 6 && cmd[0] == "zquery") {
    do_zquery(cmd, out);
  } else if (cmd.size() == 6 && cmd[0] == "zcount") {
    do_zcount(cmd, out);
  } else if (cmd.size() == 6 && cmd[0] == "zqueryr") {
    do_zqueryr(cmd, out);
  } else if (cmd.size() == 3 && cmd[0] == "zrank") {
    do_zrank(cmd, out);
  } else if (cmd.size() == 3 && cmd[0] == "pexpire") {
    do_expire(cmd, out);
  } else if (cmd.size() == 2 && cmd[0] == "pttl") {
    do_ttl(cmd, out);
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
  conn->last_active_ms = get_monotonic_msec();
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
    dlist_detach(&conn->timer_node);
    dlist_insert_before(&g_data.idle_list, &conn->timer_node);
  }
}

static void handle_read(Conn* conn) {
  conn->last_active_ms = get_monotonic_msec();
  // 1. do a nonblocking read
  uint8_t buf[64 * 1024];
  ssize_t bytes_read = read(conn->fd, buf, sizeof(buf));
  if (bytes_read <= 0) {
    conn->want_close = true;
    return;
  }
  // 2. add new data to conn incoming buffer
  buf_append(&conn->incoming, buf, (size_t)bytes_read);
  dlist_detach(&conn->timer_node);
  dlist_insert_before(&g_data.io_list, &conn->timer_node);
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

constexpr uint64_t k_idle_timeout_ms = 5 * 1000;
constexpr uint64_t k_io_timeout_ms = 1 * 1000;

static int32_t next_timer_ms() {
  uint64_t now_ms = get_monotonic_msec();
  uint64_t next_ms = UINT64_MAX;

  if (!dlist_empty(&g_data.idle_list)) {
    Conn* conn = container_of(g_data.idle_list.next, Conn, timer_node);
    next_ms = std::min(next_ms, conn->last_active_ms + k_idle_timeout_ms);
  }
  if (!dlist_empty(&g_data.io_list)) {
    Conn* conn = container_of(g_data.io_list.next, Conn, timer_node);
    next_ms = std::min(next_ms, conn->last_active_ms + k_io_timeout_ms);
  }
  if (!g_data.heap.empty()) {
    next_ms = std::min(next_ms, g_data.heap[0].val);
  }
  if (next_ms == UINT64_MAX) return -1;
  if (next_ms <= now_ms) return 0;
  return (int32_t)(next_ms - now_ms);
}

static bool hnode_same(HNode* node, HNode* key) { return node == key; }

static void process_timers() {
  uint64_t now_ms = get_monotonic_msec();
  while (!dlist_empty(&g_data.idle_list)) {
    Conn* conn = container_of(g_data.idle_list.next, Conn, timer_node);
    uint64_t next_ms = conn->last_active_ms + k_idle_timeout_ms;
    if (next_ms >= now_ms) {
      break;
    }
    fprintf(stderr, "removing idle connection: %d\n", conn->fd);
    conn_destroy(conn);
  }
  while (!dlist_empty(&g_data.io_list)) {
    Conn* conn = container_of(g_data.io_list.next, Conn, timer_node);
    uint64_t next_ms = conn->last_active_ms + k_io_timeout_ms;
    if (next_ms >= now_ms) {
      break;
    }
    fprintf(stderr, "removing io timeout connection: %d\n", conn->fd);
    conn_destroy(conn);
  }

  constexpr size_t k_max_works = 2000;
  size_t nworks = 0;
  const std::vector<HeapItem>& heap = g_data.heap;
  while (!heap.empty() && heap[0].val < now_ms && nworks++ < k_max_works) {
    Entry* ent = container_of(heap[0].ref, Entry, heap_idx);
    hm_delete(&g_data.db, &ent->node, &hnode_same);
    fprintf(stderr, "key expired: %s\n", ent->key.c_str());
    entry_del(ent);  // delete the key
  }
}

int main() {
  dlist_init(&g_data.idle_list);
  dlist_init(&g_data.io_list);

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

  // event loop
  std::vector<struct pollfd> poll_args;
  while (true) {
    // prepare poll args
    poll_args.clear();
    // put the listening socket in first position
    struct pollfd pfd = {fd, POLLIN, 0};
    poll_args.push_back(pfd);
    // the rest are connection sockets
    for (Conn* conn : g_data.fd2conn) {
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

    int32_t timeout_ms = next_timer_ms();
    int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), timeout_ms);
    if (rv < 0 && errno == EINTR) continue;
    if (rv < 0) die("poll()");

    // handle listening socket
    if (poll_args[0].revents & POLLIN) {
      if (Conn* conn = handle_accept(fd)) {
        // add into mapping of fd to Conn
        if (g_data.fd2conn.size() <= (size_t)conn->fd) {
          g_data.fd2conn.resize(2 * conn->fd);
        }
        g_data.fd2conn[conn->fd] = conn;
      }
    }

    // handle connection sockets
    for (size_t i = 1; i < poll_args.size(); i++) {
      uint32_t ready = poll_args[i].revents;
      if (ready == 0) {
        continue;
      }
      Conn* conn = g_data.fd2conn[poll_args[i].fd];
      if (ready & POLLIN) {
        handle_read(conn);
      }
      if (ready & POLLOUT) {
        handle_write(conn);
      }
      if (ready & POLLERR || conn->want_close) {
        conn_destroy(conn);
      }
    }
    process_timers();
  }

  return 0;
}
