// fx_client_optimized.cpp
// - UDP recv: poll() + non-blocking recv()
// - Lock-free ring buffer for RX queue (SPSC-safe; producer never touches tail)
// - Optional RT priority (SCHED_FIFO)
// - Robust parsing for OK<TAG>; forms (REQ, STATUS, etc.)

#ifdef DEBUG
#define FXCLI_LOG(x) do { std::cerr << x << std::endl; } while(0)
#else
#define FXCLI_LOG(x) do {} while(0)
#endif

#include "fx_client.h"
#include "utils/elapsed_timer.h"

#include <cstring>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <cctype>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <array>
#include <algorithm>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/types.h>
#include <poll.h>
#include <pthread.h>
#include <strings.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>

#ifdef DEBUG
static ElapsedTimer g_timer_ack("chk_ACK");
#endif

// ──────────────── 내부 유틸 ────────────────
namespace {

static inline void trim(std::string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) { s.clear(); return; }
    size_t e = s.find_last_not_of(" \t\r\n");
    s.assign(s, b, e - b + 1);
}

// PATCH: robust extract_tag_word()
static bool extract_tag_word(const std::string &resp, std::string &out_word) {
    size_t l = resp.find('<');
    if (l == std::string::npos) return false;

    // allow >, ;, or space as closing
    size_t r = resp.find_first_of(">; ", l);
    if (r == std::string::npos || r <= l + 1) return false;

    std::string inside = resp.substr(l + 1, r - l - 1);
    trim(inside);
    if (inside.empty()) return false;

    size_t cut = inside.find_first_of(" \t(");
    out_word = (cut == std::string::npos) ? inside : inside.substr(0, cut);
    trim(out_word);
    return !out_word.empty();
}

// PATCH: more tolerant tag compare
static inline bool tag_equals_ci(const std::string& tag, const char* expect_upper) {
    size_t elen = std::strlen(expect_upper);
    if (tag.size() < elen) return false;
    if (::strncasecmp(tag.c_str(), expect_upper, elen) != 0) return false;
    // allow ; or space after tag
    return (tag.size() == elen) || (tag[elen] == ';') || (tag[elen] == ' ');
}

// float -> 문자열
static inline const char* format_float(char* buf, size_t bufsz, float v) {
    int n = snprintf(buf, bufsz, "%.6f", v);
    if (n <= 0) { buf[0] = '0'; buf[1] = 0; return buf; }
    int end = n - 1;
    while (end > 0 && buf[end] == '0') --end;
    if (end > 0 && buf[end] == '.') {
        // trim trailing '.'
    } else {
        end += 1;
    }
    buf[end] = '\0';
    return buf;
}

static std::string build_id_group(const std::vector<uint8_t> &ids) {
    std::ostringstream oss;
    oss << '<';
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i) oss << ' ';
        oss << static_cast<unsigned>(ids[i]);
    }
    oss << '>';
    return oss.str();
}

static inline bool begins_with_ok(const std::string& s) {
    if (s.size() < 2) return false;
    return (s[0] == 'O' || s[0] == 'o') &&
           (s[1] == 'K' || s[1] == 'k');
}

} // namespace

// ──────────────── FxCli::UdpSocket ────────────────
class FxCli::UdpSocket {
public:
    struct RxPacket {
        std::string data;
        std::chrono::steady_clock::time_point t_arrival;
    };

    struct RingBuf {
        static constexpr size_t N = 256;
        RxPacket buf[N];
        alignas(64) std::atomic<size_t> head{0};
        alignas(64) std::atomic<size_t> tail{0};
        alignas(64) std::atomic<size_t> dropped_newest{0};

        bool push(RxPacket&& pkt) {
            size_t h = head.load(std::memory_order_relaxed);
            size_t n = (h + 1) % N;
            if (n == tail.load(std::memory_order_acquire)) {
                dropped_newest.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            buf[h] = std::move(pkt);
            head.store(n, std::memory_order_release);
            return true;
        }

        bool pop(RxPacket& out) {
            size_t t = tail.load(std::memory_order_acquire);
            if (t == head.load(std::memory_order_acquire)) return false;
            out = buf[t];
            tail.store((t + 1) % N, std::memory_order_release);
            return true;
        }

        void clear() {
            tail.store(head.load(std::memory_order_acquire), std::memory_order_release);
        }

        bool empty() const {
            return tail.load(std::memory_order_acquire) == head.load(std::memory_order_acquire);
        }

        template <typename Pred>
        bool find_latest(Pred pred, RxPacket& out) {
            const size_t h = head.load(std::memory_order_acquire);
            const size_t t = tail.load(std::memory_order_acquire);
            if (t == h) return false;
            size_t i = (h + N - 1) % N;
            const size_t span = (h + N - t) % N;
            for (size_t c = 0; c < span; ++c) {
                if (pred(buf[i])) {
                    out = buf[i];
                    tail.store((i + 1) % N, std::memory_order_release);
                    return true;
                }
                i = (i + N - 1) % N;
            }
            return false;
        }
    };

    UdpSocket(const std::string &ip, uint16_t port, int recv_buf_bytes = (64 * 1024)) {
        sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ < 0) throw std::runtime_error("socket() failed");

        if (recv_buf_bytes > 0) {
            int rcvbuf = std::max(recv_buf_bytes, 256 * 1024);
            ::setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        }

        int flags = ::fcntl(sock_, F_GETFL, 0);
        ::fcntl(sock_, F_SETFL, flags | O_NONBLOCK);

        std::memset(&addr_, 0, sizeof(addr_));
        addr_.sin_family = AF_INET;
        addr_.sin_port   = htons(port);
        if (::inet_pton(AF_INET, ip.c_str(), &addr_.sin_addr) != 1)
            throw std::runtime_error("inet_pton failed");
        if (::connect(sock_, reinterpret_cast<struct sockaddr *>(&addr_), sizeof(addr_)) < 0)
            throw std::runtime_error("connect() failed");

        run_rx_.store(true);
        rx_thread_ = std::thread([this]{ this->rx_loop_polling(); });
        set_thread_rt_priority(rx_thread_, 80);
    }

    ~UdpSocket() {
        run_rx_.store(false);
        if (sock_ >= 0) {
            ::shutdown(sock_, SHUT_RDWR);
            ::close(sock_);
            sock_ = -1;
        }
        if (rx_thread_.joinable()) rx_thread_.join();
    }

    void send(const char *data, size_t len) {
        ssize_t n = ::send(sock_, data, (int)len, 0);
        if (n < 0 || (size_t)n != len) throw std::runtime_error("send() failed");
    }

    void flush_queue() { q_.clear(); }

    bool wait_for_ok_tag(const char* expect_tag_upper, std::string& out_ok, int timeout_ms) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        auto pred = [&](const RxPacket& p){
            if (!begins_with_ok(p.data)) return false;
            std::string tag;
            if (!extract_tag_word(p.data, tag)) return false;
            return tag_equals_ci(tag, expect_tag_upper);
        };
        if (q_.find_latest(pred, tmp_)) { out_ok = std::move(tmp_.data); return true; }
        while (std::chrono::steady_clock::now() < deadline) {
            if (q_.find_latest(pred, tmp_)) { out_ok = std::move(tmp_.data); return true; }
            std::this_thread::yield();
        }
        return false;
    }

    bool wait_for_any(std::string& out, int timeout_ms) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        if (!q_.empty()) {
            RxPacket p; while (q_.pop(p)) {}
            out = std::move(p.data); return true;
        }
        while (std::chrono::steady_clock::now() < deadline) {
            if (!q_.empty()) { RxPacket p; while (q_.pop(p)) {} out = std::move(p.data); return true; }
            std::this_thread::yield();
        }
        return false;
    }

private:
    int sock_{-1};
    struct sockaddr_in addr_{};
    std::atomic<bool> run_rx_{false};
    std::thread rx_thread_;
    RingBuf q_;
    RxPacket tmp_;

    static void set_thread_rt_priority(std::thread& th, int prio) {
        struct sched_param sp; sp.sched_priority = prio;
        pthread_setschedparam(th.native_handle(), SCHED_FIFO, &sp);
        mlockall(MCL_CURRENT | MCL_FUTURE);
        cpu_set_t set; CPU_ZERO(&set); CPU_SET(0, &set);
        pthread_setaffinity_np(th.native_handle(), sizeof(set), &set);
    }

    void rx_loop_polling() {
        struct pollfd pfd{ .fd = sock_, .events = POLLIN };
        std::array<char, 16384> buf;
        while (run_rx_.load(std::memory_order_relaxed)) {
            int r = ::poll(&pfd, 1, 1);
            if (r <= 0) continue;
            if (pfd.revents & POLLIN) {
                for (;;) {
                    ssize_t n = ::recv(sock_, buf.data(), buf.size(), MSG_DONTWAIT);
                    if (n < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        std::this_thread::yield(); break;
                    }
                    if ((size_t)n > buf.size()) continue;
                    RxPacket pkt;
                    pkt.data.assign(buf.data(), buf.data() + n);
                    pkt.data.erase(std::remove(pkt.data.begin(), pkt.data.end(), '\0'), pkt.data.end()); // PATCH
                    pkt.t_arrival = std::chrono::steady_clock::now();
                    q_.push(std::move(pkt));
                }
            }
        }
    }
};

// ──────────────── FxCli ────────────────
FxCli::FxCli(const std::string &ip, uint16_t port)
: socket_(new UdpSocket(ip, port)) {}

FxCli::~FxCli() {
#ifdef DEBUG
    g_timer_ack.printStatistics();
#endif
    delete socket_;
}

void FxCli::send_cmd(const std::string &cmd) {
    if (!socket_) throw std::runtime_error("socket not initialized");
    socket_->send(cmd.c_str(), cmd.size());
    FXCLI_LOG("[SEND] " << cmd);
}

bool FxCli::send_cmd_wait_ok_tag(const std::string& cmd, const char* expect_tag, int timeout_ms) {
#ifdef DEBUG
    g_timer_ack.startTimer();
#endif
    socket_->flush_queue();
    send_cmd(cmd);
    std::string out;
    bool ok = socket_->wait_for_ok_tag(expect_tag, out, timeout_ms);
#ifdef DEBUG
    g_timer_ack.stopTimer();
#endif
    if (ok) std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    return ok;
}

// ---- 공개 API ----
std::string FxCli::mcu_ping() {
    std::string out;
    socket_->flush_queue();
    send_cmd("AT+PING");
    bool ok = socket_->wait_for_ok_tag("PING", out, timeout_ms_);
    return ok ? out : std::string();
}

std::string FxCli::mcu_whoami() {
    std::string out;
    socket_->flush_queue();
    send_cmd("AT+WHOAMI");
    bool ok = socket_->wait_for_ok_tag("WHOAMI", out, timeout_ms_);
    return ok ? out : std::string();
}

bool FxCli::motor_start(const std::vector<uint8_t> &ids) {
    return send_cmd_wait_ok_tag("AT+START " + build_id_group(ids), "START", timeout_ms_);
}
bool FxCli::motor_stop(const std::vector<uint8_t> &ids) {
    return send_cmd_wait_ok_tag("AT+STOP " + build_id_group(ids), "STOP", timeout_ms_);
}
bool FxCli::motor_estop(const std::vector<uint8_t> &ids) {
    return send_cmd_wait_ok_tag("AT+ESTOP " + build_id_group(ids), "ESTOP", timeout_ms_);
}
bool FxCli::motor_setzero(const std::vector<uint8_t> &ids) {
    return send_cmd_wait_ok_tag("AT+SETZERO " + build_id_group(ids), "SETZERO", timeout_ms_);
}

void FxCli::operation_control(const std::vector<uint8_t> &ids,
                              const std::vector<float> &pos,
                              const std::vector<float> &vel,
                              const std::vector<float> &kp,
                              const std::vector<float> &kd,
                              const std::vector<float> &tau) {
    const size_t n = ids.size();
    if (!(pos.size() == n && vel.size() == n && kp.size() == n && kd.size() == n && tau.size() == n))
        throw std::invalid_argument("All parameter arrays must have the same length");
    std::string cmd; cmd.reserve(32 * n + 16);
    cmd.append("AT+MIT ");
    char fb[32];
    for (size_t i = 0; i < n; ++i) {
        cmd.push_back('<');
        char ib[8]; int k = snprintf(ib, sizeof(ib), "%u", (unsigned)ids[i]);
        cmd.append(ib, ib + k); cmd.push_back(' ');
        cmd.append(format_float(fb, sizeof(fb), pos[i])); cmd.push_back(' ');
        cmd.append(format_float(fb, sizeof(fb), vel[i])); cmd.push_back(' ');
        cmd.append(format_float(fb, sizeof(fb), kp[i]));  cmd.push_back(' ');
        cmd.append(format_float(fb, sizeof(fb), kd[i]));  cmd.push_back(' ');
        cmd.append(format_float(fb, sizeof(fb), tau[i])); cmd.push_back('>');
        if (i + 1 < n) cmd.push_back(' ');
    }
    send_cmd(cmd);
}

std::string FxCli::req(const std::vector<uint8_t> &ids) {
    std::string out;
    socket_->flush_queue();
    send_cmd("AT+REQ " + build_id_group(ids));
    bool ok = socket_->wait_for_ok_tag("REQ", out, timeout_ms_rt_);
    return ok ? out : std::string();
}

std::string FxCli::status() {
    std::string out;
    socket_->flush_queue();
    send_cmd("AT+STATUS");
    bool ok = socket_->wait_for_ok_tag("STATUS", out, timeout_ms_rt_);
    return ok ? out : std::string();
}

void FxCli::flush() {
    if (!socket_) return;
    socket_->flush_queue();
    FXCLI_LOG("[FLUSH] queue cleared");
}
