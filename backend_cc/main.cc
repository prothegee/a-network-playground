#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <thread>
#include <atomic>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>

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
                    task();
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
            worker.join();
        }
    }
};

// --------------------------------------------------------- //

class HttpServer {
private:
    int server_fd;
    int port;
    std::atomic<bool> running;
    ThreadPool thread_pool;
    
    const std::string home_response_header = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 4\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";
    const std::string home_body = "home";
    
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
    
public:
    HttpServer(int port_num, size_t pool_size = std::thread::hardware_concurrency())
        : port(port_num), running(true), thread_pool(pool_size ? pool_size : 4) {
        
        server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd == -1) {
            throw std::runtime_error("Failed to create socket");
        }
        
        // Set SO_REUSEADDR option
        int opt = 1;
        if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
            close(server_fd);
            throw std::runtime_error("Failed to set socket options");
        }
    }
    
    ~HttpServer() {
        stop();
    }
    
    void start() {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port);
        
        // Bind socket
        if (bind(server_fd, (sockaddr*)&address, sizeof(address)) == -1) {
            throw std::runtime_error("Failed to bind socket");
        }
        
        // Listen for connections
        if (listen(server_fd, 128) == -1) {
            throw std::runtime_error("Failed to listen on socket");
        }
        
        std::cout << "backend_cc: run on 0.0.0.0:" << port << "\n";
        
        // Accept connections
        while (running) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            
            int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
            
            if (client_fd == -1) {
                if (!running) {
                    break;
                }
                continue;
            }
            
            // Handle client using thread pool
            thread_pool.enqueue([this, client_fd]() {
                handle_client(client_fd);
            });
        }
    }
    
    void stop() {
        running = false;
        if (server_fd != -1) {
            shutdown(server_fd, SHUT_RDWR);
            close(server_fd);
            server_fd = -1;
        }
    }
    
private:
    bool starts_with(const std::string& str, const std::string& prefix) {
        return str.size() >= prefix.size() && 
               str.compare(0, prefix.size(), prefix) == 0;
    }
    
    void handle_client(int client_fd) {
        const int MAX_REQUESTS_PER_CONNECTION = 100;
        int request_count = 0;
        
        while (request_count < MAX_REQUESTS_PER_CONNECTION) {
            char buffer[4096];
            ssize_t total_read = 0;
            bool got_request_line = false;
            bool connection_close_requested = false;
            
            // Read until we get a complete request line or timeout
            while (total_read < static_cast<ssize_t>(sizeof(buffer)) - 1) {
                ssize_t bytes_read = read(client_fd, buffer + total_read, 
                                        sizeof(buffer) - 1 - total_read);
                if (bytes_read <= 0) {
                    // Connection closed by client or error
                    close(client_fd);
                    return;
                }
                
                total_read += bytes_read;
                buffer[total_read] = '\0';
                
                // Check if we have at least one complete line
                for (ssize_t i = 0; i < total_read - 1; i++) {
                    if (buffer[i] == '\r' && buffer[i+1] == '\n') {
                        got_request_line = true;
                        break;
                    }
                }
                
                if (got_request_line) {
                    // Check for Connection: close header in the full request
                    std::string request_str(buffer, total_read);
                    if (request_str.find("Connection: close") != std::string::npos ||
                        request_str.find("Connection: Close") != std::string::npos) {
                        connection_close_requested = true;
                    }
                    break;
                }
            }
            
            if (!got_request_line) {
                // Malformed request
                close(client_fd);
                return;
            }
            
            buffer[total_read] = '\0';
            std::string request_line(buffer);
            
            // Extract first line only
            size_t end_of_line = request_line.find('\r');
            if (end_of_line == std::string::npos) {
                end_of_line = request_line.find('\n');
            }
            if (end_of_line != std::string::npos) {
                request_line = request_line.substr(0, end_of_line);
            }
            
            // Parse method and path from request line
            std::string method, path;
            size_t first_space = request_line.find(' ');
            if (first_space != std::string::npos) {
                method = request_line.substr(0, first_space);
                size_t second_space = request_line.find(' ', first_space + 1);
                if (second_space != std::string::npos) {
                    path = request_line.substr(first_space + 1, second_space - first_space - 1);
                    
                    // Remove query string
                    size_t query_pos = path.find('?');
                    if (query_pos != std::string::npos) {
                        path = path.substr(0, query_pos);
                    }
                }
            }
            
            // Determine response
            const char* response_header = nullptr;
            const char* response_body = nullptr;
            size_t header_len = 0;
            size_t body_len = 0;
            
            if (method != "GET") {
                std::string response = method_not_allowed_response;
                write(client_fd, response.c_str(), response.length());
                if (connection_close_requested) {
                    close(client_fd);
                    return;
                }
                // Continue to next request on same connection
                request_count++;
                continue;
            }
            else if (path == "/cc") {
                response_header = home_response_header.c_str();
                response_body = home_body.c_str();
                header_len = home_response_header.length();
                body_len = home_body.length();
            } else {
                std::string response = not_found_response;
                write(client_fd, response.c_str(), response.length());
                if (connection_close_requested) {
                    close(client_fd);
                    return;
                }
                request_count++;
                continue;
            }
            
            // Send response in two parts
            write(client_fd, response_header, header_len);
            write(client_fd, response_body, body_len);
            
            // Check if client wants to close connection
            if (connection_close_requested) {
                close(client_fd);
                return;
            }
            
            request_count++;
        }
        
        // Close connection after max requests
        close(client_fd);
    }
};

// --------------------------------------------------------- //

int main() {
    try {
        HttpServer server(9002);
        server.start();
    } catch (const std::exception& e) {
        std::cerr << "Server error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
