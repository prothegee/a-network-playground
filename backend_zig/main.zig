const std = @import("std");

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

fn handleClient(stream: std.Io.net.Stream, io: std.Io) void {
    defer stream.close(io);
    
    var receive_buffer: [CLIENT_REQUEST_BUFFER_SIZE]u8 = undefined;
    var send_buffer: [CLIENT_REQUEST_BUFFER_SIZE]u8 = undefined;
    
    var conn_reader = stream.reader(io, &receive_buffer);
    var conn_writer = stream.writer(io, &send_buffer);
    var server: std.http.Server = .init(&conn_reader.interface, &conn_writer.interface);
    
    // Handle multiple requests on same connection (keep-alive like C++)
    while (true) {
        var request = server.receiveHead() catch |err| {
            if (err == error.HttpConnectionClosing) break;
            break;
        };
        // server manages lifecycle automatically via discardBody()?
        
        const path = request.head.target;
        
        if (std.mem.eql(u8, path, "/zig")) {
            request.respond("home", .{
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
    
    std.debug.print("backend_zig: run on {s}:{d}\n", .{ ADDRESS_IP, ADDRESS_PORT });
    std.debug.print("  -/cc\n", .{});
    
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
//     Latency   272.00us  565.59us  22.78ms   94.74%
//     Req/Sec    65.92k    12.65k   87.12k    63.80%
//   3967470 requests in 10.10s, 158.91MB read
// Requests/sec: 392816.21
// Transfer/sec:     15.73MB
//

