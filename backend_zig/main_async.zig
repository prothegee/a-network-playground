const std = @import("std");

const ADDRESS_IP: []const u8 = "0.0.0.0";
const ADDRESS_PORT: u16 = 9007;
const CONNECTION_THREADS_TARGET: usize = 0;
const CLIENT_REQUEST_BUFFER_SIZE: usize = 8192;

// --------------------------------------------------------- //

//
// TODO:
// [X] multi-threaded http server
// [X] threadpool for scalability
// [X] blocking I/O (not epoll)
// [X] keep-alive (manual)
// [?] websocket support
// [X] query parameters parsing
// [X] dynamic thread count (hardware_concurrency)
// [?] public file access
// [X] not full http spec
// [X] async / non-blocking server (std.Io)
//

// --------------------------------------------------------- //

// Async client handler - new std.Io async I/O system (Zig 0.16.x)
fn handleClient(stream: std.Io.net.Stream, io: std.Io) void {
    defer stream.close(io);

    var receive_buffer: [CLIENT_REQUEST_BUFFER_SIZE]u8 = undefined;
    var send_buffer: [CLIENT_REQUEST_BUFFER_SIZE]u8 = undefined;

    // Async reader/writer wrappers for non-blocking operations
    var conn_reader = stream.reader(io, &receive_buffer);
    var conn_writer = stream.writer(io, &send_buffer);
    var server = std.http.Server.init(&conn_reader.interface, &conn_writer.interface);

    // Handle multiple requests on same connection (keep-alive like C++)
    // NOTE: perhaps handle sig handle (postpone)
    while (true) {
        // Async receive: suspends until data available or connection closed
        var request = server.receiveHead() catch |err| {
            if (err == error.HttpConnectionClosing) break;
            if (err == error.ConnectionResetByPeer) break;
            break;
        };

        const full_path = request.head.target;

        // Split path and query string
        const path = if (std.mem.indexOfScalar(u8, full_path, '?')) |qpos|
            full_path[0..qpos]
        else
            full_path;

        const query = if (std.mem.indexOfScalar(u8, full_path, '?')) |qpos|
            full_path[qpos + 1 ..]
        else
            "";

        if (std.mem.eql(u8, path, "/zig")) {
            // Async respond: suspends until write completes
            // respond() internally handles discardBody() for keep-alive
            request.respond("home", .{
                .status = std.http.Status.ok,
                .keep_alive = true,
                .version = std.http.Version.@"HTTP/1.1",
            }) catch break;
        } else if (std.mem.eql(u8, path, "/zig/json")) {
            request.respond("{\"string\":\"string\",\"decimal\":3.14,\"round\":69,\"boolean\":true}", .{
                .status = std.http.Status.ok,
                .keep_alive = true,
                .version = std.http.Version.@"HTTP/1.1",
            }) catch break;
        } else if (std.mem.eql(u8, path, "/zig/echo")) {
            // Build JSON response for query parameters
            var response_body: std.ArrayList(u8) = .empty;
            defer response_body.deinit(std.heap.smp_allocator);

            if (query.len == 0) {
                // No params → return null
                response_body.appendSlice(std.heap.smp_allocator, "null") catch break;
            } else {
                // All params as JSON object
                response_body.append(std.heap.smp_allocator, '{') catch break;
                var first = true;
                var pos: usize = 0;
                while (pos < query.len) {
                    const amp_pos = std.mem.indexOfScalarPos(u8, query, pos, '&') orelse query.len;
                    const pair = query[pos..amp_pos];

                    if (std.mem.indexOfScalar(u8, pair, '=')) |eq_pos| {
                        const key = pair[0..eq_pos];
                        const val = pair[eq_pos + 1 ..];

                        if (!first) response_body.appendSlice(std.heap.smp_allocator, ",") catch break;
                        first = false;

                        response_body.append(std.heap.smp_allocator, '"') catch break;
                        response_body.appendSlice(std.heap.smp_allocator, key) catch break;
                        response_body.appendSlice(std.heap.smp_allocator, "\":") catch break;

                        if (val.len == 0) {
                            response_body.appendSlice(std.heap.smp_allocator, "null") catch break;
                        } else {
                            response_body.append(std.heap.smp_allocator, '"') catch break;
                            response_body.appendSlice(std.heap.smp_allocator, val) catch break;
                            response_body.append(std.heap.smp_allocator, '"') catch break;
                        }
                    } else {
                        if (!first) response_body.appendSlice(std.heap.smp_allocator, ",") catch break;
                        first = false;

                        response_body.append(std.heap.smp_allocator, '"') catch break;
                        response_body.appendSlice(std.heap.smp_allocator, pair) catch break;
                        response_body.appendSlice(std.heap.smp_allocator, "\":null") catch break;
                    }

                    pos = amp_pos + 1;
                }
                response_body.append(std.heap.smp_allocator, '}') catch break;
            }

            request.respond(response_body.items, .{
                .status = std.http.Status.ok,
                .keep_alive = true,
                .version = std.http.Version.@"HTTP/1.1",
            }) catch break;
        } else {
            request.respond("Not Found", .{
                .status = std.http.Status.not_found,
                .keep_alive = true,
                .version = std.http.Version.@"HTTP/1.1",
            }) catch break;
        }

        // REMOVED: discardBody() is private - respond() handles it internally
    }
}

// Async-aware server loop with io.concurrent() for task spawning
fn runServer(io: std.Io) !void {
    const addr = try std.Io.net.IpAddress.resolve(io, ADDRESS_IP, ADDRESS_PORT);
    var tcp_server = try addr.listen(io, .{
        .mode = .stream,
        .kernel_backlog = 1024,
        .reuse_address = true,
    });
    defer tcp_server.deinit(io);

    std.debug.print("backend_zig_async: run on {s}:{d}\n", .{ ADDRESS_IP, ADDRESS_PORT });
    std.debug.print("  - HTTP endpoint: /zig\n", .{});
    std.debug.print("  - JSON endpoint: /zig/json\n", .{});
    std.debug.print("  - Echo endpoint: /zig/echo?text=hello (query parameters)\n", .{});

    while (true) {
        // Async accept: suspends until new connection available
        var stream = tcp_server.accept(io) catch |err| {
            std.debug.print("Accept error: {}\n", .{err});
            continue;
        };

        // Use io.concurrent() - direct task spawning (like C++ ThreadPool.enqueue)
        // Stream passed by value (ownership transferred to task)
        _ = io.concurrent(handleClient, .{ stream, io }) catch |err| {
            std.debug.print("Concurrent error: {}\n", .{err});
            stream.close(io);
        };
    }
}

// --------------------------------------------------------- //

pub fn main() !void {
    // Thread count configuration: 0 = all threads, >0 = specific count
    const concurrent_limit: std.Io.Limit = if (CONNECTION_THREADS_TARGET == 0)
        .unlimited
    else
        .{ .limited = CONNECTION_THREADS_TARGET };

    // Threaded I/O with concurrent task support - new std.Io async system
    // async_limit auto-detected from CPU count if not specified
    var io = std.Io.Threaded.init(std.heap.smp_allocator, .{
        .stack_size = std.Thread.SpawnConfig.default_stack_size,
        .concurrent_limit = concurrent_limit,
        // async_limit defaults to cpu_count - 1 if not provided
    });
    defer io.deinit();

    // // log detected CPU count for debugging
    // if (std.Thread.getCpuCount()) |cpu_count| {
    //     std.debug.print("Detected {d} CPU cores\n", .{cpu_count});
    // } else |_| {
    //     std.debug.print("CPU count detection failed, using defaults\n", .{});
    // }

    try runServer(io.io());
}

//
// IMPORTANT:
// this implementation result:
// ➜ wrk -c100 -t6 -d10s http://localhost:9007/zig
// Running 10s test @ http://localhost:9007/zig
//   6 threads and 100 connections
//   Thread Stats   Avg      Stdev     Max   +/- Stdev
//     Latency   249.91us  527.67us  24.95ms   95.42%
//     Req/Sec    67.15k    14.19k  105.54k    63.52%
//   4028055 requests in 10.10s, 161.34MB read
// Requests/sec: 398844.18
// Transfer/sec:     15.98MB
//

//
// ASYNC VERIFICATION (Zig 0.16.x - std.Io):
// - std.Io.Threaded provides async I/O backend with thread pool
// - io.concurrent() spawns tasks that suspend/resume on I/O
// - stream.reader(io, buffer) and writer(io, buffer) are async interfaces
// - receiveHead() and respond() suspend when waiting for I/O (non-blocking)
// - Kernel handles suspension efficiently (no busy-waiting)
//
// THREAD DETECTION (from std/Io/Threaded.zig):
// - init() calls std.Thread.getCpuCount() internally [[std/Io/Threaded.zig:1637]]
// - async_limit defaults to cpu_count - 1 if not provided [[std/Io/Threaded.zig:1642]]
// - concurrent_limit controls max concurrent tasks (prevents thread explosion)
// - NO n_threads field in InitOptions - thread pool is dynamic
//
// PERFORMANCE CHARACTERISTICS:
// - M:N scheduling (many tasks, fewer threads)
// - Suspension happens in kernel (efficient, no userspace overhead)
// - smp_allocator is thread-safe (lock-free per-CPU arenas)
// - CONNECTION_THREADS_TARGET = 0 means unlimited concurrent tasks
// - For 10k+ concurrent: consider std.Io.Evented (epoll/kqueue/io_uring)
//

