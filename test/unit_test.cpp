#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "jsonrpc.hpp"
#include <sstream>
#include <chrono>
#include <thread>

using namespace jsonrpc;
using json = nlohmann::json;
using namespace std::chrono_literals;

#define CHECK_JSONRPC_EXCEPTION(statement, expected_code)            \
    try {                                                            \
        statement;                                                   \
        CHECK_MESSAGE(false, "Should have thrown JsonRpcException"); \
    } catch (const JsonRpcException& e) {                            \
        CHECK(e.code == expected_code);                              \
    } catch (...) {                                                  \
        CHECK_MESSAGE(false, "Threw wrong type of exception");       \
    }

TEST_CASE("Error Serialization/Deserialization") {
    SUBCASE("Edge Case: Error missing 'code'") {
        json j = {
            {"message", "Missing code"}
        };
        CHECK_JSONRPC_EXCEPTION(j.get<Error>(), spec::kInternalError);
    }

    SUBCASE("Edge Case: Error 'code' is string") {
        json j = {
            {"code", "123"}, // Must be int.
            {"message", "Bad code type"}
        };
        CHECK_JSONRPC_EXCEPTION(j.get<Error>(), spec::kInternalError);
    }

    SUBCASE("Edge Case: Error missing 'message'") {
        json j = {
            {"code", -32000}
        };
        CHECK_JSONRPC_EXCEPTION(j.get<Error>(), spec::kInternalError);
    }
}


TEST_CASE("Request Serialization/Deserialization") {

    SUBCASE("Round-Trip: Valid Request with Array Params") {
        Request original{ 1, "add", json::array({1, 2}) };

        // 1. Struct -> JSON
        json j = original;
        CHECK(j["jsonrpc"] == "2.0");
        CHECK(j["method"] == "add");
        CHECK(j["id"] == 1);

        // 2. JSON -> Struct
        Request restored = j;
        CHECK(restored.id == 1);
        CHECK(restored.method == "add");
        CHECK(restored.params.is_array());
    }

    SUBCASE("Round-Trip: Valid Notification (No ID)") {
        Request original{ std::nullopt, "log", nullptr };

        json j = original;
        CHECK_FALSE(j.contains("id"));

        Request restored = j;
        CHECK(restored.id.has_value() == false);
        CHECK(restored.method == "log");
    }

    SUBCASE("Edge Case: Invalid Params Type (String)") {
        json j = {
            {"jsonrpc", "2.0"},
            {"method", "foo"},
            {"params", "I am a string"} // Invalid string
        };
        CHECK_JSONRPC_EXCEPTION(j.get<Request>(), spec::kInvalidRequest);
    }

    SUBCASE("Edge Case: Invalid ID Type (String)") {
        json j = {
            {"jsonrpc", "2.0"},
            {"method", "foo"},
            {"id", "123"} // Support int id only
        };
        CHECK_JSONRPC_EXCEPTION(j.get<Request>(), spec::kInvalidRequest);
    }
}

TEST_CASE("Request Advanced Validation") {
    SUBCASE("Edge Case: Request missing 'method'") {
        json j = {
            {"jsonrpc", "2.0"},
            {"id", 1},
            {"params", json::array()}
        };
        CHECK_JSONRPC_EXCEPTION(j.get<Request>(), spec::kInvalidRequest);
    }

    SUBCASE("Edge Case: Request 'method' is not string") {
        json j = {
            {"jsonrpc", "2.0"},
            {"id", 1},
            {"method", 123} // method must be string.
        };
        CHECK_JSONRPC_EXCEPTION(j.get<Request>(), spec::kInvalidRequest);
    }

    SUBCASE("Edge Case: Request params is null (Valid)") {
        json j = {
            {"jsonrpc", "2.0"},
            {"id", 1},
            {"method", "foo"},
            {"params", nullptr}
        };
        Request r = j;
        CHECK(r.params == nullptr);
    }
}

TEST_CASE("Response Serialization/Deserialization") {

    SUBCASE("Round-Trip: Success Response") {
        Response original = Response::make_success(42, "OK");

        json j = original;
        CHECK(j["id"] == 42);
        CHECK(j.contains("result"));
        CHECK_FALSE(j.contains("error"));

        Response restored = j;
        CHECK(restored.id == 42);
        CHECK(std::holds_alternative<json>(restored.content));
        CHECK(std::get<json>(restored.content) == "OK");
    }

    SUBCASE("Round-Trip: Error Response") {
        Response original = Response::make_error(42, spec::kMethodNotFound, "No such method");

        json j = original;
        CHECK(j["id"] == 42);
        CHECK(j.contains("error"));
        CHECK_FALSE(j.contains("result"));

        Response restored = j;
        CHECK(restored.id == 42);
        CHECK(std::holds_alternative<Error>(restored.content));

        auto& err = std::get<Error>(restored.content);
        CHECK(err.code == spec::kMethodNotFound);
        CHECK(err.message == "No such method");
    }

    SUBCASE("Edge Case: Response with BOTH result and error") {
        json j = {
            {"jsonrpc", "2.0"},
            {"id", 1},
            {"result", "ok"},
            {"error", {{"code", 0}, {"message", ""}}}
        };
        CHECK_JSONRPC_EXCEPTION(j.get<Response>(), spec::kInvalidRequest);
    }

    SUBCASE("Edge Case: Response with NEITHER result nor error") {
        json j = {
            {"jsonrpc", "2.0"},
            {"id", 1}
        };
        CHECK_JSONRPC_EXCEPTION(j.get<Response>(), spec::kInvalidRequest);
    }

    SUBCASE("Edge Case: Response with Null ID (Should fail at struct level)") {
        json j = {
            {"jsonrpc", "2.0"},
            {"id", nullptr},
            {"error", {{"code", -32600}, {"message", "error"}}}
        };
        CHECK_JSONRPC_EXCEPTION(j.get<Response>(), spec::kInvalidRequest);
    }
    SUBCASE("Edge Case: Response with no ID") {
        json j = {
            {"jsonrpc", "2.0"},
            {"error", {{"code", -32600}, {"message", "error"}}}
        };
        CHECK_JSONRPC_EXCEPTION(j.get<Response>(), spec::kInvalidRequest);
    }
}

TEST_CASE("Parser Behavior") {

    SUBCASE("Parse Valid Request") {
        json j = { {"jsonrpc", "2.0"}, {"method", "add"}, {"id", 1} };
        auto msg = Parser::parse(j);
        CHECK(std::holds_alternative<Request>(msg));
    }

    SUBCASE("Parse Valid Response") {
        json j = { {"jsonrpc", "2.0"}, {"result", "ok"}, {"id", 1} };
        auto msg = Parser::parse(j);
        CHECK(std::holds_alternative<Response>(msg));
    }

    SUBCASE("Parse Fatal Error (ID is null)") {
        json j = {
            {"jsonrpc", "2.0"},
            {"id", nullptr},
            {"error", {{"code", -32700}, {"message", "Parse error"}}}
        };

        auto msg = Parser::parse(j);

        CHECK(std::holds_alternative<Error>(msg));
        CHECK(std::get<Error>(msg).code == -32700);
    }

    SUBCASE("Parse Garbage (Missing method and id)") {
        json j = { {"jsonrpc", "2.0"}, {"foo", "bar"} };
        CHECK_JSONRPC_EXCEPTION(Parser::parse(j), spec::kInvalidRequest);
    }
}

TEST_CASE("Parser Advanced Boundaries") {
    SUBCASE("Parse Invalid: id is null but has result (Ambiguous)") {
        json j = {
            {"jsonrpc", "2.0"},
            {"id", nullptr},
            {"result", "Should not happen"}
        };
        CHECK_JSONRPC_EXCEPTION(Parser::parse(j), spec::kInvalidRequest);
    }

    SUBCASE("Parse Invalid: Not an object (Batch request?)") {
        // Batch is not supported.
        json j = json::array();
        CHECK_JSONRPC_EXCEPTION(Parser::parse(j), spec::kInvalidRequest);
    }
}

static void write_rpc_packet(std::ostream& os, const std::string& body) {
    os << "Content-Length: " << body.length() << "\r\n\r\n" << body << std::flush;
}

TEST_CASE("Conn: I/O Failure") {
    std::stringstream mock_in;
    std::stringstream mock_out;
    std::stringstream mock_err;

    bool waked = false;
    Conn conn([&]() { waked = true; }, mock_in, mock_out, mock_err, 1024);

    SUBCASE("Immediate EOF: Empty stream") {
        std::stringstream empty_in("");
        Conn eof_conn([]() {}, empty_in, mock_out, mock_err);

        eof_conn.start();
        std::this_thread::sleep_for(10ms);

        CHECK_FALSE(eof_conn.is_running());
    }

    SUBCASE("Partial EOF: Stream ends mid-body") {
        conn.start();
        mock_in << "Content-Length: 100\r\n\r\nPartial";

        std::this_thread::sleep_for(10ms);
        CHECK_FALSE(conn.is_running());
        CHECK(waked);
    }

    SUBCASE("Malformed header: missing length value") {
        conn.start();
        mock_in << "Content-Length: \r\n\r\n{}";
        std::this_thread::sleep_for(10ms);
        CHECK_FALSE(conn.is_running());
        CHECK(waked);
    }

    SUBCASE("Malformed header: invalid length") {
        conn.start();
        mock_in << "Content-Length: ABC \r\n\r\n{}";
        std::this_thread::sleep_for(10ms);
        CHECK_FALSE(conn.is_running());
        CHECK(waked);
    }

    SUBCASE("Malformed header: incorrect spelling") {
        conn.start();
        mock_in << "CONTENT-LENGTH: 123 \r\n\r\n{}";
        std::this_thread::sleep_for(10ms);
        CHECK_FALSE(conn.is_running());
        CHECK(waked);
    }

    SUBCASE("Oversized packet rejection") {
        conn.start();
        mock_in << "Content-Length: 2000\r\n\r\n{}";
        std::this_thread::sleep_for(10ms);
        CHECK_FALSE(conn.is_running());
        CHECK(mock_err.str().find("Packet too large") != std::string::npos);
        CHECK(waked);
    }
}

TEST_CASE("Conn: Protocol Robustness (Recoverable)") {
    std::stringstream mock_in, mock_out, mock_err;
    Conn conn([]() {}, mock_in, mock_out, mock_err);
    conn.start();

    SUBCASE("Broken JSON Body - Should stay alive and reply error") {
        write_rpc_packet(mock_in, R"({"jsonrpc":"2.0", "method": "test)");
        write_rpc_packet(mock_in, R"({"jsonrpc":"2.0", "method": "ping", "id": 2})");

        std::this_thread::sleep_for(10ms);
        conn.process_queue();
        CHECK(mock_out.str().find("-32700") != std::string::npos);
        CHECK(mock_out.str().find(R"("id":2)") != std::string::npos);
    }

    SUBCASE("Header Robustness: Whitespace flexibility") {
        conn.start();
        // 1 space
        mock_in << "Content-Length: 22\r\n\r\n{\"id\":1, \"method\":\"a\"}";
        // 0 space
        mock_in << "Content-Length:22\r\n\r\n{\"id\":2, \"method\":\"a\"}";
        // many space
        mock_in << "Content-Length:   22\r\n\r\n{\"id\":3, \"method\":\"a\"}";
        std::this_thread::sleep_for(20ms);
        conn.process_queue();

        CHECK(mock_out.str().find(R"("id":1)") != std::string::npos);
        CHECK(mock_out.str().find(R"("id":2)") != std::string::npos);
        CHECK(mock_out.str().find(R"("id":3)") != std::string::npos);

        conn.stop();
    }

    conn.stop();
}

TEST_CASE("Conn: Application Logic (Normal)") {
    std::stringstream in, out, err;
    Conn conn([]() {}, in, out, err);

    SUBCASE("Synchronous Method: add") {
        conn.register_method("add", [](const json& p) {
            return p[0].get<int>() + p[1].get<int>();
            });

        conn.start();
        write_rpc_packet(in, R"({"jsonrpc":"2.0","method":"add","params":[10,20],"id":1})");

        std::this_thread::sleep_for(20ms);
        conn.process_queue();

        CHECK(out.str().find(R"("result":30)") != std::string::npos);
        CHECK(out.str().find(R"("id":1)") != std::string::npos);
        conn.stop();
    }

    SUBCASE("Asynchronous Method: delayed_reply") {
        conn.register_async_method("delay", [](Context ctx, const json& p) {
            std::thread([ctx, p]() mutable {
                std::this_thread::sleep_for(10ms);
                ctx.reply(p[0].get<std::string>());
                }).detach();
            });

        conn.start();
        write_rpc_packet(in, R"({"jsonrpc":"2.0","method":"delay","params":["ok"],"id":42})");

        std::this_thread::sleep_for(10ms);
        conn.process_queue();

        std::this_thread::sleep_for(30ms);
        CHECK(out.str().find(R"("result":"ok")") != std::string::npos);
        CHECK(out.str().find(R"("id":42)") != std::string::npos);
        conn.stop();
    }

    SUBCASE("Notification: log_event") {
        std::string logs;
        conn.register_notification("log", [&](const json& p) {
            logs = p[0].get<std::string>();
            });

        conn.start();
        write_rpc_packet(in, R"({"jsonrpc":"2.0","method":"log","params":["hello"]})");

        std::this_thread::sleep_for(10ms);
        conn.process_queue();

        CHECK(logs == "hello");
        CHECK(out.str().empty());
        conn.stop();
    }
}

TEST_CASE("Conn: Boundary Errors (Spec Validation)") {
    std::stringstream in, out, err;
    Conn conn([]() {}, in, out, err);
    auto test = [&](std::string input, const char* msg) -> void {
        out.clear();
        in.clear();
        write_rpc_packet(in, input);
        conn.start();
        std::this_thread::sleep_for(10ms);
        conn.process_queue();
        CHECK(out.str().find(msg) != std::string::npos);
        conn.stop();
        };

    SUBCASE("Parser Level Errors") {
        // parse_not_object
        test(R"([1, 2, 3])", spec::details::parse_not_object);
        // parse_broken
        test(R"({"foo": "bar"})", spec::details::parse_broken);
        // parse_id_null_no_error
        test(R"({"jsonrpc":"2.0", "id": null})", spec::details::parse_id_null_no_error);
        // parse_id_type
        test(R"({"jsonrpc":"2.0", "id": 1.23})", spec::details::parse_id_type);
    }

    SUBCASE("Request Object Validation") {
        // req_method_miss
        test(R"({"jsonrpc":"2.0", "method": 123, "id": 1})", spec::details::req_method_miss);
        // req_id_type
        test(R"({"jsonrpc":"2.0", "method": "test", "id": "string_id"})", spec::details::req_id_type);
        // req_params_type
        test(R"({"jsonrpc":"2.0", "method": "test", "params": "string"})", spec::details::req_params_type);
    }

    SUBCASE("Response Object Validation") {
        // resp_missing_id
        // resp_id_type

        // resp_conflict
        test(R"({"jsonrpc":"2.0", "id": 1, "result": {}, "error": {"code":1,"message":"x"}})", spec::details::resp_conflict);
        // resp_incomplete
        test(R"({"jsonrpc":"2.0", "id": 1})", spec::details::resp_incomplete);
    }

    SUBCASE("Error Object Validation") {
        // err_code_miss
        test(R"({"jsonrpc":"2.0", "id": null, "error": {"message": "no code"}})", spec::details::err_code_miss);
        // err_code_type
        test(R"({"jsonrpc":"2.0", "id": null, "error": {"code": "1", "message": "msg"}})", spec::details::err_code_type);
        // err_msg_miss
        test(R"({"jsonrpc":"2.0", "id": null, "error": {"code": 1}})", spec::details::err_msg_miss);
        // err_msg_type
        test(R"({"jsonrpc":"2.0", "id": null, "error": {"code": 1, "message": 123}})", spec::details::err_msg_type);
    }
}

TEST_CASE("Conn: Active Client Logic (Outbound)") {
    std::stringstream in, out, err;
    Conn conn([]() {}, in, out, err);

    SUBCASE("Send Request & Handle Successful Response") {
        bool handled = false;
        conn.send_request("query", { {"id", "user1"} }, [&](const Response& resp) {
            handled = true;
            CHECK(std::get<json>(resp.content)["status"] == "active");
           });
        CHECK(out.str().find(R"("id":1)") != std::string::npos);

        write_rpc_packet(in, R"({"jsonrpc":"2.0", "id": 1, "result": {"status": "active"}})");
        conn.start();
        std::this_thread::sleep_for(10ms);
        conn.process_queue();
        CHECK(handled);
    }

    SUBCASE("Send Request & Handle Error Response") {
        bool handled = false;
        conn.send_request("risky_call", nullptr, [&](const Response& resp) {
            handled = true;
            CHECK(resp.is_error());
            auto& err = std::get<Error>(resp.content);
            CHECK(err.code == -1);
            CHECK(err.message == "fail");
            });

        write_rpc_packet(in, R"({"jsonrpc":"2.0", "id": 1, "error": {"code": -1, "message": "fail"}})");
        conn.start();
        std::this_thread::sleep_for(10ms);
        conn.process_queue();
        CHECK(handled);
    }

    SUBCASE("ID Generation Logic") {
        conn.send_request("req1", nullptr, [](auto) {});
        conn.send_request("req2", nullptr, [](auto) {});

        std::string output = out.str();
        CHECK(output.find(R"("id":1)") != std::string::npos);
        CHECK(output.find(R"("id":2)") != std::string::npos);
    }
}

TEST_CASE("Conn: Raw Handler Interception") {
    std::stringstream in, out, err;
    Conn conn([]() {}, in, out, err);

    bool intercepted = false;
    conn.set_raw_handler([&](const IncomingMessage&, Conn&) {
        intercepted = true;
        return true;
        });

    conn.register_method("ping", [](const json&) { return "pong"; });
    conn.start();
    write_rpc_packet(in, R"({"jsonrpc":"2.0","method":"ping","id":1})");

    std::this_thread::sleep_for(10ms);
    conn.process_queue();

    CHECK(intercepted);
    CHECK(out.str().empty());
}

TEST_CASE("Conn: User Logic Exceptions") {
    std::stringstream in, out, err;
    Conn conn([]() {}, in, out, err);

    conn.register_method("validate_age", [](const json& p) -> json {
        int age = p[0].get<int>();
        if (age < 0 || age > 150) {
            throw JsonRpcException(spec::kInvalidParams, "Age must be between 0 and 150");
        }
        return "ok";
        });

    conn.register_method("crash_me", [](const json&) -> json {
        throw std::runtime_error("Database connection lost");
        });

    conn.start();

    SUBCASE("User-defined Protocol Error: Invalid Params (-32602)") {
        write_rpc_packet(in, R"({"jsonrpc":"2.0","method":"validate_age","params":[-5],"id":10})");

        std::this_thread::sleep_for(10ms);
        conn.process_queue();

        std::string output = out.str();
        CHECK(output.find(R"("code":-32602)") != std::string::npos);
        CHECK(output.find("Age must be between 0 and 150") != std::string::npos);
        CHECK(output.find(R"("id":10)") != std::string::npos);
    }

    SUBCASE("Generic Exception: Internal Error (-32603)") {
        write_rpc_packet(in, R"({"jsonrpc":"2.0","method":"crash_me","params":null,"id":20})");

        std::this_thread::sleep_for(10ms);
        conn.process_queue();

        std::string output = out.str();
        CHECK(output.find(R"("code":-32603)") != std::string::npos);
        CHECK(output.find("Database connection lost") != std::string::npos);
        CHECK(output.find(R"("id":20)") != std::string::npos);
    }

    conn.stop();
}

TEST_CASE("Conn: Bidirectional RPC - notify_emacs (non-blocking)") {
    std::stringstream in, out, err;
    Conn conn([]() {}, in, out, err);
    conn.register_async_method("test_notify", [](Context ctx, const json& p) {
        ctx.notify_emacs("logged", p);
        ctx.reply("done");
    });
    conn.start();

    write_rpc_packet(in, R"({"jsonrpc":"2.0","method":"test_notify","params":{"msg":"hi"},"id":5})");

    std::this_thread::sleep_for(10ms);
    conn.process_queue();
    std::this_thread::sleep_for(10ms);
    conn.process_queue();

    std::string output = out.str();
    CHECK(output.find(R"("method":"logged")") != std::string::npos);
    CHECK(output.find(R"("msg":"hi")") != std::string::npos);
    CHECK(out.str().find(R"("id":5)") != std::string::npos);
    CHECK(out.str().find(R"("result":"done")") != std::string::npos);
    conn.stop();
}
