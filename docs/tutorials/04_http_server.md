# Tutorial 4: HTTP Server

This tutorial explains how to build HTTP servers using hotcoco's HTTP components, powered by the llhttp parser.

## Table of Contents

1. [HTTP Server Architecture](#http-server-architecture)
2. [HttpRequest and HttpResponse](#httprequest-and-httpresponse)
3. [HttpParser: llhttp Integration](#httpparser-llhttp-integration)
4. [Building Routes](#building-routes)
5. [Complete HTTP Server Example](#complete-http-server-example)

---

## HTTP Server Architecture

The hotcoco HTTP server is built on three layers:

```
┌─────────────────────────────────────┐
│           HttpServer                │  High-level API
│   - Handles connections             │
│   - Routes requests                 │
└─────────────────────────────────────┘
                 ↓
┌─────────────────────────────────────┐
│      HttpParser (llhttp)            │  Protocol parsing
│   - Parses HTTP/1.1 requests        │
│   - Zero-copy where possible        │
└─────────────────────────────────────┘
                 ↓
┌─────────────────────────────────────┐
│     TcpListener / TcpStream         │  Transport layer
│   - Async TCP                        │
└─────────────────────────────────────┘
```

---

## HttpRequest and HttpResponse

### HttpRequest

Represents a parsed HTTP request from the client:

```cpp
class HttpRequest {
public:
    HttpMethod method;   // GET, POST, PUT, DELETE, etc.
    std::string url;     // Full URL: "/search?q=test"
    std::string path;    // Path only: "/search"
    std::string query;   // Query string: "q=test"
    std::string body;    // Request body (for POST/PUT)
    HeaderMap headers;  // Case-insensitive (RFC 7230)
    
    std::string GetHeader(const std::string& name) const;
    bool HasHeader(const std::string& name) const;
};
```

### HttpResponse

Builder for HTTP responses with convenient factory methods:

```cpp
// Plain text response
auto response = HttpResponse::Ok("Hello, World!");

// HTML page
auto html = HttpResponse::Html("<h1>Welcome</h1>");

// JSON API response
auto json = HttpResponse::Json(R"({"status": "ok"})");

// Error responses
auto notFound = HttpResponse::NotFound("Page not found");
auto error = HttpResponse::InternalError("Something went wrong");

// Custom headers
response.SetHeader("X-Custom", "value");
```

### Response Serialization

HttpResponse serializes to HTTP wire format:

```cpp
HttpResponse response = HttpResponse::Ok("Hello");
std::string wire = response.Serialize();
// Result:
// HTTP/1.1 200 OK
// Content-Type: text/plain
// Content-Length: 5
//
// Hello
```

---

## HttpParser: llhttp Integration

### What is llhttp?

**llhttp** is Node.js's HTTP parser - a fast, portable, and dependency-free HTTP parser written in C. It's used by Node.js, Nginx Unit, and many other projects.

Key benefits:
- **Fast**: Optimized for high-performance parsing
- **Streaming**: Can parse partial data as it arrives
- **Robust**: Battle-tested in Node.js

### Using HttpParser

```cpp
HttpParser parser;

// Parse incoming data (can be called multiple times for streaming)
const char* data = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
bool complete = parser.Parse(data, strlen(data));

if (complete) {
    HttpRequest request = parser.GetRequest();
    std::cout << "Path: " << request.path << std::endl;  // "/hello"
}

// Reset for next request (keep-alive)
parser.Reset();
```

### Error Handling

```cpp
if (parser.HasError()) {
    std::cout << "Parse error: " << parser.GetError() << std::endl;
}
```

### How llhttp Callbacks Work

llhttp uses a callback-based architecture:

```cpp
// When URL is parsed
static int OnUrl(llhttp_t* parser, const char* at, size_t len) {
    auto* self = static_cast<HttpParser*>(parser->data);
    self->request_.url.append(at, len);
    return 0;  // 0 = success
}

// When header field is parsed
static int OnHeaderField(llhttp_t* parser, const char* at, size_t len);

// When header value is parsed
static int OnHeaderValue(llhttp_t* parser, const char* at, size_t len);

// When body data arrives
static int OnBody(llhttp_t* parser, const char* at, size_t len);

// When complete request is parsed
static int OnMessageComplete(llhttp_t* parser) {
    auto* self = static_cast<HttpParser*>(parser->data);
    self->complete_ = true;
    return 0;
}
```

---

## Building Routes

### Simple Route Handler

```cpp
HttpResponse HandleRequest(const HttpRequest& req) {
    if (req.path == "/") {
        return HttpResponse::Html("<h1>Welcome!</h1>");
    }
    
    if (req.path == "/api/status") {
        return HttpResponse::Json(R"({"status": "running"})");
    }
    
    if (req.path.starts_with("/api/users/")) {
        std::string user_id = req.path.substr(12);
        return HttpResponse::Json(R"({"id": ")" + user_id + R"("})");
    }
    
    return HttpResponse::NotFound("Not found: " + req.path);
}
```

### Method-Based Routing

```cpp
HttpResponse HandleRequest(const HttpRequest& req) {
    if (req.path == "/api/data") {
        switch (req.method) {
            case HttpMethod::GET:
                return HttpResponse::Json(R"({"action": "read"})");
            
            case HttpMethod::POST:
                return HttpResponse::Json(R"({"action": "create"})");
            
            case HttpMethod::DELETE:
                return HttpResponse::Json(R"({"action": "delete"})");
            
            default:
                return HttpResponse::BadRequest("Method not allowed");
        }
    }
    
    return HttpResponse::NotFound();
}
```

---

## Complete HTTP Server Example

### Basic Server

```cpp
#include "hotcoco/http/http.hpp"
#include "hotcoco/io/libuv_executor.hpp"

int main() {
    auto executor_result = hotcoco::LibuvExecutor::Create();
    auto& executor = *executor_result.Value();

    hotcoco::HttpServer server(executor, "127.0.0.1", 8080);

    server.OnRequest([](const hotcoco::HttpRequest& req) {
        std::cout << hotcoco::HttpMethodToString(req.method) << " " << req.path << std::endl;
        return hotcoco::HttpResponse::Ok("Hello, World!");
    });

    std::cout << "Server listening on http://127.0.0.1:8080" << std::endl;

    auto server_task = server.Run();
    executor.Schedule(server_task.GetHandle());
    executor.Run();

    return 0;
}
```

### Server with Multiple Routes

See `examples/05_http_server.cpp` for a complete example with:
- HTML index page
- JSON API endpoints  
- Request logging
- Request statistics

---

## Under the Hood

### HttpServer Connection Flow

```
New Connection
      │
      ▼
┌─────────────────────┐
│  Accept connection  │
│  via TcpListener    │
└─────────────────────┘
      │
      ▼
┌─────────────────────┐
│  Spawn coroutine    │
│  HandleConnection() │
└─────────────────────┘
      │
      ▼
┌─────────────────────┐
│  Read data via      │
│  TcpStream::Read()  │◄──┐
└─────────────────────┘   │
      │                   │
      ▼                   │
┌─────────────────────┐   │
│  Parse with         │   │
│  HttpParser         │   │
└─────────────────────┘   │
      │                   │
      ▼                   │
┌─────────────────────┐   │
│  Call request       │   │
│  handler callback   │   │
└─────────────────────┘   │
      │                   │
      ▼                   │
┌─────────────────────┐   │
│  Send response via  │   │
│  TcpStream::Write() │   │
└─────────────────────┘   │
      │                   │
      ▼                   │
   Keep-alive? ───────────┘
      │ no
      ▼
   Close connection
```

### Keep-Alive Support

HTTP/1.1 uses persistent connections by default:

```cpp
// In HandleConnection:
if (request.GetHeader("Connection") == "close") {
    break;  // Close after this response
}
parser.Reset();  // Ready for next request
```

---

## Summary

| Component | Purpose |
|-----------|---------|
| `HttpRequest` | Parsed HTTP request |
| `HttpResponse` | Response with factory methods |
| `HttpParser` | llhttp-based request parser |
| `HttpServer` | High-level async HTTP server |

### Key Takeaways

1. **llhttp is fast and robust**: Battle-tested in Node.js
2. **Response factories are convenient**: `HttpResponse::Ok()`, `Json()`, `Html()`
3. **Routing is simple**: Just check `request.path` and `request.method`
4. **Keep-alive is automatic**: Parser tracks connection state

---

## What's Next?

With a working HTTP server, you can now:
- Build REST APIs
- Add custom request handlers
- Combine with other hotcoco primitives (cancellation, timeouts, etc.)
