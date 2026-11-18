#include "admin/resp_probe.hpp"

#include "admin/postgres_probe.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <vector>

namespace dynaxis::admin {
namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

struct ParsedRedisEndpoint {
    std::string host;
    unsigned short port{6379};
    std::string password;
};

ParsedRedisEndpoint parse_redis_endpoint(const std::string& uri,
                                         const std::string& fallback_host,
                                         unsigned short fallback_port) {
    ParsedRedisEndpoint out;
    out.host = fallback_host.empty() ? std::string("127.0.0.1") : fallback_host;
    out.port = (fallback_port > 0) ? fallback_port : 6379;

    if (uri.empty()) {
        return out;
    }

    std::string_view rest(uri);
    const std::size_t scheme = rest.find("://");
    if (scheme != std::string_view::npos) {
        rest.remove_prefix(scheme + 3);
    }
    const std::size_t slash = rest.find('/');
    if (slash != std::string_view::npos) {
        rest = rest.substr(0, slash);
    }

    const std::size_t at = rest.rfind('@');
    if (at != std::string_view::npos) {
        const std::string_view auth = rest.substr(0, at);
        rest.remove_prefix(at + 1);
        const std::size_t colon = auth.find(':');
        if (colon != std::string_view::npos) {
            out.password = std::string(auth.substr(colon + 1));
        } else {
            out.password = std::string(auth);
        }
    }

    if (!rest.empty()) {
        if (rest.front() == '[') {
            const std::size_t close_b = rest.find(']');
            if (close_b != std::string_view::npos) {
                out.host = std::string(rest.substr(1, close_b - 1));
                if (close_b + 2 <= rest.size() && rest[close_b + 1] == ':') {
                    const long p = std::strtol(std::string(rest.substr(close_b + 2)).c_str(), nullptr, 10);
                    if (p > 0 && p <= 65535) {
                        out.port = static_cast<unsigned short>(p);
                    }
                }
            }
        } else {
            const std::size_t colon = rest.rfind(':');
            if (colon != std::string_view::npos) {
                out.host = std::string(rest.substr(0, colon));
                const long p = std::strtol(std::string(rest.substr(colon + 1)).c_str(), nullptr, 10);
                if (p > 0 && p <= 65535) {
                    out.port = static_cast<unsigned short>(p);
                }
            } else {
                out.host = std::string(rest);
            }
        }
    }

    if (out.host.empty()) {
        out.host = fallback_host.empty() ? std::string("127.0.0.1") : fallback_host;
    }
    return out;
}

void append_resp_command(std::string& out, const std::vector<std::string_view>& args) {
    out += "*" + std::to_string(args.size()) + "\r\n";
    for (std::string_view arg : args) {
        out += "$" + std::to_string(arg.size()) + "\r\n";
        out.append(arg.data(), arg.size());
        out += "\r\n";
    }
}

std::size_t count_resp_lines(std::string_view buf) {
    std::size_t lines = 0;
    std::size_t pos = 0;
    while (pos < buf.size()) {
        const std::size_t end = buf.find("\r\n", pos);
        if (end == std::string_view::npos) {
            break;
        }
        ++lines;
        pos = end + 2;
    }
    return lines;
}

} // namespace

RedisProbeResult probe_redis_resp(const std::string& redis_uri,
                                  const std::string& fallback_host,
                                  unsigned short fallback_port,
                                  const std::string& dlq_stream_key,
                                  std::uint32_t deadline_ms) {
    RedisProbeResult result;
    const ParsedRedisEndpoint endpoint = parse_redis_endpoint(redis_uri, fallback_host, fallback_port);
    const std::string safe_dlq_key = dlq_stream_key.empty() ? std::string("session_events_dlq") : dlq_stream_key;
    const auto started_at = std::chrono::steady_clock::now();

    try {
        asio::io_context ioc;
        tcp::resolver resolver(ioc);
        tcp::socket socket(ioc);
        asio::steady_timer timer(ioc);

        std::string payload;
        std::size_t expected_replies = 2;
        if (!endpoint.password.empty()) {
            append_resp_command(payload, {"AUTH", endpoint.password});
            expected_replies = 3;
        }
        append_resp_command(payload, {"PING"});
        append_resp_command(payload, {"XLEN", safe_dlq_key});

        bool finished = false;
        bool timed_out = false;
        std::string raw_reply;
        std::array<char, 1024> scratch{};

        auto finish = [&]() {
            if (finished) {
                return;
            }
            finished = true;
            boost::system::error_code ignore_ec;
            timer.cancel();
            resolver.cancel();
            socket.shutdown(tcp::socket::shutdown_both, ignore_ec);
            socket.close(ignore_ec);
        };

        std::function<void()> start_read;
        start_read = [&]() {
            if (finished) {
                return;
            }
            socket.async_read_some(
                asio::buffer(scratch),
                [&](const boost::system::error_code& ec, std::size_t n) {
                    if (finished) {
                        return;
                    }
                    if (n > 0) {
                        if (raw_reply.size() + n > 8192) {
                            result.error = "RESP reply exceeds size cap";
                            finish();
                            return;
                        }
                        raw_reply.append(scratch.data(), n);
                        if (count_resp_lines(raw_reply) >= expected_replies) {
                            finish();
                            return;
                        }
                    }
                    if (ec) {
                        if (ec != asio::error::eof && result.error.empty()) {
                            result.error = sanitize_connection_error("redis read failed: " + ec.message());
                        }
                        finish();
                        return;
                    }
                    start_read();
                });
        };

        timer.expires_after(std::chrono::milliseconds(std::max<std::uint32_t>(deadline_ms, 50)));
        timer.async_wait([&](const boost::system::error_code& ec) {
            if (!ec && !finished) {
                timed_out = true;
                result.error = "redis probe timeout";
                finish();
            }
        });

        resolver.async_resolve(
            endpoint.host,
            std::to_string(endpoint.port),
            [&](const boost::system::error_code& ec, const tcp::resolver::results_type& endpoints) {
                if (finished) {
                    return;
                }
                if (ec) {
                    result.error = sanitize_connection_error("redis resolve failed: " + ec.message());
                    finish();
                    return;
                }
                asio::async_connect(
                    socket,
                    endpoints,
                    [&](const boost::system::error_code& connect_ec, const tcp::endpoint&) {
                        if (finished) {
                            return;
                        }
                        if (connect_ec) {
                            result.error = sanitize_connection_error("redis connect failed: " + connect_ec.message());
                            finish();
                            return;
                        }
                        asio::async_write(
                            socket,
                            asio::buffer(payload),
                            [&](const boost::system::error_code& write_ec, std::size_t) {
                                if (finished) {
                                    return;
                                }
                                if (write_ec) {
                                    result.error = sanitize_connection_error("redis write failed: " + write_ec.message());
                                    finish();
                                    return;
                                }
                                start_read();
                            });
                    });
            });

        ioc.run();

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at);
        result.latency_ms = static_cast<std::uint32_t>(std::max<long long>(0, elapsed.count()));

        if (!result.error.empty()) {
            return result;
        }
        if (timed_out) {
            result.error = "redis probe timeout";
            return result;
        }

        std::vector<std::string_view> lines;
        std::size_t pos = 0;
        while (pos < raw_reply.size()) {
            const std::size_t end = raw_reply.find("\r\n", pos);
            if (end == std::string::npos) {
                break;
            }
            lines.emplace_back(raw_reply.data() + pos, end - pos);
            pos = end + 2;
        }
        if (lines.size() < expected_replies) {
            result.error = "incomplete RESP reply";
            return result;
        }

        const std::size_t ping_idx = endpoint.password.empty() ? 0u : 1u;
        if (!endpoint.password.empty() && lines[0].rfind("+OK", 0) != 0) {
            result.error = "redis AUTH failed";
            return result;
        }
        if (lines[ping_idx].rfind("+PONG", 0) != 0) {
            result.error = "unexpected redis PING reply";
            return result;
        }

        const std::string_view xlen_line = lines[ping_idx + 1];
        if (!xlen_line.empty() && xlen_line.front() == ':') {
            char* end_ptr = nullptr;
            const long long len = std::strtoll(std::string(xlen_line.substr(1)).c_str(), &end_ptr, 10);
            if (end_ptr != nullptr && *end_ptr == '\0' && len >= 0) {
                result.dlq_length = len;
            }
        }
        result.healthy = true;
        return result;
    } catch (const std::exception& ex) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at);
        result.latency_ms = static_cast<std::uint32_t>(std::max<long long>(0, elapsed.count()));
        result.error = sanitize_connection_error(ex.what());
        return result;
    }
}

namespace {

enum class RespKind {
    SimpleString,
    Error,
    Integer,
    BulkString,
    Array,
    Null
};

struct RespValue {
    RespKind kind{RespKind::Null};
    std::string str;
    long long integer{0};
    std::vector<RespValue> elements;
};

enum class RespParseStatus {
    Ok,
    Incomplete,
    Malformed
};

RespParseStatus parse_resp_value(std::string_view buf,
                                 std::size_t& offset,
                                 RespValue& out,
                                 int depth = 0) {
    if (depth > 16) {
        return RespParseStatus::Malformed;
    }
    if (offset >= buf.size()) {
        return RespParseStatus::Incomplete;
    }

    const std::size_t line_end = buf.find("\r\n", offset);
    if (line_end == std::string_view::npos) {
        return RespParseStatus::Incomplete;
    }

    const char prefix = buf[offset];
    const std::string_view header(buf.data() + offset + 1, line_end - (offset + 1));

    if (prefix == '+' || prefix == '-') {
        out = RespValue{};
        out.kind = (prefix == '+') ? RespKind::SimpleString : RespKind::Error;
        out.str = std::string(header);
        offset = line_end + 2;
        return RespParseStatus::Ok;
    }

    if (prefix == ':') {
        char* end_ptr = nullptr;
        const std::string num_str(header);
        const long long val = std::strtoll(num_str.c_str(), &end_ptr, 10);
        if (num_str.empty() || end_ptr == nullptr || *end_ptr != '\0') {
            return RespParseStatus::Malformed;
        }
        out = RespValue{};
        out.kind = RespKind::Integer;
        out.integer = val;
        offset = line_end + 2;
        return RespParseStatus::Ok;
    }

    if (prefix == '$') {
        char* end_ptr = nullptr;
        const std::string len_str(header);
        const long long len = std::strtoll(len_str.c_str(), &end_ptr, 10);
        if (len_str.empty() || end_ptr == nullptr || *end_ptr != '\0') {
            return RespParseStatus::Malformed;
        }
        if (len < 0) {
            out = RespValue{};
            out.kind = RespKind::Null;
            offset = line_end + 2;
            return RespParseStatus::Ok;
        }
        const std::size_t ulen = static_cast<std::size_t>(len);
        if (ulen > 512 * 1024) {
            return RespParseStatus::Malformed;
        }
        const std::size_t body_start = line_end + 2;
        if (buf.size() < body_start + ulen + 2) {
            return RespParseStatus::Incomplete;
        }
        out = RespValue{};
        out.kind = RespKind::BulkString;
        out.str.assign(buf.data() + body_start, ulen);
        offset = body_start + ulen + 2;
        return RespParseStatus::Ok;
    }

    if (prefix == '*') {
        char* end_ptr = nullptr;
        const std::string cnt_str(header);
        const long long cnt = std::strtoll(cnt_str.c_str(), &end_ptr, 10);
        if (cnt_str.empty() || end_ptr == nullptr || *end_ptr != '\0') {
            return RespParseStatus::Malformed;
        }
        if (cnt < 0) {
            out = RespValue{};
            out.kind = RespKind::Null;
            offset = line_end + 2;
            return RespParseStatus::Ok;
        }
        if (cnt > 10000) {
            return RespParseStatus::Malformed;
        }
        std::size_t cursor = line_end + 2;
        RespValue arr;
        arr.kind = RespKind::Array;
        arr.elements.reserve(static_cast<std::size_t>(cnt));
        for (long long i = 0; i < cnt; ++i) {
            RespValue item;
            const auto child_status = parse_resp_value(buf, cursor, item, depth + 1);
            if (child_status != RespParseStatus::Ok) {
                return child_status;
            }
            arr.elements.push_back(std::move(item));
        }
        out = std::move(arr);
        offset = cursor;
        return RespParseStatus::Ok;
    }

    return RespParseStatus::Malformed;
}

RespParseStatus parse_resp_pipeline(std::string_view buf,
                                    std::size_t expected_count,
                                    std::vector<RespValue>& out) {
    out.clear();
    out.reserve(expected_count);
    std::size_t offset = 0;
    for (std::size_t i = 0; i < expected_count; ++i) {
        RespValue val;
        const auto status = parse_resp_value(buf, offset, val, 0);
        if (status != RespParseStatus::Ok) {
            return status;
        }
        out.push_back(std::move(val));
    }
    return RespParseStatus::Ok;
}

bool execute_resp_pipeline(const ParsedRedisEndpoint& endpoint,
                           const std::vector<std::vector<std::string_view>>& commands,
                           std::uint32_t deadline_ms,
                           std::vector<RespValue>& replies,
                           std::string& out_error) {
    out_error.clear();
    replies.clear();

    std::string payload;
    std::size_t expected_replies = commands.size();
    if (!endpoint.password.empty()) {
        append_resp_command(payload, {"AUTH", endpoint.password});
        expected_replies += 1;
    }
    for (const auto& cmd : commands) {
        append_resp_command(payload, cmd);
    }

    try {
        asio::io_context ioc;
        tcp::resolver resolver(ioc);
        tcp::socket socket(ioc);
        asio::steady_timer timer(ioc);

        bool finished = false;
        bool timed_out = false;
        bool malformed = false;
        std::string raw_reply;
        std::array<char, 2048> scratch{};

        auto finish = [&]() {
            if (finished) {
                return;
            }
            finished = true;
            boost::system::error_code ignore_ec;
            timer.cancel();
            resolver.cancel();
            socket.shutdown(tcp::socket::shutdown_both, ignore_ec);
            socket.close(ignore_ec);
        };

        std::function<void()> start_read;
        start_read = [&]() {
            if (finished) {
                return;
            }
            socket.async_read_some(
                asio::buffer(scratch),
                [&](const boost::system::error_code& ec, std::size_t n) {
                    if (finished) {
                        return;
                    }
                    if (n > 0) {
                        if (raw_reply.size() + n > 512 * 1024) {
                            out_error = "RESP reply exceeds size cap";
                            finish();
                            return;
                        }
                        raw_reply.append(scratch.data(), n);
                        const auto status = parse_resp_pipeline(raw_reply, expected_replies, replies);
                        if (status == RespParseStatus::Ok) {
                            finish();
                            return;
                        }
                        if (status == RespParseStatus::Malformed) {
                            malformed = true;
                            out_error = "malformed RESP reply";
                            finish();
                            return;
                        }
                    }
                    if (ec) {
                        if (ec != asio::error::eof && out_error.empty()) {
                            out_error = sanitize_connection_error("redis read failed: " + ec.message());
                        }
                        finish();
                        return;
                    }
                    start_read();
                });
        };

        timer.expires_after(std::chrono::milliseconds(std::max<std::uint32_t>(deadline_ms, 50)));
        timer.async_wait([&](const boost::system::error_code& ec) {
            if (!ec && !finished) {
                timed_out = true;
                out_error = "redis probe timeout";
                finish();
            }
        });

        resolver.async_resolve(
            endpoint.host,
            std::to_string(endpoint.port),
            [&](const boost::system::error_code& ec, const tcp::resolver::results_type& endpoints) {
                if (finished) {
                    return;
                }
                if (ec) {
                    out_error = sanitize_connection_error("redis resolve failed: " + ec.message());
                    finish();
                    return;
                }
                asio::async_connect(
                    socket,
                    endpoints,
                    [&](const boost::system::error_code& connect_ec, const tcp::endpoint&) {
                        if (finished) {
                            return;
                        }
                        if (connect_ec) {
                            out_error = sanitize_connection_error("redis connect failed: " + connect_ec.message());
                            finish();
                            return;
                        }
                        asio::async_write(
                            socket,
                            asio::buffer(payload),
                            [&](const boost::system::error_code& write_ec, std::size_t) {
                                if (finished) {
                                    return;
                                }
                                if (write_ec) {
                                    out_error = sanitize_connection_error("redis write failed: " + write_ec.message());
                                    finish();
                                    return;
                                }
                                start_read();
                            });
                    });
            });

        ioc.run();

        if (!out_error.empty()) {
            return false;
        }
        if (timed_out) {
            out_error = "redis probe timeout";
            return false;
        }
        if (malformed || replies.size() < expected_replies) {
            out_error = "incomplete RESP reply";
            return false;
        }

        if (!endpoint.password.empty()) {
            if (replies[0].kind == RespKind::Error || replies[0].str != "OK") {
                out_error = "redis AUTH failed";
                return false;
            }
            replies.erase(replies.begin());
        }
        return true;
    } catch (const std::exception& ex) {
        out_error = sanitize_connection_error(ex.what());
        return false;
    }
}

} // namespace

long long query_redis_pttl_ms(const std::string& redis_uri,
                              const std::string& fallback_host,
                              unsigned short fallback_port,
                              const std::string& key,
                              std::uint32_t deadline_ms) {
    if (key.empty()) {
        return -2;
    }
    const ParsedRedisEndpoint endpoint = parse_redis_endpoint(redis_uri, fallback_host, fallback_port);
    std::vector<RespValue> replies;
    std::string error;
    if (!execute_resp_pipeline(endpoint, {{"PTTL", key}}, deadline_ms, replies, error) || replies.empty()) {
        return -3;
    }
    if (replies[0].kind != RespKind::Integer) {
        return -3;
    }
    return replies[0].integer;
}

WbStreamsSnapshot inspect_wb_streams_resp(const std::string& redis_uri,
                                          const std::string& fallback_host,
                                          unsigned short fallback_port,
                                          const std::string& wb_stream,
                                          const std::string& dlq_stream,
                                          const std::string& dead_stream,
                                          std::size_t preview_limit,
                                          std::uint32_t deadline_ms) {
    WbStreamsSnapshot snapshot;
    const ParsedRedisEndpoint endpoint = parse_redis_endpoint(redis_uri, fallback_host, fallback_port);
    const std::string wb_key = wb_stream.empty() ? std::string("session_events") : wb_stream;
    const std::string dlq_key = dlq_stream.empty() ? std::string("session_events_dlq") : dlq_stream;
    const std::string dead_key = dead_stream.empty() ? std::string("session_events_dead") : dead_stream;
    const std::size_t clamped_limit = std::clamp<std::size_t>(preview_limit, 1, 100);
    const std::string count_str = std::to_string(clamped_limit);

    std::vector<RespValue> replies;
    if (!execute_resp_pipeline(
            endpoint,
            {
                {"XLEN", wb_key},
                {"XLEN", dlq_key},
                {"XLEN", dead_key},
                {"XRANGE", dlq_key, "-", "+", "COUNT", count_str},
            },
            deadline_ms,
            replies,
            snapshot.error)) {
        return snapshot;
    }

    if (replies.size() < 4) {
        snapshot.error = "incomplete stream inspection reply";
        return snapshot;
    }

    if (replies[0].kind == RespKind::Integer) {
        snapshot.pending_stream_length = replies[0].integer;
    }
    if (replies[1].kind == RespKind::Integer) {
        snapshot.dlq_length = replies[1].integer;
    }
    if (replies[2].kind == RespKind::Integer) {
        snapshot.dead_length = replies[2].integer;
    }

    if (replies[3].kind == RespKind::Error) {
        snapshot.error = sanitize_connection_error(replies[3].str);
        return snapshot;
    }

    if (replies[3].kind == RespKind::Array) {
        for (const auto& entry_val : replies[3].elements) {
            if (entry_val.kind != RespKind::Array || entry_val.elements.size() < 2) {
                continue;
            }
            WbDlqPreviewEntry entry;
            entry.stream_id = entry_val.elements[0].str;
            entry.orig_event_id = entry.stream_id;

            const auto& fields_val = entry_val.elements[1];
            if (fields_val.kind == RespKind::Array) {
                for (std::size_t i = 0; i + 1 < fields_val.elements.size(); i += 2) {
                    const std::string& k = fields_val.elements[i].str;
                    const std::string& v = fields_val.elements[i + 1].str;
                    if (k == "orig_event_id" && !v.empty()) {
                        entry.orig_event_id = v;
                    } else if (k == "type") {
                        entry.type = v;
                    } else if (k == "ts_ms") {
                        entry.ts_ms = v;
                    } else if (k == "user_id") {
                        entry.user_id = v;
                    } else if (k == "session_id") {
                        entry.session_id = v;
                    } else if (k == "room_id") {
                        entry.room_id = v;
                    } else if (k == "retry_count") {
                        try {
                            entry.retry_count = std::stoi(v);
                        } catch (...) {
                            entry.retry_count = 0;
                        }
                    } else if (k == "error") {
                        entry.error = sanitize_connection_error(v);
                    }
                }
            }
            snapshot.preview_entries.push_back(std::move(entry));
        }
    }

    snapshot.ok = true;
    return snapshot;
}

bool ensure_stream_group_from_zero_resp(const std::string& redis_uri,
                                        const std::string& fallback_host,
                                        unsigned short fallback_port,
                                        const std::string& stream_key,
                                        const std::string& group_name,
                                        std::uint32_t deadline_ms) {
    if (stream_key.empty() || group_name.empty()) {
        return false;
    }
    const ParsedRedisEndpoint endpoint = parse_redis_endpoint(redis_uri, fallback_host, fallback_port);
    std::vector<RespValue> replies;
    std::string error;
    if (!execute_resp_pipeline(
            endpoint,
            {{"XGROUP", "CREATE", stream_key, group_name, "0", "MKSTREAM"}},
            deadline_ms,
            replies,
            error)
        || replies.empty()) {
        return false;
    }
    if (replies[0].kind == RespKind::SimpleString && replies[0].str == "OK") {
        return true;
    }
    if (replies[0].kind == RespKind::Error && replies[0].str.find("BUSYGROUP") != std::string::npos) {
        return true;
    }
    return false;
}

bool ack_and_delete_stream_entry_resp(const std::string& redis_uri,
                                      const std::string& fallback_host,
                                      unsigned short fallback_port,
                                      const std::string& stream_key,
                                      const std::string& group_name,
                                      const std::string& entry_id,
                                      std::uint32_t deadline_ms) {
    if (stream_key.empty() || group_name.empty() || entry_id.empty()) {
        return false;
    }
    const ParsedRedisEndpoint endpoint = parse_redis_endpoint(redis_uri, fallback_host, fallback_port);
    std::vector<RespValue> replies;
    std::string error;
    if (!execute_resp_pipeline(
            endpoint,
            {
                {"XACK", stream_key, group_name, entry_id},
                {"XDEL", stream_key, entry_id},
            },
            deadline_ms,
            replies,
            error)
        || replies.size() < 2) {
        return false;
    }
    return replies[0].kind != RespKind::Error && replies[1].kind != RespKind::Error;
}

} // namespace dynaxis::admin
