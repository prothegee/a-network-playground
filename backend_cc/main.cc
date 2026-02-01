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

#define ADDRESS_IP "0.0.0.0"
#define ADDRESS_PORT 9002

#define CONNECTIO_TIMEOUT false
#define CONNECTION_MAX_PER_IP 1'000'000
#define CONNECTION_TIMEOUT_SEC 6
#define CONNECTION_CONCURRENT_TARGET 256

#define CLIENT_REQUEST_RETRY 6
#define CLIENT_REQUEST_RETRY_SLEEP 100 // in µs
#define CLIENT_REQUEST_BUFFER_SIZE 8192 // in nKB

// --------------------------------------------------------- //

/*
TODO:
[X] multi-threaded http server

[X] threadpool for scalability

[X] blocking I/O (not epoll)

[X] keep-alive (manual)

[?] not full http spec

[?] not async / non-blocking server
*/

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

class HttpServer {
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
    
    static bool send_all(int fd, const char* data, size_t len) {
        int retry_count = 0;
        while (len > 0 && retry_count < CLIENT_REQUEST_RETRY) { // check/do retry?
            ssize_t written = write(fd, data, len);
            if (written <= 0) {
                if (written == -1) {
                    if (errno == EINTR) continue;  // keep retry for EINTR
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        retry_count++;
                        usleep(CLIENT_REQUEST_RETRY_SLEEP);  // sleep before retry
                        continue;
                    }
                }
                return false;
            }
            data += written;
            len -= written;
            retry_count = 0;  // reset after partial write
        }
        return (len == 0);
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
            timeout.tv_sec = CONNECCONNECTION_TIMEOUT_SEC;
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
    
    void handle_client(int client_fd) {
        // FIX: Gunakan struct RAII sederhana (bukan unique_ptr dengan lambda)
        struct SocketCloser {
            int fd;
            explicit SocketCloser(int fd_) : fd(fd_) {}
            ~SocketCloser() { if (fd != -1) close(fd); }
            SocketCloser(const SocketCloser&) = delete;
            SocketCloser& operator=(const SocketCloser&) = delete;
        } closer(client_fd);
        
        // constexpr uint64_t MAX_REQUESTS_PER_CONNECTION = 10000;
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
            
            const char* response = nullptr;
            size_t response_len = 0;
            
            if (method != "GET") {
                response = method_not_allowed_response.data();
                response_len = method_not_allowed_response.size();
            } else if (path == "/cc") {
                response = home_response.data();
                response_len = home_response.size();
            } else {
                response = not_found_response.data();
                response_len = not_found_response.size();
            }
            
            struct iovec iov;
            iov.iov_base = const_cast<char*>(response);
            iov.iov_len = response_len;
            
            ssize_t written = writev(client_fd, &iov, 1);
            if (written != (ssize_t)response_len) {
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

            if (request_count % CONNECTION_MAX_PER_IP == 0) {
                // periodically check if connection is still alive
                char dummy;
                if (recv(client_fd, &dummy, 1, MSG_PEEK | MSG_DONTWAIT) == 0) {
                    return; // client closed connection
                }
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
        std::cout << "backend_cc: run on "
                  << ADDRESS_IP << ":" << ADDRESS_PORT << "\n";
        server.start();
    } catch (const std::exception& e) {
        std::cerr << "server error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
