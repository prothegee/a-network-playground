use std::collections::{HashMap, VecDeque};
use std::convert::TryInto;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::{atomic::{AtomicBool, Ordering}, Arc, Condvar, Mutex};
use std::thread;

const ADDRESS_IP: &str = "0.0.0.0";
const ADDRESS_PORT: u16 = 9006;
const CONNECTION_CONCURRENT_TARGET: usize = 256;
const CLIENT_REQUEST_BUFFER_SIZE: usize = 8192;

// --------------------------------------------------------- //

/*
TODO:
[X] multi-threaded http server
[X] threadpool for scalability
[X] blocking I/O (not epoll)
[X] keep-alive (manual)
[X] websocket support
[X] query parameters parsing
[?] not full http spec
[?] not async / non-blocking server
*/

// --------------------------------------------------------- //

// Global WebSocket connections per room (like Drogon's g_connections)
type RoomName = String;
type WebSocketPeers = Arc<Mutex<HashMap<RoomName, Vec<std::net::TcpStream>>>>;

// --------------------------------------------------------- //

// SHA-1 implementation for WebSocket handshake
struct Sha1 {
    state: [u32; 5],
    count: u64,
    buffer: [u8; 64],
}

impl Sha1 {
    fn new() -> Self {
        Sha1 {
            state: [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0],
            count: 0,
            buffer: [0; 64],
        }
    }

    #[inline]
    fn update(&mut self, data: &[u8]) {
        for &byte in data {
            self.buffer[(self.count % 64) as usize] = byte;
            self.count += 1;
            if self.count % 64 == 0 {
                self.transform();
            }
        }
    }

    #[inline]
    fn transform(&mut self) {
        let mut w = [0u32; 80];
        for i in 0..16 {
            w[i] = u32::from_be_bytes([
                self.buffer[i * 4],
                self.buffer[i * 4 + 1],
                self.buffer[i * 4 + 2],
                self.buffer[i * 4 + 3],
            ]);
        }

        for i in 16..80 {
            let val = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = val.rotate_left(1);
        }

        let (mut a, mut b, mut c, mut d, mut e) = (
            self.state[0],
            self.state[1],
            self.state[2],
            self.state[3],
            self.state[4],
        );

        for i in 0..80 {
            let (f, k) = if i < 20 {
                ((b & c) | (!b & d), 0x5A827999)
            } else if i < 40 {
                (b ^ c ^ d, 0x6ED9EBA1)
            } else if i < 60 {
                ((b & c) | (b & d) | (c & d), 0x8F1BBCDC)
            } else {
                (b ^ c ^ d, 0xCA62C1D6)
            };

            let temp = a
                .rotate_left(5)
                .wrapping_add(f)
                .wrapping_add(e)
                .wrapping_add(k)
                .wrapping_add(w[i]);
            e = d;
            d = c;
            c = b.rotate_left(30);
            b = a;
            a = temp;
        }

        self.state[0] = self.state[0].wrapping_add(a);
        self.state[1] = self.state[1].wrapping_add(b);
        self.state[2] = self.state[2].wrapping_add(c);
        self.state[3] = self.state[3].wrapping_add(d);
        self.state[4] = self.state[4].wrapping_add(e);
    }

    fn finalize(mut self) -> [u8; 20] {
        let bit_len = self.count * 8;
        let pad_len = 64 - (self.count % 64);
        let pad_len = if pad_len < 9 { pad_len + 64 } else { pad_len };

        let mut padding = vec![0u8; pad_len as usize];
        padding[0] = 0x80;

        let len_bytes = bit_len.to_be_bytes();
        for i in 0..8 {
            padding[pad_len as usize - 8 + i] = len_bytes[i];
        }

        self.update(&padding);

        let mut digest = [0u8; 20];
        for i in 0..5 {
            let bytes = self.state[i].to_be_bytes();
            for j in 0..4 {
                digest[i * 4 + j] = bytes[j];
            }
        }
        digest
    }
}

// --------------------------------------------------------- //

// Base64 encoding for WebSocket accept key
#[inline]
fn base64_encode(input: &[u8]) -> String {
    const CHARSET: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut output = String::with_capacity((input.len() + 2) / 3 * 4);
    let mut i = 0;

    while i < input.len() {
        let octet_a = input[i];
        let octet_b = if i + 1 < input.len() { input[i + 1] } else { 0 };
        let octet_c = if i + 2 < input.len() { input[i + 2] } else { 0 };

        let triple = ((octet_a as u32) << 16) | ((octet_b as u32) << 8) | (octet_c as u32);

        output.push(CHARSET[((triple >> 18) & 0x3F) as usize] as char);
        output.push(CHARSET[((triple >> 12) & 0x3F) as usize] as char);
        output.push(if i + 1 < input.len() { CHARSET[((triple >> 6) & 0x3F) as usize] as char } else { '=' });
        output.push(if i + 2 < input.len() { CHARSET[(triple & 0x3F) as usize] as char } else { '=' });

        i += 3;
    }
    output
}

// --------------------------------------------------------- //

type Job = Box<dyn FnOnce() + Send + 'static>;

// Optimized ThreadPool with work-stealing and minimal contention
struct ThreadPool {
    _workers: Vec<Option<thread::JoinHandle<()>>>,
    sender: Arc<(Mutex<VecDeque<Job>>, Condvar)>,
    stop_signal: Arc<AtomicBool>,
}

impl ThreadPool {
    fn new(size: usize) -> Self {
        let shared_data = Arc::new((Mutex::new(VecDeque::with_capacity(size * 2)), Condvar::new()));
        let stop_signal = Arc::new(AtomicBool::new(false));
        let mut workers = Vec::with_capacity(size);

        for _ in 0..size {
            let shared = Arc::clone(&shared_data);
            let stop = Arc::clone(&stop_signal);

            workers.push(Some(thread::spawn(move || {
                let (lock, cvar) = &*shared;

                loop {
                    let task: Job = {
                        let mut queue = lock.lock().unwrap();

                        while !stop.load(Ordering::Acquire) && queue.is_empty() {
                            queue = cvar.wait(queue).unwrap();
                        }

                        if stop.load(Ordering::Acquire) && queue.is_empty() {
                            return;
                        }

                        match queue.pop_front() {
                            Some(t) => t,
                            None => continue,
                        }
                    };

                    task();
                }
            })));
        }

        ThreadPool {
            _workers: workers,
            sender: shared_data,
            stop_signal,
        }
    }

    #[inline]
    fn enqueue<F>(&self, f: F)
    where
        F: FnOnce() + Send + 'static,
    {
        let (lock, cvar) = &*self.sender;
        {
            let mut queue = lock.lock().unwrap();
            if self.stop_signal.load(Ordering::Relaxed) {
                return;
            }
            queue.push_back(Box::new(f));
        }
        cvar.notify_one();
    }
}

impl Drop for ThreadPool {
    fn drop(&mut self) {
        self.stop_signal.store(true, Ordering::Release);
        let (_, cvar) = &*self.sender;
        cvar.notify_all();

        for worker in &mut self._workers {
            if let Some(thread) = worker.take() {
                let _ = thread.join();
            }
        }
    }
}

// --------------------------------------------------------- //

// WebSocket Frame Structure (RFC 6455)
#[derive(Debug, PartialEq)]
enum Opcode {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
}

impl From<u8> for Opcode {
    #[inline]
    fn from(byte: u8) -> Self {
        match byte {
            0x1 => Opcode::Text,
            0x2 => Opcode::Binary,
            0x8 => Opcode::Close,
            0x9 => Opcode::Ping,
            0xA => Opcode::Pong,
            _ => Opcode::Continuation,
        }
    }
}

struct WebSocketFrame {
    fin: bool,
    opcode: Opcode,
    payload: Vec<u8>,
}

impl WebSocketFrame {
    // Parse WebSocket frame from buffer
    #[inline]
    fn parse(buffer: &[u8]) -> Result<(Self, usize), String> {
        if buffer.len() < 2 {
            return Err("Incomplete header".to_string());
        }

        let first_byte = buffer[0];

        // First byte: FIN + RSV + Opcode
        let fin = (first_byte & 0x80) != 0;
        let opcode = Opcode::from(first_byte & 0x0F);

        let second_byte = buffer[1];

        // Second byte: MASK + Payload length
        let mask = (second_byte & 0x80) != 0;
        let mut payload_len = (second_byte & 0x7F) as u64;
        let mut offset = 2;

        // Extended payload length
        if payload_len == 126 {
            if buffer.len() < 4 {
                return Err("Incomplete length".to_string());
            }
            let bytes: [u8; 2] = buffer[2..4].try_into().unwrap();
            payload_len = u16::from_be_bytes(bytes) as u64;
            offset += 2;
        } else if payload_len == 127 {
            if buffer.len() < 10 {
                return Err("Incomplete length".to_string());
            }
            let bytes: [u8; 8] = buffer[2..10].try_into().unwrap();
            payload_len = u64::from_be_bytes(bytes);
            offset += 8;
        }

        // Masking key
        let mut masking_key = [0u8; 4];
        if mask {
            if buffer.len() < offset + 4 {
                return Err("Incomplete mask".to_string());
            }
            masking_key.copy_from_slice(&buffer[offset..offset + 4]);
            offset += 4;
        }

        // Payload
        if buffer.len() < offset + payload_len as usize {
            return Err("Incomplete payload".to_string());
        }

        let mut payload = vec![0u8; payload_len as usize];
        payload.copy_from_slice(&buffer[offset..offset + payload_len as usize]);

        // Unmask payload if masked
        if mask {
            for i in 0..payload_len as usize {
                payload[i] ^= masking_key[i % 4];
            }
        }

        Ok((WebSocketFrame { fin, opcode, payload }, offset + payload_len as usize))
    }

    // Serialize WebSocket frame to buffer
    #[inline]
    fn serialize(&self) -> Vec<u8> {
        let payload_len = self.payload.len();
        let header_len = if payload_len <= 125 {
            2
        } else if payload_len <= 65535 {
            4
        } else {
            10
        };

        // Pre-allocate buffer to avoid reallocations
        let mut buffer = Vec::with_capacity(header_len + payload_len);

        // First byte: FIN + Opcode
        let b1 = (if self.fin { 0x80 } else { 0x00 }) | (match self.opcode {
            Opcode::Continuation => 0x0,
            Opcode::Text => 0x1,
            Opcode::Binary => 0x2,
            Opcode::Close => 0x8,
            Opcode::Ping => 0x9,
            Opcode::Pong => 0xA,
        });
        buffer.push(b1);

        // Second byte and extended length: Payload length (no mask for server->client)
        if payload_len <= 125 {
            buffer.push(payload_len as u8);
        } else if payload_len <= 65535 {
            buffer.push(126);
            buffer.extend_from_slice(&(payload_len as u16).to_be_bytes());
        } else {
            buffer.push(127);
            buffer.extend_from_slice(&(payload_len as u64).to_be_bytes());
        }

        // Payload
        buffer.extend_from_slice(&self.payload);
        buffer
    }

    #[inline]
    fn create_pong(payload: Vec<u8>) -> Self {
        WebSocketFrame { fin: true, opcode: Opcode::Pong, payload }
    }

    #[inline]
    fn create_close() -> Self {
        WebSocketFrame { fin: true, opcode: Opcode::Close, payload: vec![] }
    }
}

// --------------------------------------------------------- //

// Fast query parameter parsing
fn parse_query_params(query: &[u8]) -> Vec<(String, String)> {
    let mut params = Vec::new();
    if query.is_empty() {
        return params;
    }
    let mut i = 0;
    let len = query.len();

    while i < len {
        // Find key
        let key_start = i;
        while i < len && query[i] != b'=' && query[i] != b'&' {
            i += 1;
        }
        let key_end = i;
        let key = if key_start < key_end {
            percent_decode(&query[key_start..key_end])
        } else {
            String::new()
        };

        // Check if we have '='
        let mut value = String::new();
        if i < len && query[i] == b'=' {
            i += 1; // skip '='
            let val_start = i;
            while i < len && query[i] != b'&' {
                i += 1;
            }
            let val_end = i;
            if val_start < val_end {
                value = percent_decode(&query[val_start..val_end]);
            }
        }

        params.push((key, value));

        // Skip '&' if present
        if i < len && query[i] == b'&' {
            i += 1;
        }
    }
    params
}

fn percent_decode(bytes: &[u8]) -> String {
    let mut result = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        match bytes[i] {
            b'%' if i + 2 < bytes.len() => {
                let hex = &bytes[i+1..i+3];
                if let Ok(byte) = u8::from_str_radix(std::str::from_utf8(hex).unwrap_or("00"), 16) {
                    result.push(byte);
                    i += 3;
                } else {
                    result.push(b'%');
                    i += 1;
                }
            }
            b'+' => {
                result.push(b' ');
                i += 1;
            }
            c => {
                result.push(c);
                i += 1;
            }
        }
    }
    String::from_utf8(result).unwrap_or_default()
}

fn split_path_query(path: &[u8]) -> (&[u8], &[u8]) {
    if let Some(qpos) = path.iter().position(|&b| b == b'?') {
        (&path[..qpos], &path[qpos+1..])
    } else {
        (path, &[])
    }
}

// --------------------------------------------------------- //

#[inline]
fn websocket_handshake(key: &str) -> String {
    let magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    let combined = format!("{}{}", key, magic);
    let mut sha1 = Sha1::new();
    sha1.update(combined.as_bytes());
    let digest = sha1.finalize();
    let accept_key = base64_encode(&digest);
    format!(
        "HTTP/1.1 101 Switching Protocols\r\n\
         Upgrade: websocket\r\n\
         Connection: Upgrade\r\n\
         Sec-WebSocket-Accept: {}\r\n\r\n",
        accept_key
    )
}

// Find value for a header key in raw buffer, returns slice into original buffer
#[inline]
fn find_header_value<'a>(buffer: &'a [u8], key: &[u8]) -> Option<&'a [u8]> {
    let mut pos = 0;
    while pos < buffer.len() {
        let line_start = pos;
        if let Some(end) = buffer[pos..].iter().position(|&b| b == b'\r') {
            let line_end = pos + end;
            if buffer[line_start..].starts_with(key) {
                let mut val_start = line_start + key.len();
                while val_start < line_end && (buffer[val_start] == b' ' || buffer[val_start] == b':' || buffer[val_start] == b'\t') {
                    val_start += 1;
                }
                return Some(&buffer[val_start..line_end]);
            }
            pos = line_end + 2;
        } else {
            break;
        }
    }
    None
}

fn handle_websocket_client(mut stream: std::net::TcpStream, room_name: String, peers: WebSocketPeers) {
    struct SocketCloser {
        stream: Option<std::net::TcpStream>,
        peer_addr: std::net::SocketAddr,
        room_name: String,
        peers: WebSocketPeers,
    }

    impl Drop for SocketCloser {
        fn drop(&mut self) {
            // Equivalent to Drogon's handleConnectionClosed
            if self.stream.is_some() {
                let mut rooms = self.peers.lock().unwrap();
                if let Some(room_peers) = rooms.get_mut(&self.room_name) {
                    let before_count = room_peers.len();
                    room_peers.retain(|peer| {
                        peer.peer_addr().map(|a| a != self.peer_addr).unwrap_or(false)
                    });
                    let after_count = room_peers.len();

                    // Only log if a connection was actually removed
                    if before_count > after_count {
                        println!("WebSocket client disconnected from room '{}'. Total in room: {}", 
                                 self.room_name, after_count);
                    }

                    // Remove room if empty and log (like C++)
                    if room_peers.is_empty() {
                        rooms.remove(&self.room_name);
                        println!("Room '{}' removed (empty).", self.room_name);
                    }
                }
            }
        }
    }

    let peer_addr = stream.peer_addr().unwrap();
    let closer = SocketCloser {
        stream: Some(stream.try_clone().unwrap()),
        peer_addr,
        room_name: room_name.clone(),
        peers: peers.clone(),
    };

    {
        let mut rooms = peers.lock().unwrap();
        let room_peers = rooms.entry(room_name.clone()).or_insert_with(Vec::new);
        if let Ok(clone) = closer.stream.as_ref().unwrap().try_clone() {
            room_peers.push(clone);
            println!("WebSocket client connected to room '{}'. Total in room: {}", room_name, room_peers.len());
        }
    }

    let mut buffer = vec![0u8; CLIENT_REQUEST_BUFFER_SIZE];
    let mut buffer_len = 0;

    loop {
        match stream.read(&mut buffer[buffer_len..]) {
            Ok(0) => break,
            Ok(n) => {
                buffer_len += n;

                let mut offset = 0;
                while offset < buffer_len {
                    match WebSocketFrame::parse(&buffer[offset..buffer_len]) {
                        Ok((frame, consumed)) => {
                            offset += consumed;

                            match frame.opcode {
                                Opcode::Text | Opcode::Binary => {
                                    // Broadcast to all clients in the same room
                                    let resp_data = frame.serialize();
                                    let rooms = peers.lock().unwrap();
                                    if let Some(room_peers) = rooms.get(&room_name) {
                                        for mut peer in room_peers.iter() {
                                            let _ = peer.write_all(&resp_data);
                                            let _ = peer.flush();
                                        }
                                    }
                                }
                                Opcode::Ping => {
                                    let pong = WebSocketFrame::create_pong(frame.payload);
                                    let _ = stream.write_all(&pong.serialize());
                                }
                                Opcode::Close => {
                                    let close = WebSocketFrame::create_close();
                                    let _ = stream.write_all(&close.serialize());
                                    buffer_len = 0;
                                    break;
                                }
                                _ => {}
                            }
                        }
                        Err(_) => break,
                    }
                }

                if offset > 0 && offset < buffer_len {
                    unsafe {
                        let src = buffer.as_ptr().add(offset);
                        let dst = buffer.as_mut_ptr();
                        std::ptr::copy(src, dst, buffer_len - offset);
                    }
                    buffer_len -= offset;
                } else if offset >= buffer_len {
                    buffer_len = 0;
                }

                if buffer_len == 0 && offset == 0 {
                    break;
                }
            }
            Err(_) => break,
        }
    }

    drop(closer);
}

// Parse HTTP method from raw buffer without String allocation
#[inline]
fn parse_method(buffer: &[u8]) -> Option<&[u8]> {
    let end = buffer.iter().position(|&b| b == b' ')?;
    Some(&buffer[..end])
}

// Parse HTTP path from raw buffer without String allocation
// returns the full path including query string (if any)
#[inline]
fn parse_path(buffer: &[u8]) -> Option<&[u8]> {
    let start = buffer.iter().position(|&b| b == b' ')? + 1;
    let end = buffer[start..].iter().position(|&b| b == b' ' || b == b'\r')?;

    Some(&buffer[start..start + end])
}

// Find end of headers in buffer, returns position after \r\n\r\n
#[inline]
fn find_headers_end(buffer: &[u8]) -> Option<usize> {
    for i in 0..buffer.len().saturating_sub(3) {
        if buffer[i] == b'\r' && buffer[i + 1] == b'\n' && buffer[i + 2] == b'\r' && buffer[i + 3] == b'\n' {
            return Some(i + 4);
        }
    }
    None
}

// Helper function to join path segments with "/"
fn join_path_segments(segments: &[&[u8]]) -> String {
    let mut result = String::new();
    for (i, segment) in segments.iter().enumerate() {
        if i > 0 {
            result.push('/');
        }
        result.push_str(&String::from_utf8_lossy(segment));
    }
    result
}

// Optimized client handler with keep-alive support
fn handle_client(mut stream: TcpStream, peers: WebSocketPeers) {
    // Set TCP_NODELAY to disable Nagle's algorithm
    let _ = stream.set_nodelay(true);

    // Stack-allocated buffer to avoid heap allocations
    let mut buffer = [0u8; CLIENT_REQUEST_BUFFER_SIZE];
    let mut used = 0usize;

    // HTTP keep-alive loop: handle multiple requests per connection
    loop {
        // Read more data if buffer is not full
        if used < buffer.len() {
            match stream.read(&mut buffer[used..]) {
                Ok(0) => break, // Connection closed by client
                Ok(n) => used += n,
                Err(_) => break, // Connection error
            }
        }

        // Process as many complete requests as possible from the buffer
        let mut processed = 0;
        while processed < used {
            // Find the end of headers for the current request
            let headers_end = match find_headers_end(&buffer[processed..used]) {
                Some(end) => processed + end,
                None => {
                    // Incomplete headers: need more data
                    break;
                }
            };

            // Parse method and full path (including query)
            let method = match parse_method(&buffer[processed..used]) {
                Some(m) => m,
                None => {
                    // Malformed request line
                    const BAD_REQUEST: &[u8] = b"HTTP/1.1 400 Bad Request\r\n\
                                                Connection: close\r\n\
                                                \r\n";
                    let _ = stream.write_all(BAD_REQUEST);
                    return; // Close connection
                }
            };

            let full_path = match parse_path(&buffer[processed..used]) {
                Some(p) => p,
                None => {
                    // Malformed request line
                    const BAD_REQUEST: &[u8] = b"HTTP/1.1 400 Bad Request\r\n\
                                                Connection: close\r\n\
                                                \r\n";
                    let _ = stream.write_all(BAD_REQUEST);
                    return;
                }
            };

            // Split path and query
            let (path, query) = split_path_query(full_path);
            let query_params = parse_query_params(query);

            // Route the request
            if method == b"GET" {
                if path == b"/rs" {
                    // Fast path for /rs
                    const HOME_RESPONSE: &[u8] = b"HTTP/1.1 200 OK\r\n\
                                                  Content-Type: text/plain\r\n\
                                                  Content-Length: 4\r\n\
                                                  Connection: keep-alive\r\n\
                                                  \r\n\
                                                  home";
                    if stream.write_all(HOME_RESPONSE).is_err() {
                        return;
                    }
                } else if path == b"/rs/json" {
                    // JSON response for /rs/json
                    const JSON_RESPONSE: &[u8] = b"HTTP/1.1 200 OK\r\n\
                                                 Content-Type: application/json\r\n\
                                                 Content-Length: 60\r\n\
                                                 Connection: keep-alive\r\n\
                                                 \r\n\
                                                 {\"string\":\"string\",\"decimal\":3.14,\"round\":69,\"boolean\":true}";
                    if stream.write_all(JSON_RESPONSE).is_err() {
                        return;
                    }
                } else if path == b"/rs/echo" {
                    // Echo endpoint with query parameter "text"
                    let response_body;

                    // Find "text" parameter
                    let text_param = query_params.iter().find(|(k, _)| k == "text");

                    if let Some((_, text_value)) = text_param {
                        response_body = format!("Echo: {}", text_value);
                    } else {
                        // Return all parameters as JSON
                        if query_params.is_empty() {
                            response_body = "{}".to_string();
                        } else {
                            let mut parts = Vec::new();
                            for (k, v) in &query_params {
                                parts.push(format!("\"{}\":\"{}\"", k, v));
                            }
                            response_body = format!("{{{}}}", parts.join(","));
                        }
                    }

                    let response = format!(
                        "HTTP/1.1 200 OK\r\n\
                         Content-Type: application/json\r\n\
                         Content-Length: {}\r\n\
                         Connection: keep-alive\r\n\
                         \r\n\
                         {}",
                        response_body.len(),
                        response_body
                    );

                    if stream.write_all(response.as_bytes()).is_err() { return; }
                    if stream.flush().is_err() { return; }
                } else if path.starts_with(b"/rs/") && !path.starts_with(b"/rs/json/") && path != b"/rs/json" {
                    // Dynamic path for /rs/* (except /rs/json and /rs/json/*)
                    // Parse path segments after /rs/
                    let mut segments: Vec<&[u8]> = Vec::new();
                    let mut start = 4; // after "/rs/"

                    while start < path.len() {
                        if let Some(end) = path[start..].iter().position(|&b| b == b'/') {
                            if start < start + end {
                                segments.push(&path[start..start + end]);
                            }
                            start += end + 1;
                        } else {
                            if start < path.len() {
                                segments.push(&path[start..]);
                            }
                            break;
                        }
                    }

                    // Create response body based on path segments
                    let path_string = join_path_segments(&segments);
                    let body = if segments.is_empty() {
                        "value path: /rs/".to_string()
                    } else {
                        format!("value path: /rs/{}", path_string)
                    };

                    // Build HTTP response
                    let response = format!(
                        "HTTP/1.1 200 OK\r\n\
                         Content-Type: text/plain\r\n\
                         Content-Length: {}\r\n\
                         Connection: keep-alive\r\n\
                         \r\n\
                         {}",
                        body.len(),
                        body
                    );

                    if stream.write_all(response.as_bytes()).is_err() {
                        return;
                    }
                } else if path.starts_with(b"/chat/") {
                    // WebSocket with room name from path
                    // Parse room name from path after /chat/
                    let room_name_bytes = &path[6..]; // after "/chat/"

                    // Validate room name
                    if room_name_bytes.is_empty() {
                        const BAD_REQUEST: &[u8] = b"HTTP/1.1 400 Bad Request\r\n\
                                                    Content-Length: 15\r\n\
                                                    Connection: close\r\n\
                                                    \r\n\
                                                    Room name empty";
                        let _ = stream.write_all(BAD_REQUEST);
                        return;
                    }

                    // Check for WebSocket upgrade
                    if let Some(key) = find_header_value(&buffer[processed..used], b"Sec-WebSocket-Key:") {
                        if let Some(upgrade) = find_header_value(&buffer[processed..used], b"Upgrade:") {
                            if upgrade.eq_ignore_ascii_case(b"websocket") {
                                if let Ok(key_str) = std::str::from_utf8(key) {
                                    let response = websocket_handshake(key_str);
                                    if stream.write_all(response.as_bytes()).is_ok() {
                                        // Convert room name to String
                                        let room_name = String::from_utf8_lossy(room_name_bytes).to_string();
                                        // Switch to WebSocket mode with room name
                                        handle_websocket_client(stream, room_name, peers);
                                        return;
                                    }
                                }
                            }
                        }
                    }
                    // Not a WebSocket request
                    const NOT_FOUND: &[u8] = b"HTTP/1.1 404 Not Found\r\n\
                                              Content-Length: 9\r\n\
                                              Connection: keep-alive\r\n\
                                              \r\n\
                                              Not Found";
                    if stream.write_all(NOT_FOUND).is_err() {
                        return;
                    }
                } else {
                    const NOT_FOUND: &[u8] = b"HTTP/1.1 404 Not Found\r\n\
                                              Content-Length: 9\r\n\
                                              Connection: keep-alive\r\n\
                                              \r\n\
                                              Not Found";
                    if stream.write_all(NOT_FOUND).is_err() {
                        return;
                    }
                }
            } else {
                const METHOD_NOT_ALLOWED: &[u8] = b"HTTP/1.1 405 Method Not Allowed\r\n\
                                                   Content-Length: 18\r\n\
                                                   Connection: keep-alive\r\n\
                                                   \r\n\
                                                   Method Not Allowed";
                if stream.write_all(METHOD_NOT_ALLOWED).is_err() {
                    return;
                }
            }

            // Move to next request in buffer
            processed = headers_end;
        }

        // Shift remaining data to the beginning of the buffer
        if processed > 0 {
            if processed < used {
                buffer.copy_within(processed..used, 0);
                used -= processed;
            } else {
                used = 0;
            }
        } else {
            // No complete request found: if buffer is full, it's a bad request
            if used == buffer.len() {
                const BAD_REQUEST: &[u8] = b"HTTP/1.1 400 Bad Request\r\n\
                                            Connection: close\r\n\
                                            \r\n";
                let _ = stream.write_all(BAD_REQUEST);
                return;
            }
            // Otherwise, continue reading more data
        }
    }
}

// --------------------------------------------------------- //

fn main() {
    let listener = TcpListener::bind(format!("{}:{}", ADDRESS_IP, ADDRESS_PORT))
        .expect("Failed to bind port");

    // Set larger listen backlog
    let _ = listener.set_ttl(64);

    let pool = ThreadPool::new(CONNECTION_CONCURRENT_TARGET);
    let peers: WebSocketPeers = Arc::new(Mutex::new(HashMap::new()));

    println!("backend_rs: run on {}:{}", ADDRESS_IP, ADDRESS_PORT);
    println!("  - HTTP endpoint: /rs");
    println!("  - JSON endpoint: /rs/json");
    println!("  - Echo endpoint: /rs/echo?text=hello (query parameters)");
    println!("  - Dynamic path: /rs/{{path1}}/{{path2}}/... (except /rs/json)");
    println!("  - WebSocket endpoint: /chat/{{room_name}} (per room broadcast)");

    for stream in listener.incoming() {
        match stream {
            Ok(stream) => {
                let peers = Arc::clone(&peers);
                pool.enqueue(move || {
                    handle_client(stream, peers);
                });
            }
            Err(e) => {
                eprintln!("Connection failed: {}", e);
            }
        }
    }
}

/*
IMPORTANT:
this implementation result:
➜ wrk -c 100 -t 6 -d 10s http://localhost:9006/rs
Running 10s test @ http://localhost:9006/rs
  6 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   239.34us  497.83us   9.61ms   92.60%
    Req/Sec    81.77k     7.27k  141.37k    79.40%
  4897314 requests in 10.10s, 429.68MB read
Requests/sec: 484917.72
Transfer/sec:     42.55MB
*/
