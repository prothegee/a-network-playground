const std = @import("std");

const ADDRESS_IP: []const u8 = "0.0.0.0";
const ADDRESS_PORT: u16 = 9007;
const DEFAULT_BUFFER: i32 = 1024;

fn runServer(io: std.Io) !void {
    const addr = try std.Io.net.IpAddress.resolve(io, ADDRESS_IP, ADDRESS_PORT);
    var tcp_server = try addr.listen(io, .{});
    var receive_buffer: [DEFAULT_BUFFER]u8 = undefined;
    var send_buffer: [DEFAULT_BUFFER]u8 = undefined;

    while (true) {
        var stream = tcp_server.accept(io) catch |err| {
            std.debug.print("Accept error: {}\n", .{err});
            continue;
        };
        defer stream.close(io);

        var conn_reader = stream.reader(io, &receive_buffer);
        var conn_writer = stream.writer(io, &send_buffer);
        var server: std.http.Server = .init(&conn_reader.interface, &conn_writer.interface);

        var request = server.receiveHead() catch |err| {
            if (err == error.HttpConnectionClosing) {
                continue;
            }
            std.debug.print("Error receiving request: {}\n", .{err});
            continue;
        };

        const path = request.head.target;
        
        if (std.mem.eql(u8, path, "/zig")) {
            const header = std.http.Header{
                .name = "Content-Type",
                .value = "text/plain",
            };

            request.respond("home", .{
                .status = std.http.Status.ok,
                .keep_alive = false,
                .version = std.http.Version.@"HTTP/1.1",
                .extra_headers = &.{header},
            }) catch |err| {
                std.debug.print("Error sending response: {}\n", .{err});
                continue;
            };
        } else {
            const header = std.http.Header{
                .name = "Content-Type",
                .value = "text/plain",
            };

            request.respond("Not Found", .{
                .status = std.http.Status.not_found,
                .keep_alive = false,
                .version = std.http.Version.@"HTTP/1.1",
                .extra_headers = &.{header},
            }) catch |err| {
                std.debug.print("Error sending response: {}\n", .{err});
                continue;
            };
        }
    }
}

pub fn main(init: std.process.Init) !void {
    std.debug.print("backend_zig: run on {s}:{d}\n", .{ ADDRESS_IP, ADDRESS_PORT });
    try runServer(init.io);
}
