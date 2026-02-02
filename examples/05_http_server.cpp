// ============================================================================
// Example 05: HTTP Server
// ============================================================================
//
// This example demonstrates building a simple HTTP server with hotcoco.
// It handles multiple routes and serves both HTML and JSON responses.
//
// RUN:
//   cd build && ./examples/05_http_server
//
// TEST WITH:
//   curl http://localhost:8080/
//   curl http://localhost:8080/api/hello
//   curl http://localhost:8080/api/time
//
// ============================================================================

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "hotcoco/core/task.hpp"
#include "hotcoco/http/http.hpp"
#include "hotcoco/io/libuv_executor.hpp"

using namespace hotcoco;

// Request counter for demo
int g_request_count = 0;

// Format current time as string
std::string GetCurrentTime() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t_now), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

// HTML template for index page
std::string GetIndexHtml() {
    std::ostringstream html;
    html << R"(<!DOCTYPE html>
<html>
<head>
    <title>Hotcoco HTTP Server</title>
    <style>
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Arial, sans-serif;
            max-width: 800px;
            margin: 50px auto;
            padding: 20px;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
        }
        .container {
            background: white;
            border-radius: 10px;
            padding: 30px;
            box-shadow: 0 10px 40px rgba(0,0,0,0.2);
        }
        h1 {
            color: #667eea;
            margin-bottom: 10px;
        }
        .subtitle {
            color: #666;
            margin-bottom: 30px;
        }
        .endpoint {
            background: #f5f5f5;
            border-radius: 5px;
            padding: 15px;
            margin: 10px 0;
            font-family: monospace;
        }
        .method {
            background: #667eea;
            color: white;
            padding: 3px 8px;
            border-radius: 3px;
            font-size: 12px;
        }
        .stats {
            margin-top: 30px;
            padding-top: 20px;
            border-top: 1px solid #eee;
            color: #666;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>🥥 Hotcoco HTTP Server</h1>
        <p class="subtitle">A C++20 coroutine-based HTTP server</p>
        
        <h3>Available Endpoints</h3>
        <div class="endpoint">
            <span class="method">GET</span> /
            <br><small>This page</small>
        </div>
        <div class="endpoint">
            <span class="method">GET</span> /api/hello
            <br><small>Returns a greeting in JSON</small>
        </div>
        <div class="endpoint">
            <span class="method">GET</span> /api/time
            <br><small>Returns current server time</small>
        </div>
        <div class="endpoint">
            <span class="method">GET</span> /api/stats
            <br><small>Returns request statistics</small>
        </div>
        
        <div class="stats">
            <strong>Server Time:</strong> )" << GetCurrentTime() << R"(
            <br>
            <strong>Requests Served:</strong> )" << g_request_count << R"(
        </div>
    </div>
</body>
</html>
)";
    return html.str();
}

// Handle HTTP request
HttpResponse HandleRequest(const HttpRequest& req) {
    g_request_count++;
    
    std::cout << "[" << GetCurrentTime() << "] "
              << HttpMethodToString(req.method) << " " << req.path
              << std::endl;
    
    // Route the request
    if (req.path == "/" || req.path == "/index.html") {
        return HttpResponse::Html(GetIndexHtml());
    }
    
    if (req.path == "/api/hello") {
        return HttpResponse::Json(R"({"message": "Hello from hotcoco!", "status": "ok"})");
    }
    
    if (req.path == "/api/time") {
        std::ostringstream json;
        json << R"({"time": ")" << GetCurrentTime() << R"(", "timezone": "server"})";
        return HttpResponse::Json(json.str());
    }
    
    if (req.path == "/api/stats") {
        std::ostringstream json;
        json << R"({"requests": )" << g_request_count << R"(, "uptime": "running"})";
        return HttpResponse::Json(json.str());
    }
    
    return HttpResponse::NotFound("404 - Page not found: " + req.path);
}

int main() {
    std::cout << "=== Hotcoco Example 05: HTTP Server ===" << std::endl;
    std::cout << std::endl;
    
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
    
    HttpServer server(executor, "127.0.0.1", 8080);
    server.OnRequest(HandleRequest);
    
    std::cout << "Server listening on http://127.0.0.1:8080" << std::endl;
    std::cout << "Press Ctrl+C to stop" << std::endl;
    std::cout << std::endl;
    std::cout << "Try these URLs:" << std::endl;
    std::cout << "  curl http://localhost:8080/" << std::endl;
    std::cout << "  curl http://localhost:8080/api/hello" << std::endl;
    std::cout << "  curl http://localhost:8080/api/time" << std::endl;
    std::cout << std::endl;
    
    auto server_task = server.Run();
    executor.Schedule(server_task.GetHandle());
    executor.Run();
    
    return 0;
}
