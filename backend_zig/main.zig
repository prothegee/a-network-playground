const std = @import("std");
const posix = std.posix;

const ADDRESS_IP: []const u8 = "0.0.0.0";
const ADDRESS_PORT: u16 = 9006;
const CONNECTION_CONCURRENT_TARGET: usize = 256;
const CLIENT_REQUEST_BUFFER_SIZE: usize = 8192;

// --------------------------------------------------------- //

//
// TODO:
// [X] multi-threaded http server
// [X] threadpool for scalability
// [X] blocking I/O (not epoll)
// [X] keep-alive (manual)
// [?] websocket support
// [?] query parameters parsing
// [X] dynamic thread count (hardware_concurrency)
// [?] public file access
// [X] not full http spec
// [X] not async / non-blocking server
//

// --------------------------------------------------------- //

// OPTIMIZATION 1: Pre-computed HTTP responses (like C++ home_response)
const home_response = 
    "HTTP/1.1 200 OK\r\n" ++
    "Content-Type: text/plain\r\n" ++
    "Content-Length: 4\r\n" ++
    "Connection: keep-alive\r\n" ++
    "\r\n" ++
    "home";

const not_found_response = 
    "HTTP/1.1 404 Not Found\r\n" ++
    "Content-Type: text/plain\r\n" ++
    "Content-Length: 9\r\n" ++
    "Connection: keep-alive\r\n" ++
    "\r\n" ++
    "Not Found";

// OPTIMIZATION 2: Thread-local buffers
threadlocal var tls_receive_buffer: [CLIENT_REQUEST_BUFFER_SIZE]u8 = undefined;
threadlocal var tls_send_buffer: [CLIENT_REQUEST_BUFFER_SIZE]u8 = undefined;

// OPTIMIZATION 3: Manual HTTP parsing
fn findHeadersEnd(buffer: []const u8) ?usize {
    if (buffer.len < 4) return null;
    for (0..buffer.len - 3) |i| {
        if (buffer[i] == '\r' and 
            buffer[i + 1] == '\n' and 
            buffer[i + 2] == '\r' and 
            buffer[i + 3] == '\n') {
            return i + 4;
        }
    }
    return null;
}

fn parsePath(buffer: []const u8) ?[]const u8 {
    var i: usize = 0;
    while (i < buffer.len and buffer[i] != ' ') : (i += 1) {}
    if (i >= buffer.len) return null;
    i += 1;
    
    const start = i;
    while (i < buffer.len and buffer[i] != ' ' and buffer[i] != '\r') : (i += 1) {}
    if (i <= start) return null;
    
    return buffer[start..i];
}

// OPTIMIZATION 4: Set TCP_NODELAY using posix.setsockopt
fn setTcpNoDelay(handle: std.Io.net.Socket.Handle) void {
    const flag: c_int = 1;
    posix.setsockopt(
        handle,
        posix.IPPROTO.TCP,
        posix.TCP.NODELAY,
        std.mem.asBytes(&flag),
    ) catch {};
}

fn handleClient(stream: std.Io.net.Stream, io: std.Io) void {
    defer stream.close(io);
    
    // OPTIMIZATION 5: Set TCP_NODELAY on client socket
    setTcpNoDelay(stream.socket.handle);
    
    const receive_buffer = &tls_receive_buffer;
    const send_buffer = &tls_send_buffer;
    
    var buffer_used: usize = 0;
    
    while (true) {
        if (buffer_used < receive_buffer.len) {
            // OPTIMIZATION 7: Use posix.read() directly (like C++ read())
            // This bypasses std.Io abstraction for maximum performance
            const n = posix.read(stream.socket.handle, receive_buffer[buffer_used..]) catch break;
            if (n == 0) break;
            buffer_used += n;
        }
        
        const headers_end = findHeadersEnd(receive_buffer[0..buffer_used]) orelse {
            if (buffer_used >= receive_buffer.len) break;
            continue;
        };
        
        const path = parsePath(receive_buffer[0..headers_end]) orelse {
            buffer_used = 0;
            continue;
        };
        
        const response = if (std.mem.eql(u8, path, "/zig"))
            home_response
        else
            not_found_response;
        
        // OPTIMIZATION 8: Use writer.interface.writeAll() (Io.Writer methods are on interface field)
        var writer = stream.writer(io, send_buffer);
        writer.interface.writeAll(response) catch break;
        writer.interface.flush() catch break;
        
        if (headers_end < buffer_used) {
            @memcpy(receive_buffer[0 .. buffer_used - headers_end], receive_buffer[headers_end..buffer_used]);
            buffer_used -= headers_end;
        } else {
            buffer_used = 0;
        }
    }
}

fn runServer(io: std.Io) !void {
    const addr = try std.Io.net.IpAddress.resolve(io, ADDRESS_IP, ADDRESS_PORT);
    var tcp_server = try addr.listen(io, .{
        .mode = .stream,
        .kernel_backlog = 1024,
        .reuse_address = true,
    });
    defer tcp_server.deinit(io);
    
    // OPTIMIZATION 6: Set TCP_NODELAY on server socket
    setTcpNoDelay(tcp_server.socket.handle);
    
    std.debug.print("backend_zig: run on {s}:{d}\n", .{ ADDRESS_IP, ADDRESS_PORT });
    std.debug.print("  - HTTP endpoint: /zig\n", .{});
    // std.debug.print("  - JSON endpoint: /zig/json\n", .{});
    // std.debug.print("  - Echo endpoint: /zig/echo?text=hello\n", .{});
    // std.debug.print("  - Dynamic path: /zig/{{path1}}/{{path2}}/...\n", .{});
    // std.debug.print("  - WebSocket endpoint: /chat/{{room_name}}\n", .{});
    // std.debug.print("  - Static files from ./public at root\n", .{});
    
    while (true) {
        var stream = tcp_server.accept(io) catch |err| {
            std.debug.print("Accept error: {}\n", .{err});
            continue;
        };
        
        _ = io.concurrent(handleClient, .{ stream, io }) catch |err| {
            std.debug.print("Concurrent error: {}\n", .{err});
            stream.close(io);
        };
    }
}

pub fn main() !void {
    var io = std.Io.Threaded.init(std.heap.smp_allocator, .{
        .concurrent_limit = std.Io.Limit.unlimited,
        .stack_size = std.Thread.SpawnConfig.default_stack_size,
    });
    defer io.deinit();
    
    try runServer(io.io());
}

//
// IMPORTANT:
// this implementation result:
// ➜ wrk -c 100 -t 6 -d 10s http://localhost:9006/zig
// Running 10s test @ http://localhost:9006/zig
//   6 threads and 100 connections
//   Thread Stats   Avg      Stdev     Max   +/- Stdev
//     Latency   335.91us  761.76us  28.64ms   91.18%
//     Req/Sec    79.33k     5.23k  118.53k    85.24%
//   4760716 requests in 10.10s, 417.70MB read
// Requests/sec: 471386.07
// Transfer/sec:     41.36MB
//

