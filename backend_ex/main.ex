# v1 - Fixed TCP line reading
# Elixir 1.16+
#
# Design (Elixir/OTP way - no plugins)
#
# #-----------------------------------------------------------------#
# |                        Supervisor                               |
# |  #-----------------------------------------------------------#  |
# |  |  HttpServer (GenServer)                                   |  |
# |  |  - Binds to 0.0.0.0:9007                                  |  |
# |  |  - Accepts connections                                    |  |
# |  #-----------------------------------------------------------#  |
# #-----------------------------------------------------------------#
#    |
#    v (per connection)
# #-----------------#
# | Task.Supervisor |  ← Spawns a lightweight process per request
# | #-------------# |
# | | Task.start  | |  ← Each request handled in isolated process
# | | (handler)   | |    - Pattern match on path/method
# | |             | |    - Send response
# | #-------------# |
# #-----------------#
#
# Elixir handles concurrency naturally via the BEAM VM.
# Each connection is handled by a separate lightweight process,
# scheduled across all available CPU cores automatically.
# No manual thread pool needed - the VM scheduler does this efficiently.
#

defmodule HttpServer do
  use GenServer

  @ip {0, 0, 0, 0}
  @port 9007
  @max_clients 1024 * 4

  ## --------------------------------------------------------- ##

  # HTTP Response templates
  defmodule Templates do
    @page_ok "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\nOK"
    @page_not_found "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 9\r\nConnection: keep-alive\r\n\r\nNot Found"
    @page_method_not_allowed "HTTP/1.1 405 Method Not Allowed\r\nContent-Type: text/plain\r\nContent-Length: 18\r\nConnection: keep-alive\r\n\r\nMethod Not Allowed"

    def page_ok, do: @page_ok
    def page_not_found, do: @page_not_found
    def page_method_not_allowed, do: @page_method_not_allowed
  end

  ## --------------------------------------------------------- ##

  # Server API
  def start_link(opts \\ []) do
    port = Keyword.get(opts, :port, @port)
    GenServer.start_link(__MODULE__, port, name: __MODULE__)
  end

  @impl true
  def init(port) do
    # Create a Task supervisor for handling concurrent connections
    {:ok, task_supervisor} = Task.Supervisor.start_link()

    # Log startup
    num_schedulers = :erlang.system_info(:schedulers_online)
    ip_str = :inet.ntoa( @ip) |> to_string()
    IO.puts("HttpServer running: #{ip_str}:#{port} (#{num_schedulers} workers)")

    # Open listening socket
    listen_options = [
      :binary,
      active: false,
      reuseaddr: true,
      backlog: @max_clients
    ]

    case :gen_tcp.listen(port, listen_options) do
      {:ok, listen_socket} ->
        {:ok, %{listen_socket: listen_socket, task_supervisor: task_supervisor}}

      {:error, reason} ->
        IO.puts("Error: failed to listen on port #{port}: #{reason}")
        {:stop, reason}
    end
  end

  @impl true
  def handle_info(:accept, state = %{listen_socket: listen_socket, task_supervisor: task_supervisor}) do
    case :gen_tcp.accept(listen_socket) do
      {:ok, client_socket} ->
        # Spawn a new process to handle this connection (concurrent)
        Task.Supervisor.start_child(task_supervisor, fn ->
          handle_connection(client_socket)
        end)

        # Continue accepting more connections
        :gen_server.cast(self(), :accept)
        {:noreply, state}

      {:error, reason} ->
        IO.puts("Error: accept failed: #{reason}")
        :gen_server.cast(self(), :accept)
        {:noreply, state}
    end
  end

  @impl true
  def handle_cast(:accept, state) do
    handle_info(:accept, state)
  end

  @impl true
  def terminate(_reason, %{listen_socket: listen_socket}) do
    :gen_tcp.close(listen_socket)
  end

  ## --------------------------------------------------------- ##

  # Start accepting connections
  def run do
    {:ok, pid} = start_link()
    # Trigger the accept loop
    :gen_server.cast(pid, :accept)

    # Keep the main process alive
    try do
      :timer.sleep(:infinity)
    catch
      :exit, _ -> :ok
    end
  end

  ## --------------------------------------------------------- ##

  # Handle a single client connection (keep-alive support)
  defp handle_connection(socket) do
    # Pass an initial empty buffer for line reading
    handle_request_loop(socket, "")
    :gen_tcp.close(socket)
  end

  # Buffered request loop for keep-alive connections
  defp handle_request_loop(socket, buffer) do
    # Try to read a complete request line from the current buffer
    case read_line_from_buffer(socket, buffer) do
      {:ok, request_line, rest_buffer} ->
        {method, path} = parse_request_line(request_line)

        # Skip remaining headers and body (we don't need them for this simple server)
        {_headers_done, final_buffer} = skip_headers_and_body(socket, rest_buffer)

        # Generate response
        response = generate_response(method, path)

        # Send response
        :gen_tcp.send(socket, response)

        # Continue loop for keep-alive with whatever remains in the buffer
        handle_request_loop(socket, final_buffer)

      {:error, :closed} ->
        :ok

      {:error, reason} ->
        IO.puts("Request error: #{reason}")
        :ok
    end
  end

  ## --------------------------------------------------------- ##
  # TCP Stream Parsing (Fixed)
  ## --------------------------------------------------------- ##

  # Read a full line (ending with "\r\n") from the socket,
  # using an existing buffer. Returns {:ok, line, rest_buffer}
  # or {:error, reason}.
  defp read_line_from_buffer(socket, buffer) do
    # Check if we already have a complete line in the buffer
    case :binary.split(buffer, "\r\n") do
      [line, rest] ->
        # Complete line found
        {:ok, line, rest}

      [_incomplete] ->
        # Need more data
        case :gen_tcp.recv(socket, 0, 5000) do
          {:ok, data} ->
            read_line_from_buffer(socket, buffer <> data)

          {:error, :closed} ->
            {:error, :closed}

          {:error, :timeout} ->
            {:error, :timeout}
        end
    end
  end

  # Skip all headers (until an empty line "\r\n") and any body.
  # Returns {done?, remaining_buffer}.
  defp skip_headers_and_body(socket, buffer) do
    # Look for the empty line that marks the end of headers
    case :binary.split(buffer, "\r\n\r\n") do
      [_headers, rest] ->
        # Headers ended; we ignore the body and return the remaining buffer
        {true, rest}

      [_incomplete] ->
        # Need more data to find the header terminator
        case :gen_tcp.recv(socket, 0, 5000) do
          {:ok, data} ->
            skip_headers_and_body(socket, buffer <> data)

          {:error, _} = _error ->
            # On error, just return what we have
            {false, buffer}
        end
    end
  end

  # --------------------------------------------------------- #

  # Parse request line: "METHOD /path HTTP/1.1"
  defp parse_request_line(request_line) do
    parts = String.split(request_line, " ")

    method =
      case parts do
        [m | _] -> m
        _ -> "UNKNOWN"
      end

    path =
      case parts do
        [_m, p | _] ->
          # Strip query parameters
          case String.split(p, "?") do
            [clean_path | _] -> clean_path
            _ -> p
          end

        _ ->
          "/"
      end

    if path == "", do: "/"
    {method, path}
  end

  # --------------------------------------------------------- #

  # Generate HTTP response based on path and method
  defp generate_response(method, path) do
    case path do
      "/" ->
        if method == "GET" do
          Templates.page_ok()
        else
          Templates.page_method_not_allowed()
        end

      _ ->
        Templates.page_not_found()
    end
  end
end

# --------------------------------------------------------- #

# Main entry point
IO.puts("DEBUG MODE")
HttpServer.run()
