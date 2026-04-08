# Backend Elixir — Summary & Lessons Learned

## Current State (v1 — Working)

**Architecture:** GenServer + Task.Supervisor  
**Port:** 9007  
**Concurrency:** One lightweight process per connection (BEAM-native)

### How it works

```
Main Process (GenServer)
  └─ accept loop via :gen_server.cast
       └─ :gen_tcp.accept(listen_socket)
            └─ Task.Supervisor.start_child(fn → handle_connection(socket) end)
                 └─ Buffered TCP line reading
                      └─ parse → respond → loop (keep-alive)
```

### Key design decisions

| Component | Choice | Why |
|-----------|--------|-----|
| Socket mode | `active: false` | Full control over when data is read; no race conditions |
| Data reading | Manual buffering with `:gen_tcp.recv(socket, 0, 5000)` | Avoids all packet-mode pitfalls (see below) |
| Concurrency | `Task.Supervisor` per connection | OTP-standard; automatic cleanup on crash |
| Responses | `Connection: keep-alive` | Loop reuses the same socket for multiple requests |

### Benchmark

```
wrk -c100 -t1 -d10s http://localhost:9007/
Requests/sec: 125,206
Socket errors: 0
```

---

## What DOESN'T work — and why

### 1. `:gen_tcp.recv(socket, 0)` without timeout or packet mode

```elixir
# BAD — blocks until socket closes
case :gen_tcp.recv(socket, 0) do
  {:ok, data} -> ...
end
```

**Why it fails:** `recv(socket, 0)` with `0` bytes means "read until the other side closes." The call blocks for the full socket timeout (~5 seconds) because the client (curl/wrk) keeps the connection open waiting for the response.

**Fix:** Use manual buffering: `recv(socket, 0, timeout)` with a timeout, and accumulate data until `\r\n` is found.

---

### 2. `packet: :line` — skip_headers blocks forever

```elixir
# BAD — recv hangs after the first line
listen_options = [:binary, packet: :line, active: false]
```

**Why it fails:** `packet: :line` makes each `recv` return one line. After reading the request line, the `skip_headers` function calls `recv` repeatedly looking for `\r\n`. But the client sends all headers in one TCP segment, so after reading them all, the next `recv` blocks waiting for data that never arrives.

**Fix:** Don't use `packet: :line`. Read raw data and split manually.

---

### 3. `packet: :http` — complex path routing

```elixir
# WORKS but awkward — path is in the request tuple
listen_options = [:binary, packet: :http, active: false]

case :gen_tcp.recv(socket, 0) do
  {:ok, {:http_request, method, {abs_path, _query}, _version}} -> ...
end
```

**Why it's problematic:** The HTTP packet parser returns the path in a nested tuple that's harder to pattern-match and manipulate (query strings are separate, method is an atom, etc.). For a simple server this adds unnecessary complexity.

**Verdict:** Viable, but overkill for simple routing.

---

### 4. `active: :once` — socket ownership race condition

```elixir
# BAD — :tcp messages go to the wrong process
spawn(fn ->
  :inet.setopts(socket, active: :once)
  handle_connection(socket)
end)

# This doesn't fix it either:
spawn(fn ->
  :gen_tcp.controlling_process(socket, self())
  :inet.setopts(socket, active: :once)
  handle_connection(socket)
end)
```

**Why it fails:** The socket is owned by the process that called `accept`. When you `spawn` a new process, `:tcp` messages still go to the parent. `:gen_tcp.controlling_process/2` is supposed to transfer ownership, but there's a race condition — data can arrive before the transfer completes, causing messages to be lost.

**Fix:** Use `active: false` and read synchronously. No ownership issues.

---

### 5. `Connection: close` without consuming headers

```elixir
# BAD — TCP RST causes "read errors" in wrk
:gen_tcp.send(socket, response)
:gen_tcp.close(socket)   # sends RST with unread data
```

**Why it fails:** The server sends the response and immediately closes the socket. But the client has unread header data still in the TCP buffer. The OS sends a TCP RST (reset), which wrk reports as "Socket errors: read N".

**Fix:** Always consume all headers before closing, OR use `:gen_tcp.shutdown(socket, :write)` for graceful half-close.

---

### 6. GenServer accept loop with `handle_info` + `cast` — stale processes

```elixir
# PROBLEMATIC — old server instances can linger
def run do
  {:ok, pid} = start_link()
  :gen_server.cast(pid, :accept)
  :timer.sleep(:infinity)  # blocks forever
end
```

**Why it's tricky:** If you restart the server without killing the old BEAM VM, the old process still holds the listen socket. New connections go to the old instance, which may have different code (no debug prints, different handlers, etc.).

**Fix:** Always `pkill -9 -f elixir` before restarting, or use `lsof -i :9007` to verify the port is free.

---

## What works — summary table

| Approach | Works? | Zero errors? | Verdict |
|----------|--------|-------------|---------|
| Manual buffering (`active: false`) | ✅ | ✅ | **Use this** |
| `packet: :line` | ❌ | ❌ | Don't use |
| `packet: :http` | ✅ | ✅ | Overkill for simple server |
| `active: :once` + `controlling_process` | ❌ | ❌ | Race condition |
| `active: :once` without `controlling_process` | ❌ | ❌ | Messages to wrong process |
| `Connection: close` without consuming headers | ❌ | ❌ | TCP RST errors |
| `recv(socket, 0)` without timeout | ❌ | ❌ | Blocks 5 seconds |

---

## Run

```bash
cd backend_ex && elixir main.ex
```

## Test

```bash
# Basic
curl http://localhost:9007/
curl http://localhost:9007/notfound
curl -X POST http://localhost:9007/

# Benchmark
wrk -c100 -t1 -d10s http://localhost:9007/
```
