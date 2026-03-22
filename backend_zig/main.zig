const std = @import("std");
const posix = std.posix;
const linux = std.os.linux;

const ADDRESS_IP: []const u8 = "0.0.0.0";
const ADDRESS_PORT: u16 = 9006;
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
// [?] not async / non-blocking server
//

// --------------------------------------------------------- //

const home_response =
    "HTTP/1.1 200 OK\r\n" ++
    "Content-Type: text/plain\r\n" ++
    "Content-Length: 4\r\n" ++
    "Connection: keep-alive\r\n" ++
    "\r\n" ++
    "home";

const json_response =
    "HTTP/1.1 200 OK\r\n" ++
    "Content-Type: application/json\r\n" ++
    "Content-Length: 60\r\n" ++
    "Connection: keep-alive\r\n" ++
    "\r\n" ++
    "{\"string\":\"string\",\"decimal\":3.14,\"round\":69,\"boolean\":true}";

const not_found_response =
    "HTTP/1.1 404 Not Found\r\n" ++
    "Content-Type: text/plain\r\n" ++
    "Content-Length: 9\r\n" ++
    "Connection: keep-alive\r\n" ++
    "\r\n" ++
    "Not Found";

// --------------------------------------------------------- //

threadlocal var tls_receive_buffer: [CLIENT_REQUEST_BUFFER_SIZE]u8 = undefined;
threadlocal var tls_send_buffer: [CLIENT_REQUEST_BUFFER_SIZE]u8 = undefined;

// --------------------------------------------------------- //

fn findHeadersEnd(buffer: []const u8) ?usize {
    if (buffer.len < 4) return null;
    for (0..buffer.len - 3) |i| {
        if (buffer[i] == '\r' and buffer[i + 1] == '\n' and
            buffer[i + 2] == '\r' and buffer[i + 3] == '\n')
        {
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

fn setTcpNoDelay(handle: std.posix.socket_t) void {
    const flag: c_int = 1;
    posix.setsockopt(handle, posix.IPPROTO.TCP, posix.TCP.NODELAY, std.mem.asBytes(&flag)) catch {};
}

// --------------------------------------------------------- //

fn ipAddressToSockAddr(addr: std.Io.net.IpAddress, sockaddr: *posix.sockaddr) posix.socklen_t {
    return switch (addr) {
        .ip4 => |a| {
            const in = @as(*posix.sockaddr.in, @ptrCast(@alignCast(sockaddr)));
            in.* = .{
                .family = posix.AF.INET,
                .port = std.mem.nativeToBig(u16, a.port),
                .addr = @bitCast(a.bytes),
                .zero = @splat(0),
            };
            return @sizeOf(posix.sockaddr.in);
        },
        .ip6 => |a| {
            const in6 = @as(*posix.sockaddr.in6, @ptrCast(@alignCast(sockaddr)));
            in6.* = .{
                .family = posix.AF.INET6,
                .port = std.mem.nativeToBig(u16, a.port),
                .flowinfo = a.flow,
                .addr = a.bytes,
                .scope_id = if (a.interface.isNone()) 0 else a.interface.index,
            };
            return @sizeOf(posix.sockaddr.in6);
        },
    };
}

fn ipAddressFromSockAddr(sa: *const posix.sockaddr, len: posix.socklen_t) !std.Io.net.IpAddress {
    if (len == @sizeOf(posix.sockaddr.in) and sa.family == posix.AF.INET) {
        const in = @as(*const posix.sockaddr.in, @ptrCast(@alignCast(sa)));
        return .{ .ip4 = .{
            .bytes = @bitCast(in.addr),
            .port = std.mem.bigToNative(u16, in.port),
        } };
    } else if (len >= @sizeOf(posix.sockaddr.in6) and sa.family == posix.AF.INET6) {
        const in6 = @as(*const posix.sockaddr.in6, @ptrCast(@alignCast(sa)));
        return .{ .ip6 = .{
            .bytes = in6.addr,
            .port = std.mem.bigToNative(u16, in6.port),
            .flow = in6.flowinfo,
            .interface = if (in6.scope_id != 0) .{ .index = in6.scope_id } else .none,
        } };
    }
    return error.AddressFamilyUnsupported;
}

// --------------------------------------------------------- //

fn createListeningSocket(addr: std.Io.net.IpAddress) !posix.fd_t {
    const domain: u32 = switch (addr) {
        .ip4 => linux.AF.INET,
        .ip6 => linux.AF.INET6,
    };
    const sock_type = linux.SOCK.STREAM | linux.SOCK.CLOEXEC;
    const protocol = 0;

    const fd = try checkSyscall(linux.socket(domain, sock_type, protocol));
    errdefer _ = linux.close(@intCast(fd));

    const reuse: c_int = 1;
    const optval = std.mem.asBytes(&reuse);
    _ = try checkSyscall(linux.setsockopt(
        @intCast(fd),
        linux.SOL.SOCKET,
        linux.SO.REUSEADDR,
        optval.ptr,
        @as(u32, @intCast(optval.len)),
    ));
    _ = try checkSyscall(linux.setsockopt(
        @intCast(fd),
        linux.SOL.SOCKET,
        linux.SO.REUSEPORT,
        optval.ptr,
        @as(u32, @intCast(optval.len)),
    ));

    var sockaddr: posix.sockaddr = undefined;
    const addr_len = ipAddressToSockAddr(addr, &sockaddr);
    _ = try checkSyscall(linux.bind(
        @intCast(fd),
        @ptrCast(&sockaddr),
        addr_len,
    ));

    const backlog: u32 = 1024;
    _ = try checkSyscall(linux.listen(@intCast(fd), backlog));

    return @as(posix.fd_t, @intCast(fd));
}

fn checkSyscall(rc: usize) !usize {
    switch (linux.errno(rc)) {
        .SUCCESS => return rc,
        .INTR => return error.SignalInterrupt,
        .BADF => return error.BadFileDescriptor,
        .ACCES => return error.AccessDenied,
        .PERM => return error.PermissionDenied,
        .INVAL => return error.InvalidArgument,
        .FAULT => return error.BadAddress,
        .MFILE => return error.ProcessFdQuotaExceeded,
        .NFILE => return error.SystemFdQuotaExceeded,
        .NOMEM => return error.SystemResources,
        .NOSPC => return error.NoSpaceLeft,
        .ROFS => return error.ReadOnlyFileSystem,
        .ISDIR => return error.IsDir,
        .NOTDIR => return error.NotDir,
        .NOENT => return error.FileNotFound,
        .EXIST => return error.PathAlreadyExists,
        .LOOP => return error.SymLinkLoop,
        .NAMETOOLONG => return error.NameTooLong,
        .OPNOTSUPP => return error.OperationUnsupported,
        .AFNOSUPPORT => return error.AddressFamilyUnsupported,
        .ADDRINUSE => return error.AddressInUse,
        .ADDRNOTAVAIL => return error.AddressUnavailable,
        .NETDOWN => return error.NetworkDown,
        .NETUNREACH => return error.NetworkUnreachable,
        .HOSTUNREACH => return error.HostUnreachable,
        .CONNREFUSED => return error.ConnectionRefused,
        .CONNRESET => return error.ConnectionResetByPeer,
        .TIMEDOUT => return error.Timeout,
        else => |err| return posix.unexpectedErrno(err),
    }
}

// --------------------------------------------------------- //

// blocking I/O
fn handleClient(stream: std.Io.net.Stream, io: std.Io) void {
    defer stream.close(io);
    setTcpNoDelay(stream.socket.handle);

    const receive_buffer = &tls_receive_buffer;
    const send_buffer = &tls_send_buffer;

    var buffer_used: usize = 0;

    while (true) {
        if (buffer_used < receive_buffer.len) {
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

        var response: []const u8 = "";

        if (std.mem.eql(u8, path, "/zig")) {
            response = home_response;
        } else if (std.mem.eql(u8, path, "/zig/json")) {
            response = json_response;
        } else {
            response = not_found_response;
        }

        // const response = if (std.mem.eql(u8, path, "/zig"))
        //     home_response
        // else if (std.mem.eql(u8, path, "/zig/json"))
        //     json_response
        // else
        //     not_found_response;

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

// --------------------------------------------------------- //

fn workerThread(allocator: std.mem.Allocator) !void {
    var io = std.Io.Threaded.init(allocator, .{
        .concurrent_limit = std.Io.Limit.unlimited,
        .stack_size = std.Thread.SpawnConfig.default_stack_size,
    });
    defer io.deinit();

    const addr = try std.Io.net.IpAddress.resolve(io.io(), ADDRESS_IP, ADDRESS_PORT);
    const fd = try createListeningSocket(addr);
    defer _ = linux.close(@intCast(fd));

    setTcpNoDelay(fd);

    // std.debug.print("Worker thread {d} listening on {s}:{d} (fd={d})\n", .{
    //     std.Thread.getCurrentId(),
    //     ADDRESS_IP,
    //     ADDRESS_PORT,
    //     fd,
    // });

    var tcp_server = std.Io.net.Server{
        .socket = .{
            .handle = fd,
            .address = addr,
        },
        .options = {},
    };

    while (true) {
        const stream = tcp_server.accept(io.io()) catch |err| {
            std.debug.print("Accept error in thread {}: {}\n", .{ std.Thread.getCurrentId(), err });
            continue;
        };
        _ = io.io().concurrent(handleClient, .{ stream, io.io() }) catch |err| {
            std.debug.print("Concurrent error: {}\n", .{err});
            stream.close(io.io());
        };
    }
}

// --------------------------------------------------------- //

pub fn main() !void {
    const allocator = std.heap.smp_allocator;

    std.debug.print("backend_zig: run on {s}:{d}\n", .{ ADDRESS_IP, ADDRESS_PORT });
    std.debug.print("  - HTTP endpoint: /zig\n", .{});
    std.debug.print("  - JSON endpoint: /zig/json\n", .{});
    // std.debug.print("  - Echo endpoint: /zig/echo?text=hello\n", .{});
    // std.debug.print("  - Dynamic path: /zig/{{path1}}/{{path2}}/...\n", .{});
    // std.debug.print("  - WebSocket endpoint: /chat/{{room_name}}\n", .{});
    // std.debug.print("  - Static files from ./public at root\n", .{});

    const cpu_count = try std.Thread.getCpuCount();
    // std.debug.print("Starting {d} worker threads (blocking, reuse_port)\n", .{cpu_count});

    const threads = try allocator.alloc(std.Thread, cpu_count);
    defer allocator.free(threads);

    for (threads) |*t| {
        t.* = try std.Thread.spawn(.{}, workerThread, .{allocator});
    }

    for (threads) |t| {
        t.join();
    }
}

//
// IMPORTANT:
// this implementation result (blocking I/O, per‑core accept):
// ➜ wrk -c 100 -t 6 -d 10s http://localhost:9006/zig
// Running 10s test @ http://localhost:9006/zig
//   6 threads and 100 connections
//   Thread Stats   Avg      Stdev     Max   +/- Stdev
//     Latency   359.85us  747.72us  14.34ms   90.04%
//     Req/Sec    81.61k     3.79k   98.88k    80.30%
//   4903795 requests in 10.10s, 430.25MB read
// Requests/sec: 485539.59
// Transfer/sec:     42.60MB
//
