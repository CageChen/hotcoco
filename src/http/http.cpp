// ============================================================================
// hotcoco/http/http.cpp - HTTP Implementation
// ============================================================================

#include "hotcoco/http/http.hpp"

#include "hotcoco/core/spawn.hpp"

#include <chrono>
#include <sstream>

namespace hotcoco {

// ============================================================================
// HttpMethod helpers
// ============================================================================

const char* HttpMethodToString(HttpMethod method) {
    switch (method) {
        case HttpMethod::GET:
            return "GET";
        case HttpMethod::POST:
            return "POST";
        case HttpMethod::PUT:
            return "PUT";
        case HttpMethod::DELETE:
            return "DELETE";
        case HttpMethod::PATCH:
            return "PATCH";
        case HttpMethod::HEAD:
            return "HEAD";
        case HttpMethod::OPTIONS:
            return "OPTIONS";
        default:
            return "UNKNOWN";
    }
}

HttpMethod HttpMethodFromLlhttp(llhttp_method_t method) {
    switch (method) {
        case HTTP_GET:
            return HttpMethod::GET;
        case HTTP_POST:
            return HttpMethod::POST;
        case HTTP_PUT:
            return HttpMethod::PUT;
        case HTTP_DELETE:
            return HttpMethod::DELETE;
        case HTTP_PATCH:
            return HttpMethod::PATCH;
        case HTTP_HEAD:
            return HttpMethod::HEAD;
        case HTTP_OPTIONS:
            return HttpMethod::OPTIONS;
        default:
            return HttpMethod::UNKNOWN;
    }
}

// ============================================================================
// HttpRequest
// ============================================================================

std::string HttpRequest::GetHeader(const std::string& name) const {
    auto it = headers.find(name);
    if (it != headers.end()) {
        return it->second;
    }
    return "";
}

bool HttpRequest::HasHeader(const std::string& name) const {
    return headers.find(name) != headers.end();
}

bool HttpRequest::ShouldKeepAlive() const {
    std::string conn = GetHeader("Connection");
    // Case-insensitive value comparison
    auto toLower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
        return s;
    };
    std::string conn_lower = toLower(conn);

    if (http_major == 1 && http_minor == 0) {
        // HTTP/1.0 defaults to close; keep-alive only if explicitly requested
        return conn_lower == "keep-alive";
    }
    // HTTP/1.1+ defaults to keep-alive; close only if explicitly requested
    return conn_lower != "close";
}

// ============================================================================
// HttpResponse
// ============================================================================

HttpResponse HttpResponse::Ok(const std::string& body, const std::string& content_type) {
    HttpResponse res;
    res.status_code = 200;
    res.status_text = "OK";
    res.body = body;
    res.headers["Content-Type"] = content_type;
    res.headers["Content-Length"] = std::to_string(body.size());
    return res;
}

HttpResponse HttpResponse::Html(const std::string& html) {
    return Ok(html, "text/html; charset=utf-8");
}

HttpResponse HttpResponse::Json(const std::string& json) {
    return Ok(json, "application/json");
}

HttpResponse HttpResponse::NotFound(const std::string& message) {
    HttpResponse res;
    res.status_code = 404;
    res.status_text = "Not Found";
    res.body = message;
    res.headers["Content-Type"] = "text/plain";
    res.headers["Content-Length"] = std::to_string(message.size());
    return res;
}

HttpResponse HttpResponse::BadRequest(const std::string& message) {
    HttpResponse res;
    res.status_code = 400;
    res.status_text = "Bad Request";
    res.body = message;
    res.headers["Content-Type"] = "text/plain";
    res.headers["Content-Length"] = std::to_string(message.size());
    return res;
}

HttpResponse HttpResponse::InternalError(const std::string& message) {
    HttpResponse res;
    res.status_code = 500;
    res.status_text = "Internal Server Error";
    res.body = message;
    res.headers["Content-Type"] = "text/plain";
    res.headers["Content-Length"] = std::to_string(message.size());
    return res;
}

HttpResponse& HttpResponse::SetHeader(const std::string& name, const std::string& value) {
    headers[name] = value;
    return *this;
}

std::string HttpResponse::Serialize() const {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status_code << " " << status_text << "\r\n";

    for (const auto& [name, value] : headers) {
        oss << name << ": " << value << "\r\n";
    }

    oss << "\r\n";
    oss << body;

    return oss.str();
}

// ============================================================================
// HttpParser
// ============================================================================

HttpParser::HttpParser() {
    llhttp_settings_init(&settings_);

    settings_.on_url = OnUrl;
    settings_.on_header_field = OnHeaderField;
    settings_.on_header_value = OnHeaderValue;
    settings_.on_body = OnBody;
    settings_.on_message_complete = OnMessageComplete;

    llhttp_init(&parser_, HTTP_REQUEST, &settings_);
    parser_.data = this;
}

HttpParser::~HttpParser() = default;

bool HttpParser::Parse(const char* data, size_t len) {
    if (error_) return false;

    llhttp_errno_t err = llhttp_execute(&parser_, data, len);

    if (err != HPE_OK) {
        // Only overwrite error_message_ if the callback didn't already set
        // a custom message (e.g., "URL too long", "Too many headers").
        if (!error_) {
            error_message_ = llhttp_errno_name(err);
        }
        error_ = true;
        return false;
    }

    return complete_;
}

HttpRequest HttpParser::GetRequest() {
    request_.method = HttpMethodFromLlhttp(static_cast<llhttp_method_t>(parser_.method));
    request_.http_major = parser_.http_major;
    request_.http_minor = parser_.http_minor;
    // Parse path and query from the complete URL
    auto query_pos = request_.url.find('?');
    if (query_pos != std::string::npos) {
        request_.path = request_.url.substr(0, query_pos);
        request_.query = request_.url.substr(query_pos + 1);
    } else {
        request_.path = request_.url;
    }
    return std::move(request_);
}

void HttpParser::Reset() {
    llhttp_reset(&parser_);
    request_ = HttpRequest{};
    current_header_field_.clear();
    current_header_value_.clear();
    header_state_ = HeaderState::kNone;
    complete_ = false;
    error_ = false;
    error_message_.clear();
}

int HttpParser::OnUrl(llhttp_t* parser, const char* at, size_t len) {
    auto* self = static_cast<HttpParser*>(parser->data);
    if (self->request_.url.size() + len > HttpParser::kMaxUrlLength) {
        self->error_ = true;
        self->error_message_ = "URL too long";
        return HPE_USER;
    }
    self->request_.url.append(at, len);
    // Defer path/query parsing to GetRequest() — OnUrl may be called
    // incrementally as the URL arrives in chunks.
    return 0;
}

int HttpParser::OnHeaderField(llhttp_t* parser, const char* at, size_t len) {
    auto* self = static_cast<HttpParser*>(parser->data);
    // llhttp may call this callback multiple times for a single header field
    // (chunked parsing). Must append, not assign, to handle partial delivery.
    if (self->header_state_ == HeaderState::kValue) {
        // Starting a new header field — save previous and reset
        self->current_header_field_.clear();
    }
    self->current_header_field_.append(at, len);
    self->header_state_ = HeaderState::kField;
    return 0;
}

int HttpParser::OnHeaderValue(llhttp_t* parser, const char* at, size_t len) {
    auto* self = static_cast<HttpParser*>(parser->data);
    // Similarly, header values may arrive in chunks
    if (self->header_state_ == HeaderState::kField) {
        // Starting a new value for the current field
        self->current_header_value_.clear();
    }
    self->current_header_value_.append(at, len);
    // RFC 7230 §3.2.2: multiple headers with the same field name are
    // semantically equivalent to a single header with comma-separated values.
    auto it = self->request_.headers.find(self->current_header_field_);
    if (it != self->request_.headers.end() && self->header_state_ == HeaderState::kField) {
        // New occurrence of an existing field — combine with ", "
        it->second += ", " + self->current_header_value_;
    } else if (it != self->request_.headers.end()) {
        // Same header, chunked value delivery — replace with latest chunk assembly
        it->second = self->current_header_value_;
    } else {
        self->request_.headers[self->current_header_field_] = self->current_header_value_;
    }
    self->header_state_ = HeaderState::kValue;

    // Enforce header count limit
    if (self->request_.headers.size() > HttpParser::kMaxHeaderCount) {
        self->error_ = true;
        self->error_message_ = "Too many headers";
        return HPE_USER;
    }
    // Enforce cumulative header size limit
    size_t total = 0;
    for (const auto& [k, v] : self->request_.headers) {
        total += k.size() + v.size();
    }
    if (total > HttpParser::kMaxTotalHeaderSize) {
        self->error_ = true;
        self->error_message_ = "Headers too large";
        return HPE_USER;
    }
    return 0;
}

int HttpParser::OnBody(llhttp_t* parser, const char* at, size_t len) {
    auto* self = static_cast<HttpParser*>(parser->data);
    if (self->request_.body.size() + len > HttpParser::kMaxBodySize) {
        self->error_ = true;
        self->error_message_ = "Body too large";
        return HPE_USER;
    }
    self->request_.body.append(at, len);
    return 0;
}

int HttpParser::OnMessageComplete(llhttp_t* parser) {
    auto* self = static_cast<HttpParser*>(parser->data);
    self->complete_ = true;
    return 0;
}

// ============================================================================
// HttpServer
// ============================================================================

HttpServer::HttpServer(LibuvExecutor& executor, const std::string& host, uint16_t port)
    : executor_(executor), host_(host), port_(port) {}

Task<int> HttpServer::Run() {
    TcpListener listener(executor_);

    int result = listener.Bind(host_, port_);
    if (result != 0) {
        co_return result;
    }

    result = listener.Listen();
    if (result != 0) {
        co_return result;
    }

    while (running_) {
        auto stream = co_await listener.Accept();
        if (stream) {
            // Use Spawn() to manage the connection handler's lifetime.
            // This ensures the coroutine frame stays alive until completion.
            // Previously, the Task was a local variable that got destroyed
            // immediately after scheduling, causing use-after-free.
            Spawn(executor_, HandleConnection(std::move(stream)));
        }
    }

    co_return 0;
}

Task<void> HttpServer::HandleConnection(std::unique_ptr<TcpStream> stream) {
    HttpParser parser;
    auto deadline = std::chrono::steady_clock::now() + read_timeout_;

    while (stream->IsOpen()) {
        // Check read timeout (slowloris protection)
        if (std::chrono::steady_clock::now() >= deadline) {
            auto response = HttpResponse::BadRequest("Request timeout");
            co_await stream->Write(response.Serialize());
            break;
        }

        auto data = co_await stream->Read();

        if (data.empty()) {
            break;
        }

        bool complete = parser.Parse(data.data(), data.size());

        if (parser.HasError()) {
            auto response = HttpResponse::BadRequest(parser.GetError());
            co_await stream->Write(response.Serialize());
            break;
        }

        if (complete) {
            HttpRequest request = parser.GetRequest();

            HttpResponse response;
            if (handler_) {
                response = handler_(request);
            } else {
                response = HttpResponse::NotFound("No handler configured");
            }

            co_await stream->Write(response.Serialize());

            // Check keep-alive semantics (HTTP/1.0 vs HTTP/1.1)
            if (!request.ShouldKeepAlive()) {
                break;
            }

            parser.Reset();
            // Reset deadline for next request on keep-alive connections
            deadline = std::chrono::steady_clock::now() + read_timeout_;
        }
    }

    stream->Close();
}

}  // namespace hotcoco
