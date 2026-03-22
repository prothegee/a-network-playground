const std = @import("std");

const ADDRESS_IP: []const u8 = "0.0.0.0";
const ADDRESS_PORT: u16 = 9007;
const CONNECTION_THREADS_TARGET: usize = 0;
const CLIENT_REQUEST_BUFFER_SIZE: usize = 8192;

// --------------------------------------------------------- //

// NOTE:
// TESTED in: zig 0.16.0-dev.2974+83c7aba12
// consider std.Io.Evented (epoll/kqueue/io_uring) for more burst/high concurrent
//

//
// TODO:
// [X] multi-threaded http server
// [X] threadpool for scalability
// [X] blocking I/O (not epoll)
// [X] keep-alive (manual)
// [X] websocket support
// [X] query parameters parsing
// [X] dynamic thread count (hardware_concurrency)
// [X] not full http spec
// [X] async / non-blocking server (std.Io)
// [X] dynamic path routing
// [X] public file access, from `$(pwd)/public` relative from the exec run
// [?] able upload file multiples `$(pwd)/public/u` relative from the exec run
//

// --------------------------------------------------------- //

// MIME type mapping for static files
fn mimeType(ext: []const u8) []const u8 {
    if (std.mem.eql(u8, ext, "html")) return "text/html";
    if (std.mem.eql(u8, ext, "css")) return "text/css";
    if (std.mem.eql(u8, ext, "js")) return "application/javascript";
    if (std.mem.eql(u8, ext, "json")) return "application/json";
    if (std.mem.eql(u8, ext, "png")) return "image/png";
    if (std.mem.eql(u8, ext, "jpg") or std.mem.eql(u8, ext, "jpeg")) return "image/jpeg";
    if (std.mem.eql(u8, ext, "gif")) return "image/gif";
    if (std.mem.eql(u8, ext, "svg")) return "image/svg+xml";
    if (std.mem.eql(u8, ext, "webp")) return "image/webp";
    if (std.mem.eql(u8, ext, "mp4")) return "video/mp4";
    if (std.mem.eql(u8, ext, "webm")) return "video/webm";
    if (std.mem.eql(u8, ext, "ogg")) return "video/ogg";
    if (std.mem.eql(u8, ext, "txt")) return "text/plain";
    if (std.mem.eql(u8, ext, "pdf")) return "application/pdf";
    return "application/octet-stream";
}

// Get file extension from path
fn getFileExtension(path: []const u8) []const u8 {
    if (std.mem.lastIndexOfScalar(u8, path, '.')) |dot_pos| {
        if (dot_pos + 1 < path.len) {
            return path[dot_pos + 1 ..];
        }
    }
    return "";
}

// Serve static file from ./public directory
// Returns true if file was served (200, 403, or 404 response sent)
// Returns false if public dir missing or path not applicable
fn serveStaticFile(request: *std.http.Server.Request, sub_path: []const u8, io: std.Io) !bool {
    // Security: prevent directory traversal
    if (std.mem.indexOf(u8, sub_path, "..") != null) {
        return false;
    }

    // Build full path: ./public/{sub_path}
    const public_dir = "./public";
    var full_path_buf: [512]u8 = undefined;

    if (public_dir.len + 1 + sub_path.len > full_path_buf.len) {
        return false;
    }

    @memcpy(full_path_buf[0..public_dir.len], public_dir);
    full_path_buf[public_dir.len] = '/';
    @memcpy(full_path_buf[public_dir.len + 1 ..][0..sub_path.len], sub_path);
    const full_path = full_path_buf[0 .. public_dir.len + 1 + sub_path.len];

    // Open file
    const file = std.Io.Dir.cwd().openFile(io, full_path, .{}) catch {
        // File not found - do NOT send response here, let caller handle it
        return false;
    };
    defer file.close(io);

    // Get file stats
    const stat = file.stat(io) catch {
        return false;
    };

    // Only serve regular files
    if (stat.kind != .file) {
        return false;
    }

    // Get MIME type
    const content_type = mimeType(getFileExtension(sub_path));

    // Build response headers
    var header_buffer: [1024]u8 = undefined;
    const header_slice = std.fmt.bufPrint(
        &header_buffer,
        "HTTP/1.1 200 OK\r\n" ++
            "Content-Type: {s}\r\n" ++
            "Content-Length: {d}\r\n" ++
            "Connection: keep-alive\r\n" ++
            "\r\n",
        .{ content_type, stat.size },
    ) catch return false;

    // Send headers using request.server.out directly (std.Io.Writer)
    request.server.out.writeAll(header_slice) catch return false;

    // Stream file contents - READER CREATED ONCE BEFORE LOOP (like C++ open() + read loop)
    var file_read_buffer: [8192]u8 = undefined;
    var file_reader = file.reader(io, &file_read_buffer);
    var copy_buffer: [8192]u8 = undefined;
    var remaining = stat.size;

    while (remaining > 0) {
        const to_read = @min(remaining, copy_buffer.len);
        const bytes_read = file_reader.interface.readSliceShort(copy_buffer[0..to_read]) catch break;
        if (bytes_read == 0) break;
        request.server.out.writeAll(copy_buffer[0..bytes_read]) catch break;
        remaining -= bytes_read;
    }

    // revent lock/stuck when requesting exists served file
    request.server.out.flush() catch return false;

    return true;
}

// --------------------------------------------------------- //

// WebSocket connection tracking (global, thread-safe)
const WebSocketConnection = struct {
    stream: std.Io.net.Stream,
    io: std.Io,
    room: []const u8,
};

const WebSocketRoom = struct {
    connections: std.ArrayList(*WebSocketConnection),
    mutex: std.Io.Mutex,
};

var g_websocket_rooms: std.StringHashMap(WebSocketRoom) = undefined;
var g_rooms_mutex: std.Io.Mutex = .init;
var g_ws_allocator: std.mem.Allocator = undefined;

// WebSocket opcodes (RFC 6455)
const WebSocketOpcode = enum(u8) {
    continuation = 0x0,
    text = 0x1,
    binary = 0x2,
    close = 0x8,
    ping = 0x9,
    pong = 0xA,
};

// WebSocket frame structure
const WebSocketFrame = struct {
    fin: bool,
    opcode: WebSocketOpcode,
    payload_length: u64,
    masking_key: [4]u8,
    masked: bool,
    payload: []const u8,
};

// Parse WebSocket frame from buffer
fn parseWebSocketFrame(data: []const u8) ?struct { frame: WebSocketFrame, consumed: usize } {
    if (data.len < 2) return null;
    var offset: usize = 0;

    // First byte: FIN + RSV + Opcode
    const first_byte = data[0];
    const fin = (first_byte & 0x80) != 0;
    const opcode_val = first_byte & 0x0F;
    const opcode = @as(WebSocketOpcode, @enumFromInt(opcode_val));
    offset += 1;

    // Second byte: MASK + Payload length
    const second_byte = data[1];
    const masked = (second_byte & 0x80) != 0;
    var payload_length: u64 = second_byte & 0x7F;
    offset += 1;

    // Extended payload length
    if (payload_length == 126) {
        if (data.len < offset + 2) return null;

        payload_length = @as(u64, data[offset]) << 8 | data[offset + 1];

        offset += 2;
    } else if (payload_length == 127) {
        if (data.len < offset + 8) return null;

        payload_length = 0;

        for (0..8) |i| {
            payload_length = (payload_length << 8) | data[offset + i];
        }

        offset += 8;
    }

    // Masking key
    var masking_key: [4]u8 = .{ 0, 0, 0, 0 };

    if (masked) {
        if (data.len < offset + 4) return null;
        @memcpy(&masking_key, data[offset .. offset + 4]);
        offset += 4;
    }

    // Payload
    if (data.len < offset + payload_length) return null;

    const payload_start = offset;
    const payload_end = offset + payload_length;

    // Unmask payload if masked (client->server frames are masked)
    var unmasked_payload: [4096]u8 = undefined;
    const payload_slice = if (masked) blk: {
        for (0..payload_length) |i| {
            unmasked_payload[i] = data[payload_start + i] ^ masking_key[i % 4];
        }
        break :blk unmasked_payload[0..payload_length];
    } else data[payload_start..payload_end];

    offset = payload_end;

    return .{
        .frame = .{
            .fin = fin,
            .opcode = opcode,
            .payload_length = payload_length,
            .masking_key = masking_key,
            .masked = masked,
            .payload = payload_slice,
        },
        .consumed = offset,
    };
}

// Serialize WebSocket frame to buffer (server->client, no mask)
fn serializeWebSocketFrame(buffer: []u8, opcode: WebSocketOpcode, payload: []const u8) usize {
    var offset: usize = 0;

    // First byte: FIN + Opcode
    buffer[offset] = 0x80 | @intFromEnum(opcode);
    offset += 1;

    // Second byte + extended length
    if (payload.len <= 125) {
        buffer[offset] = @intCast(payload.len);
        offset += 1;
    } else if (payload.len <= 65535) {
        buffer[offset] = 126;
        buffer[offset + 1] = @intCast((payload.len >> 8) & 0xFF);
        buffer[offset + 2] = @intCast(payload.len & 0xFF);
        offset += 3;
    } else {
        buffer[offset] = 127;
        for (0..8) |i| {
            buffer[offset + 1 + i] = @as(u8, @intCast((payload.len >> (@as(u6, @intCast(7 - i)) * 8)) & 0xFF));
        }
        offset += 9;
    }

    // Payload - copy to temp first to avoid aliasing
    var temp_payload: [4096]u8 = undefined;
    const copy_len = @min(payload.len, temp_payload.len);
    @memcpy(temp_payload[0..copy_len], payload[0..copy_len]);
    @memcpy(buffer[offset .. offset + copy_len], temp_payload[0..copy_len]);
    offset += copy_len;

    return offset;
}

// Compute WebSocket accept key (SHA1 + Base64)
fn computeWebSocketAccept(key: []const u8, buffer: *[64]u8) ![]const u8 {
    const magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    // Concatenate key + magic
    var hash_input: [60]u8 = undefined;
    @memcpy(hash_input[0..key.len], key);
    @memcpy(hash_input[key.len..], magic);

    // SHA1 hash
    var hash: [20]u8 = undefined;
    std.crypto.hash.Sha1.hash(&hash_input, &hash, .{});

    // Base64 encode
    const base64_encoder = std.base64.standard.Encoder;
    const encoded_len = base64_encoder.calcSize(20);
    return base64_encoder.encode(buffer[0..encoded_len], &hash);
}

// Add connection to room
fn addConnectionToRoom(room_name: []const u8, conn: *WebSocketConnection, io: std.Io) void {
    g_rooms_mutex.lock(io) catch return;
    defer g_rooms_mutex.unlock(io);

    const gop = g_websocket_rooms.getOrPut(room_name) catch return;
    if (!gop.found_existing) {
        gop.value_ptr.* = .{
            .connections = .empty,
            .mutex = .init,
        };
    }

    gop.value_ptr.mutex.lock(io) catch return;
    defer gop.value_ptr.mutex.unlock(io);

    gop.value_ptr.connections.append(g_ws_allocator, conn) catch return;

    // Log connection (like C++ reference)
    const room = gop.value_ptr;
    std.debug.print("WebSocket client connected to room '{s}'. Total in room: {d}\n", .{ room_name, room.connections.items.len });
}

// Remove connection from room
fn removeConnectionFromRoom(room_name: []const u8, conn: *WebSocketConnection, io: std.Io) void {
    g_rooms_mutex.lock(io) catch return;
    defer g_rooms_mutex.unlock(io);

    if (g_websocket_rooms.getPtr(room_name)) |room| {
        room.mutex.lock(io) catch return;

        const before_count = room.connections.items.len;

        // Remove connection from list
        var i: usize = 0;
        while (i < room.connections.items.len) {
            if (room.connections.items[i] == conn) {
                _ = room.connections.orderedRemove(i);
            } else {
                i += 1;
            }
        }

        const after_count = room.connections.items.len;

        // Log disconnect with remaining count (like C++ reference)
        if (before_count > after_count) {
            std.debug.print("WebSocket client disconnected from room '{s}'. Total in room: {d}\n", .{ room_name, after_count });
        }

        // Check if room is empty BEFORE unlocking
        const is_empty = room.connections.items.len == 0;

        // Unlock FIRST, then remove from HashMap
        room.mutex.unlock(io);

        // Remove room if empty and log (like C++ reference)
        if (is_empty) {
            room.connections.deinit(g_ws_allocator);
            _ = g_websocket_rooms.remove(room_name);
            std.debug.print("Room '{s}' removed (empty).\n", .{room_name});
        }
    }
}

// Broadcast message to all connections in a room
fn broadcastToRoom(room_name: []const u8, message: []const u8, io: std.Io) void {
    g_rooms_mutex.lock(io) catch return;
    defer g_rooms_mutex.unlock(io);

    if (g_websocket_rooms.getPtr(room_name)) |room| {
        room.mutex.lock(io) catch return;
        defer room.mutex.unlock(io);

        // Copy payload to separate buffer to avoid aliasing
        var payload_copy: [4096]u8 = undefined;
        const copy_len = @min(message.len, payload_copy.len);
        @memcpy(payload_copy[0..copy_len], message[0..copy_len]);
        const payload_to_send = payload_copy[0..copy_len];

        // Serialize frame ONCE before any writes
        var frame_buffer: [4096]u8 = undefined;
        const frame_len = serializeWebSocketFrame(&frame_buffer, .text, payload_to_send);
        const frame_data = frame_buffer[0..frame_len];

        // Collect failed connections (don't modify list while iterating)
        var failed_indices: [64]usize = undefined;
        var failed_count: usize = 0;

        // Write to all clients
        var i: usize = 0;
        while (i < room.connections.items.len) {
            const conn = room.connections.items[i];

            // Each connection needs its own writer buffer
            var write_buffer: [4096]u8 = undefined;
            var conn_writer = conn.stream.writer(conn.io, &write_buffer);
            conn_writer.interface.writeAll(frame_data) catch {
                // Connection failed, mark for removal
                if (failed_count < failed_indices.len) {
                    failed_indices[failed_count] = i;
                    failed_count += 1;
                }
                i += 1;
                continue;
            };
            conn_writer.interface.flush() catch {
                if (failed_count < failed_indices.len) {
                    failed_indices[failed_count] = i;
                    failed_count += 1;
                }
                i += 1;
                continue;
            };
            i += 1;
        }

        // Remove failed connections (in reverse order to preserve indices)
        var fi: usize = failed_count;
        while (fi > 0) {
            fi -= 1;
            const conn = room.connections.items[failed_indices[fi]];
            _ = room.connections.orderedRemove(failed_indices[fi]);
            g_ws_allocator.destroy(conn);
        }

        // Remove room if empty
        if (room.connections.items.len == 0) {
            room.connections.deinit(g_ws_allocator);
            _ = g_websocket_rooms.remove(room_name);
        }
    }
}

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

        // Check for WebSocket upgrade request (/chat/{room_name})
        const is_chat_path = std.mem.startsWith(u8, path, "/chat/");
        const is_get_method = request.head.method == .GET;

        if (is_chat_path and is_get_method) {
            // Extract room name from path
            const room_name = path[6..];
            if (room_name.len == 0) {
                request.respond("Bad Request", .{
                    .status = std.http.Status.bad_request,
                    .keep_alive = true,
                    .version = std.http.Version.@"HTTP/1.1",
                }) catch break;
                break;
            }

            // Check for WebSocket upgrade headers
            var sec_ws_key: ?[]const u8 = null;
            var is_upgrade = false;
            var it = request.iterateHeaders();
            while (it.next()) |header| {
                if (std.ascii.eqlIgnoreCase(header.name, "upgrade")) {
                    is_upgrade = std.mem.indexOfScalar(u8, header.value, 'w') != null;
                } else if (std.ascii.eqlIgnoreCase(header.name, "sec-websocket-key")) {
                    sec_ws_key = header.value;
                }
            }

            if (is_upgrade and sec_ws_key != null) {
                // Perform WebSocket handshake
                var accept_buffer: [64]u8 = undefined;
                const accept_key = computeWebSocketAccept(sec_ws_key.?, &accept_buffer) catch {
                    break;
                };

                // Send handshake response
                var handshake_response: [512]u8 = undefined;
                const handshake_slice = std.fmt.bufPrint(&handshake_response,
                    "HTTP/1.1 101 Switching Protocols\r\n" ++
                        "Upgrade: websocket\r\n" ++
                        "Connection: Upgrade\r\n" ++
                        "Sec-WebSocket-Accept: {s}\r\n" ++
                        "\r\n",
                    .{accept_key},
                ) catch break;

                // Use stream.writer for raw WebSocket communication
                var ws_writer = stream.writer(io, &send_buffer);
                ws_writer.interface.writeAll(handshake_slice) catch break;
                ws_writer.interface.flush() catch break;

                // Create connection on HEAP, not stack
                const ws_conn = g_ws_allocator.create(WebSocketConnection) catch break;
                ws_conn.* = .{
                    .stream = stream,
                    .io = io,
                    .room = room_name,
                };
                defer g_ws_allocator.destroy(ws_conn);

                // Add to room
                addConnectionToRoom(room_name, ws_conn, io);

                // WebSocket frame loop (per-room broadcast)
                var frame_buffer: [CLIENT_REQUEST_BUFFER_SIZE]u8 = undefined;
                var buffer_used: usize = 0;
                while (true) {
                    // Read more data if needed - use readSliceShort, NOT stream()
                    if (buffer_used < frame_buffer.len - 1) {
                        // Create a temporary writer to receive data into frame_buffer
                        var temp_writer = std.Io.Writer.fixed(frame_buffer[buffer_used..]);
                        const bytes_read = conn_reader.interface.stream(&temp_writer, .unlimited) catch break;
                        if (bytes_read == 0) break;
                        buffer_used += bytes_read;
                    }

                    // Parse WebSocket frames
                    var offset: usize = 0;
                    while (offset < buffer_used) {
                        const result = parseWebSocketFrame(frame_buffer[offset..]) orelse break;
                        const frame = result.frame;
                        const consumed = result.consumed;

                        // Handle different opcodes
                        switch (frame.opcode) {
                            .text, .binary => {
                                // Broadcast to all connections in same room
                                broadcastToRoom(room_name, frame.payload, io);
                            },
                            .ping => {
                                // Respond with PONG
                                var pong_buffer: [4096]u8 = undefined;
                                const pong_len = serializeWebSocketFrame(&pong_buffer, .pong, frame.payload);
                                conn_writer.interface.writeAll(pong_buffer[0..pong_len]) catch break;
                                conn_writer.interface.flush() catch break;
                            },
                            .pong => {
                                // Ignore PONG
                            },
                            .close => {
                                // Send close frame back and close connection
                                var close_buffer: [4096]u8 = undefined;
                                const close_len = serializeWebSocketFrame(&close_buffer, .close, &.{});
                                conn_writer.interface.writeAll(close_buffer[0..close_len]) catch {};
                                conn_writer.interface.flush() catch {};
                                break;
                            },
                            .continuation => {
                                // Not handling fragmented messages
                            },
                        }
                        offset += consumed;
                    }

                    // Remove processed data from buffer
                    if (offset > 0 and offset < buffer_used) {
                        @memmove(frame_buffer[0 .. buffer_used - offset], frame_buffer[offset..buffer_used]);
                        buffer_used -= offset;
                    } else if (offset >= buffer_used) {
                        buffer_used = 0;
                    }
                }

                // Clean up connection
                removeConnectionFromRoom(room_name, ws_conn, io);
                break;
            }
            // Not a WebSocket request, fall through to normal HTTP
        }

        // Dynamic route handling FIRST (like C++ reference - check routes before static files)
        var handled = false;

        if (std.mem.eql(u8, path, "/zig")) {
            // Async respond: suspends until write completes
            // respond() internally handles discardBody() for keep-alive
            request.respond("home", .{
                .status = std.http.Status.ok,
                .keep_alive = true,
                .version = std.http.Version.@"HTTP/1.1",
            }) catch break;
            handled = true;
        } else if (std.mem.eql(u8, path, "/zig/json")) {
            request.respond("{\"string\":\"string\",\"decimal\":3.14,\"round\":69,\"boolean\":true}", .{
                .status = std.http.Status.ok,
                .keep_alive = true,
                .version = std.http.Version.@"HTTP/1.1",
            }) catch break;
            handled = true;
        } else if (std.mem.eql(u8, path, "/zig/echo")) {
            // Build JSON response for query parameters
            var response_body: std.ArrayList(u8) = .empty;
            defer response_body.deinit(std.heap.smp_allocator);
            if (query.len == 0) {
                // No params — return null
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

            handled = true;
        } else if (std.mem.startsWith(u8, path, "/zig/")) {
            // Dynamic path routing: /zig/{path1}/{path2}/...
            var response_body: std.ArrayList(u8) = .empty;
            defer response_body.deinit(std.heap.smp_allocator);

            // Build: "value path: /zig/{segments}"
            response_body.appendSlice(std.heap.smp_allocator, "value path: ") catch break;
            response_body.appendSlice(std.heap.smp_allocator, path) catch break;

            request.respond(response_body.items, .{
                .status = std.http.Status.ok,
                .keep_alive = true,
                .version = std.http.Version.@"HTTP/1.1",
            }) catch break;

            handled = true;
        }

        // Try static files ONLY if no dynamic route matched (like C++ reference else block)
        if (!handled) {
            if (std.mem.startsWith(u8, path, "/")) {
                const sub_path = path[1..];
                const file_served = serveStaticFile(&request, sub_path, io) catch false;
                if (file_served) {
                    // File served successfully, continue to next request
                    continue;
                }
            }

            // No route matched and static file not found — send 404
            request.respond("Not Found", .{
                .status = std.http.Status.not_found,
                .keep_alive = true,
                .version = std.http.Version.@"HTTP/1.1",
            }) catch break;
        }

        // REMOVED: discardBody() is private - respond() handles it internally?
    }
}

// --------------------------------------------------------- //

// Async-aware server loop with io.concurrent() for task spawning
fn runServer(io: std.Io) !void {
    const addr = try std.Io.net.IpAddress.resolve(io, ADDRESS_IP, ADDRESS_PORT);
    var tcp_server = try addr.listen(io, .{
        .mode = .stream,
        .kernel_backlog = 1024,
        .reuse_address = true,
    });
    defer tcp_server.deinit(io);

    // Initialize global WebSocket room map
    g_websocket_rooms = std.StringHashMap(WebSocketRoom).init(std.heap.smp_allocator);
    g_ws_allocator = std.heap.smp_allocator;

    std.debug.print("backend_zig_async: run on {s}:{d}\n", .{ ADDRESS_IP, ADDRESS_PORT });
    std.debug.print("  - HTTP endpoint: /zig\n", .{});
    std.debug.print("  - JSON endpoint: /zig/json\n", .{});
    std.debug.print("  - Echo endpoint: /zig/echo?text=hello (query parameters)\n", .{});
    std.debug.print("  - Dynamic path: /zig/{{path1}}/{{path2}}/... (except /zig/json)\n", .{});
    std.debug.print("  - WebSocket endpoint: /chat/{{room_name}} (per room broadcast)\n", .{});
    std.debug.print("  - Static files from ./public at root (e.g., /image.jpg)\n", .{});

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

    try runServer(io.io());
}

//
// IMPORTANT:
// this implementation result:
// → wrk -c100 -t6 -d10s http://localhost:9007/zig
// Running 10s test @ http://localhost:9007/zig
//   6 threads and 100 connections
//   Thread Stats   Avg      Stdev     Max   +/- Stdev
//     Latency   249.91us  527.67us  24.95ms   95.42%
//     Req/Sec    67.15k    14.19k  105.54k    63.52%
//   4028055 requests in 10.10s, 161.34MB read
// Requests/sec: 398844.18
// Transfer/sec:     15.98MB
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

