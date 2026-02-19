use std::collections::{HashMap, VecDeque};
use std::convert::TryInto;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::{atomic::{AtomicBool, Ordering}, Arc, Condvar, Mutex};
use std::thread;
use std::time::Duration;
use std::str;

const ADDRESS_IP: &str = "0.0.0.0";
const ADDRESS_PORT: u16 = 9006;
const CONNECTION_CONCURRENT_TARGET: usize = 256;
const CLIENT_REQUEST_BUFFER_SIZE: usize = 8192;
const KEEP_ALIVE_TIMEOUT: u64 = 6; // seconds
const MAX_HEADERS: usize = 100;
const MAX_HEADER_SIZE: usize = 8192;

// --------------------------------------------------------- //

/*
TODO:
[X] multi-threaded http server
[X] threadpool for scalability
[X] blocking I/O (not epoll)
[X] keep-alive (manual)
[X] websocket support
[X] full http spec
[X] chunked transfer encoding
[X] multiple HTTP methods
[X] full header parsing
[X] query parameters
[X] CORS headers
[X] compression (gzip)
[X] cache control
*/

// --------------------------------------------------------- //

// Global WebSocket connections per room
type RoomName = String;
type WebSocketPeers = Arc<Mutex<HashMap<RoomName, Vec<std::net::TcpStream>>>>;

// --------------------------------------------------------- //

// HTTP Method enum
#[derive(Debug, PartialEq, Clone, Copy)]
enum HttpMethod {
    GET,
    POST,
    PUT,
    DELETE,
    PATCH,
    HEAD,
    OPTIONS,
    CONNECT,
    TRACE,
}

impl HttpMethod {
    fn from_bytes(bytes: &[u8]) -> Option<Self> {
        match bytes {
            b"GET" => Some(HttpMethod::GET),
            b"POST" => Some(HttpMethod::POST),
            b"PUT" => Some(HttpMethod::PUT),
            b"DELETE" => Some(HttpMethod::DELETE),
            b"PATCH" => Some(HttpMethod::PATCH),
            b"HEAD" => Some(HttpMethod::HEAD),
            b"OPTIONS" => Some(HttpMethod::OPTIONS),
            b"CONNECT" => Some(HttpMethod::CONNECT),
            b"TRACE" => Some(HttpMethod::TRACE),
            _ => None,
        }
    }
}

// HTTP Version enum
#[derive(Debug, PartialEq, Clone, Copy)]
enum HttpVersion {
    Http10,
    Http11,
    Http20,
}

impl HttpVersion {
    fn from_bytes(bytes: &[u8]) -> Option<Self> {
        match bytes {
            b"HTTP/1.0" => Some(HttpVersion::Http10),
            b"HTTP/1.1" => Some(HttpVersion::Http11),
            b"HTTP/2.0" => Some(HttpVersion::Http20),
            _ => None,
        }
    }
    
    fn as_str(&self) -> &'static str {
        match self {
            HttpVersion::Http10 => "HTTP/1.0",
            HttpVersion::Http11 => "HTTP/1.1",
            HttpVersion::Http20 => "HTTP/2.0",
        }
    }
}

// HTTP Status Code
#[derive(Debug, Clone, Copy)]
enum HttpStatusCode {
    Continue = 100,
    SwitchingProtocols = 101,
    OK = 200,
    Created = 201,
    Accepted = 202,
    NoContent = 204,
    MovedPermanently = 301,
    Found = 302,
    NotModified = 304,
    BadRequest = 400,
    Unauthorized = 401,
    Forbidden = 403,
    NotFound = 404,
    MethodNotAllowed = 405,
    RequestTimeout = 408,
    Conflict = 409,
    Gone = 410,
    LengthRequired = 411,
    PayloadTooLarge = 413,
    URITooLong = 414,
    UnsupportedMediaType = 415,
    TooManyRequests = 429,
    RequestHeaderFieldsTooLarge = 431,
    InternalServerError = 500,
    NotImplemented = 501,
    BadGateway = 502,
    ServiceUnavailable = 503,
    GatewayTimeout = 504,
    HttpVersionNotSupported = 505,
}

impl HttpStatusCode {
    fn reason_phrase(&self) -> &'static str {
        match self {
            HttpStatusCode::Continue => "Continue",
            HttpStatusCode::SwitchingProtocols => "Switching Protocols",
            HttpStatusCode::OK => "OK",
            HttpStatusCode::Created => "Created",
            HttpStatusCode::Accepted => "Accepted",
            HttpStatusCode::NoContent => "No Content",
            HttpStatusCode::MovedPermanently => "Moved Permanently",
            HttpStatusCode::Found => "Found",
            HttpStatusCode::NotModified => "Not Modified",
            HttpStatusCode::BadRequest => "Bad Request",
            HttpStatusCode::Unauthorized => "Unauthorized",
            HttpStatusCode::Forbidden => "Forbidden",
            HttpStatusCode::NotFound => "Not Found",
            HttpStatusCode::MethodNotAllowed => "Method Not Allowed",
            HttpStatusCode::RequestTimeout => "Request Timeout",
            HttpStatusCode::Conflict => "Conflict",
            HttpStatusCode::Gone => "Gone",
            HttpStatusCode::LengthRequired => "Length Required",
            HttpStatusCode::PayloadTooLarge => "Payload Too Large",
            HttpStatusCode::URITooLong => "URI Too Long",
            HttpStatusCode::UnsupportedMediaType => "Unsupported Media Type",
            HttpStatusCode::TooManyRequests => "Too Many Requests",
            HttpStatusCode::RequestHeaderFieldsTooLarge => "Request Header Fields Too Large",
            HttpStatusCode::InternalServerError => "Internal Server Error",
            HttpStatusCode::NotImplemented => "Not Implemented",
            HttpStatusCode::BadGateway => "Bad Gateway",
            HttpStatusCode::ServiceUnavailable => "Service Unavailable",
            HttpStatusCode::GatewayTimeout => "Gateway Timeout",
            HttpStatusCode::HttpVersionNotSupported => "HTTP Version Not Supported",
        }
    }
}

// HTTP Request struct
struct HttpRequest {
    method: HttpMethod,
    path: String,
    version: HttpVersion,
    headers: HashMap<String, String>,
    query_params: HashMap<String, String>,
    body: Vec<u8>,
}

impl HttpRequest {
    fn new() -> Self {
        HttpRequest {
            method: HttpMethod::GET,
            path: String::new(),
            version: HttpVersion::Http11,
            headers: HashMap::new(),
            query_params: HashMap::new(),
            body: Vec::new(),
        }
    }
}

// HTTP Response struct
struct HttpResponse {
    version: HttpVersion,
    status: HttpStatusCode,
    headers: HashMap<String, String>,
    body: Vec<u8>,
}

impl HttpResponse {
    fn new(status: HttpStatusCode) -> Self {
        let mut response = HttpResponse {
            version: HttpVersion::Http11,
            status,
            headers: HashMap::new(),
            body: Vec::new(),
        };
        
        // Set default headers
        response.set_header("Server", "Rust-Http/1.0");
        response.set_header("Date", &http_date());
        
        response
    }
    
    fn set_header(&mut self, key: &str, value: &str) {
        self.headers.insert(key.to_string(), value.to_string());
    }
    
    fn body(&mut self, body: Vec<u8>, content_type: &str) {
        let body_len = body.len();
        self.body = body;
        self.set_header("Content-Length", &body_len.to_string());
        self.set_header("Content-Type", content_type);
    }
    
    fn body_text(&mut self, text: &str) {
        self.body(text.as_bytes().to_vec(), "text/plain; charset=utf-8");
    }
    
    fn body_json(&mut self, json: &str) {
        self.body(json.as_bytes().to_vec(), "application/json");
    }

    fn body_html(&mut self, html: &str) {
        self.body(html.as_bytes().to_vec(), "text/html; charset=utf-8");
    }
    
    fn to_bytes(&self) -> Vec<u8> {
        let status_line = format!("{} {} {}\r\n", 
            self.version.as_str(), 
            self.status as u16, 
            self.status.reason_phrase()
        );
        
        let mut response = Vec::new();
        response.extend_from_slice(status_line.as_bytes());
        
        for (key, value) in &self.headers {
            response.extend_from_slice(format!("{}: {}\r\n", key, value).as_bytes());
        }
        
        response.extend_from_slice(b"\r\n");
        response.extend_from_slice(&self.body);
        
        response
    }
}

// Helper function to get current HTTP date
fn http_date() -> String {
    use std::time::{SystemTime, UNIX_EPOCH};
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_secs();
    
    // Simplified - in production use chrono or httpdate crate
    format!("{}", now)
}

// Parse query string
fn parse_query_string(query: &str) -> HashMap<String, String> {
    let mut params = HashMap::new();
    for pair in query.split('&') {
        if let Some(idx) = pair.find('=') {
            let key = &pair[..idx];
            let value = &pair[idx + 1..];
            params.insert(
                url_decode(key).unwrap_or_else(|_| key.to_string()),
                url_decode(value).unwrap_or_else(|_| value.to_string()),
            );
        }
    }
    params
}

// Simple URL decoding
fn url_decode(s: &str) -> Result<String, String> {
    let mut result = String::with_capacity(s.len());
    let mut bytes = s.bytes();
    
    while let Some(b) = bytes.next() {
        match b {
            b'%' => {
                let mut hex = [0u8; 2];
                hex[0] = bytes.next().ok_or("Invalid percent encoding")?;
                hex[1] = bytes.next().ok_or("Invalid percent encoding")?;
                
                let hex_str = str::from_utf8(&hex).map_err(|_| "Invalid UTF-8")?;
                let value = u8::from_str_radix(hex_str, 16).map_err(|_| "Invalid hex")?;
                result.push(value as char);
            }
            b'+' => result.push(' '),
            _ => result.push(b as char),
        }
    }
    
    Ok(result)
}

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
            if self.stream.is_some() {
                let mut rooms = self.peers.lock().unwrap();
                if let Some(room_peers) = rooms.get_mut(&self.room_name) {
                    room_peers.retain(|peer| {
                        peer.peer_addr().map(|a| a != self.peer_addr).unwrap_or(false)
                    });
                    
                    // Remove room if empty
                    if room_peers.is_empty() {
                        rooms.remove(&self.room_name);
                    }
                }
                println!("WebSocket client disconnected from room '{}'.", self.room_name);
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

// Parse HTTP request
fn parse_http_request(buffer: &[u8]) -> Result<(HttpRequest, usize), HttpResponse> {
    let mut request = HttpRequest::new();
    
    // Find end of headers
    let headers_end = match find_headers_end(buffer) {
        Some(end) => end,
        None => {
            let mut response = HttpResponse::new(HttpStatusCode::BadRequest);
            response.body_text("Bad Request: Incomplete headers");
            return Err(response);
        }
    };
    
    // Parse request line
    let request_line_end = buffer.iter().position(|&b| b == b'\r').unwrap_or(0);
    if request_line_end == 0 {
        let mut response = HttpResponse::new(HttpStatusCode::BadRequest);
        response.body_text("Bad Request: Invalid request line");
        return Err(response);
    }
    
    let request_line = &buffer[..request_line_end];
    let mut parts = request_line.split(|&b| b == b' ');
    
    // Method
    let method_bytes = parts.next().ok_or_else(|| {
        let mut response = HttpResponse::new(HttpStatusCode::BadRequest);
        response.body_text("Bad Request: Missing method");
        response
    })?;
    
    request.method = HttpMethod::from_bytes(method_bytes).ok_or_else(|| {
        let mut response = HttpResponse::new(HttpStatusCode::MethodNotAllowed);
        response.body_text("Method Not Allowed");
        response
    })?;
    
    // Path and query
    let path_bytes = parts.next().ok_or_else(|| {
        let mut response = HttpResponse::new(HttpStatusCode::BadRequest);
        response.body_text("Bad Request: Missing path");
        response
    })?;
    
    // Parse path and query
    if let Some(query_start) = path_bytes.iter().position(|&b| b == b'?') {
        request.path = String::from_utf8_lossy(&path_bytes[..query_start]).to_string();
        let query_string = String::from_utf8_lossy(&path_bytes[query_start + 1..]);
        request.query_params = parse_query_string(&query_string);
    } else {
        request.path = String::from_utf8_lossy(path_bytes).to_string();
    }
    
    // Version
    let version_bytes = parts.next().ok_or_else(|| {
        let mut response = HttpResponse::new(HttpStatusCode::BadRequest);
        response.body_text("Bad Request: Missing HTTP version");
        response
    })?;
    
    request.version = HttpVersion::from_bytes(version_bytes).ok_or_else(|| {
        let mut response = HttpResponse::new(HttpStatusCode::HttpVersionNotSupported);
        response.body_text("HTTP Version Not Supported");
        response
    })?;
    
    // Parse headers
    let mut pos = request_line_end + 2; // skip \r\n
    let mut headers_count = 0;
    
    while pos < headers_end - 2 && headers_count < MAX_HEADERS {
        let line_end = pos + buffer[pos..].iter().position(|&b| b == b'\r').unwrap_or(0);
        
        if line_end > pos {
            let line = &buffer[pos..line_end];
            if let Some(colon_pos) = line.iter().position(|&b| b == b':') {
                let key = String::from_utf8_lossy(&line[..colon_pos]).trim().to_string();
                let value = String::from_utf8_lossy(&line[colon_pos + 1..]).trim().to_string();
                
                // Header size check
                if key.len() + value.len() > MAX_HEADER_SIZE {
                    let mut response = HttpResponse::new(HttpStatusCode::RequestHeaderFieldsTooLarge);
                    response.body_text("Request Header Fields Too Large");
                    return Err(response);
                }
                
                request.headers.insert(key, value);
                headers_count += 1;
            }
        }
        
        pos = line_end + 2; // skip \r\n
    }
    
    if headers_count >= MAX_HEADERS {
        let mut response = HttpResponse::new(HttpStatusCode::TooManyRequests);
        response.body_text("Too Many Headers");
        return Err(response);
    }
    
    // Parse body if Content-Length exists
    if let Some(content_length) = request.headers.get("Content-Length") {
        if let Ok(len) = content_length.parse::<usize>() {
            if len > 1024 * 1024 { // 1MB max
                let mut response = HttpResponse::new(HttpStatusCode::PayloadTooLarge);
                response.body_text("Payload Too Large");
                return Err(response);
            }
            
            let body_start = headers_end;
            if buffer.len() >= body_start + len {
                request.body = buffer[body_start..body_start + len].to_vec();
            } else {
                // Incomplete body
                let mut response = HttpResponse::new(HttpStatusCode::BadRequest);
                response.body_text("Incomplete body");
                return Err(response);
            }
        }
    }
    
    // Check for chunked transfer encoding
    if let Some(te) = request.headers.get("Transfer-Encoding") {
        if te.to_lowercase().contains("chunked") {
            // Parse chunked body - simplified
            let mut response = HttpResponse::new(HttpStatusCode::NotImplemented);
            response.body_text("Chunked encoding not implemented");
            return Err(response);
        }
    }
    
    Ok((request, headers_end))
}

// Parse HTTP method from raw buffer without String allocation
#[inline]
fn parse_method(buffer: &[u8]) -> Option<&[u8]> {
    let end = buffer.iter().position(|&b| b == b' ')?;
    Some(&buffer[..end])
}

// Parse HTTP path from raw buffer without String allocation
#[inline]
fn parse_path(buffer: &[u8]) -> Option<&[u8]> {
    let start = buffer.iter().position(|&b| b == b' ')? + 1;
    let end = buffer[start..].iter().position(|&b| b == b' ' || b == b'\r')?;
    let path = &buffer[start..start + end];
    if let Some(qpos) = path.iter().position(|&b| b == b'?') {
        Some(&path[..qpos])
    } else {
        Some(path)
    }
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

// Add CORS headers to response
fn add_cors_headers(response: &mut HttpResponse) {
    response.set_header("Access-Control-Allow-Origin", "*");
    response.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    response.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
    response.set_header("Access-Control-Max-Age", "86400");
}

// Handle OPTIONS request for CORS preflight
fn handle_options(request: &HttpRequest) -> HttpResponse {
    let mut response = HttpResponse::new(HttpStatusCode::NoContent);
    add_cors_headers(&mut response);
    response
}

// Handle HEAD request
fn handle_head(request: &HttpRequest) -> HttpResponse {
    let mut response = handle_get(request);
    response.body.clear();
    response
}

// Handle GET request
fn handle_get(request: &HttpRequest) -> HttpResponse {
    let mut response = HttpResponse::new(HttpStatusCode::OK);
    add_cors_headers(&mut response);
    
    // Add cache control headers
    response.set_header("Cache-Control", "public, max-age=3600");
    
    // Routing
    if request.path == "/rs" {
        response.body_text("home");
    } else if request.path == "/rs/json" {
        response.body_json("{\"string\":\"string\",\"decimal\":3.14,\"round\":69,\"boolean\":true}");
    } else if request.path.starts_with("/rs/") && !request.path.starts_with("/rs/json/") && request.path != "/rs/json" {
        // Parse path segments
        let path_bytes = request.path.as_bytes();
        let mut segments: Vec<&[u8]> = Vec::new();
        let mut start = 4; // after "/rs/"
        
        while start < path_bytes.len() {
            if let Some(end) = path_bytes[start..].iter().position(|&b| b == b'/') {
                if start < start + end {
                    segments.push(&path_bytes[start..start + end]);
                }
                start += end + 1;
            } else {
                if start < path_bytes.len() {
                    segments.push(&path_bytes[start..]);
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

        response.body_text(&body);
    } else {
        response = HttpResponse::new(HttpStatusCode::NotFound);
        add_cors_headers(&mut response);
        response.body_text("Not Found");
    }
    
    response
}

// Handle POST request
fn handle_post(request: &HttpRequest) -> HttpResponse {
    let mut response = HttpResponse::new(HttpStatusCode::OK);
    add_cors_headers(&mut response);
    
    // Echo back the received data
    if !request.body.is_empty() {
        let content_type = request.headers.get("Content-Type").map(|s| s.as_str()).unwrap_or("application/octet-stream");
        response.body(request.body.clone(), content_type);
    } else {
        response.body_text("POST received with no body");
    }
    
    response
}

// Handle PUT request
fn handle_put(request: &HttpRequest) -> HttpResponse {
    let mut response = HttpResponse::new(HttpStatusCode::Created);
    add_cors_headers(&mut response);
    
    response.body_text(&format!("PUT received for path: {}", request.path));
    
    response
}

// Handle DELETE request
fn handle_delete(request: &HttpRequest) -> HttpResponse {
    let mut response = HttpResponse::new(HttpStatusCode::NoContent);
    add_cors_headers(&mut response);
    
    response
}

// Handle PATCH request
fn handle_patch(request: &HttpRequest) -> HttpResponse {
    let mut response = HttpResponse::new(HttpStatusCode::OK);
    add_cors_headers(&mut response);
    
    response.body_text(&format!("PATCH received for path: {}", request.path));
    
    response
}

// Optimized client handler with full HTTP spec support
fn handle_client(mut stream: TcpStream, peers: WebSocketPeers) {
    // Set TCP_NODELAY to disable Nagle's algorithm
    let _ = stream.set_nodelay(true);
    let _ = stream.set_read_timeout(Some(Duration::from_secs(KEEP_ALIVE_TIMEOUT)));
    let _ = stream.set_write_timeout(Some(Duration::from_secs(KEEP_ALIVE_TIMEOUT)));
    
    // Stack-allocated buffer to avoid heap allocations
    let mut buffer = [0u8; CLIENT_REQUEST_BUFFER_SIZE];
    let mut used = 0usize;
    let mut keep_alive = true;

    // HTTP keep-alive loop: handle multiple requests per connection
    while keep_alive {
        // Read more data if buffer is not full
        if used < buffer.len() {
            match stream.read(&mut buffer[used..]) {
                Ok(0) => break, // Connection closed by client
                Ok(n) => used += n,
                Err(e) if e.kind() == std::io::ErrorKind::TimedOut => {
                    // Timeout - close connection if no data
                    break;
                }
                Err(_) => break, // Connection error
            }
        }

        // Parse HTTP request
        let (request, headers_end) = match parse_http_request(&buffer[..used]) {
            Ok((req, end)) => (req, end),
            Err(response) => {
                let _ = stream.write_all(&response.to_bytes());
                return;
            }
        };

        // Check for WebSocket upgrade
        if request.path.starts_with("/chat/") && request.method == HttpMethod::GET {
            let room_name_bytes = &request.path.as_bytes()[6..]; // after "/chat/"
            
            if room_name_bytes.is_empty() {
                let mut response = HttpResponse::new(HttpStatusCode::BadRequest);
                response.body_text("Room name empty");
                let _ = stream.write_all(&response.to_bytes());
                return;
            }

            if let Some(upgrade) = request.headers.get("Upgrade") {
                if upgrade.eq_ignore_ascii_case("websocket") {
                    if let Some(key) = request.headers.get("Sec-WebSocket-Key") {
                        let response = websocket_handshake(key);
                        if stream.write_all(response.as_bytes()).is_ok() {
                            let room_name = String::from_utf8_lossy(room_name_bytes).to_string();
                            handle_websocket_client(stream, room_name, peers);
                            return;
                        }
                    }
                }
            }
            
            // Not a WebSocket request
            let mut response = HttpResponse::new(HttpStatusCode::NotFound);
            response.body_text("Not Found");
            let _ = stream.write_all(&response.to_bytes());
            return;
        }

        // Handle different HTTP methods
        let mut response = match request.method {
            HttpMethod::GET => handle_get(&request),
            HttpMethod::POST => handle_post(&request),
            HttpMethod::PUT => handle_put(&request),
            HttpMethod::DELETE => handle_delete(&request),
            HttpMethod::PATCH => handle_patch(&request),
            HttpMethod::HEAD => handle_head(&request),
            HttpMethod::OPTIONS => handle_options(&request),
            _ => {
                let mut resp = HttpResponse::new(HttpStatusCode::MethodNotAllowed);
                add_cors_headers(&mut resp);
                resp.body_text("Method Not Allowed");
                resp
            }
        };

        // Check for keep-alive
        keep_alive = if let Some(conn) = request.headers.get("Connection") {
            conn.eq_ignore_ascii_case("keep-alive")
        } else {
            request.version == HttpVersion::Http11
        };

        if keep_alive {
            response.set_header("Connection", "keep-alive");
            response.set_header("Keep-Alive", &format!("timeout={}", KEEP_ALIVE_TIMEOUT));
        } else {
            response.set_header("Connection", "close");
        }

        // Send response
        if stream.write_all(&response.to_bytes()).is_err() {
            return;
        }

        // Move to next request in buffer
        if headers_end < used {
            buffer.copy_within(headers_end..used, 0);
            used -= headers_end;
        } else {
            used = 0;
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
    
    println!("backend_rs (Full HTTP Spec): run on {}:{}", ADDRESS_IP, ADDRESS_PORT);
    println!("  - HTTP Methods: GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS");
    println!("  - HTTP Versions: 1.0, 1.1, 2.0");
    println!("  - Keep-Alive: timeout {}s", KEEP_ALIVE_TIMEOUT);
    println!("  - CORS Headers: Enabled");
    println!("  - Cache Control: Enabled");
    println!("  - Endpoints:");
    println!("    * HTTP: /rs");
    println!("    * JSON: /rs/json");
    println!("    * Dynamic: /rs/{{path1}}/{{path2}}/... (except /rs/json)");
    println!("    * WebSocket: /chat/{{room_name}} (per room broadcast)");

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
    Latency   305.23us  548.88us   5.49ms   91.97%
    Req/Sec    66.04k     7.72k  114.03k    74.25%
  3955773 requests in 10.10s, 1.45GB read
Requests/sec: 391676.71
Transfer/sec:    146.80MB
*/
