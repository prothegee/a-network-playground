#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <cstring>
#include <thread>
#include <atomic>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <csignal>
#include <cerrno>
#include <string_view>
#include <algorithm>
#include <array>

#define ADDRESS_IP "0.0.0.0"
#define ADDRESS_PORT 9002

#define CONNECTIO_TIMEOUT false
#define CONNECTION_MAX_PER_IP 1'000'000
#define CONNECTION_TIMEOUT_SEC 6
#define CONNECTION_CONCURRENT_TARGET 256

#define CLIENT_REQUEST_RETRY 6
#define CLIENT_REQUEST_RETRY_SLEEP 100 // in µs
#define CLIENT_REQUEST_BUFFER_SIZE 8192 // in bytes

// --------------------------------------------------------- //

/*
TODO:
[X] multi-threaded http server
[X] threadpool for scalability
[X] blocking I/O (not epoll)
[X] keep-alive (manual)
[X] websocket support
[?] not full http spec
[?] not async / non-blocking server
*/

// --------------------------------------------------------- //
// Global WebSocket connections (like Drogon's g_connections)
static std::vector<int> g_websocket_connections;
static std::mutex g_connections_mutex;

// --------------------------------------------------------- //
class SignalHandler {
public:
    SignalHandler() {
        std::signal(SIGPIPE, SIG_IGN);
    }
} sigpipe_handler;

// --------------------------------------------------------- //

class ThreadPool {
private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    std::atomic<bool> stop;
    
public:
    ThreadPool(size_t threads) : stop(false) {
        for (size_t i = 0; i < threads; ++i) {
            workers.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this] {
                            return this->stop || !this->tasks.empty();
                        });
                        if (this->stop && this->tasks.empty()) {
                            return;
                        }
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    try {
                        task();
                    } catch (...) {
                        // prevent worker thread termination
                    }
                }
            });
        }
    }
    
    template<class F>
    void enqueue(F&& f) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            if (stop) {
                throw std::runtime_error("enqueue on stopped ThreadPool");
            }
            tasks.emplace(std::forward<F>(f));
        }
        condition.notify_one();
    }
    
    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for (std::thread &worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }
};

// --------------------------------------------------------- //
// WebSocket Frame Structure (RFC 6455)
class WebSocketFrame {
public:
    enum Opcode : uint8_t {
        CONTINUATION = 0x0,
        TEXT = 0x1,
        BINARY = 0x2,
        CLOSE = 0x8,
        PING = 0x9,
        PONG = 0xA
    };
    
    bool fin;
    bool mask;
    Opcode opcode;
    uint64_t payload_length;
    std::array<uint8_t, 4> masking_key;
    std::vector<uint8_t> payload;
    
    WebSocketFrame() : fin(true), mask(false), opcode(TEXT), payload_length(0) {}
    
    // Parse WebSocket frame from buffer
    static bool parse(const uint8_t* data, size_t len, WebSocketFrame& frame, size_t& consumed) {
        if (len < 2) return false;
        
        const uint8_t* ptr = data;
        
        // First byte: FIN + RSV + Opcode
        frame.fin = (*ptr & 0x80) != 0;
        frame.opcode = static_cast<Opcode>(*ptr & 0x0F);
        ptr++;
        
        // Second byte: MASK + Payload length
        frame.mask = (*ptr & 0x80) != 0;
        uint8_t payload_len_indicator = *ptr & 0x7F;
        ptr++;
        
        // Extended payload length
        if (payload_len_indicator == 126) {
            if (len < static_cast<size_t>(ptr - data) + 2) return false;
            frame.payload_length = (static_cast<uint64_t>(ptr[0]) << 8) | ptr[1];
            ptr += 2;
        } else if (payload_len_indicator == 127) {
            if (len < static_cast<size_t>(ptr - data) + 8) return false;
            frame.payload_length = 0;
            for (int i = 0; i < 8; i++) {
                frame.payload_length = (frame.payload_length << 8) | ptr[i];
            }
            ptr += 8;
        } else {
            frame.payload_length = payload_len_indicator;
        }
        
        // Masking key
        if (frame.mask) {
            if (len < static_cast<size_t>(ptr - data) + 4) return false;
            std::copy(ptr, ptr + 4, frame.masking_key.begin());
            ptr += 4;
        }
        
        // Payload
        size_t payload_offset = static_cast<size_t>(ptr - data);
        size_t total_frame_size = payload_offset + frame.payload_length;
        
        if (len < total_frame_size) return false;
        
        frame.payload.resize(frame.payload_length);
        if (frame.payload_length > 0) {
            std::copy(ptr, ptr + frame.payload_length, frame.payload.begin());
            
            // Unmask payload if masked
            if (frame.mask) {
                for (size_t i = 0; i < frame.payload_length; i++) {
                    frame.payload[i] ^= frame.masking_key[i % 4];
                }
            }
        }
        
        consumed = total_frame_size;
        return true;
    }
    
    // Serialize WebSocket frame to buffer
    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buffer;
        
        // First byte: FIN + Opcode
        uint8_t first_byte = (fin ? 0x80 : 0x00) | static_cast<uint8_t>(opcode);
        buffer.push_back(first_byte);
        
        // Second byte and extended length: Payload length (no mask for server->client)
        if (payload_length <= 125) {
            buffer.push_back(static_cast<uint8_t>(payload_length));
        } else if (payload_length <= 65535) {
            buffer.push_back(126);
            buffer.push_back(static_cast<uint8_t>((payload_length >> 8) & 0xFF));
            buffer.push_back(static_cast<uint8_t>(payload_length & 0xFF));
        } else {
            buffer.push_back(127);
            for (int i = 7; i >= 0; i--) {
                buffer.push_back(static_cast<uint8_t>((payload_length >> (i * 8)) & 0xFF));
            }
        }
        
        // Payload
        buffer.insert(buffer.end(), payload.begin(), payload.end());
        
        return buffer;
    }
    
    // Create text frame
    static WebSocketFrame create_text_frame(const std::string& text) {
        WebSocketFrame frame;
        frame.opcode = TEXT;
        frame.payload.assign(text.begin(), text.end());
        frame.payload_length = text.length();
        return frame;
    }
    
    // Create ping frame
    static WebSocketFrame create_ping_frame() {
        WebSocketFrame frame;
        frame.opcode = PING;
        return frame;
    }
    
    // Create pong frame
    static WebSocketFrame create_pong_frame() {
        WebSocketFrame frame;
        frame.opcode = PONG;
        return frame;
    }
    
    // Create close frame
    static WebSocketFrame create_close_frame(uint16_t code = 1000, const std::string& reason = "") {
        WebSocketFrame frame;
        frame.opcode = CLOSE;
        
        // Close frame payload: 2-byte code + optional reason
        frame.payload.resize(2 + reason.length());
        frame.payload[0] = (code >> 8) & 0xFF;
        frame.payload[1] = code & 0xFF;
        
        if (!reason.empty()) {
            std::copy(reason.begin(), reason.end(), frame.payload.begin() + 2);
        }
        
        frame.payload_length = frame.payload.size();
        return frame;
    }
};

// --------------------------------------------------------- //
// Base64 encoding for WebSocket accept key
class Base64Encoder {
public:
    static std::string encode(const std::string& data) {
        static const char* encoding_table = 
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        
        size_t data_len = data.length();
        size_t output_len = 4 * ((data_len + 2) / 3);
        std::string output(output_len, ' ');
        
        size_t i = 0, j = 0;
        while (i < data_len) {
            uint32_t octet_a = i < data_len ? static_cast<uint8_t>(data[i++]) : 0;
            uint32_t octet_b = i < data_len ? static_cast<uint8_t>(data[i++]) : 0;
            uint32_t octet_c = i < data_len ? static_cast<uint8_t>(data[i++]) : 0;
            
            uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;
            
            output[j++] = encoding_table[(triple >> 18) & 0x3F];
            output[j++] = encoding_table[(triple >> 12) & 0x3F];
            output[j++] = encoding_table[(triple >> 6) & 0x3F];
            output[j++] = encoding_table[triple & 0x3F];
        }
        
        // Handle padding
        size_t mod_table[] = {0, 2, 1};
        size_t padding = mod_table[data_len % 3];
        for (size_t i = 0; i < padding; i++) {
            output[output_len - 1 - i] = '=';
        }
        
        return output;
    }
};

// --------------------------------------------------------- //
// SHA-1 implementation for WebSocket handshake
class SHA1 {
private:
    uint32_t state[5];
    uint64_t count;
    uint8_t buffer[64];
    
    void transform(const uint8_t* block) {
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
        uint32_t w[80];
        
        for (int i = 0; i < 16; i++) {
            w[i] = (block[i * 4 + 0] << 24) | (block[i * 4 + 1] << 16) |
                   (block[i * 4 + 2] << 8) | (block[i * 4 + 3]);
        }
        
        for (int i = 16; i < 80; i++) {
            w[i] = (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]);
            w[i] = (w[i] << 1) | (w[i] >> 31);
        }
        
        for (int i = 0; i < 80; i++) {
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
        
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
    }
    
public:
    SHA1() {
        reset();
    }
    
    void reset() {
        state[0] = 0x67452301;
        state[1] = 0xEFCDAB89;
        state[2] = 0x98BADCFE;
        state[3] = 0x10325476;
        state[4] = 0xC3D2E1F0;
        count = 0;
    }
    
    void update(const uint8_t* data, size_t len) {
        for (size_t i = 0; i < len; i++) {
            buffer[count % 64] = data[i];
            count++;
            
            if (count % 64 == 0) {
                transform(buffer);
            }
        }
    }
    
    void finalize(uint8_t* digest) {
        uint8_t padding[64];
        size_t padding_len = 64 - (count % 64);
        if (padding_len < 9) padding_len += 64;
        
        padding[0] = 0x80;
        for (size_t i = 1; i < padding_len - 8; i++) {
            padding[i] = 0;
        }
        
        // Append bit length (in bits)
        uint64_t bitlen = count * 8;
        for (size_t i = 0; i < 8; i++) {
            padding[padding_len - 8 + i] = (bitlen >> (56 - i * 8)) & 0xFF;
        }
        
        update(padding, padding_len);
        
        for (int i = 0; i < 5; i++) {
            digest[i * 4 + 0] = (state[i] >> 24) & 0xFF;
            digest[i * 4 + 1] = (state[i] >> 16) & 0xFF;
            digest[i * 4 + 2] = (state[i] >> 8) & 0xFF;
            digest[i * 4 + 3] = state[i] & 0xFF;
        }
    }
    
    static std::string hash(const std::string& input) {
        SHA1 sha1;
        sha1.update(reinterpret_cast<const uint8_t*>(input.data()), input.length());
        
        uint8_t digest[20];
        sha1.finalize(digest);
        
        std::string result;
        result.reserve(20);
        result.assign(reinterpret_cast<char*>(digest), 20);
        return result;
    }
};

// --------------------------------------------------------- //

class HttpServer {
private:
    int server_fd;
    int port;
    std::atomic<bool> running;
    ThreadPool thread_pool;
    
    const std::string home_response = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 4\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "home";
    
    const std::string not_found_response = 
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 9\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "Not Found";
    
    const std::string method_not_allowed_response = 
        "HTTP/1.1 405 Method Not Allowed\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 18\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "Method Not Allowed";
    
    const std::string bad_request_response = 
        "HTTP/1.1 400 Bad Request\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 11\r\n"
        "Connection: close\r\n"
        "\r\n"
        "Bad Request";
    
    static bool send_all(int fd, const char* data, size_t len) {
        int retry_count = 0;
        while (len > 0 && retry_count < CLIENT_REQUEST_RETRY) {
            ssize_t written = write(fd, data, len);
            if (written <= 0) {
                if (written == -1) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        retry_count++;
                        usleep(CLIENT_REQUEST_RETRY_SLEEP);
                        continue;
                    }
                }
                return false;
            }
            data += written;
            len -= written;
            retry_count = 0;
        }
        return (len == 0);
    }
    
    // WebSocket handshake response
    std::string websocket_handshake_response(const std::string& sec_websocket_key) {
        std::string accept_key = sec_websocket_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        std::string sha1_hash = SHA1::hash(accept_key);
        std::string base64_accept = Base64Encoder::encode(sha1_hash);
        
        return "HTTP/1.1 101 Switching Protocols\r\n"
               "Upgrade: websocket\r\n"
               "Connection: Upgrade\r\n"
               "Sec-WebSocket-Accept: " + base64_accept + "\r\n"
               "\r\n";
    }

    // Handle WebSocket connection
    void handle_websocket(int client_fd) {
        struct SocketCloser {
            int fd;
            explicit SocketCloser(int fd_) : fd(fd_) {}
            // E.Q.: drogon ws handleConnectionClosed
            ~SocketCloser() { 
                if (fd != -1) {
                    // Remove from global connections
                    {
                        std::lock_guard<std::mutex> lock(g_connections_mutex);
                        auto it = std::find(g_websocket_connections.begin(), 
                                          g_websocket_connections.end(), fd);
                        if (it != g_websocket_connections.end()) {
                            g_websocket_connections.erase(it);
                            std::cout << "WebSocket connection closed: " << fd << "\n";
                        }
                    }
                    close(fd);
                }
            }
            SocketCloser(const SocketCloser&) = delete;
            SocketCloser& operator=(const SocketCloser&) = delete;
        } closer(client_fd);

        // Register connection (E.Q.: drogon ws handleNewConnection)
        {
            std::lock_guard<std::mutex> lock(g_connections_mutex);
            g_websocket_connections.push_back(client_fd);
            std::cout << "WebSocket connection established: " << client_fd << "\n";
        }
        
        char buffer[CLIENT_REQUEST_BUFFER_SIZE];
        size_t buffer_used = 0;
        
        // Read WebSocket frames
        while (true) {
            if (buffer_used < sizeof(buffer) - 1) {
                ssize_t bytes_read = read(client_fd, buffer + buffer_used, sizeof(buffer) - 1 - buffer_used);
                if (bytes_read <= 0) {
                    if (bytes_read == -1) {
                        if (errno == EINTR) continue;
                        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                    }
                    return;
                }
                buffer_used += bytes_read;
            }
            
            // Parse WebSocket frames
            size_t offset = 0;
            while (offset < buffer_used) {
                WebSocketFrame frame;
                size_t consumed = 0;
                
                if (!WebSocketFrame::parse(reinterpret_cast<const uint8_t*>(buffer + offset), 
                                          buffer_used - offset, frame, consumed)) {
                    // Incomplete frame, wait for more data
                    if (offset == 0 && buffer_used < sizeof(buffer) - 1) {
                        break;
                    }
                    // Malformed frame
                    WebSocketFrame close_frame = WebSocketFrame::create_close_frame(1002, "Protocol error");
                    auto response = close_frame.serialize();
                    send_all(client_fd, reinterpret_cast<const char*>(response.data()), response.size());
                    return;
                }
                
                offset += consumed;
                
                // Handle different opcodes
                switch (frame.opcode) {
                    case WebSocketFrame::TEXT:
                    case WebSocketFrame::BINARY: {
                        // Broadcast to all connections (E.Q.: drogon ws handleNewMessage)
                        {
                            std::lock_guard<std::mutex> lock(g_connections_mutex);
                            for (int conn_fd : g_websocket_connections) {
                                WebSocketFrame broadcast_frame;
                                broadcast_frame.opcode = frame.opcode;
                                broadcast_frame.payload = frame.payload;
                                broadcast_frame.payload_length = frame.payload_length;
                                
                                auto response = broadcast_frame.serialize();
                                send_all(conn_fd, reinterpret_cast<const char*>(response.data()), response.size());
                            }
                        }
                        break;
                    }
                    
                    case WebSocketFrame::PING: {
                        // Respond with PONG
                        WebSocketFrame pong_frame = WebSocketFrame::create_pong_frame();
                        if (!frame.payload.empty()) {
                            pong_frame.payload = frame.payload;
                            pong_frame.payload_length = frame.payload_length;
                        }
                        
                        auto response = pong_frame.serialize();
                        send_all(client_fd, reinterpret_cast<const char*>(response.data()), response.size());
                        break;
                    }
                    
                    case WebSocketFrame::PONG: {
                        // Ignore PONG frames
                        break;
                    }
                    
                    case WebSocketFrame::CLOSE: {
                        // Send close frame back and close connection
                        WebSocketFrame close_frame = WebSocketFrame::create_close_frame();
                        auto response = close_frame.serialize();
                        send_all(client_fd, reinterpret_cast<const char*>(response.data()), response.size());
                        return;
                    }
                    
                    case WebSocketFrame::CONTINUATION: {
                        // Not handling fragmented messages for simplicity
                        WebSocketFrame close_frame = WebSocketFrame::create_close_frame(1003, "Fragmentation not supported");
                        auto response = close_frame.serialize();
                        send_all(client_fd, reinterpret_cast<const char*>(response.data()), response.size());
                        return;
                    }
                }
            }
            
            // Remove processed data from buffer
            if (offset > 0 && offset < buffer_used) {
                memmove(buffer, buffer + offset, buffer_used - offset);
                buffer_used -= offset;
            } else if (offset >= buffer_used) {
                buffer_used = 0;
            }
        }
    }
    
public:
    HttpServer(int port_num, size_t concurrency_target = 128)
        : port(port_num), 
          running(true),
          thread_pool(std::max(
              std::min(concurrency_target, size_t(CONNECTION_CONCURRENT_TARGET)),
              size_t(std::thread::hardware_concurrency())
          )) {
        
        server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd == -1) {
            throw std::runtime_error("Failed to create socket: " + std::string(strerror(errno)));
        }
        
        int opt = 1;
        if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
            close(server_fd);
            throw std::runtime_error("Failed to set SO_REUSEADDR: " + std::string(strerror(errno)));
        }
    }
    
    ~HttpServer() {
        stop();
    }
    
    void start() {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = inet_addr(ADDRESS_IP);
        address.sin_port = htons(port);
        
        if (bind(server_fd, (sockaddr*)&address, sizeof(address)) == -1) {
            throw std::runtime_error("Failed to bind socket: " + std::string(strerror(errno)));
        }
        
        if (listen(server_fd, 1024) == -1) {
            throw std::runtime_error("Failed to listen on socket: " + std::string(strerror(errno)));
        }
        
        std::cout << "backend_cc: run on " << ADDRESS_IP << ":" << ADDRESS_PORT << "\n";
        std::cout << "  - HTTP endpoint: /cc\n";
        std::cout << "  - WebSocket endpoint: /chat\n";
        
        while (running.load(std::memory_order_acquire)) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            
            int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
            
            if (client_fd == -1) {
                if (!running.load(std::memory_order_acquire)) break;
                if (errno == EINTR) continue;
                if (errno == ECONNABORTED || errno == EPROTO) continue;
                continue;
            }
            
        #if CONNECTIO_TIMEOUT
            struct timeval timeout;
            timeout.tv_sec = CONNECTION_TIMEOUT_SEC;
            timeout.tv_usec = 0;
            setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        #endif

            int flag = 1;
            setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
            
            try {
                thread_pool.enqueue([this, client_fd]() {
                    try {
                        handle_client(client_fd);
                    } catch (...) {
                        close(client_fd);
                    }
                });
            } catch (...) {
                close(client_fd);
            }
        }
    }
    
    void stop() {
        if (!running.exchange(false, std::memory_order_release)) return;
        
        // Close all WebSocket connections
        {
            std::lock_guard<std::mutex> lock(g_connections_mutex);
            for (int fd : g_websocket_connections) {
                shutdown(fd, SHUT_RDWR);
                close(fd);
            }
            g_websocket_connections.clear();
        }
        
        if (server_fd != -1) {
            shutdown(server_fd, SHUT_RDWR);
            close(server_fd);
            server_fd = -1;
        }
    }
    
private:
    size_t thread_pool_size() const {
        return std::thread::hardware_concurrency();
    }
    
    std::string extract_header_value(const std::string& headers, const std::string& header_name) {
        size_t pos = headers.find(header_name);
        if (pos == std::string::npos) return "";
        
        pos += header_name.length();
        size_t end = headers.find("\r\n", pos);
        if (end == std::string::npos) return "";
        
        std::string value = headers.substr(pos, end - pos);
        
        // Trim leading/trailing whitespace and colon
        value.erase(0, value.find_first_not_of(" \t:"));
        value.erase(value.find_last_not_of(" \t") + 1);
        
        return value;
    }
    
    void handle_client(int client_fd) {
        struct SocketCloser {
            int fd;
            explicit SocketCloser(int fd_) : fd(fd_) {}
            ~SocketCloser() { if (fd != -1) close(fd); }
            SocketCloser(const SocketCloser&) = delete;
            SocketCloser& operator=(const SocketCloser&) = delete;
        } closer(client_fd);
        
        uint64_t request_count = 0;
        char buffer[CLIENT_REQUEST_BUFFER_SIZE];
        size_t buffer_used = 0;
        
        while (request_count < CONNECTION_MAX_PER_IP) {
            if (buffer_used < sizeof(buffer) - 1) {
                ssize_t bytes_read = read(client_fd, buffer + buffer_used, sizeof(buffer) - 1 - buffer_used);
                if (bytes_read <= 0) {
                    if (bytes_read == -1) {
                        if (errno == EINTR) continue;
                        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                    }
                    return;
                }
                buffer_used += bytes_read;
                buffer[buffer_used] = '\0';
            }
            
            std::string_view full_buffer(buffer, buffer_used);
            size_t end_of_headers = full_buffer.find("\r\n\r\n");
            if (end_of_headers == std::string_view::npos) {
                if (buffer_used >= sizeof(buffer) - 1) return;
                continue;
            }
            
            size_t end_of_line = full_buffer.find("\r\n");
            if (end_of_line == std::string_view::npos || end_of_line > end_of_headers) {
                return;
            }
            
            std::string_view request_line = full_buffer.substr(0, end_of_line);
            size_t first_space = request_line.find(' ');
            if (first_space == std::string_view::npos) {
                send_all(client_fd, not_found_response.data(), not_found_response.size());
                buffer_used = 0;
                request_count++;
                continue;
            }
            
            std::string_view method = request_line.substr(0, first_space);
            size_t second_space = request_line.find(' ', first_space + 1);
            if (second_space == std::string_view::npos || second_space > end_of_line) {
                send_all(client_fd, not_found_response.data(), not_found_response.size());
                buffer_used = 0;
                request_count++;
                continue;
            }
            
            std::string_view path = request_line.substr(first_space + 1, second_space - first_space - 1);
            size_t query_pos = path.find('?');
            if (query_pos != std::string_view::npos) {
                path = path.substr(0, query_pos);
            }
            
            // WebSocket handshake check
            if (method == "GET" && path == "/chat") {
                // Extract headers to check for WebSocket upgrade
                std::string headers(full_buffer.data(), end_of_headers + 2);
                
                std::string upgrade = extract_header_value(headers, "Upgrade:");
                std::string connection = extract_header_value(headers, "Connection:");
                std::string sec_websocket_key = extract_header_value(headers, "Sec-WebSocket-Key:");
                
                // Check if this is a WebSocket upgrade request
                if (!upgrade.empty() && !connection.empty() && !sec_websocket_key.empty()) {
                    bool is_websocket = (upgrade.find("websocket") != std::string::npos || 
                                       upgrade.find("WebSocket") != std::string::npos);
                    bool is_upgrade = (connection.find("Upgrade") != std::string::npos ||
                                     connection.find("upgrade") != std::string::npos);
                    
                    if (is_websocket && is_upgrade && !sec_websocket_key.empty()) {
                        // Perform WebSocket handshake
                        std::string handshake_response = websocket_handshake_response(sec_websocket_key);
                        send_all(client_fd, handshake_response.c_str(), handshake_response.length());
                        
                        // Switch to WebSocket mode
                        // E.Q.: drogon ws handleNewConnection
                        handle_websocket(client_fd);
                        return;
                    }
                }
                
                // Not a WebSocket request, fall through to normal HTTP
            }
            
            const char* response = nullptr;
            size_t response_len = 0;
            
            if (method != "GET") {
                response = method_not_allowed_response.data();
                response_len = method_not_allowed_response.size();
            } else if (path == "/cc") {
                response = home_response.data();
                response_len = home_response.size();
            } else if (path == "/chat") {
                // WebSocket endpoint accessed via regular HTTP
                response = not_found_response.data();
                response_len = not_found_response.size();
            } else {
                response = not_found_response.data();
                response_len = not_found_response.size();
            }
            
            struct iovec iov;
            iov.iov_base = const_cast<char*>(response);
            iov.iov_len = response_len;
            
            ssize_t written = writev(client_fd, &iov, 1);
            if (written != static_cast<ssize_t>(response_len)) {
                if (written == -1 && (errno == EINTR || errno == EAGAIN)) {
                    writev(client_fd, &iov, 1);
                }
                return;
            }
            
            size_t consumed = end_of_headers + 4;
            if (consumed < buffer_used) {
                memmove(buffer, buffer + consumed, buffer_used - consumed);
                buffer_used = buffer_used - consumed;
            } else {
                buffer_used = 0;
            }
            
            request_count++;
        }
    }
};

// --------------------------------------------------------- //

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    
    try {
        HttpServer server(ADDRESS_PORT, CONNECTION_CONCURRENT_TARGET);
        server.start();
    } catch (const std::exception& e) {
        std::cerr << "server error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
