// ============================================================================
// hotcoco/http/http.hpp - HTTP Request/Response Types
// ============================================================================
//
// This header provides HTTP primitives for building HTTP servers:
// - HttpRequest: Parsed HTTP request from client
// - HttpResponse: Response to send back
// - HttpParser: llhttp-based request parser
// - HttpServer: High-level async HTTP server
//
// USAGE:
// ------
//   HttpServer server(executor, "127.0.0.1", 8080);
//   server.OnRequest([](const HttpRequest& req) -> HttpResponse {
//       return HttpResponse::Ok("Hello, World!");
//   });
//   co_await server.Run();
//
// ============================================================================

#pragma once

#include "hotcoco/core/task.hpp"
#include "hotcoco/io/libuv_executor.hpp"
#include "hotcoco/io/tcp.hpp"

#include <algorithm>
#include <functional>
#include <llhttp.h>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace hotcoco {

// ============================================================================
// Case-insensitive comparator for HTTP headers (RFC 7230)
// ============================================================================
struct CaseInsensitiveLess {
    bool operator()(const std::string& a, const std::string& b) const {
        return std::lexicographical_compare(
            a.begin(), a.end(), b.begin(), b.end(),
            [](unsigned char ca, unsigned char cb) { return std::tolower(ca) < std::tolower(cb); });
    }
};

using HeaderMap = std::map<std::string, std::string, CaseInsensitiveLess>;

// ============================================================================
// HttpMethod - HTTP request methods
// ============================================================================
enum class HttpMethod { GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS, UNKNOWN };

const char* HttpMethodToString(HttpMethod method);
HttpMethod HttpMethodFromLlhttp(llhttp_method_t method);

// ============================================================================
// HttpRequest - Parsed HTTP request
// ============================================================================
class HttpRequest {
   public:
    HttpMethod method = HttpMethod::GET;
    std::string url;
    std::string path;
    std::string query;
    HeaderMap headers;
    std::string body;
    int http_major = 1;
    int http_minor = 1;

    // Helper methods
    [[nodiscard]] std::string GetHeader(const std::string& name) const;
    [[nodiscard]] bool HasHeader(const std::string& name) const;

    // HTTP/1.0 defaults to close; HTTP/1.1 defaults to keep-alive
    [[nodiscard]] bool ShouldKeepAlive() const;
};

// ============================================================================
// HttpResponse - HTTP response to send
// ============================================================================
class HttpResponse {
   public:
    int status_code = 200;
    std::string status_text = "OK";
    HeaderMap headers;
    std::string body;

    // Factory methods for common responses
    [[nodiscard]] static HttpResponse Ok(const std::string& body = "", const std::string& content_type = "text/plain");
    [[nodiscard]] static HttpResponse Html(const std::string& html);
    [[nodiscard]] static HttpResponse Json(const std::string& json);
    [[nodiscard]] static HttpResponse NotFound(const std::string& message = "Not Found");
    [[nodiscard]] static HttpResponse BadRequest(const std::string& message = "Bad Request");
    [[nodiscard]] static HttpResponse InternalError(const std::string& message = "Internal Server Error");

    // Set a header
    HttpResponse& SetHeader(const std::string& name, const std::string& value);

    // Serialize to HTTP wire format
    [[nodiscard]] std::string Serialize() const;
};

// ============================================================================
// HttpParser - llhttp-based request parser
// ============================================================================
class HttpParser {
   public:
    static constexpr size_t kMaxBodySize = 10 * 1024 * 1024;  // 10 MB
    static constexpr size_t kMaxUrlLength = 8192;             // 8 KB
    static constexpr size_t kMaxHeaderCount = 100;
    static constexpr size_t kMaxTotalHeaderSize = 64 * 1024;  // 64 KB

    HttpParser();
    ~HttpParser();

    // Parse incoming data, returns true when complete request is ready
    [[nodiscard]] bool Parse(const char* data, size_t len);

    // Get the parsed request (valid after Parse returns true)
    [[nodiscard]] HttpRequest GetRequest();

    // Reset parser for next request
    void Reset();

    // Check if an error occurred
    [[nodiscard]] bool HasError() const { return error_; }
    [[nodiscard]] std::string GetError() const { return error_message_; }

   private:
    // llhttp callbacks
    static int OnUrl(llhttp_t* parser, const char* at, size_t len);
    static int OnHeaderField(llhttp_t* parser, const char* at, size_t len);
    static int OnHeaderValue(llhttp_t* parser, const char* at, size_t len);
    static int OnBody(llhttp_t* parser, const char* at, size_t len);
    static int OnMessageComplete(llhttp_t* parser);

    llhttp_t parser_;
    llhttp_settings_t settings_;

    // Parsing state
    HttpRequest request_;
    std::string current_header_field_;
    std::string current_header_value_;
    enum class HeaderState { kField, kValue, kNone };
    HeaderState header_state_ = HeaderState::kNone;
    bool complete_ = false;
    bool error_ = false;
    std::string error_message_;
};

// ============================================================================
// HttpServer - Async HTTP server
// ============================================================================
class HttpServer {
   public:
    using RequestHandler = std::function<HttpResponse(const HttpRequest&)>;
    using Duration = std::chrono::milliseconds;

    static constexpr Duration kDefaultReadTimeout{30000};  // 30 seconds

    HttpServer(LibuvExecutor& executor, const std::string& host, uint16_t port);

    // Set the request handler
    void OnRequest(RequestHandler handler) { handler_ = std::move(handler); }

    // Set the per-connection read timeout (default 30s, prevents slowloris)
    void SetReadTimeout(Duration timeout) { read_timeout_ = timeout; }

    // Run the server (returns a Task that runs until stopped)
    // Returns 0 on success, negative libuv error code on bind/listen failure.
    Task<int> Run();

    // Stop the server
    void Stop() { running_ = false; }

   private:
    Task<void> HandleConnection(std::unique_ptr<TcpStream> stream);

    LibuvExecutor& executor_;
    std::string host_;
    uint16_t port_;
    RequestHandler handler_;
    bool running_ = true;
    Duration read_timeout_ = kDefaultReadTimeout;
};

}  // namespace hotcoco
