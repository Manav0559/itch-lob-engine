#pragma once
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "pipeline/book_snapshot.hpp"

// A read-only TCP JSON-lines query surface over a pipeline::SnapshotStore
// (see include/pipeline/book_snapshot.hpp for the ingest-side/query-side
// concurrency boundary this reads through). One request per line in, one
// JSON response per line out — a client can be as simple as `nc localhost
// <port>` typing `{"cmd":"list"}` and pressing enter.
//
// Deliberately out of scope, the same way net/multicast_receiver.hpp
// documents MoldUdp64Receiver's simplifications versus the full spec: no
// auth (it's a read-only diagnostic surface over data that's already public
// once you can run the replay binary yourself, and it binds loopback-only by
// default — see the constructor), no TLS, no HTTP framing/keep-alive
// semantics, no general JSON parsing (query_wire::extract_* below is a
// narrow scanner over this one fixed, self-controlled request schema, not a
// spec-compliant parser — no escaping, no nesting).
namespace net {

// Hand-rolled request/response encode/decode for the fixed schema this
// server actually needs: a flat request object with a "cmd" string field and
// an optional "locate" unsigned-integer field, and response objects built
// from pipeline::LocateSnapshot. A real JSON library would be overkill for a
// two-command, flat-object protocol this codebase fully controls both ends
// of, and this project otherwise has zero third-party dependencies beyond
// zlib/Threads/Catch2 (see CMakeLists.txt) — not worth adding one just for
// this.
namespace query_wire {

// Finds `"key"` followed by `:` (whitespace-tolerant) followed by a quoted
// string value, and returns that value unescaped-as-is. std::nullopt if the
// key isn't present as a quoted string field anywhere in `s` — including if
// it's actually present as a differently-typed value, which this scanner
// makes no attempt to distinguish from "absent" (fine for this protocol: the
// one string field, "cmd", is never sent as anything but a string by a
// well-formed client, and a malformed one gets treated as "missing" either
// way).
inline std::optional<std::string> extract_string_field(const std::string& s, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    auto pos = s.find(needle);
    if (pos == std::string::npos) return std::nullopt;
    pos += needle.size();
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
    if (pos >= s.size() || s[pos] != ':') return std::nullopt;
    ++pos;
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
    if (pos >= s.size() || s[pos] != '"') return std::nullopt;
    ++pos;
    const auto end = s.find('"', pos);
    if (end == std::string::npos) return std::nullopt;
    return s.substr(pos, end - pos);
}

// Same shape as extract_string_field, for an unquoted non-negative integer
// value. Returns std::nullopt for a missing key, a non-numeric value, or a
// value too large for std::uint64_t (std::stoull's out_of_range is caught
// rather than left to tear down the connection thread over adversarial
// input).
inline std::optional<std::uint64_t> extract_uint_field(const std::string& s, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    auto pos = s.find(needle);
    if (pos == std::string::npos) return std::nullopt;
    pos += needle.size();
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
    if (pos >= s.size() || s[pos] != ':') return std::nullopt;
    ++pos;
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
    const auto start = pos;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
    if (pos == start) return std::nullopt;
    try {
        return std::stoull(s.substr(start, pos - start));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// The fields shared by a single quote object and a list entry — factored out
// so "quote" and "list" can't drift into reporting different fields for the
// same underlying pipeline::LocateSnapshot.
inline std::string quote_fields(const pipeline::LocateSnapshot& q) {
    std::string s;
    s += "\"locate\":" + std::to_string(q.locate) + ",";
    s += "\"best_bid\":" + (q.has_bid ? std::to_string(q.bid_price) : std::string("null")) + ",";
    s += "\"best_bid_shares\":" + (q.has_bid ? std::to_string(q.bid_shares) : std::string("null")) + ",";
    s += "\"best_ask\":" + (q.has_ask ? std::to_string(q.ask_price) : std::string("null")) + ",";
    s += "\"best_ask_shares\":" + (q.has_ask ? std::to_string(q.ask_shares) : std::string("null")) + ",";
    s += "\"open_orders\":" + std::to_string(q.open_orders) + ",";
    s += "\"bid_levels\":" + std::to_string(q.bid_levels) + ",";
    s += "\"ask_levels\":" + std::to_string(q.ask_levels);
    return s;
}

// snapshot_version lets a caller tell "no data published yet" (0) apart from
// "as of publish #N" and notice when two responses reflect the same,
// unchanged snapshot (see pipeline::SnapshotStore::version()). unknown_refs
// is the ingest side's session-wide (not per-locate) count of rejected book
// mutations as of that same snapshot — see pipeline::SnapshotStore::publish()
// — surfaced here so a caller can tell whether *any* book being served might
// be missing messages, not just discover it later from the process's own
// stdout report.
inline std::string quote_json(const pipeline::LocateSnapshot& q, std::uint64_t snapshot_version,
                              std::uint64_t unknown_refs) {
    return "{" + quote_fields(q) + ",\"snapshot_version\":" + std::to_string(snapshot_version) +
          ",\"unknown_refs\":" + std::to_string(unknown_refs) + "}";
}

inline std::string list_json(const std::vector<pipeline::LocateSnapshot>& all,
                              std::uint64_t snapshot_version, std::uint64_t unknown_refs) {
    std::string out = "{\"snapshot_version\":" + std::to_string(snapshot_version) +
                      ",\"unknown_refs\":" + std::to_string(unknown_refs) + ",\"books\":[";
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (i) out += ",";
        out += "{" + quote_fields(all[i]) + "}";
    }
    out += "]}";
    return out;
}

inline std::string error_json(const std::string& msg) { return "{\"error\":\"" + msg + "\"}"; }

inline std::string error_json_locate(const std::string& msg, std::uint16_t locate) {
    return "{\"error\":\"" + msg + "\",\"locate\":" + std::to_string(locate) + "}";
}

}  // namespace query_wire

// RAII TCP listener + accept loop, one thread per connection. Construction
// throws std::system_error the same way net::MulticastReceiver's does
// (socket/bind/listen are a system-call boundary that can fail for reasons
// outside the program's control) — see that class's header comment for the
// same rationale.
class QueryServer {
public:
    // Binds loopback-only (127.0.0.1), not the wildcard address
    // MulticastReceiver binds to for its multicast listener: this is a
    // read-only *diagnostic* surface with no authentication, so the
    // deliberate default is "reachable from this machine only" — a caller
    // that actually wants it reachable from elsewhere can be extended to
    // take a bind address, but that's not this project's demo need. `port`
    // 0 asks the OS for an ephemeral free port (see bound_port() to recover
    // it) — what the test suite uses to avoid hardcoded-port collisions.
    explicit QueryServer(std::uint16_t port, const pipeline::SnapshotStore& store) : store_(store) {
        // Writing a response after the peer has already closed its end
        // raises SIGPIPE, whose default disposition is to terminate the
        // whole process — not a single failed send() call. Every other
        // socket-facing class in this codebase (MulticastReceiver,
        // MoldUdp64Receiver) is UDP, where this doesn't arise; this is the
        // first TCP connection-oriented socket, so it's the first place
        // that default needs overriding.
        std::signal(SIGPIPE, SIG_IGN);

        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) throw std::system_error(errno, std::generic_category(), "socket failed");

        const int reuse = 1;
        if (::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
            const int err = errno;
            ::close(listen_fd_);
            throw std::system_error(err, std::generic_category(), "setsockopt(SO_REUSEADDR) failed");
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
            ::close(listen_fd_);
            throw std::system_error(EINVAL, std::generic_category(), "invalid bind address");
        }
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            const int err = errno;
            ::close(listen_fd_);
            throw std::system_error(err, std::generic_category(),
                                    "bind failed: port " + std::to_string(port));
        }

        constexpr int kBacklog = 16;
        if (::listen(listen_fd_, kBacklog) != 0) {
            const int err = errno;
            ::close(listen_fd_);
            throw std::system_error(err, std::generic_category(), "listen failed");
        }

        sockaddr_in actual{};
        socklen_t actual_len = sizeof(actual);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&actual), &actual_len) != 0) {
            const int err = errno;
            ::close(listen_fd_);
            throw std::system_error(err, std::generic_category(), "getsockname failed");
        }
        bound_port_ = ntohs(actual.sin_port);
    }

    ~QueryServer() { stop(); }

    QueryServer(const QueryServer&) = delete;
    QueryServer& operator=(const QueryServer&) = delete;

    // Spawns the accept-loop thread; returns immediately. Call once.
    void start() {
        accept_thread_ = std::thread([this] { accept_loop(); });
    }

    // Idempotent, safe to call from the destructor and explicitly: signals
    // the accept loop to stop, closes the listening socket (unblocking a
    // poll() that's currently waiting on it), then joins the accept thread
    // and every still-open per-connection thread. Once stop() returns, no
    // thread owned by this server touches `store_` again — safe to destroy
    // the SnapshotStore it was constructed with right after.
    void stop() {
        if (stop_requested_.exchange(true)) return;
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (accept_thread_.joinable()) accept_thread_.join();

        std::vector<Connection> threads;
        {
            std::lock_guard<std::mutex> lock(conn_threads_mu_);
            threads.swap(conn_threads_);
        }
        for (auto& c : threads)
            if (c.thread.joinable()) c.thread.join();
    }

    // The actual bound port — only useful to call this instead of just
    // remembering what was passed to the constructor when that was 0.
    std::uint16_t bound_port() const { return bound_port_; }

private:
    // 64 KiB without a newline is already far past any legitimate request
    // this protocol has (a quote response for a single locate is well under
    // 200 bytes) — a bound here turns "client sends garbage forever" into a
    // clean error + disconnect instead of an unbounded per-connection buffer.
    static constexpr std::size_t kMaxLineLen = 65536;

    // Hard cap on concurrent connections: without this, a client opening
    // many parallel connections and never sending data or disconnecting
    // pins one OS thread per connection indefinitely (handle_connection's
    // idle timeout below bounds how long any *single* connection can do
    // that, but not how many can do it at once) — a straightforward
    // thread-exhaustion DoS against a diagnostic surface with no auth. Once
    // at capacity, a new connection is accepted and immediately closed
    // rather than queued or refused at the listen backlog, so the client
    // sees a clean close instead of a hang.
    static constexpr std::size_t kMaxConnections = 256;

    // A connection that never sends a complete line and never closes used
    // to keep its thread alive forever — the 300ms SO_RCVTIMEO below exists
    // only to let it notice stop_requested_, not to bound idle time. This is
    // the per-connection half of the kMaxConnections defense above: even
    // under the cap, an idle connection now gets closed instead of holding
    // its slot (and its thread) indefinitely.
    static constexpr std::chrono::seconds kIdleTimeout{60};

    // One per-connection thread plus a completion flag the thread itself
    // sets right before returning — lets reap_finished_connections() find
    // and join finished threads without blocking on a thread that's still
    // running (std::thread has no non-blocking "is it done yet" query of
    // its own). A shared_ptr so the flag safely outlives the thread's own
    // stack frame regardless of join order.
    struct Connection {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    void accept_loop() {
        while (!stop_requested_.load(std::memory_order_acquire)) {
            reap_finished_connections();

            pollfd pfd{listen_fd_, POLLIN, 0};
            const int rv = ::poll(&pfd, 1, 200);  // 200ms: bounds how long stop() can be kept waiting
            if (rv <= 0 || !(pfd.revents & POLLIN)) continue;  // timeout/EINTR/spurious wake — recheck the flag

            const int client_fd = ::accept(listen_fd_, nullptr, nullptr);
            if (client_fd < 0) continue;  // benign race with stop()'s close(), or a transient accept error

            std::lock_guard<std::mutex> lock(conn_threads_mu_);
            if (conn_threads_.size() >= kMaxConnections) {
                ::close(client_fd);  // at capacity — see kMaxConnections
                continue;
            }
            auto done = std::make_shared<std::atomic<bool>>(false);
            std::thread t([this, client_fd, done] {
                handle_connection(client_fd);
                done->store(true, std::memory_order_release);
            });
            conn_threads_.push_back(Connection{std::move(t), std::move(done)});
        }
    }

    // Joins and drops every connection thread whose completion flag is set,
    // called once per accept_loop iteration (so at worst every ~200ms) —
    // without this, conn_threads_ only ever grew, and a long-running server
    // handling many short-lived connections would accumulate one already-
    // finished-but-unjoined std::thread per connection for its entire
    // lifetime instead of reclaiming them as they finish.
    void reap_finished_connections() {
        std::lock_guard<std::mutex> lock(conn_threads_mu_);
        for (auto it = conn_threads_.begin(); it != conn_threads_.end();) {
            if (it->done->load(std::memory_order_acquire)) {
                if (it->thread.joinable()) it->thread.join();
                it = conn_threads_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Same SO_RCVTIMEO idiom net::MulticastReceiver/moldudp64::set_recv_timeout
    // use, and for the identical reason: a per-connection thread blocked in
    // recv() must still notice stop_requested_ within a bounded time, not
    // rely on the client ever sending more data or closing its end.
    static void set_recv_timeout(int fd, std::chrono::milliseconds timeout) {
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(timeout);
        timeval tv{};
        tv.tv_sec = static_cast<time_t>(us.count() / 1'000'000);
        tv.tv_usec = static_cast<suseconds_t>(us.count() % 1'000'000);
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    // Loops until every byte is written (or a real error/peer-close):
    // send() on a blocking socket is explicitly permitted to write fewer
    // bytes than requested under send-buffer pressure, and a "list" response
    // over a few thousand symbols is easily large enough (~100 bytes/symbol)
    // to hit that in practice — a short write here used to get silently
    // treated as "fully sent," truncating that response and desyncing the
    // client's line-based framing for the rest of the connection. Returns
    // false only when the connection should be torn down (a real error, or
    // the peer closing its read side); the EAGAIN/EWOULDBLOCK/EINTR cases
    // that recv()'s loop already treats as "retry" get the same treatment
    // here for the identical reason.
    static bool send_all(int fd, const char* data, std::size_t len) {
        std::size_t sent = 0;
        while (sent < len) {
            const ssize_t n = ::send(fd, data + sent, len - sent, 0);
            if (n > 0) {
                sent += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
            return false;
        }
        return true;
    }

    void handle_connection(int client_fd) {
        set_recv_timeout(client_fd, std::chrono::milliseconds(300));
        std::string buf;
        char chunk[4096];
        auto last_activity = std::chrono::steady_clock::now();

        while (!stop_requested_.load(std::memory_order_acquire)) {
            const ssize_t n = ::recv(client_fd, chunk, sizeof(chunk), 0);
            if (n > 0) {
                last_activity = std::chrono::steady_clock::now();
                buf.append(chunk, static_cast<std::size_t>(n));

                std::size_t nl;
                while ((nl = buf.find('\n')) != std::string::npos) {
                    std::string line = buf.substr(0, nl);
                    buf.erase(0, nl + 1);
                    if (!line.empty() && line.back() == '\r') line.pop_back();  // tolerate CRLF clients
                    if (line.empty()) continue;  // blank line — no response, matches most line protocols

                    const std::string response = handle_line(line) + "\n";
                    if (!send_all(client_fd, response.data(), response.size())) {
                        ::close(client_fd);
                        return;
                    }
                }

                if (buf.size() > kMaxLineLen) {
                    const std::string response = query_wire::error_json("request line too long") + "\n";
                    send_all(client_fd, response.data(), response.size());
                    ::close(client_fd);
                    return;
                }
                continue;
            }
            if (n == 0) break;  // peer closed its end
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                // 300ms recv timeout firing repeatedly with no data is the
                // normal idle case, not an error — only actually give up
                // once kIdleTimeout has passed since the last byte arrived.
                if (std::chrono::steady_clock::now() - last_activity > kIdleTimeout) break;
                continue;
            }
            break;  // a real error: give up on this connection
        }
        ::close(client_fd);
    }

    // The two supported commands: "list" (every locate currently known) and
    // "quote" (one locate, which must be present in the request as an
    // unsigned integer field). Both answer straight out of the SnapshotStore
    // — never the live book — so a query never blocks on, or races with,
    // whatever is mutating the book right now.
    std::string handle_line(const std::string& line) {
        const auto cmd = query_wire::extract_string_field(line, "cmd");
        if (!cmd) return query_wire::error_json("bad request");

        if (*cmd == "list")
            return query_wire::list_json(store_.read_all(), store_.version(), store_.unknown_refs());

        if (*cmd == "quote") {
            const auto locate_field = query_wire::extract_uint_field(line, "locate");
            if (!locate_field) return query_wire::error_json("missing locate");
            const auto locate = static_cast<std::uint16_t>(*locate_field);
            const auto snap = store_.find(locate);
            if (!snap) return query_wire::error_json_locate("unknown locate", locate);
            return query_wire::quote_json(*snap, store_.version(), store_.unknown_refs());
        }

        return query_wire::error_json("unknown command");
    }

    const pipeline::SnapshotStore& store_;
    // atomic, not plain int: accept_loop() (its own thread) reads this every
    // poll()/accept() call while stop() (whatever thread calls it, including
    // the destructor's thread) closes the socket and resets it to -1 with no
    // other synchronization between the two — a real, pre-existing data race
    // TSan flags on the plain-int version of this field, found while
    // touching this file for an unrelated fix. Each individual access below
    // was already effectively fine on every real platform (an int-sized
    // write is atomic in practice), but "in practice" isn't the same as
    // well-defined, and TSan is right to flag it regardless of whether it
    // was ever observed to misbehave.
    std::atomic<int> listen_fd_{-1};
    std::uint16_t bound_port_ = 0;
    std::atomic<bool> stop_requested_{false};
    std::thread accept_thread_;
    std::mutex conn_threads_mu_;
    std::vector<Connection> conn_threads_;
};

}  // namespace net
