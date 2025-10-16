// fx_client.cpp

#ifdef DEBUG
#define FXCLI_LOG(x) do { std::cerr << x << std::endl; } while(0)
#else
#define FXCLI_LOG(x) do {} while(0)
#endif

#include "fx_client.h"
#include "utils/elapsed_timer.h"
#include "utils/elapsed_timer_rt.h"

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
#include <mutex>
#include <condition_variable>
#include <unordered_map>   // ← 추가

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

#ifdef DEBUG
static ElapsedTimerRT g_timer_ack_buf("chk_ACK_buf");
static ElapsedTimerRT g_timer_ack_rt("chk_ACK_rt");
static ElapsedTimerRT g_timer_ack_n("chk_ACK_n");
#endif


// ──────────────── 내부 유틸 ────────────────
namespace {

static inline void trim(std::string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) { s.clear(); return; }
    size_t e = s.find_last_not_of(" \t\r\n");
    s.assign(s, b, e - b + 1);
}

static bool extract_tag_word(const std::string &resp, std::string &out_word) {
    size_t l = resp.find('<');
    if (l == std::string::npos) return false;
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

static inline bool tag_equals_ci(const std::string& tag, const char* expect_upper) {
    size_t elen = std::strlen(expect_upper);
    if (tag.size() < elen) return false;
    if (::strncasecmp(tag.c_str(), expect_upper, elen) != 0) return false;
    return (tag.size() == elen) || (tag[elen] == ';') || (tag[elen] == ' ');
}

static inline bool begins_with_ok(const std::string& s) {
    return (s.size() >= 2 &&
           (s[0] == 'O' || s[0] == 'o') &&
           (s[1] == 'K' || s[1] == 'k'));
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

static inline const char* format_float(char* buf, size_t bufsz, float v) {
    int n = snprintf(buf, bufsz, "%.6f", v);
    if (n <= 0) { buf[0] = '0'; buf[1] = 0; return buf; }
    int end = n - 1;
    while (end > 0 && buf[end] == '0') --end;
    if (end > 0 && buf[end] == '.') {} else end += 1;
    buf[end] = '\0';
    return buf;
}

// ─────────────────────────────────────────────
// ✅ LatestBufferRT — 항상 최신 1개만 유지 (Lock-free / No CV/Mutex)
// ─────────────────────────────────────────────
struct LatestBufferRT {
    std::atomic<uint64_t> wseq{0};  // producer가 쓴 횟수
    std::atomic<uint64_t> rseq{0};  // consumer가 읽은 마지막 시퀀스
    std::string latest;             // 단일 슬롯(항상 최신)

    // Producer: rx_thread_에서만 호출
    inline void push(std::string&& pkt) noexcept {
#ifdef DEBUG
        // 선택: push 경로의 지연을 보고 싶으면 주석 해제해 사용
        // g_timer_ack_push.start();
#endif
        latest = std::move(pkt);  // 단일 생산자라 데이터 레이스 없음
        wseq.fetch_add(1, std::memory_order_release);
#ifdef DEBUG
        // g_timer_ack_push.stop();
        // g_timer_ack_push.print_latest(); // [RTTimer|chk_ACK_push] X.XXX ms
#endif
    }

    // Consumer: wait_for_ok_tag()에서 호출 (timeout_ms 동안 최신 데이터 기다림)
    inline bool pop_latest(std::string& out, int timeout_ms) noexcept {
#ifdef DEBUG
        g_timer_ack_buf.startTimer();           // ✔ 네가 쓰던 이름 그대로 유지
#endif
        const uint64_t start_r = rseq.load(std::memory_order_relaxed);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

        for (;;) {
            // 새 데이터가 올라왔는지 확인
            uint64_t cur_w = wseq.load(std::memory_order_acquire);
            if (cur_w != start_r) {
                // 최신 1개만 전달
                out = std::move(latest);
                rseq.store(cur_w, std::memory_order_release);
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
#ifdef DEBUG
                g_timer_ack_buf.stopTimer();
                g_timer_ack_buf.printLatest();
                g_timer_ack_buf.printStatistics();
#endif
                return false; // timeout
            }
            // 너무 바쁘지 않게 살짝 양보 (sleep_for(30~50us)도 가능)
            //std::this_thread::yield();
            std::this_thread::sleep_for(std::chrono::microseconds(5));
        }
    }

    inline void clear() noexcept {
        uint64_t cur_w = wseq.load(std::memory_order_acquire);
        rseq.store(cur_w, std::memory_order_release);
        latest.clear();
    }
};

// ─────────────────────────────────────────────
// ✅ LatestBuffer
// ─────────────────────────────────────────────
struct LatestBuffer {
    std::mutex mtx;
    std::condition_variable cv;
    bool has_data{false};
    std::string latest;

    void push(std::string&& pkt) noexcept {
        {
            std::lock_guard<std::mutex> lock(mtx);
            latest = std::move(pkt);
            has_data = true;
        }
        cv.notify_one();
    }

    bool pop_latest(std::string& out, int timeout_ms) {
    #ifdef DEBUG
        g_timer_ack_buf.startTimer();
    #endif
        std::unique_lock<std::mutex> lock(mtx);
        bool got = cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                            [&]{ return has_data; });

    #ifdef DEBUG
        g_timer_ack_buf.stopTimer();
        g_timer_ack_buf.printLatest();
    #endif

        if (!got) {
        #ifdef DEBUG
            g_timer_ack_buf.printStatistics();
        #endif
            return false;
        }
        out = std::move(latest);
        has_data = false;
        return true;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mtx);
        has_data = false;
        latest.clear();
    }
};

// "SEQ_NUM: cnt:<num>;" 형태 파싱
static bool parse_seq_num(const std::string& s, uint64_t& out) {
    const char* key = "SEQ_NUM";
    size_t p = s.find(key);
    if (p == std::string::npos) return false;
    p = s.find("cnt:", p);
    if (p == std::string::npos) return false;
    p += 4;
    while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p;
    uint64_t val = 0;
    bool any = false;
    while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) {
        val = val * 10 + (s[p] - '0');
        ++p; any = true;
    }
    if (!any) return false;
    out = val;
    return true;
}

} // namespace

// ──────────────── FxCli::UdpSocket ────────────────
class FxCli::UdpSocket {
public:
    explicit UdpSocket(const std::string &ip, uint16_t port, int recv_buf_bytes = (64 * 1024)) {
        sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ < 0) throw std::runtime_error("socket() failed");

        if (recv_buf_bytes > 0) {
            // --- 1) 수신 버퍼 크기 설정 (패킷 누락 방지) ---
            int rcvbuf = std::max(recv_buf_bytes, 256 * 1024);
            if (::setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) != 0)
                perror("[WARN] setsockopt(SO_RCVBUF)");

            // --- 2) 수신 타임아웃 설정 (최대 3ms blocking) ---
            struct timeval tv;
            tv.tv_sec  = 0;
            tv.tv_usec = 3000;  // 3 ms
            if (::setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0)
                perror("[WARN] setsockopt(SO_RCVTIMEO)");
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
        rx_thread_ = std::thread(&UdpSocket::rx_thread_entry, this);
    }

    ~UdpSocket() {
        run_rx_.store(false);
        if (sock_ >= 0) {
            ::shutdown(sock_, SHUT_RDWR);
            ::close(sock_);
        }
        if (rx_thread_.joinable()) rx_thread_.join();
    }

    void send(const char *data, size_t len) {
        ssize_t n = ::send(sock_, data, (int)len, 0);
        if (n < 0) throw std::runtime_error(std::string("send() failed: ") + strerror(errno));
        if ((size_t)n != len) throw std::runtime_error("partial send()");
    }

    void flush_queue() { q_.clear(); }

    // 태그 미스매치가 들어올 수 있으므로, 남은 시간 동안 재시도
    bool wait_for_ok_tag(const char* expect_tag_upper, std::string& out_ok, int timeout_ms) {
        using clock = std::chrono::steady_clock;
        auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);

        for (;;) {
            int remain = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - clock::now()).count();
            if (remain <= 0) return false;

            std::string data;
            if (!q_.pop_latest(data, remain)) return false; // timeout

            // #ifdef DEBUG
            // FXCLI_LOG("[RX] " << data);
            // #endif

            if (!begins_with_ok(data)) continue;

            std::string tag;
            if (!extract_tag_word(data, tag)) continue;
            if (!tag_equals_ci(tag, expect_tag_upper)) {
                // 다른 태그면 스킵하고 남은 시간 내 재시도
                continue;
            }

            // ── SEQ 연속성 검증: 모든 태그 공통 적용 ─────────────────────────
            // 응답 문자열에 SEQ_NUM 필드가 있는 경우에만 동작 (없으면 스킵)
            uint64_t seq{};
            if (parse_seq_num(data, seq)) {
                std::lock_guard<std::mutex> lock(seq_mtx_);
                uint64_t& prev = seq_map_[expect_tag_upper]; // 태그별로 저장
                if (prev != 0 && seq != prev + 1) {
                    FXCLI_LOG(std::string("[DROP?] ") + expect_tag_upper +
                              " SEQ jump: prev=" + std::to_string(prev) +
                              " curr=" + std::to_string(seq));
                }
                prev = seq;
            }
            // ────────────────────────────────────────────────────────────────

            out_ok = std::move(data);
            return true;
        }
    }

private:
    int sock_{-1};
    struct sockaddr_in addr_{};
    std::atomic<bool> run_rx_{false};
    std::thread rx_thread_;
    LatestBufferRT q_;  // ✅ 항상 최신 데이터만 유지

    // 태그별 SEQ 추적용 (예: "REQ", "STATUS", "MIT" 등)
    std::unordered_map<std::string, uint64_t> seq_map_;
    std::mutex seq_mtx_;

    void rx_thread_entry() {
        // prio, cpu_index는 환경 맞춰 조정
        set_thread_rt_and_affinity(/*fifo_prio=*/85, /*cpu_index=*/4);
        rx_loop_polling();
    }

    void rx_loop_polling() {
        struct pollfd pfd{ .fd = sock_, .events = POLLIN };
        std::array<char, 65536> buf;

        using clock = std::chrono::steady_clock;

        // 바깥(전체 시스템) 5ms 예산 중 RX는 2~3ms만 사용 (예: 3ms)
        const auto RX_BUDGET = std::chrono::milliseconds(3);

        while (run_rx_.load(std::memory_order_relaxed)) {
            // 1) poll로 이벤트 감시 (1ms 정도; 필요시 남은 전체 예산으로 조정)
            int r = ::poll(&pfd, 1, /*timeout_ms=*/1);
            if (r <= 0) continue;
            if (!(pfd.revents & POLLIN)) continue;

            // 2) drain 은 반드시 비-블로킹으로, 그리고 시간 제한!
            const auto drain_deadline = clock::now() + RX_BUDGET;
            for (;;) {
                if (clock::now() >= drain_deadline) break; // 예산 소진 → 즉시 탈출

                // 비-블로킹 수신: 절대 기다리지 않음
                ssize_t n = ::recv(sock_, buf.data(), buf.size(), MSG_DONTWAIT);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break; // 현재 큐 비었음
                    perror("[WARN] recv"); break;
                }
                if (n == 0) break; // UDP에선 거의 없음

                std::string pkt(buf.data(), buf.data() + n);
                q_.push(std::move(pkt)); // 최신으로 덮어쓰기
            }
        }
    }

};

// ──────────────── FxCli ────────────────
FxCli::FxCli(const std::string &ip, uint16_t port)
: socket_(new UdpSocket(ip, port)) {}

FxCli::~FxCli() {
#ifdef DEBUG
    g_timer_ack_rt.printStatistics();
    g_timer_ack_n.printStatistics();
#endif
    delete socket_;
}

void FxCli::send_cmd(const std::string &cmd) {
    if (!socket_) throw std::runtime_error("socket not initialized");
    socket_->send(cmd.c_str(), cmd.size());
#ifdef DEBUG
    FXCLI_LOG("[SEND] " << cmd);
#endif
}

// 비실시간 명령 전용: flush + 1s 안정화 sleep 유지
bool FxCli::send_cmd_wait_ok_tag(const std::string& cmd, const char* expect_tag, int timeout_ms) {
#ifdef DEBUG
    g_timer_ack_n.startTimer();
#endif
    socket_->flush_queue();      // Non-RT만 사용
    send_cmd(cmd);
    std::string out;
    bool ok = socket_->wait_for_ok_tag(expect_tag, out, timeout_ms);
#ifdef DEBUG
    g_timer_ack_n.stopTimer();
    g_timer_ack_n.printLatest();
#endif
    if (ok) std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // ✅ 유지(Non-RT 안정화)
    return ok;
}

// ─────────────────────────────────────────────
// 공개 API
// ─────────────────────────────────────────────
std::string FxCli::mcu_ping() {
    // Non-RT: flush + 안정화
    send_cmd_wait_ok_tag("AT+PING", "PING", timeout_ms_);
    return std::string("OK <PING>");
}

std::string FxCli::mcu_whoami() {
    send_cmd_wait_ok_tag("AT+WHOAMI", "WHOAMI", timeout_ms_);
    return std::string("OK <WHOAMI>");
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

bool FxCli::operation_control(const std::vector<uint8_t> &ids,
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
    std::string out;
    // bool ok = socket_->wait_for_ok_tag("MIT", out, timeout_ms_rt_);
    return true;
}

std::string FxCli::req(const std::vector<uint8_t> &ids) {
#ifdef DEBUG
    g_timer_ack_rt.startTimer();
#endif
    std::string out;
    send_cmd("AT+REQ " + build_id_group(ids));
    bool ok = socket_->wait_for_ok_tag("REQ", out, timeout_ms_rt_);
#ifdef DEBUG
    g_timer_ack_rt.stopTimer();
    g_timer_ack_rt.printLatest();
#endif
    return ok ? out : std::string();
}

std::string FxCli::status() {
#ifdef DEBUG
    g_timer_ack_rt.startTimer();
#endif
    std::string out;
    send_cmd("AT+STATUS");
    bool ok = socket_->wait_for_ok_tag("STATUS", out, timeout_ms_rt_);
#ifdef DEBUG
    g_timer_ack_rt.stopTimer();
    g_timer_ack_rt.printLatest();
#endif
    return ok ? out : std::string();
}

void FxCli::flush() {
    if (!socket_) return;
    socket_->flush_queue();
#ifdef DEBUG
    FXCLI_LOG("[FLUSH] queue cleared");
#endif
}
