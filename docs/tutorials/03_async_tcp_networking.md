# Tutorial 3: Async TCP Networking

This tutorial explains how to build asynchronous TCP servers and clients using hotcoco's `TcpListener` and `TcpStream` classes.

## Table of Contents

1. [The Challenge of Async Networking](#the-challenge-of-async-networking)
2. [TcpListener: Accepting Connections](#tcplistener-accepting-connections)
3. [TcpStream: Reading and Writing](#tcpstream-reading-and-writing)
4. [Building an Echo Server](#building-an-echo-server)
5. [How It Works Under the Hood](#how-it-works-under-the-hood)

---

## The Challenge of Async Networking

Traditional networking code blocks threads:

```cpp
// BLOCKING - one thread per connection!
void HandleClient(int socket) {
    while (true) {
        char buf[1024];
        int n = read(socket, buf, sizeof(buf));  // Thread blocked here
        if (n <= 0) break;
        write(socket, buf, n);  // Thread blocked here
    }
}
```

This approach doesn't scale. For 10,000 concurrent connections, you need 10,000 threads!

### The Async Solution

With coroutines and an event loop, one thread can handle thousands of connections:

```cpp
// NON-BLOCKING - one thread, many connections!
Task<void> HandleClient(std::unique_ptr<TcpStream> stream) {
    while (stream->IsOpen()) {
        auto data = co_await stream->Read();  // Coroutine suspends, thread is free!
        if (data.empty()) break;
        co_await stream->Write(data);         // Suspends again
    }
}
```

---

## TcpListener: Accepting Connections

`TcpListener` binds to an address and listens for incoming connections.

### Basic Usage

```cpp
Task<void> RunServer(LibuvExecutor& executor) {
    TcpListener listener(executor);
    
    // Bind to address and port
    int result = listener.Bind("127.0.0.1", 8080);
    if (result != 0) {
        std::cout << "Bind failed: " << result << std::endl;
        co_return;
    }
    
    // Start listening (backlog = max pending connections)
    listener.Listen(128);
    
    while (true) {
        // Accept a new connection
        auto stream = co_await listener.Accept();
        
        // Handle it...
    }
}
```

### How Accept Works

1. **Call Accept()** - Returns an `AcceptAwaitable`
2. **co_await** - Suspends until a connection arrives
3. **Resume** - Returns a `std::unique_ptr<TcpStream>`

```
Client connects
      │
      ▼
┌─────────────────────┐
│    libuv detects    │
│  connection event   │
└─────────────────────┘
      │
      ▼
┌─────────────────────┐
│  OnConnection()     │
│  callback fires     │
└─────────────────────┘
      │
      ▼
┌─────────────────────┐
│  Resume waiting     │
│  Accept coroutine   │
└─────────────────────┘
      │
      ▼
┌─────────────────────┐
│  await_resume()     │
│  returns TcpStream  │
└─────────────────────┘
```

---

## TcpStream: Reading and Writing

`TcpStream` represents a TCP connection for bidirectional data transfer.

### Reading Data

```cpp
Task<void> ProcessData(TcpStream& stream) {
    while (stream.IsOpen()) {
        // Read up to 4096 bytes
        auto data = co_await stream.Read(4096);
        
        if (data.empty()) {
            // Connection closed or error
            break;
        }
        
        // Process the data
        std::string message(data.begin(), data.end());
        std::cout << "Received: " << message << std::endl;
    }
}
```

### Writing Data

```cpp
Task<void> SendResponse(TcpStream& stream) {
    std::string response = "HTTP/1.1 200 OK\r\n\r\nHello, World!";
    
    int bytes_written = co_await stream.Write(response);
    
    if (bytes_written < 0) {
        std::cout << "Write error: " << bytes_written << std::endl;
    } else {
        std::cout << "Sent " << bytes_written << " bytes" << std::endl;
    }
}
```

### Connecting as a Client

```cpp
Task<void> ConnectToServer(LibuvExecutor& executor) {
    TcpStream client(executor);
    
    int result = co_await client.Connect("127.0.0.1", 8080);
    if (result != 0) {
        std::cout << "Connect failed: " << result << std::endl;
        co_return;
    }
    
    // Connected! Now read/write...
    co_await client.Write("Hello, server!");
    auto response = co_await client.Read();
    
    client.Close();
}
```

---

## Building an Echo Server

Let's build a complete echo server that handles multiple clients concurrently.

### The Handler Coroutine

Each client gets its own handler coroutine:

```cpp
Task<void> HandleClient(std::unique_ptr<TcpStream> client, int id) {
    std::cout << "[Client " << id << "] Connected" << std::endl;
    
    while (client->IsOpen()) {
        // Read data
        auto data = co_await client->Read();
        
        if (data.empty()) {
            std::cout << "[Client " << id << "] Disconnected" << std::endl;
            break;
        }
        
        // Echo it back
        co_await client->Write(std::string_view(data.data(), data.size()));
    }
    
    client->Close();
}
```

### The Server Loop

The main server accepts connections and spawns handlers:

```cpp
Task<void> RunServer(LibuvExecutor& executor) {
    TcpListener listener(executor);
    listener.Bind("127.0.0.1", 8080);
    listener.Listen();
    
    std::cout << "Server listening on port 8080" << std::endl;
    
    int client_id = 0;
    while (true) {
        auto client = co_await listener.Accept();
        
        // Spawn a handler for this client
        Spawn(executor, HandleClient(std::move(client), ++client_id));
    }
}
```

### Main Function

```cpp
int main() {
    auto executor_result = LibuvExecutor::Create();
    auto& executor = *executor_result.Value();

    auto server = RunServer(executor);
    executor.Schedule(server.GetHandle());

    executor.Run();
    return 0;
}
```

### Testing the Server

```bash
# Terminal 1: Start server
./04_tcp_echo_server

# Terminal 2: Connect with netcat
echo "Hello, hotcoco!" | nc localhost 8080
# Output: Echo: Hello, hotcoco!
```

---

## How It Works Under the Hood

### The Awaitable Pattern

Each async operation returns an **awaitable** with three methods:

```cpp
class ReadAwaitable {
    bool await_ready() const noexcept;     // Is data available now?
    void await_suspend(std::coroutine_handle<>);  // Start reading, schedule resume
    std::vector<char> await_resume();      // Return the data
};
```

### libuv Integration

Hotcoco uses libuv's non-blocking I/O:

#### Accept Flow

```cpp
// 1. uv_listen() tells libuv to watch for connections
uv_listen(server, backlog, OnConnection);

// 2. When a connection arrives, libuv calls OnConnection
void OnConnection(uv_stream_t* server, int status) {
    // 3. Accept the connection
    uv_accept(server, client);
    
    // 4. Resume the waiting coroutine
    accept_waiter_.resume();
}
```

#### Read Flow

```cpp
// 1. uv_read_start() tells libuv to read data
uv_read_start(stream, AllocBuffer, OnRead);

// 2. libuv allocates a buffer for incoming data
void AllocBuffer(uv_handle_t*, size_t suggested, uv_buf_t* buf) {
    buf->base = new char[suggested];
    buf->len = suggested;
}

// 3. When data arrives, libuv calls OnRead
void OnRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    // 4. Store the data
    read_buffer_.assign(buf->base, buf->base + nread);
    
    // 5. Resume the waiting coroutine
    read_waiter_.resume();
}
```

#### Write Flow

```cpp
// 1. uv_write() queues the write operation
uv_write_t req;
uv_buf_t buf = uv_buf_init(data.data(), data.size());
uv_write(&req, stream, &buf, 1, OnWrite);

// 2. When write completes, libuv calls OnWrite
void OnWrite(uv_write_t* req, int status) {
    // 3. Resume the waiting coroutine
    write_waiter_.resume();
}
```

### Memory Management

Each `TcpStream` manages its own libuv handle:

```cpp
TcpStream::TcpStream(LibuvExecutor& executor) {
    uv_tcp_init(executor.GetLoop(), &socket_);
    socket_.data = this;  // Store pointer for callbacks
}

TcpStream::~TcpStream() {
    Close();  // Stop reads, close handle
}
```

---

## Summary

| Component | Purpose |
|-----------|---------|
| `TcpListener` | Bind, listen, accept connections |
| `TcpStream` | Read and write data |
| `AcceptAwaitable` | Suspend until connection arrives |
| `ReadAwaitable` | Suspend until data available |
| `WriteAwaitable` | Suspend until write completes |
| `ConnectAwaitable` | Suspend until connection established |

### Key Takeaways

1. **One thread, many connections**: Coroutines suspend instead of blocking
2. **libuv handles the I/O**: We wrap its callbacks with awaitables
3. **Each operation is an awaitable**: Accept, Read, Write, Connect
4. **RAII for resource management**: Streams close on destruction

---

## Next Steps

In the next tutorial, we'll build an HTTP server on top of this TCP foundation:
- Parse HTTP requests using llhttp
- Build HTTP responses
- Create a complete web server example
