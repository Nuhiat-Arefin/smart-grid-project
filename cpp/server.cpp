#include "smartgrid.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <netinet/in.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sg {
namespace {

constexpr std::size_t kMaxRequestBytes = 2U * 1024U * 1024U;
constexpr std::size_t kMaxBodyBytes = 1U * 1024U * 1024U;

volatile std::sig_atomic_t stop_requested = 0;

void request_stop(int) {
    stop_requested = 1;
}

struct BadRequest : std::runtime_error {
    explicit BadRequest(const std::string& message) : std::runtime_error(message) {}
};

struct Request {
    std::string method;
    std::string target;
    std::map<std::string, std::string> headers;
    std::string body;
};

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool send_all(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool read_request(int fd, Request& request) {
    std::string raw;
    raw.reserve(4096);
    std::size_t header_end = std::string::npos;
    std::size_t expected_total = 0;
    char buffer[8192];
    while (raw.size() < kMaxRequestBytes) {
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        raw.append(buffer, static_cast<std::size_t>(n));
        if (header_end == std::string::npos) {
            header_end = raw.find("\r\n\r\n");
            if (header_end == std::string::npos) continue;
            expected_total = header_end + 4;
            const auto first_line_end = raw.find("\r\n");
            if (first_line_end == std::string::npos) return false;
            std::istringstream line(raw.substr(0, first_line_end));
            std::string version;
            if (!(line >> request.method >> request.target >> version) || version != "HTTP/1.1") {
                return false;
            }
            std::size_t cursor = first_line_end + 2;
            while (cursor < header_end) {
                const auto end = raw.find("\r\n", cursor);
                if (end == std::string::npos || end > header_end) return false;
                const auto colon = raw.find(':', cursor);
                if (colon == std::string::npos || colon > end) return false;
                request.headers[lower(trim(raw.substr(cursor, colon - cursor)))] =
                    trim(raw.substr(colon + 1, end - colon - 1));
                cursor = end + 2;
            }
            std::size_t body_length = 0;
            if (const auto it = request.headers.find("content-length"); it != request.headers.end()) {
                try {
                    const unsigned long long parsed = std::stoull(it->second);
                    if (parsed > kMaxBodyBytes) return false;
                    body_length = static_cast<std::size_t>(parsed);
                } catch (const std::exception&) {
                    return false;
                }
            }
            expected_total += body_length;
            if (expected_total > kMaxRequestBytes) return false;
        }
        if (header_end != std::string::npos && raw.size() >= expected_total) {
            request.body = raw.substr(header_end + 4, expected_total - header_end - 4);
            return true;
        }
    }
    return false;
}

std::string percent_decode(const std::string& encoded) {
    std::string decoded;
    decoded.reserve(encoded.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '%') {
            if (i + 2 >= encoded.size()) throw BadRequest("invalid URL escape");
            const int hi = hex(encoded[i + 1]);
            const int lo = hex(encoded[i + 2]);
            if (hi < 0 || lo < 0) throw BadRequest("invalid URL escape");
            decoded.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
        } else if (encoded[i] == '+') {
            decoded.push_back(' ');
        } else {
            decoded.push_back(encoded[i]);
        }
    }
    return decoded;
}

std::pair<std::string, std::map<std::string, std::string>> split_target(const std::string& target) {
    const auto question = target.find('?');
    const std::string encoded_path = target.substr(0, question);
    const std::string encoded_query = question == std::string::npos ? "" : target.substr(question + 1);
    std::map<std::string, std::string> query;
    std::size_t start = 0;
    while (start <= encoded_query.size()) {
        const auto amp = encoded_query.find('&', start);
        const std::string pair = encoded_query.substr(start, amp == std::string::npos
                                                              ? std::string::npos : amp - start);
        if (!pair.empty()) {
            const auto equals = pair.find('=');
            const std::string key = percent_decode(pair.substr(0, equals));
            const std::string value = equals == std::string::npos ? "" : percent_decode(pair.substr(equals + 1));
            if (!key.empty()) query[key] = value;
        }
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
    return {percent_decode(encoded_path), std::move(query)};
}

int parse_hour(const std::string& text) {
    if (text.empty()) throw BadRequest("hour is required");
    std::size_t consumed = 0;
    int hour = 0;
    try {
        const long value = std::stol(text, &consumed);
        if (consumed != text.size() || value < 0 || value > 23) throw BadRequest("hour must be an integer from 0 to 23");
        hour = static_cast<int>(value);
    } catch (const BadRequest&) {
        throw;
    } catch (const std::exception&) {
        throw BadRequest("hour must be an integer from 0 to 23");
    }
    return hour;
}

bool parse_bool_text(const std::string& text, bool default_value) {
    if (text.empty()) return default_value;
    if (text == "1" || text == "true" || text == "yes") return true;
    if (text == "0" || text == "false" || text == "no") return false;
    throw BadRequest("boolean query value must be true or false");
}

std::vector<std::string> split_faults(const std::string& text) {
    std::vector<std::string> faults;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto comma = text.find(',', start);
        const std::string item = trim(text.substr(start, comma == std::string::npos
                                                         ? std::string::npos : comma - start));
        if (!item.empty()) faults.push_back(item);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return faults;
}

std::unordered_set<std::string> string_keys(const Json& value) {
    std::unordered_set<std::string> keys;
    for (const auto& item : value.items()) keys.insert(item.first);
    for (const Json& item : value.elements()) {
        if (item.contains("id")) keys.insert(item["id"].string());
    }
    return keys;
}

std::string required_string(const Json& value, const std::string& name) {
    if (!value.contains(name)) throw BadRequest(name + " is required");
    const std::string result = value[name].string();
    if (result.empty()) throw BadRequest(name + " must be a non-empty string");
    return result;
}

struct EventRequest {
    int hour = 19;
    std::vector<std::string> faults;
    std::string policy = "engine";
    std::string battery = "dp";
    bool audit = true;
};

void validate_event(EventRequest& event, const std::unordered_set<std::string>& line_ids,
                    const std::unordered_set<std::string>& battery_ids) {
    if (event.hour < 0 || event.hour > 23) throw BadRequest("hour must be an integer from 0 to 23");
    for (const std::string& fault : event.faults) {
        if (line_ids.find(fault) == line_ids.end()) throw BadRequest("unknown fault line: " + fault);
    }
    if (event.policy == "global") event.policy = "proposal";
    if (event.policy != "proposal" && event.policy != "mincut" &&
        event.policy != "engine" && event.policy != "global-exchange") {
        throw BadRequest("policy must be proposal, mincut, engine, or global-exchange");
    }
    if (battery_ids.find(event.battery) == battery_ids.end()) {
        throw BadRequest("unknown battery policy: " + event.battery);
    }
}

EventRequest from_query(const std::map<std::string, std::string>& query,
                        const std::unordered_set<std::string>& line_ids,
                        const std::unordered_set<std::string>& battery_ids) {
    EventRequest event;
    if (const auto it = query.find("hour"); it != query.end()) event.hour = parse_hour(it->second);
    if (const auto it = query.find("faults"); it != query.end()) event.faults = split_faults(it->second);
    const bool exchange = query.find("exchange") == query.end()
                              ? true : parse_bool_text(query.at("exchange"), true);
    if (const auto it = query.find("policy"); it != query.end()) event.policy = it->second;
    else if (const auto it = query.find("shedding"); it != query.end()) {
        if (it->second == "global") event.policy = exchange ? "global-exchange" : "proposal";
        else if (it->second == "mincut") event.policy = exchange ? "engine" : "mincut";
        else throw BadRequest("shedding must be mincut or global");
    }
    if (const auto it = query.find("battery_policy"); it != query.end()) event.battery = it->second;
    else if (const auto it = query.find("battery"); it != query.end()) event.battery = it->second;
    if (const auto it = query.find("audit"); it != query.end()) event.audit = parse_bool_text(it->second, true);
    validate_event(event, line_ids, battery_ids);
    return event;
}

EventRequest from_json(const Json& request,
                       const std::unordered_set<std::string>& line_ids,
                       const std::unordered_set<std::string>& battery_ids) {
    if (request.is_object()) {
        EventRequest event;
        if (request.contains("hour")) {
            const double raw = request["hour"].number(std::numeric_limits<double>::quiet_NaN());
            if (!std::isfinite(raw) || raw != std::floor(raw) || raw < 0.0 || raw > 23.0) {
                throw BadRequest("hour must be an integer from 0 to 23");
            }
            event.hour = static_cast<int>(raw);
        }
        if (request.contains("faults")) {
            const Json& faults = request["faults"];
            if (faults.is_array()) {
                for (const Json& item : faults.elements()) {
                    if (!item.is_string()) throw BadRequest("faults must contain strings");
                    const std::string fault = item.string();
                    event.faults.push_back(fault);
                }
            } else if (faults.is_string()) {
                const std::string text = faults.string();
                event.faults = split_faults(text);
            } else {
                throw BadRequest("faults must be an array or comma-separated string");
            }
        }
        if (request.contains("exchange") && !request["exchange"].is_boolean()) {
            throw BadRequest("exchange must be boolean");
        }
        if (request.contains("policy")) event.policy = required_string(request, "policy");
        else if (request.contains("shedding")) {
            const std::string shedding = required_string(request, "shedding");
            const bool exchange = request.contains("exchange") ? request["exchange"].boolean(true) : true;
            if (shedding == "global") event.policy = exchange ? "global-exchange" : "proposal";
            else if (shedding == "mincut") event.policy = exchange ? "engine" : "mincut";
            else throw BadRequest("shedding must be mincut or global");
        }
        if (request.contains("battery_policy")) event.battery = required_string(request, "battery_policy");
        else if (request.contains("battery")) event.battery = required_string(request, "battery");
        if (request.contains("audit")) {
            if (!request["audit"].is_boolean()) throw BadRequest("audit must be boolean");
            event.audit = request["audit"].boolean(true);
        }
        validate_event(event, line_ids, battery_ids);
        return event;
    }
    throw BadRequest("request body must be a JSON object");
}

Json error_json(const std::string& message) {
    Json error = Json::object();
    error["error"] = Json(message);
    return error;
}

void respond(int fd, int status, const std::string& status_text, const std::string& type,
             const std::string& body) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status << ' ' << status_text << "\r\n"
             << "Content-Type: " << type << "\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Cache-Control: no-store\r\n"
             << "Connection: close\r\n\r\n";
    response << body;
    send_all(fd, response.str());
}

void respond_json(int fd, int status, const Json& value) {
    const char* reason = status == 200 ? "OK" : status == 400 ? "Bad Request" :
                         status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed" : "Error";
    respond(fd, status, reason, "application/json; charset=utf-8", value.dump());
}

std::string mime_for(const std::string& filename) {
    if (filename == "index.html") return "text/html; charset=utf-8";
    if (filename == "app.js") return "text/javascript; charset=utf-8";
    return "text/css; charset=utf-8";
}

void serve_connection(int fd, const std::filesystem::path& web_root,
                      const Json& grid, const std::unordered_set<std::string>& line_ids,
                      const std::unordered_set<std::string>& battery_ids) {
    Request request;
    if (!read_request(fd, request)) {
        respond(fd, 400, "Bad Request", "application/json; charset=utf-8", error_json("malformed HTTP request").dump());
        return;
    }
    try {
        const auto [path, query] = split_target(request.target);
        if (request.method == "GET" && (path == "/api/grid" || path == "/api/boot")) {
            respond_json(fd, 200, grid);
            return;
        }
        if ((request.method == "POST" || request.method == "GET") && path == "/api/run") {
            EventRequest event;
            if (request.method == "POST") {
                if (request.body.empty()) throw BadRequest("request body is required");
                event = from_json(Json::parse(request.body), line_ids, battery_ids);
            } else {
                event = from_query(query, line_ids, battery_ids);
            }
            respond_json(fd, 200, run_event(event.hour, event.faults, event.policy, event.battery, event.audit));
            return;
        }
        if (request.method != "GET") {
            respond(fd, 405, "Method Not Allowed", "application/json; charset=utf-8", error_json("method not allowed").dump());
            return;
        }
        std::string filename;
        if (path == "/" || path == "/index.html") filename = "index.html";
        else if (path == "/app.js") filename = "app.js";
        else if (path == "/style.css") filename = "style.css";
        else {
            respond_json(fd, 404, error_json("not found"));
            return;
        }
        const std::filesystem::path file = web_root / filename;
        std::ifstream input(file, std::ios::binary);
        if (!input) {
            respond_json(fd, 404, error_json("static asset not found"));
            return;
        }
        std::ostringstream contents;
        contents << input.rdbuf();
        const std::string body = contents.str();
        if (body.size() > kMaxRequestBytes) {
            respond_json(fd, 404, error_json("static asset too large"));
            return;
        }
        respond(fd, 200, "OK", mime_for(filename), body);
    } catch (const BadRequest& exc) {
        respond_json(fd, 400, error_json(exc.what()));
    } catch (const std::exception& exc) {
        respond_json(fd, 400, error_json(exc.what()));
    }
}

} // namespace

int run_server(int port, const std::string& web_root) {
    if (port < 1 || port > 65535) throw std::invalid_argument("port must be from 1 to 65535");
    stop_requested = 0;

    const Json grid = boot();
    const Json lines = grid["lines"];
    const Json batteries = grid["battery_plans"];
    const auto line_ids = string_keys(lines);
    const auto battery_ids = string_keys(batteries);
    if (line_ids.empty()) throw std::runtime_error("boot payload has no line identifiers");
    if (battery_ids.empty()) throw std::runtime_error("boot payload has no battery policies");

    int listen_fd = -1;
    int bound_port = port;
    for (int candidate = port; candidate < port + 20 && candidate <= 65535; ++candidate) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) continue;
        int reuse = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(candidate));
        ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && ::listen(fd, 16) == 0) {
            listen_fd = fd;
            bound_port = candidate;
            break;
        }
        ::close(fd);
    }
    if (listen_fd < 0) throw std::runtime_error("no free port in requested range");

    struct sigaction action{};
    action.sa_handler = request_stop;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);

    // A browser closing its connection must not terminate the POSIX server.
    struct sigaction ignore_pipe{};
    ignore_pipe.sa_handler = SIG_IGN;
    sigemptyset(&ignore_pipe.sa_mask);
    sigaction(SIGPIPE, &ignore_pipe, nullptr);

    const std::filesystem::path root = web_root.empty() ? std::filesystem::path("web") : std::filesystem::path(web_root);
    std::cout << "Smart Grid dashboard: http://127.0.0.1:" << bound_port << "/   (Ctrl+C to stop)\n";
    std::cout.flush();
    while (!stop_requested) {
        sockaddr_in client{};
        socklen_t length = sizeof(client);
        const int client_fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&client), &length);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (stop_requested) break;
            continue;
        }
        timeval timeout{};
        timeout.tv_sec = 5;
        ::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        serve_connection(client_fd, root, grid, line_ids, battery_ids);
        ::close(client_fd);
    }
    ::close(listen_fd);
    return 0;
}

} // namespace sg
