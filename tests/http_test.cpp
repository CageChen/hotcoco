// ============================================================================
// HTTP Unit Tests
// ============================================================================

#include "hotcoco/http/http.hpp"

#include "hotcoco/core/task.hpp"
#include "hotcoco/io/libuv_executor.hpp"
#include "hotcoco/io/timer.hpp"

#include <gtest/gtest.h>

using namespace hotcoco;
using namespace std::chrono_literals;

// ============================================================================
// HttpResponse Tests
// ============================================================================

TEST(HttpTest, ResponseOk) {
    auto response = HttpResponse::Ok("Hello, World!");
    EXPECT_EQ(response.status_code, 200);
    EXPECT_EQ(response.status_text, "OK");
    EXPECT_EQ(response.body, "Hello, World!");
    EXPECT_EQ(response.headers["Content-Type"], "text/plain");
}

TEST(HttpTest, ResponseHtml) {
    auto response = HttpResponse::Html("<h1>Title</h1>");
    EXPECT_EQ(response.status_code, 200);
    EXPECT_EQ(response.headers["Content-Type"], "text/html; charset=utf-8");
}

TEST(HttpTest, ResponseJson) {
    auto response = HttpResponse::Json(R"({"key": "value"})");
    EXPECT_EQ(response.status_code, 200);
    EXPECT_EQ(response.headers["Content-Type"], "application/json");
}

TEST(HttpTest, ResponseNotFound) {
    auto response = HttpResponse::NotFound("Page not found");
    EXPECT_EQ(response.status_code, 404);
    EXPECT_EQ(response.body, "Page not found");
}

TEST(HttpTest, ResponseSerialize) {
    auto response = HttpResponse::Ok("Test");
    std::string serialized = response.Serialize();

    EXPECT_TRUE(serialized.find("HTTP/1.1 200 OK") != std::string::npos);
    EXPECT_TRUE(serialized.find("Content-Type: text/plain") != std::string::npos);
    EXPECT_TRUE(serialized.find("Test") != std::string::npos);
}

// ============================================================================
// HttpParser Tests
// ============================================================================

TEST(HttpTest, ParserSimpleGet) {
    HttpParser parser;

    const char* request =
        "GET /hello HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";

    bool complete = parser.Parse(request, strlen(request));
    EXPECT_TRUE(complete);
    EXPECT_FALSE(parser.HasError());

    auto req = parser.GetRequest();
    EXPECT_EQ(req.method, HttpMethod::GET);
    EXPECT_EQ(req.path, "/hello");
    EXPECT_EQ(req.GetHeader("Host"), "localhost");
}

TEST(HttpTest, ParserPostWithBody) {
    HttpParser parser;

    const char* request =
        "POST /api/data HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 11\r\n"
        "\r\n"
        "Hello World";

    bool complete = parser.Parse(request, strlen(request));
    EXPECT_TRUE(complete);

    auto req = parser.GetRequest();
    EXPECT_EQ(req.method, HttpMethod::POST);
    EXPECT_EQ(req.path, "/api/data");
    EXPECT_EQ(req.body, "Hello World");
}

TEST(HttpTest, ParserQueryString) {
    HttpParser parser;

    const char* request =
        "GET /search?q=test&page=1 HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";

    ASSERT_TRUE(parser.Parse(request, strlen(request)));
    auto req = parser.GetRequest();

    EXPECT_EQ(req.path, "/search");
    EXPECT_EQ(req.query, "q=test&page=1");
}

// ============================================================================
// Bug regression: header fields/values delivered in chunks must be appended
// ============================================================================
// llhttp may deliver header field/value data across multiple callbacks.
// Previously, OnHeaderField used assign() which truncated partial data.

TEST(HttpTest, ParserChunkedHeaderDelivery) {
    HttpParser parser;

    // Feed the request in small chunks to trigger partial callback delivery
    const char* request =
        "GET / HTTP/1.1\r\n"
        "X-Custom-Long-Header: some-very-long-value-here\r\n"
        "Host: localhost\r\n"
        "\r\n";

    // Feed one byte at a time to maximize chunked callback delivery
    size_t len = strlen(request);
    bool complete = false;
    for (size_t i = 0; i < len; ++i) {
        complete = parser.Parse(request + i, 1);
        ASSERT_FALSE(parser.HasError()) << "Parse error at byte " << i;
        if (complete) break;
    }

    ASSERT_TRUE(complete);
    auto req = parser.GetRequest();

    EXPECT_EQ(req.GetHeader("X-Custom-Long-Header"), "some-very-long-value-here");
    EXPECT_EQ(req.GetHeader("Host"), "localhost");
}

// ============================================================================
// HttpServer Integration Test
// ============================================================================

// Test that HttpServer properly handles multiple concurrent connections.
// This test reproduces a bug where the connection handler Task was destroyed
// immediately after scheduling, causing use-after-free.
TEST(HttpTest, ServerHandlesMultipleConnections) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
    std::atomic<int> requests_handled{0};
    const int num_clients = 3;

    HttpServer server(executor, "127.0.0.1", 19876);
    server.OnRequest([&](const HttpRequest& req) -> HttpResponse {
        requests_handled++;
        return HttpResponse::Ok("Response for " + req.path);
    });

    // Server task
    auto server_task = [&]() -> Task<void> {
        // Run server in a spawned task, stop after timeout
        auto run_task = server.Run();
        executor.Schedule(run_task.GetHandle());

        // Wait for all clients to complete
        co_await AsyncSleep(300ms);
        server.Stop();
        executor.Stop();
    };

    // Client task - makes a request and verifies response
    auto client_task = [&](int id) -> Task<void> {
        co_await AsyncSleep(std::chrono::milliseconds(50 + id * 20));

        TcpStream client(executor);
        int result = co_await client.Connect("127.0.0.1", 19876);
        if (result != 0) co_return;

        std::string request = "GET /client" + std::to_string(id) +
                              " HTTP/1.1\r\n"
                              "Host: localhost\r\n"
                              "Connection: close\r\n"
                              "\r\n";
        co_await client.Write(request);

        auto response = co_await client.Read();
        std::string resp_str(response.begin(), response.end());

        EXPECT_TRUE(resp_str.find("200 OK") != std::string::npos);
        EXPECT_TRUE(resp_str.find("Response for /client" + std::to_string(id)) != std::string::npos);

        client.Close();
    };

    auto server_coro = server_task();
    executor.Schedule(server_coro.GetHandle());

    // Launch multiple clients
    std::vector<Task<void>> clients;
    for (int i = 0; i < num_clients; i++) {
        clients.push_back(client_task(i));
    }
    for (auto& c : clients) {
        executor.Schedule(c.GetHandle());
    }

    executor.Run();

    EXPECT_EQ(requests_handled.load(), num_clients);
}

TEST(HttpTest, ServerHandlesRequest) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
    bool request_received = false;
    std::string received_path;
    std::string response_sent;

    // Server task - handle request manually
    auto server_task = [&]() -> Task<void> {
        TcpListener listener(executor);
        EXPECT_EQ(listener.Bind("127.0.0.1", 19877), 0);
        EXPECT_EQ(listener.Listen(), 0);

        auto stream = co_await listener.Accept();
        EXPECT_NE(stream, nullptr);

        // Read request
        auto data = co_await stream->Read();
        std::string request_str(data.begin(), data.end());

        // Parse with HttpParser
        HttpParser parser;
        EXPECT_TRUE(parser.Parse(data.data(), data.size()));
        auto req = parser.GetRequest();

        request_received = true;
        received_path = req.path;

        // Send response
        auto response = HttpResponse::Ok("Hello from server!");
        response_sent = response.Serialize();
        co_await stream->Write(response_sent);

        stream->Close();
        executor.Stop();
    };

    // Client task
    auto client_task = [&]() -> Task<void> {
        co_await AsyncSleep(50ms);  // Wait for server

        TcpStream client(executor);
        int result = co_await client.Connect("127.0.0.1", 19877);
        EXPECT_EQ(result, 0);

        // Send HTTP request
        const char* request =
            "GET /test HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n"
            "\r\n";
        co_await client.Write(request);

        // Read response
        auto response = co_await client.Read();
        std::string resp_str(response.begin(), response.end());

        EXPECT_TRUE(resp_str.find("200 OK") != std::string::npos);
        EXPECT_TRUE(resp_str.find("Hello from server!") != std::string::npos);

        client.Close();
    };

    auto server = server_task();
    auto client = client_task();

    executor.Schedule(server.GetHandle());
    executor.Schedule(client.GetHandle());
    executor.Run();

    EXPECT_TRUE(request_received);
    EXPECT_EQ(received_path, "/test");
}

// ============================================================================
// Bug regression: HTTP header lookup must be case-insensitive (RFC 7230)
// ============================================================================
// Previously, headers used std::map<std::string, std::string> with default
// (case-sensitive) comparison. RFC 7230 requires header field names to be
// case-insensitive.

TEST(HttpTest, HeaderLookupCaseInsensitive) {
    HttpParser parser;

    const char* request =
        "GET / HTTP/1.1\r\n"
        "host: localhost\r\n"
        "Content-TYPE: text/html\r\n"
        "X-Custom-Header: value123\r\n"
        "\r\n";

    bool complete = parser.Parse(request, strlen(request));
    ASSERT_TRUE(complete);

    auto req = parser.GetRequest();

    // All of these should match regardless of case
    EXPECT_EQ(req.GetHeader("Host"), "localhost");
    EXPECT_EQ(req.GetHeader("host"), "localhost");
    EXPECT_EQ(req.GetHeader("HOST"), "localhost");

    EXPECT_EQ(req.GetHeader("content-type"), "text/html");
    EXPECT_EQ(req.GetHeader("Content-Type"), "text/html");
    EXPECT_EQ(req.GetHeader("CONTENT-TYPE"), "text/html");

    EXPECT_EQ(req.GetHeader("x-custom-header"), "value123");
    EXPECT_TRUE(req.HasHeader("X-CUSTOM-HEADER"));
}

// ============================================================================
// Bug regression: HTTP/1.0 defaults to Connection: close
// ============================================================================
// Previously, the server only checked for explicit "Connection: close" header,
// treating HTTP/1.0 as keep-alive by default. HTTP/1.0 should default to
// closing the connection unless "Connection: keep-alive" is explicit.

TEST(HttpTest, Http10DefaultsToClose) {
    HttpParser parser;

    const char* request =
        "GET / HTTP/1.0\r\n"
        "Host: localhost\r\n"
        "\r\n";

    bool complete = parser.Parse(request, strlen(request));
    ASSERT_TRUE(complete);

    auto req = parser.GetRequest();
    EXPECT_EQ(req.http_major, 1);
    EXPECT_EQ(req.http_minor, 0);
    EXPECT_FALSE(req.ShouldKeepAlive()) << "HTTP/1.0 without Connection header should default to close";
}

TEST(HttpTest, Http10ExplicitKeepAlive) {
    HttpParser parser;

    const char* request =
        "GET / HTTP/1.0\r\n"
        "Host: localhost\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";

    bool complete = parser.Parse(request, strlen(request));
    ASSERT_TRUE(complete);

    auto req = parser.GetRequest();
    EXPECT_TRUE(req.ShouldKeepAlive()) << "HTTP/1.0 with explicit Connection: keep-alive should keep alive";
}

TEST(HttpTest, Http11DefaultsToKeepAlive) {
    HttpParser parser;

    const char* request =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";

    bool complete = parser.Parse(request, strlen(request));
    ASSERT_TRUE(complete);

    auto req = parser.GetRequest();
    EXPECT_EQ(req.http_major, 1);
    EXPECT_EQ(req.http_minor, 1);
    EXPECT_TRUE(req.ShouldKeepAlive()) << "HTTP/1.1 without Connection header should default to keep-alive";
}

TEST(HttpTest, Http11ExplicitClose) {
    HttpParser parser;

    const char* request =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n";

    bool complete = parser.Parse(request, strlen(request));
    ASSERT_TRUE(complete);

    auto req = parser.GetRequest();
    EXPECT_FALSE(req.ShouldKeepAlive()) << "HTTP/1.1 with Connection: close should close";
}

// ============================================================================
// Bug regression: HttpServer::Run must report bind/listen failures
// ============================================================================
// Previously, Run() returned Task<void> and silently co_returned on failure.
// Now it returns Task<int> with 0 on success, negative error code on failure.

TEST(HttpTest, ServerRunReportsBindFailure) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
    int run_result = 0;

    // Bind to an invalid address to trigger failure
    HttpServer server(executor, "999.999.999.999", 0);
    server.OnRequest([](const HttpRequest&) -> HttpResponse { return HttpResponse::Ok("should not reach"); });

    auto task = [&]() -> Task<void> {
        run_result = co_await server.Run();
        executor.Stop();
    };

    auto t = task();
    executor.Schedule(t.GetHandle());
    executor.Run();

    EXPECT_NE(run_result, 0) << "Run() must report bind failure via return value";
}

// ============================================================================
// Bug regression: URL length limit
// ============================================================================
// Previously, OnUrl appended without limit, allowing unbounded memory growth.

TEST(HttpTest, UrlLengthLimit) {
    HttpParser parser;

    // Build a request with URL exceeding kMaxUrlLength (8192)
    std::string long_url = "GET /";
    long_url.append(HttpParser::kMaxUrlLength + 1, 'a');
    long_url += " HTTP/1.1\r\nHost: localhost\r\n\r\n";

    bool complete = parser.Parse(long_url.data(), long_url.size());
    EXPECT_FALSE(complete);
    EXPECT_TRUE(parser.HasError());
    EXPECT_EQ(parser.GetError(), "URL too long");
}

// ============================================================================
// Bug regression: header count limit
// ============================================================================

TEST(HttpTest, HeaderCountLimit) {
    HttpParser parser;

    std::string request = "GET / HTTP/1.1\r\n";
    for (size_t i = 0; i <= HttpParser::kMaxHeaderCount + 1; ++i) {
        request += "X-Header-" + std::to_string(i) + ": value\r\n";
    }
    request += "\r\n";

    bool complete = parser.Parse(request.data(), request.size());
    EXPECT_FALSE(complete);
    EXPECT_TRUE(parser.HasError());
    EXPECT_EQ(parser.GetError(), "Too many headers");
}

// ============================================================================
// Bug regression: multi-valued headers must be combined per RFC 7230
// ============================================================================
// Previously, duplicate header names would overwrite the earlier value.

TEST(HttpTest, MultiValuedHeadersCombined) {
    HttpParser parser;

    std::string request =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "X-Custom: value1\r\n"
        "X-Custom: value2\r\n"
        "\r\n";

    bool complete = parser.Parse(request.data(), request.size());
    ASSERT_TRUE(complete);

    auto req = parser.GetRequest();
    auto it = req.headers.find("x-custom");
    ASSERT_NE(it, req.headers.end());
    EXPECT_EQ(it->second, "value1, value2");
}

// ============================================================================
// HttpResponse Factory Tests
// ============================================================================

TEST(HttpTest, ResponseBadRequest) {
    auto response = HttpResponse::BadRequest("Invalid input");
    EXPECT_EQ(response.status_code, 400);
    EXPECT_EQ(response.status_text, "Bad Request");
    EXPECT_EQ(response.body, "Invalid input");
    EXPECT_EQ(response.headers["Content-Type"], "text/plain");
    EXPECT_EQ(response.headers["Content-Length"], "13");
}

TEST(HttpTest, ResponseInternalError) {
    auto response = HttpResponse::InternalError("Something broke");
    EXPECT_EQ(response.status_code, 500);
    EXPECT_EQ(response.status_text, "Internal Server Error");
    EXPECT_EQ(response.body, "Something broke");
    EXPECT_EQ(response.headers["Content-Type"], "text/plain");
}

TEST(HttpTest, ResponseSetHeaderChaining) {
    auto response = HttpResponse::Ok("body").SetHeader("X-Custom", "value1").SetHeader("X-Another", "value2");
    EXPECT_EQ(response.headers["X-Custom"], "value1");
    EXPECT_EQ(response.headers["X-Another"], "value2");
    // Verify it's still a valid response
    EXPECT_EQ(response.status_code, 200);
    EXPECT_EQ(response.body, "body");
}

TEST(HttpTest, ResponseOkCustomContentType) {
    auto response = HttpResponse::Ok("<xml/>", "application/xml");
    EXPECT_EQ(response.headers["Content-Type"], "application/xml");
    EXPECT_EQ(response.body, "<xml/>");
}

// ============================================================================
// HttpMethod Helper Tests
// ============================================================================

TEST(HttpTest, HttpMethodToStringAll) {
    EXPECT_STREQ(HttpMethodToString(HttpMethod::GET), "GET");
    EXPECT_STREQ(HttpMethodToString(HttpMethod::POST), "POST");
    EXPECT_STREQ(HttpMethodToString(HttpMethod::PUT), "PUT");
    EXPECT_STREQ(HttpMethodToString(HttpMethod::DELETE), "DELETE");
    EXPECT_STREQ(HttpMethodToString(HttpMethod::PATCH), "PATCH");
    EXPECT_STREQ(HttpMethodToString(HttpMethod::HEAD), "HEAD");
    EXPECT_STREQ(HttpMethodToString(HttpMethod::OPTIONS), "OPTIONS");
    EXPECT_STREQ(HttpMethodToString(HttpMethod::UNKNOWN), "UNKNOWN");
}

TEST(HttpTest, HttpMethodFromLlhttpAll) {
    EXPECT_EQ(HttpMethodFromLlhttp(HTTP_GET), HttpMethod::GET);
    EXPECT_EQ(HttpMethodFromLlhttp(HTTP_POST), HttpMethod::POST);
    EXPECT_EQ(HttpMethodFromLlhttp(HTTP_PUT), HttpMethod::PUT);
    EXPECT_EQ(HttpMethodFromLlhttp(HTTP_DELETE), HttpMethod::DELETE);
    EXPECT_EQ(HttpMethodFromLlhttp(HTTP_PATCH), HttpMethod::PATCH);
    EXPECT_EQ(HttpMethodFromLlhttp(HTTP_HEAD), HttpMethod::HEAD);
    EXPECT_EQ(HttpMethodFromLlhttp(HTTP_OPTIONS), HttpMethod::OPTIONS);
    // Unknown llhttp method maps to UNKNOWN
    EXPECT_EQ(HttpMethodFromLlhttp(static_cast<llhttp_method_t>(999)), HttpMethod::UNKNOWN);
}

// ============================================================================
// HttpParser Reset Test
// ============================================================================

TEST(HttpTest, ParserResetAndReuse) {
    HttpParser parser;

    // Parse first request
    const char* req1 =
        "GET /first HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";
    ASSERT_TRUE(parser.Parse(req1, strlen(req1)));
    auto r1 = parser.GetRequest();
    EXPECT_EQ(r1.path, "/first");

    // Reset and parse second request
    parser.Reset();

    const char* req2 =
        "POST /second HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 4\r\n"
        "\r\n"
        "data";
    ASSERT_TRUE(parser.Parse(req2, strlen(req2)));
    auto r2 = parser.GetRequest();
    EXPECT_EQ(r2.path, "/second");
    EXPECT_EQ(r2.method, HttpMethod::POST);
    EXPECT_EQ(r2.body, "data");
}
