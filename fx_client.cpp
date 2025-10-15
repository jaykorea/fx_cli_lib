// fx_client_optimized.cpp
// - UDP recv: poll() + non-blocking recv()
// - Lock-free ring buffer for RX queue (SPSC-safe; producer never touches tail)   // [NOTE]
// - Optional RT priority (SCHED_FIFO)
// - Reduced string copies; use strncasecmp for tag match
// - Same public API as before

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

#include <algorithm>   // [OPT] std::max
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <poll.h>
#include <pthread.h>
#include <strings.h>   // strncasecmp
#include <fcntl.h>     // fcntl(), F_GETFL, F_SETFL, O_NONBLOCK
#include <sched.h>     // [OPT] CPU affinity
#include <sys/mman.h>  // [OPT] mlockall

#ifdef DEBUG
static ElapsedTimer g_timer_ack("chk_ACK");
#endif

// ========= 내부 유틸 =========
namespace {

static inline void trim(std::string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) { s.clear(); return; }
    size_t e = s.find_last_not_of(" \t\r\n");
    s.assign(s, b, e - b + 1);
}

// "OK <TAG>; ..." 혹은 "OK <TAG> ..." 에서 <TAG> 추출
static bool extract_tag_word(const std::string &resp, std::string &out_word) {
    const size_t l = resp.find('<');
    const size_t r = resp.find('>');
    if (l == std::string::npos || r == std::string::npos || r <= l + 1) return false;
    std::string inside = resp.substr(l + 1, r - l - 1);
    trim(inside);
    if (inside.empty()) return false;
    size_t cut = inside.find_first_of(" \t(");
    out_word = (cut == std::string::npos) ? inside : inside.substr(0, cut);
    trim(out_word);
    return !out_word.empty();
}

// 태그 비교: 기대 태그와 길이 동일 + 대소문자 무시 완전 일치        // [FIX]
static inline bool tag_equals_ci(const std::string& tag, const char* expect_upper) {
    const size_t elen = std::strlen(expect_upper);
    if (tag.size() != elen) return false;
    return ::strncasecmp(tag.c_str(), expect_upper, elen) == 0;
}

// float -> 문자열 (snprintf 기반, 빠르고 할당 없음)
static inline const char* format_float(char* buf, size_t bufsz, float v) {
    // 소수 6자리 고정 후 뒷 0 제거
    int n = snprintf(buf, bufsz, "%.6f", v);
    if (n <= 0) { buf[0] = '0'; buf[1] = 0; return buf; }
    // 뒤 0 트림
    int end = n - 1;
    while (end > 0 && buf[end] == '0') --end;
    // "1." -> "1" 로 정규화                                       // [FIX]
    if (end > 0 && buf[end] == '.') {
        // '.' 위치에 널 종료
    } else {
        end += 1;
    }
    buf[end] = '\0';
    return buf;
}

// ID 그룹 빌드: "<1 2 3 ...>"
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

// 응답 OK + <TAG> 대략 체크 (빠른 전처리)
static inline bool begins_with_ok(const std::string& s) {
    // 대소문자 무시 "OK"
    if (s.size() < 2) return false;
    return (s[0] == 'O' || s[0] == 'o') &&
           (s[1] == 'K' || s[1] == 'k');
}

} // namespace


// ========= FxCli 구현 =========
class FxCli::UdpSocket {
public:
    struct RxPacket {
        std::string data;
        std::chrono::steady_clock::time_point t_arrival;
    };

    // lock-free ring buffer (single producer/consumer, SPSC-safe)
    struct RingBuf {
        static constexpr size_t N = 256; // [OPT] 2의 제곱으로 두고 & (N-1) 최적화 가능

        RxPacket buf[N];

        // [OPT] false sharing 완화를 위해 캐시라인 정렬
        alignas(64) std::atomic<size_t> head{0};   // producer 전용
        alignas(64) std::atomic<size_t> tail{0};   // consumer 전용

        alignas(64) std::atomic<size_t> dropped_newest{0}; // full 시 최신 드롭 카운터  // [OPT]

        // Producer only
        bool push(RxPacket&& pkt) {
            size_t h = head.load(std::memory_order_relaxed);
            size_t n = (h + 1) % N;

            // full이면 '새로 온 패킷'을 드롭. producer는 tail을 절대 건드리지 않음.   // [FIX]
            if (n == tail.load(std::memory_order_acquire)) {
                dropped_newest.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            buf[h] = std::move(pkt);
            head.store(n, std::memory_order_release);
            return true;
        }

        // Consumer only
        bool pop(RxPacket& out) {
            size_t t = tail.load(std::memory_order_acquire);
            if (t == head.load(std::memory_order_acquire)) return false;
            out = buf[t];
            tail.store((t + 1) % N, std::memory_order_release);
            return true;
        }

        // Consumer only
        void clear() {
            tail.store(head.load(std::memory_order_acquire), std::memory_order_release);
        }

        bool empty() const {
            return tail.load(std::memory_order_acquire) == head.load(std::memory_order_acquire);
        }

        // 최신부터 역방향 스캔: 조건자 만족하는 첫 패킷 반환 (consumer only)
        template <typename Pred>
        bool find_latest(Pred pred, RxPacket& out) {
            // 경계/종료 조건 명확화                                          // [FIX]
            const size_t h = head.load(std::memory_order_acquire);
            const size_t t = tail.load(std::memory_order_acquire);
            if (t == h) return false;

            size_t i = (h + N - 1) % N;
            const size_t span = (h + N - t) % N;
            for (size_t c = 0; c < span; ++c) {
                if (pred(buf[i])) {
                    out = buf[i];
                    tail.store((i + 1) % N, std::memory_order_release); // 이전 것들은 모두 버림
                    return true;
                }
                i = (i + N - 1) % N;
            }
            return false;
        }
    };

    UdpSocket(const std::string &ip, uint16_t port, int recv_buf_bytes = (64 * 1024))
    {
        sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ < 0) throw std::runtime_error("socket() failed");

        // 수신 버퍼 확대(최소 256KB 보장)                                   // [OPT]
        if (recv_buf_bytes > 0) {
            int rcvbuf = std::max(recv_buf_bytes, 256 * 1024);
            ::setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        }

        // 비차단 모드
        int flags = ::fcntl(sock_, F_GETFL, 0);
        ::fcntl(sock_, F_SETFL, flags | O_NONBLOCK);

        std::memset(&addr_, 0, sizeof(addr_));
        addr_.sin_family = AF_INET;
        addr_.sin_port   = htons(port);
        if (::inet_pton(AF_INET, ip.c_str(), &addr_.sin_addr) != 1) {
            close_socket(sock_);
            throw std::runtime_error("inet_pton failed");
        }
        if (::connect(sock_, reinterpret_cast<struct sockaddr *>(&addr_), sizeof(addr_)) < 0) {
            close_socket(sock_);
            throw std::runtime_error("connect() failed");
        }

        // [OPT] 지연 분석용 커널 타임스탬프
        // int on = 1; ::setsockopt(sock_, SOL_SOCKET, SO_TIMESTAMPNS, &on, sizeof(on));

        // Rx 스레드 시작
        run_rx_.store(true);
        rx_thread_ = std::thread([this]{ this->rx_loop_polling(); });

        // 가능하면 RT 우선순위 부여 (실패해도 무시)
        set_thread_rt_priority(rx_thread_, 80);
    }

    ~UdpSocket() {
        run_rx_.store(false);

        if (sock_ >= 0) {
            ::shutdown(sock_, SHUT_RDWR);
            ::close(sock_);
            sock_ = -1;
        }

        if (rx_thread_.joinable()) {
            rx_thread_.join();
        }
    }

    void send(const char *data, size_t len) {
        ssize_t n = ::send(sock_, data, (int)len, 0);
        if (n < 0 || (size_t)n != len) throw std::runtime_error("send() failed");
    }

    // ---- 큐 유틸 ----
    void flush_queue() {
        q_.clear();
    }

    // OK <TAG> 패킷 대기
    bool wait_for_ok_tag(const char* expect_tag_upper, std::string& out_ok, int timeout_ms) {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 0);

        auto pred = [&](const RxPacket& p){
            if (!begins_with_ok(p.data)) return false;
            std::string tag;
            if (!extract_tag_word(p.data, tag)) return false;
            return tag_equals_ci(tag, expect_tag_upper); // 길이 동일+완전일치   // [FIX]
        };

        // 즉시 스캔
        if (q_.find_latest(pred, tmp_)) {
            out_ok = std::move(tmp_.data);
            return true;
        }
        if (timeout_ms <= 0) return false;

        // 바쁜 대기 최소화: yield                                         // [OPT]
        while (std::chrono::steady_clock::now() < deadline) {
            if (q_.find_latest(pred, tmp_)) {
                out_ok = std::move(tmp_.data);
                return true;
            }
            std::this_thread::yield();
        }
        return false;
    }

    // 아무 패킷이나 하나(가장 최근) 대기
    bool wait_for_any(std::string& out, int timeout_ms) {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 0);

        if (!q_.empty()) {
            RxPacket p;
            // 최신 하나만 소비: 전부 drain 후 마지막을 사용
            while (q_.pop(p)) { /* drain */ }
            out = std::move(p.data);
            return true;
        }
        if (timeout_ms <= 0) return false;

        while (std::chrono::steady_clock::now() < deadline) {
            if (!q_.empty()) {
                RxPacket p;
                while (q_.pop(p)) {}
                out = std::move(p.data);
                return true;
            }
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

    static void close_socket(int s) { ::close(s); }

    static void set_thread_rt_priority(std::thread& th, int prio) {
        struct sched_param sp;
        sp.sched_priority = prio;
        pthread_setschedparam(th.native_handle(), SCHED_FIFO, &sp);
        // 실패 시 무시 (권한 없을 수 있음)

        // 페이지 폴트 방지(지터 완화)                                   // [OPT]
        mlockall(MCL_CURRENT | MCL_FUTURE);

        // RX 스레드 코어 고정(마이그레이션 지터 완화)
        // ⚠ IRQ 코어와 경합 시 오히려 느려질 수 있음 → 환경에 맞춰 조정   // [OPT]
        cpu_set_t set; CPU_ZERO(&set); CPU_SET(0, &set);
        pthread_setaffinity_np(th.native_handle(), sizeof(set), &set);
    }

    void rx_loop_polling() {
        struct pollfd pfd{ .fd = sock_, .events = POLLIN };
        std::array<char, 16384> buf;  // 버퍼 확대                          // [OPT]

        while (run_rx_.load(std::memory_order_relaxed)) {
            // 안정적 커널 대기(1ms). 0으로 두면 yield 빈번 → 지터 증가 가능    // [OPT]
            int r = ::poll(&pfd, 1, 1);
            if (r <= 0) continue;

            if (pfd.revents & POLLIN) {
                for (;;) {
                    // TRUNC 미사용: 단순/저비용 경로                          // [OPT]
                    ssize_t n = ::recv(sock_, buf.data(), buf.size(), MSG_DONTWAIT);
                    if (n < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        std::this_thread::yield();
                        break;
                    }
                    // 이론상 n > buf.size() 불가(커널에서 자름), 방어 코드만 유지
                    if ((size_t)n > buf.size()) {
                        continue;
                    }
                    RxPacket pkt;
                    pkt.data.assign(buf.data(), buf.data() + n);
                    pkt.t_arrival = std::chrono::steady_clock::now();
                    q_.push(std::move(pkt)); // full이면 최신 드롭                // [FIX]
                }
            }
        }
    }
};


// ========= FxCli =========
FxCli::FxCli(const std::string &ip, uint16_t port)
: socket_(new UdpSocket(ip, port)) {}

FxCli::~FxCli() {
#ifdef DEBUG
    g_timer_ack.printStatistics();
#endif
    delete socket_;
}

// ---- 내부 I/O ----
void FxCli::send_cmd(const std::string &cmd) {
    if (!socket_) throw std::runtime_error("socket not initialized");
    socket_->send(cmd.c_str(), cmd.size());
    FXCLI_LOG("[SEND] " << cmd);
}

// 원하는 TAG가 나올 때까지 대기
bool FxCli::send_cmd_wait_ok_tag(const std::string& cmd, const char* expect_tag, int timeout_ms)
{
#ifdef DEBUG
    g_timer_ack.startTimer();
#endif
    socket_->flush_queue();   // 혼선 방지 정책 유지 (의도적으로 flush)         // [NOTE]
    send_cmd(cmd);

    std::string out;
    bool ok = socket_->wait_for_ok_tag(expect_tag, out, timeout_ms);
#ifdef DEBUG
    g_timer_ack.stopTimer();
    if (ok) std::cout << "[DEBUG] " << expect_tag << " OK: " << out << std::endl;
    else    std::cerr << "[DEBUG] " << expect_tag << " FAIL: Timeout waiting correct tag" << std::endl;
#endif
    if (ok) {
        constexpr int POST_OK_DELAY_MS = 2000;  // 2 seconds (의도 유지)       // [NOTE]
        std::this_thread::sleep_for(std::chrono::milliseconds(POST_OK_DELAY_MS));
    }
    return ok;
}

// ---- 공개 API ----
std::string FxCli::mcu_ping() {
    std::string out;
    socket_->flush_queue();                    // 과거 응답 오매칭 방지             // [NOTE]
    send_cmd("AT+PING");
#ifdef DEBUG
    g_timer_ack.startTimer();
#endif
    bool ok = socket_->wait_for_ok_tag("PING", out, timeout_ms_);
#ifdef DEBUG
    g_timer_ack.stopTimer();
#endif
    return ok ? out : std::string();
}

std::string FxCli::mcu_whoami() {
    std::string out;
    socket_->flush_queue();                    // [NOTE]
    send_cmd("AT+WHOAMI");
#ifdef DEBUG
    g_timer_ack.startTimer();
#endif
    bool ok = socket_->wait_for_ok_tag("WHOAMI", out, timeout_ms_);
#ifdef DEBUG
    g_timer_ack.stopTimer();
#endif
    return ok ? out : std::string();
}

bool FxCli::motor_start(const std::vector<uint8_t> &ids) {
    const std::string cmd = "AT+START " + build_id_group(ids);
    return send_cmd_wait_ok_tag(cmd, "START", timeout_ms_);
}
bool FxCli::motor_stop(const std::vector<uint8_t> &ids) {
    const std::string cmd = "AT+STOP " + build_id_group(ids);
    return send_cmd_wait_ok_tag(cmd, "STOP", timeout_ms_);
}
bool FxCli::motor_estop(const std::vector<uint8_t> &ids) {
    const std::string cmd = "AT+ESTOP " + build_id_group(ids);
    return send_cmd_wait_ok_tag(cmd, "ESTOP", timeout_ms_);
}
bool FxCli::motor_setzero(const std::vector<uint8_t> &ids) {
    const std::string cmd = "AT+SETZERO " + build_id_group(ids);
    return send_cmd_wait_ok_tag(cmd, "SETZERO", timeout_ms_);
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

    // reserve 여유 확대(동적 재할당 가능성 더 낮춤)                          // [OPT]
    std::string cmd;
    cmd.reserve(32 * n + 16);
    cmd.append("AT+MIT ");
    char fb[32];

    for (size_t i = 0; i < n; ++i) {
        cmd.push_back('<');
        // id
        {
            char ib[8];
            int k = snprintf(ib, sizeof(ib), "%u", (unsigned)ids[i]);
            cmd.append(ib, ib + k);
            cmd.push_back(' ');
        }
        // pos vel kp kd tau
        cmd.append(format_float(fb, sizeof(fb), pos[i]));
        cmd.push_back(' ');
        cmd.append(format_float(fb, sizeof(fb), vel[i]));
        cmd.push_back(' ');
        cmd.append(format_float(fb, sizeof(fb), kp[i]));
        cmd.push_back(' ');
        cmd.append(format_float(fb, sizeof(fb), kd[i]));
        cmd.push_back(' ');
        cmd.append(format_float(fb, sizeof(fb), tau[i]));
        cmd.push_back('>');
        if (i + 1 < n) cmd.push_back(' ');
    }
    send_cmd(cmd);
}

std::string FxCli::req(const std::vector<uint8_t> &ids) {
    const std::string cmd = "AT+REQ " + build_id_group(ids);
#ifdef DEBUG
    g_timer_ack.startTimer();
#endif
    socket_->flush_queue();                    // [NOTE]
    send_cmd(cmd);

    std::string out;
    bool ok = socket_->wait_for_ok_tag("REQ", out, timeout_ms_rt_);
#ifdef DEBUG
    g_timer_ack.stopTimer();
#endif
    return ok ? out : std::string();
}

std::string FxCli::status() {
    const std::string cmd = "AT+STATUS";
#ifdef DEBUG
    g_timer_ack.startTimer();
#endif
    socket_->flush_queue();                    // 유지                           // [NOTE]
    send_cmd(cmd);

    std::string out;
    bool ok = socket_->wait_for_ok_tag("STATUS", out, timeout_ms_rt_);
#ifdef DEBUG
    g_timer_ack.stopTimer();
#endif
    return ok ? out : std::string();
}

void FxCli::flush() {
    if (!socket_) return;
    socket_->flush_queue();
    FXCLI_LOG("[FLUSH] queue cleared");
}
