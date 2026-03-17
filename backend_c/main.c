#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <limits.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdbool.h>
#include <signal.h>
#include <errno.h>
#include <stdatomic.h>

#define ADDRESS_IP "0.0.0.0"
#define ADDRESS_PORT 9001
#define CONNECTION_CONCURRENT_TARGET 256
#define CLIENT_REQUEST_BUFFER_SIZE 8192
#define PUBLIC_DIR "./public"

// --------------------------------------------------------- //

/*
TODO:
[?] multi-threaded http server
[?] threadpool for scalability
[?] blocking I/O (not epoll)
[?] keep-alive (manual)
[?] websocket support
[?] query parameters parsing
[?] dynamic thread count (hardware_concurrency)
[?] public file access
[?] not full http spec
[?] not async / non-blocking server
*/

/* --------------------------------------------------------- */
/* WebSocket room structures */
typedef struct {
    int* connections;
    size_t count;
    size_t capacity;
} websocket_room_t;

typedef struct {
    char* name;
    websocket_room_t room;
} room_entry_t;

static room_entry_t* g_rooms = NULL;
static size_t g_room_count = 0;
static size_t g_room_capacity = 0;
static pthread_mutex_t g_rooms_mutex = PTHREAD_MUTEX_INITIALIZER;

/* --------------------------------------------------------- */
static void ignore_sigpipe(void) {
    signal(SIGPIPE, SIG_IGN);
}

/* --------------------------------------------------------- */
/* Thread pool */
typedef struct task {
    void (*function)(void*);
    void* arg;
    struct task* next;
} task_t;

typedef struct {
    pthread_t* threads;
    size_t thread_count;
    task_t* queue_head;
    task_t* queue_tail;
    pthread_mutex_t queue_mutex;
    pthread_cond_t condition;
    atomic_bool stop;
} thread_pool_t;

static void* worker_thread(void* arg) {
    thread_pool_t* pool = (thread_pool_t*)arg;
    while (1) {
        task_t* task = NULL;
        pthread_mutex_lock(&pool->queue_mutex);
        while (pool->queue_head == NULL && !pool->stop) {
            pthread_cond_wait(&pool->condition, &pool->queue_mutex);
        }
        if (pool->stop && pool->queue_head == NULL) {
            pthread_mutex_unlock(&pool->queue_mutex);
            break;
        }
        task = pool->queue_head;
        pool->queue_head = task->next;
        if (pool->queue_head == NULL) pool->queue_tail = NULL;
        pthread_mutex_unlock(&pool->queue_mutex);

        task->function(task->arg);
        free(task);
    }
    return NULL;
}

static thread_pool_t* thread_pool_create(size_t num_threads) {
    thread_pool_t* pool = malloc(sizeof(thread_pool_t));
    if (!pool) return NULL;
    pool->thread_count = num_threads;
    pool->queue_head = pool->queue_tail = NULL;
    pthread_mutex_init(&pool->queue_mutex, NULL);
    pthread_cond_init(&pool->condition, NULL);
    atomic_init(&pool->stop, false);

    pool->threads = malloc(num_threads * sizeof(pthread_t));
    if (!pool->threads) {
        free(pool);
        return NULL;
    }
    for (size_t i = 0; i < num_threads; ++i) {
        pthread_create(&pool->threads[i], NULL, worker_thread, pool);
        // No affinity set – let the kernel schedule threads freely
    }
    return pool;
}

static void thread_pool_enqueue(thread_pool_t* pool, void (*func)(void*), void* arg) {
    task_t* task = malloc(sizeof(task_t));
    if (!task) return;
    task->function = func;
    task->arg = arg;
    task->next = NULL;

    pthread_mutex_lock(&pool->queue_mutex);
    if (pool->queue_tail) {
        pool->queue_tail->next = task;
        pool->queue_tail = task;
    } else {
        pool->queue_head = pool->queue_tail = task;
    }
    pthread_cond_signal(&pool->condition);
    pthread_mutex_unlock(&pool->queue_mutex);
}

static void thread_pool_destroy(thread_pool_t* pool) {
    if (!pool) return;
    atomic_store(&pool->stop, true);
    pthread_cond_broadcast(&pool->condition);
    for (size_t i = 0; i < pool->thread_count; ++i) {
        pthread_join(pool->threads[i], NULL);
    }
    pthread_mutex_destroy(&pool->queue_mutex);
    pthread_cond_destroy(&pool->condition);
    free(pool->threads);
    task_t* t = pool->queue_head;
    while (t) {
        task_t* next = t->next;
        free(t);
        t = next;
    }
    free(pool);
}

/* --------------------------------------------------------- */
/* WebSocket frame (RFC 6455) */
typedef enum {
    WS_OPCODE_CONTINUATION = 0x0,
    WS_OPCODE_TEXT         = 0x1,
    WS_OPCODE_BINARY       = 0x2,
    WS_OPCODE_CLOSE        = 0x8,
    WS_OPCODE_PING         = 0x9,
    WS_OPCODE_PONG         = 0xA
} ws_opcode_t;

typedef struct {
    bool fin;
    bool mask;
    ws_opcode_t opcode;
    uint64_t payload_len;
    uint8_t masking_key[4];
    uint8_t* payload;
} websocket_frame_t;

static bool websocket_frame_parse(const uint8_t* data, size_t len, websocket_frame_t* frame, size_t* consumed) {
    if (len < 2) return false;
    const uint8_t* ptr = data;
    frame->fin = (*ptr & 0x80) != 0;
    frame->opcode = *ptr & 0x0F;
    ptr++;
    frame->mask = (*ptr & 0x80) != 0;
    uint8_t payload_len_indicator = *ptr & 0x7F;
    ptr++;
    if (payload_len_indicator == 126) {
        if (len < (size_t)(ptr - data) + 2) return false;
        frame->payload_len = ((uint64_t)ptr[0] << 8) | ptr[1];
        ptr += 2;
    } else if (payload_len_indicator == 127) {
        if (len < (size_t)(ptr - data) + 8) return false;
        frame->payload_len = 0;
        for (int i = 0; i < 8; ++i) {
            frame->payload_len = (frame->payload_len << 8) | ptr[i];
        }
        ptr += 8;
    } else {
        frame->payload_len = payload_len_indicator;
    }
    if (frame->mask) {
        if (len < (size_t)(ptr - data) + 4) return false;
        memcpy(frame->masking_key, ptr, 4);
        ptr += 4;
    }
    size_t header_len = ptr - data;
    if (len < header_len + frame->payload_len) return false;
    frame->payload = malloc(frame->payload_len);
    if (!frame->payload) return false;
    memcpy(frame->payload, ptr, frame->payload_len);
    if (frame->mask) {
        for (size_t i = 0; i < frame->payload_len; ++i) {
            frame->payload[i] ^= frame->masking_key[i % 4];
        }
    }
    *consumed = header_len + frame->payload_len;
    return true;
}

static uint8_t* websocket_frame_serialize(const websocket_frame_t* frame, size_t* out_len) {
    uint8_t header[14];
    int header_len = 0;
    uint8_t first_byte = (frame->fin ? 0x80 : 0x00) | frame->opcode;
    header[header_len++] = first_byte;
    if (frame->payload_len <= 125) {
        header[header_len++] = (uint8_t)frame->payload_len;
    } else if (frame->payload_len <= 65535) {
        header[header_len++] = 126;
        header[header_len++] = (frame->payload_len >> 8) & 0xFF;
        header[header_len++] = frame->payload_len & 0xFF;
    } else {
        header[header_len++] = 127;
        for (int i = 7; i >= 0; --i) {
            header[header_len++] = (frame->payload_len >> (i * 8)) & 0xFF;
        }
    }
    *out_len = header_len + frame->payload_len;
    uint8_t* buf = malloc(*out_len);
    if (!buf) return NULL;
    memcpy(buf, header, header_len);
    memcpy(buf + header_len, frame->payload, frame->payload_len);
    return buf;
}

static void websocket_frame_free(websocket_frame_t* frame) {
    if (frame->payload) free(frame->payload);
    frame->payload = NULL;
}

static websocket_frame_t websocket_frame_create_pong(const uint8_t* payload, size_t len) {
    websocket_frame_t frame = {0};
    frame.fin = true;
    frame.opcode = WS_OPCODE_PONG;
    frame.payload_len = len;
    if (len > 0) {
        frame.payload = malloc(len);
        if (frame.payload) memcpy(frame.payload, payload, len);
    }
    return frame;
}

static websocket_frame_t websocket_frame_create_close(uint16_t code, const char* reason) {
    websocket_frame_t frame = {0};
    frame.fin = true;
    frame.opcode = WS_OPCODE_CLOSE;
    size_t reason_len = reason ? strlen(reason) : 0;
    frame.payload_len = 2 + reason_len;
    frame.payload = malloc(frame.payload_len);
    if (frame.payload) {
        frame.payload[0] = (code >> 8) & 0xFF;
        frame.payload[1] = code & 0xFF;
        if (reason_len > 0) memcpy(frame.payload + 2, reason, reason_len);
    }
    return frame;
}

/* --------------------------------------------------------- */
/* Base64 encoding */
static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char* base64_encode(const uint8_t* data, size_t len) {
    size_t out_len = 4 * ((len + 2) / 3);
    char* out = malloc(out_len + 1);
    if (!out) return NULL;
    size_t i = 0, j = 0;
    while (i < len) {
        uint32_t octet_a = i < len ? data[i++] : 0;
        uint32_t octet_b = i < len ? data[i++] : 0;
        uint32_t octet_c = i < len ? data[i++] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;
        out[j++] = b64_table[(triple >> 18) & 0x3F];
        out[j++] = b64_table[(triple >> 12) & 0x3F];
        out[j++] = b64_table[(triple >> 6) & 0x3F];
        out[j++] = b64_table[triple & 0x3F];
    }
    size_t mod = len % 3;
    if (mod) {
        out[out_len - 1] = '=';
        if (mod == 1) out[out_len - 2] = '=';
    }
    out[out_len] = '\0';
    return out;
}

/* --------------------------------------------------------- */
/* SHA-1 implementation */
typedef struct {
    uint32_t state[5];
    uint64_t count;
    uint8_t buffer[64];
} sha1_ctx;

static void sha1_transform(sha1_ctx* ctx, const uint8_t* block) {
    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3], e = ctx->state[4];
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (block[i*4] << 24) | (block[i*4+1] << 16) | (block[i*4+2] << 8) | block[i*4+3];
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = (w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16]);
        w[i] = (w[i] << 1) | (w[i] >> 31);
    }
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
        e = d;
        d = c;
        c = (b << 30) | (b >> 2);
        b = a;
        a = temp;
    }
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
}

static void sha1_init(sha1_ctx* ctx) {
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->count = 0;
}

static void sha1_update(sha1_ctx* ctx, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx->buffer[ctx->count % 64] = data[i];
        ctx->count++;
        if (ctx->count % 64 == 0) {
            sha1_transform(ctx, ctx->buffer);
        }
    }
}

static void sha1_final(sha1_ctx* ctx, uint8_t* digest) {
    uint64_t bitlen = ctx->count * 8;
    uint8_t padding[64];
    size_t pad_len = 64 - (ctx->count % 64);
    if (pad_len < 9) pad_len += 64;
    padding[0] = 0x80;
    for (size_t i = 1; i < pad_len - 8; ++i) padding[i] = 0;
    for (size_t i = 0; i < 8; ++i) {
        padding[pad_len - 8 + i] = (bitlen >> (56 - i*8)) & 0xFF;
    }
    sha1_update(ctx, padding, pad_len);
    for (int i = 0; i < 5; ++i) {
        digest[i*4]   = (ctx->state[i] >> 24) & 0xFF;
        digest[i*4+1] = (ctx->state[i] >> 16) & 0xFF;
        digest[i*4+2] = (ctx->state[i] >> 8) & 0xFF;
        digest[i*4+3] = ctx->state[i] & 0xFF;
    }
}

static uint8_t* sha1_hash(const uint8_t* data, size_t len) {
    sha1_ctx ctx;
    uint8_t* digest = malloc(20);
    if (!digest) return NULL;
    sha1_init(&ctx);
    sha1_update(&ctx, data, len);
    sha1_final(&ctx, digest);
    return digest;
}

/* --------------------------------------------------------- */
/* Query parameter parsing (stack-based, no malloc) */
typedef struct {
    char key[64];
    char value[256];
} query_param_t;

static size_t percent_decode_to(const char* src, char* dst, size_t dst_size) {
    size_t i = 0, j = 0;
    while (src[i] && j < dst_size - 1) {
        if (src[i] == '%' && src[i+1] && src[i+2]) {
            int hex;
            char hexbuf[3] = {src[i+1], src[i+2], 0};
            if (sscanf(hexbuf, "%x", &hex) == 1) {
                dst[j++] = (char)hex;
                i += 3;
                continue;
            }
        } else if (src[i] == '+') {
            dst[j++] = ' ';
            i++;
            continue;
        }
        dst[j++] = src[i++];
    }
    dst[j] = '\0';
    return j;
}

static size_t parse_query_params(const char* query, query_param_t* params, size_t max_params) {
    if (!query || !*query) return 0;
    size_t count = 0;
    const char* start = query;
    while (*start && count < max_params) {
        const char* end = strchr(start, '&');
        if (!end) end = start + strlen(start);
        const char* eq = memchr(start, '=', end - start);
        if (eq) {
            size_t key_len = eq - start;
            if (key_len >= sizeof(params[count].key)) key_len = sizeof(params[count].key) - 1;
            memcpy(params[count].key, start, key_len);
            params[count].key[key_len] = '\0';
            percent_decode_to(params[count].key, params[count].key, sizeof(params[count].key));

            size_t val_len = end - (eq + 1);
            if (val_len >= sizeof(params[count].value)) val_len = sizeof(params[count].value) - 1;
            memcpy(params[count].value, eq + 1, val_len);
            params[count].value[val_len] = '\0';
            percent_decode_to(params[count].value, params[count].value, sizeof(params[count].value));
        } else {
            size_t key_len = end - start;
            if (key_len >= sizeof(params[count].key)) key_len = sizeof(params[count].key) - 1;
            memcpy(params[count].key, start, key_len);
            params[count].key[key_len] = '\0';
            percent_decode_to(params[count].key, params[count].key, sizeof(params[count].key));
            params[count].value[0] = '\0';
        }
        count++;
        start = (*end) ? end + 1 : end;
    }
    return count;
}

static void split_path_query(const char* full, const char** path, const char** query) {
    const char* q = strchr(full, '?');
    if (q) {
        *path = full;
        *query = q + 1;
    } else {
        *path = full;
        *query = NULL;
    }
}

/* --------------------------------------------------------- */
/* MIME type mapping (fast) */
static const char* mime_type(const char* ext) {
    if (!ext || !*ext) return "application/octet-stream";
    switch (ext[0]) {
        case 'h': if (strcmp(ext, "html") == 0) return "text/html"; break;
        case 'c': if (strcmp(ext, "css") == 0) return "text/css"; break;
        case 'j':
            if (strcmp(ext, "js") == 0) return "application/javascript";
            if (strcmp(ext, "json") == 0) return "application/json";
            if (strcmp(ext, "jpg") == 0 || strcmp(ext, "jpeg") == 0) return "image/jpeg";
            break;
        case 'p':
            if (strcmp(ext, "png") == 0) return "image/png";
            if (strcmp(ext, "pdf") == 0) return "application/pdf";
            break;
        case 'g': if (strcmp(ext, "gif") == 0) return "image/gif"; break;
        case 's':
            if (strcmp(ext, "svg") == 0) return "image/svg+xml";
            break;
        case 'w':
            if (strcmp(ext, "webp") == 0) return "image/webp";
            if (strcmp(ext, "webm") == 0) return "video/webm";
            break;
        case 'm':
            if (strcmp(ext, "mp4") == 0) return "video/mp4";
            break;
        case 'o': if (strcmp(ext, "ogg") == 0) return "video/ogg"; break;
        case 't': if (strcmp(ext, "txt") == 0) return "text/plain"; break;
    }
    return "application/octet-stream";
}

/* --------------------------------------------------------- */
/* Utility: send all data */
static bool send_all(int fd, const void* data, size_t len) {
    const char* ptr = (const char*)data;
    while (len > 0) {
        ssize_t n = write(fd, ptr, len);
        if (n <= 0) {
            if (n == -1 && (errno == EINTR || errno == EAGAIN)) continue;
            return false;
        }
        ptr += n;
        len -= n;
    }
    return true;
}

static size_t extract_header_value(const char* headers, const char* header_name, char* out, size_t out_size) {
    const char* pos = strstr(headers, header_name);
    if (!pos) return 0;
    pos += strlen(header_name);
    while (*pos == ' ' || *pos == '\t') pos++;
    if (*pos != ':') return 0;
    pos++;
    while (*pos == ' ' || *pos == '\t') pos++;
    const char* end = strstr(pos, "\r\n");
    if (!end) return 0;
    size_t len = end - pos;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, pos, len);
    out[len] = '\0';
    while (len > 0 && (out[len-1] == ' ' || out[len-1] == '\t')) out[--len] = '\0';
    return len;
}

static char* websocket_handshake_response(const char* key) {
    static const char* magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    size_t key_len = strlen(key);
    char* concat = malloc(key_len + strlen(magic) + 1);
    if (!concat) return NULL;
    strcpy(concat, key);
    strcat(concat, magic);
    uint8_t* sha = sha1_hash((uint8_t*)concat, key_len + strlen(magic));
    free(concat);
    if (!sha) return NULL;
    char* b64 = base64_encode(sha, 20);
    free(sha);
    if (!b64) return NULL;
    const char* fmt =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n";
    size_t resp_len = strlen(fmt) + strlen(b64) + 1;
    char* resp = malloc(resp_len);
    if (!resp) {
        free(b64);
        return NULL;
    }
    snprintf(resp, resp_len, fmt, b64);
    free(b64);
    return resp;
}

/* --------------------------------------------------------- */
/* WebSocket connection handling */
static void add_to_room(const char* room_name, int fd) {
    pthread_mutex_lock(&g_rooms_mutex);
    room_entry_t* entry = NULL;
    for (size_t i = 0; i < g_room_count; ++i) {
        if (strcmp(g_rooms[i].name, room_name) == 0) {
            entry = &g_rooms[i];
            break;
        }
    }
    if (!entry) {
        if (g_room_count >= g_room_capacity) {
            size_t newcap = g_room_capacity == 0 ? 4 : g_room_capacity * 2;
            room_entry_t* newrooms = realloc(g_rooms, newcap * sizeof(room_entry_t));
            if (!newrooms) { pthread_mutex_unlock(&g_rooms_mutex); return; }
            g_rooms = newrooms;
            g_room_capacity = newcap;
        }
        entry = &g_rooms[g_room_count++];
        entry->name = strdup(room_name);
        entry->room.connections = NULL;
        entry->room.count = 0;
        entry->room.capacity = 0;
    }
    if (entry->room.count >= entry->room.capacity) {
        size_t newcap = entry->room.capacity == 0 ? 4 : entry->room.capacity * 2;
        int* newconn = realloc(entry->room.connections, newcap * sizeof(int));
        if (!newconn) { pthread_mutex_unlock(&g_rooms_mutex); return; }
        entry->room.connections = newconn;
        entry->room.capacity = newcap;
    }
    entry->room.connections[entry->room.count++] = fd;
    printf("WebSocket client connected to room '%s'. Total: %zu\n", room_name, entry->room.count);
    pthread_mutex_unlock(&g_rooms_mutex);
}

static void remove_from_room(const char* room_name, int fd) {
    pthread_mutex_lock(&g_rooms_mutex);
    for (size_t i = 0; i < g_room_count; ++i) {
        if (strcmp(g_rooms[i].name, room_name) == 0) {
            websocket_room_t* room = &g_rooms[i].room;
            size_t before = room->count;
            for (size_t j = 0; j < room->count; ++j) {
                if (room->connections[j] == fd) {
                    room->connections[j] = room->connections[room->count - 1];
                    room->count--;
                    break;
                }
            }
            if (before != room->count)
                printf("WebSocket client disconnected from room '%s'. Total: %zu\n", room_name, room->count);
            if (room->count == 0) {
                free(room->connections);
                room->connections = NULL;
                room->capacity = 0;
                free(g_rooms[i].name);
                memmove(&g_rooms[i], &g_rooms[i+1], (g_room_count - i - 1) * sizeof(room_entry_t));
                g_room_count--;
                printf("Room '%s' removed (empty).\n", room_name);
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_rooms_mutex);
}

static void broadcast_to_room(const char* room_name, const websocket_frame_t* frame, int exclude_fd) {
    uint8_t* data = NULL;
    size_t data_len = 0;
    pthread_mutex_lock(&g_rooms_mutex);
    for (size_t i = 0; i < g_room_count; ++i) {
        if (strcmp(g_rooms[i].name, room_name) == 0) {
            websocket_room_t* room = &g_rooms[i].room;
            if (!data) {
                data = websocket_frame_serialize(frame, &data_len);
                if (!data) break;
            }
            for (size_t j = 0; j < room->count; ++j) {
                int fd = room->connections[j];
                if (fd != exclude_fd) send_all(fd, data, data_len);
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_rooms_mutex);
    free(data);
}

static void handle_websocket(int client_fd, const char* room_name) {
    char* room_dup = strdup(room_name);
    if (!room_dup) return;

    add_to_room(room_name, client_fd);

    uint8_t buffer[CLIENT_REQUEST_BUFFER_SIZE];
    size_t used = 0;

    while (1) {
        if (used < sizeof(buffer)) {
            ssize_t n = read(client_fd, buffer + used, sizeof(buffer) - used);
            if (n <= 0) {
                if (n == -1 && (errno == EINTR || errno == EAGAIN)) continue;
                break;
            }
            used += n;
        }

        size_t offset = 0;
        while (offset < used) {
            websocket_frame_t frame = {0};
            size_t consumed = 0;
            if (!websocket_frame_parse(buffer + offset, used - offset, &frame, &consumed)) {
                if (offset == 0 && used < sizeof(buffer)) break;
                websocket_frame_t close = websocket_frame_create_close(1002, "Protocol error");
                uint8_t* resp = websocket_frame_serialize(&close, &consumed);
                if (resp) { send_all(client_fd, resp, consumed); free(resp); }
                websocket_frame_free(&close);
                goto cleanup;
            }
            offset += consumed;

            switch (frame.opcode) {
                case WS_OPCODE_TEXT:
                case WS_OPCODE_BINARY:
                    broadcast_to_room(room_name, &frame, -1);
                    break;
                case WS_OPCODE_PING: {
                    websocket_frame_t pong = websocket_frame_create_pong(frame.payload, frame.payload_len);
                    size_t plen;
                    uint8_t* resp = websocket_frame_serialize(&pong, &plen);
                    if (resp) { send_all(client_fd, resp, plen); free(resp); }
                    websocket_frame_free(&pong);
                    break;
                }
                case WS_OPCODE_PONG:
                    break;
                case WS_OPCODE_CLOSE: {
                    websocket_frame_t close = websocket_frame_create_close(1000, "");
                    size_t plen;
                    uint8_t* resp = websocket_frame_serialize(&close, &plen);
                    if (resp) { send_all(client_fd, resp, plen); free(resp); }
                    websocket_frame_free(&close);
                    goto cleanup;
                }
                default: {
                    websocket_frame_t close = websocket_frame_create_close(1003, "Fragmentation not supported");
                    size_t plen;
                    uint8_t* resp = websocket_frame_serialize(&close, &plen);
                    if (resp) { send_all(client_fd, resp, plen); free(resp); }
                    websocket_frame_free(&close);
                    goto cleanup;
                }
            }
            websocket_frame_free(&frame);
        }

        if (offset > 0 && offset < used) {
            memmove(buffer, buffer + offset, used - offset);
            used -= offset;
        } else if (offset >= used) {
            used = 0;
        }
    }

cleanup:
    remove_from_room(room_name, client_fd);
    free(room_dup);
    close(client_fd);
}

/* --------------------------------------------------------- */
/* Static file serving with sendfile */
static bool try_serve_static_file(int client_fd, const char* path, size_t end_of_headers) {
    (void)end_of_headers;

    char decoded[PATH_MAX];
    percent_decode_to(path, decoded, sizeof(decoded));
    char* file_path = decoded;
    if (*file_path == '/') file_path++;
    if (*file_path == '\0') return false;

    char full_path[PATH_MAX];
    snprintf(full_path, sizeof(full_path), "%s/%s", PUBLIC_DIR, file_path);

    char public_resolved[PATH_MAX];
    if (!realpath(PUBLIC_DIR, public_resolved)) return false;

    char file_resolved[PATH_MAX];
    if (!realpath(full_path, file_resolved)) {
        const char* resp = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 9\r\nConnection: keep-alive\r\n\r\nNot Found";
        send_all(client_fd, resp, strlen(resp));
        return true;
    }

    if (strncmp(file_resolved, public_resolved, strlen(public_resolved)) != 0) {
        const char* resp = "HTTP/1.1 403 Forbidden\r\nContent-Type: text/plain\r\nContent-Length: 9\r\nConnection: keep-alive\r\n\r\nForbidden";
        send_all(client_fd, resp, strlen(resp));
        return true;
    }

    struct stat st;
    if (stat(file_resolved, &st) != 0 || !S_ISREG(st.st_mode)) {
        const char* resp = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 9\r\nConnection: keep-alive\r\n\r\nNot Found";
        send_all(client_fd, resp, strlen(resp));
        return true;
    }

    const char* ext = "";
    char* dot = strrchr(file_path, '.');
    if (dot) ext = dot + 1;
    const char* content_type = mime_type(ext);

    char header[512];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        content_type, (long)st.st_size);

    if (!send_all(client_fd, header, header_len)) return true;

    int file_fd = open(file_resolved, O_RDONLY);
    if (file_fd == -1) return true;

    off_t offset = 0;
    size_t remaining = st.st_size;
    while (remaining > 0) {
        ssize_t sent = sendfile(client_fd, file_fd, &offset, remaining);
        if (sent <= 0) break;
        remaining -= sent;
    }
    close(file_fd);
    return true;
}

/* --------------------------------------------------------- */
/* HTTP server */
typedef struct {
    int server_fd;
    int port;
    atomic_bool running;
    thread_pool_t* pool;
} http_server_t;

static const char home_response[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 4\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "home";

static const char json_response[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 60\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "{\"string\":\"string\",\"decimal\":3.14,\"round\":69,\"boolean\":true}";

static const char not_found_response[] =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 9\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "Not Found";

static const char method_not_allowed_response[] =
    "HTTP/1.1 405 Method Not Allowed\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 18\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "Method Not Allowed";

static const char bad_request_response[] =
    "HTTP/1.1 400 Bad Request\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 11\r\n"
    "Connection: close\r\n"
    "\r\n"
    "Bad Request";

static void handle_client(void* arg) {
    int client_fd = *(int*)arg;
    free(arg);

    uint8_t buffer[CLIENT_REQUEST_BUFFER_SIZE];
    size_t used = 0;

    while (1) {
        if (used < sizeof(buffer) - 1) {
            ssize_t n = read(client_fd, buffer + used, sizeof(buffer) - 1 - used);
            if (n <= 0) {
                if (n == -1 && (errno == EINTR || errno == EAGAIN)) continue;
                close(client_fd);
                return;
            }
            used += n;
            buffer[used] = '\0';
        }

        uint8_t* headers_end = (uint8_t*)strstr((char*)buffer, "\r\n\r\n");
        if (!headers_end) {
            if (used >= sizeof(buffer) - 1) { close(client_fd); return; }
            continue;
        }
        size_t end_of_headers = headers_end - buffer;

        uint8_t* line_end = (uint8_t*)strstr((char*)buffer, "\r\n");
        if (!line_end || line_end > headers_end) { close(client_fd); return; }
        *line_end = '\0';
        char* method = (char*)buffer;
        char* space = strchr(method, ' ');
        if (!space) goto bad_request;
        *space = '\0';
        char* path = space + 1;
        space = strchr(path, ' ');
        if (!space) goto bad_request;
        *space = '\0';

        const char* path_str;
        const char* query_str;
        split_path_query(path, &path_str, &query_str);
        query_param_t query_params[16];
        size_t query_count = 0;
        if (query_str) {
            query_count = parse_query_params(query_str, query_params, 16);
        }

        char upgrade_val[32] = {0};
        char connection_val[32] = {0};
        char sec_key_val[64] = {0};
        bool is_websocket_upgrade = false;

        if (strcmp(method, "GET") == 0 && strncmp(path_str, "/chat/", 6) == 0 && strlen(path_str) > 6) {
            extract_header_value((char*)buffer, "Upgrade:", upgrade_val, sizeof(upgrade_val));
            extract_header_value((char*)buffer, "Connection:", connection_val, sizeof(connection_val));
            extract_header_value((char*)buffer, "Sec-WebSocket-Key:", sec_key_val, sizeof(sec_key_val));
            if (upgrade_val[0] && connection_val[0] && sec_key_val[0] &&
                strstr(upgrade_val, "websocket") && strstr(connection_val, "Upgrade")) {
                is_websocket_upgrade = true;
            }
        }

        if (is_websocket_upgrade) {
            const char* room_name = path_str + 6;
            if (*room_name == '\0') {
                send_all(client_fd, bad_request_response, strlen(bad_request_response));
                close(client_fd);
                return;
            }
            char* handshake = websocket_handshake_response(sec_key_val);
            if (handshake) {
                send_all(client_fd, handshake, strlen(handshake));
                free(handshake);
                handle_websocket(client_fd, room_name);
            } else {
                close(client_fd);
            }
            return;
        }

        // Predefined responses (single buffer)
        if (strcmp(method, "GET") != 0) {
            send_all(client_fd, method_not_allowed_response, strlen(method_not_allowed_response));
        } else if (strcmp(path_str, "/c") == 0) {
            send_all(client_fd, home_response, strlen(home_response));
        } else if (strcmp(path_str, "/c/json") == 0) {
            send_all(client_fd, json_response, strlen(json_response));
        } else if (strcmp(path_str, "/c/echo") == 0) {
            char* text_val = NULL;
            for (size_t i = 0; i < query_count; ++i) {
                if (strcmp(query_params[i].key, "text") == 0) {
                    text_val = query_params[i].value;
                    break;
                }
            }
            char body[4096];
            int body_len;
            if (text_val) {
                body_len = snprintf(body, sizeof(body), "Echo: %s", text_val);
            } else {
                size_t pos = 0;
                pos += snprintf(body + pos, sizeof(body) - pos, "{");
                for (size_t i = 0; i < query_count; ++i) {
                    if (i > 0) pos += snprintf(body + pos, sizeof(body) - pos, ",");
                    pos += snprintf(body + pos, sizeof(body) - pos,
                                    "\"%s\":\"%s\"", query_params[i].key, query_params[i].value);
                }
                pos += snprintf(body + pos, sizeof(body) - pos, "}");
                body_len = pos;
            }
            char header[512];
            int header_len = snprintf(header, sizeof(header),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %d\r\n"
                "Connection: keep-alive\r\n"
                "\r\n", body_len);
            struct iovec iov[2] = {
                { .iov_base = header, .iov_len = header_len },
                { .iov_base = body, .iov_len = body_len }
            };
            writev(client_fd, iov, 2);
        } else if (strncmp(path_str, "/c/", 4) == 0 && strcmp(path_str, "/c/json") != 0 && strncmp(path_str, "/c/json/", 9) != 0) {
            const char* sub = path_str + 4;
            char body[256];
            int body_len;
            if (*sub == '\0')
                body_len = snprintf(body, sizeof(body), "value path: /c/");
            else
                body_len = snprintf(body, sizeof(body), "value path: /c/%s", sub);
            char header[512];
            int header_len = snprintf(header, sizeof(header),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: %d\r\n"
                "Connection: keep-alive\r\n"
                "\r\n", body_len);
            struct iovec iov[2] = {
                { .iov_base = header, .iov_len = header_len },
                { .iov_base = body, .iov_len = body_len }
            };
            writev(client_fd, iov, 2);
        } else {
            if (try_serve_static_file(client_fd, path_str, end_of_headers)) {
                // response already sent
            } else {
                send_all(client_fd, not_found_response, strlen(not_found_response));
            }
        }

        size_t consumed = end_of_headers + 4;
        if (consumed < used) {
            memmove(buffer, buffer + consumed, used - consumed);
            used -= consumed;
        } else {
            used = 0;
        }
        continue;

    bad_request:
        send_all(client_fd, bad_request_response, strlen(bad_request_response));
        close(client_fd);
        return;
    }
}

static http_server_t* http_server_create(int port, size_t concurrency) {
    http_server_t* srv = malloc(sizeof(http_server_t));
    if (!srv) return NULL;
    srv->port = port;
    atomic_init(&srv->running, false);

    // Determine thread count: min(concurrency, TARGET), then max with hardware cores
    size_t thread_count = concurrency;
    if (thread_count > CONNECTION_CONCURRENT_TARGET)
        thread_count = CONNECTION_CONCURRENT_TARGET;
    long hw = sysconf(_SC_NPROCESSORS_ONLN);
    if (hw > 0 && (size_t)hw > thread_count)
        thread_count = hw;

    srv->pool = thread_pool_create(thread_count);
    if (!srv->pool) {
        free(srv);
        return NULL;
    }
    srv->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->server_fd == -1) {
        thread_pool_destroy(srv->pool);
        free(srv);
        return NULL;
    }
    int opt = 1;
    setsockopt(srv->server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    return srv;
}

static void http_server_start(http_server_t* srv) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(ADDRESS_IP);
    addr.sin_port = htons(srv->port);

    if (bind(srv->server_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("bind");
        return;
    }
    if (listen(srv->server_fd, 1024) == -1) {
        perror("listen");
        return;
    }

    printf("backend_c: run on %s:%d\n", ADDRESS_IP, ADDRESS_PORT);
    printf("  - HTTP endpoint: /c\n");
    printf("  - JSON endpoint: /c/json\n");
    printf("  - Echo endpoint: /c/echo?text=hello\n");
    printf("  - Dynamic path: /c/{path1}/{path2}/...\n");
    printf("  - WebSocket endpoint: /chat/{room_name}\n");
    printf("  - Static files from ./public at root\n");
    // printf("  - Thread pool size: %zu\n", srv->pool->thread_count);

    atomic_store(&srv->running, true);
    while (atomic_load(&srv->running)) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(srv->server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd == -1) {
            if (!atomic_load(&srv->running)) break;
            continue;
        }
        int flag = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        int* fd_ptr = malloc(sizeof(int));
        if (fd_ptr) {
            *fd_ptr = client_fd;
            thread_pool_enqueue(srv->pool, handle_client, fd_ptr);
        } else {
            close(client_fd);
        }
    }
}

static void http_server_stop(http_server_t* srv) {
    if (!srv) return;
    atomic_store(&srv->running, false);
    shutdown(srv->server_fd, SHUT_RDWR);
    close(srv->server_fd);
    thread_pool_destroy(srv->pool);
    free(srv);
}

/* --------------------------------------------------------- */
int main(void) {
    ignore_sigpipe();
    http_server_t* srv = http_server_create(ADDRESS_PORT, CONNECTION_CONCURRENT_TARGET);
    if (!srv) {
        fprintf(stderr, "Failed to create server\n");
        return 1;
    }
    http_server_start(srv);
    http_server_stop(srv);
    return 0;
}

/*
IMPORTANT:
this implementation result:
➜ wrk -c 100 -t 6 -d 10s http://localhost:9001/c
Running 10s test @ http://localhost:9001/c
  6 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   265.59us  535.88us   5.63ms   91.51%
    Req/Sec    80.07k     5.44k   93.02k    69.26%
  4819780 requests in 10.10s, 422.88MB read
Requests/sec: 477214.78
Transfer/sec:     41.87MB
*/

