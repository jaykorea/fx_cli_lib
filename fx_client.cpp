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
#include <unordered_map>   // ← 기존 유지

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
static ElapsedTimerRT g_timer_ack_n("chk_ACK_n");
static ElapsedTimerRT g_timer_ack_req("chk_ACK_REQ");
static ElapsedTimerRT g_timer_ack_mit("chk_ACK_MIT");
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
// ✅ LatestBufferRT — CV 기반 실시간 안전 버퍼
//     • push() → 최신 데이터로 교체 + 즉시 notify
//     • pop_latest() → 최신 데이터만 소비 (타임아웃 지원)
//     • clear() → 내부 버퍼 완전 초기화
//     • 모든 연산은 cv_mtx로 보호되어 race-free
// ─────────────────────────────────────────────

struct LatestBufferRT {
    uint64_t wseq{0};        // write sequence (증가용 카운터)
    uint64_t rseq{0};        // read sequence  (마지막 소비된 카운터)
    std::string latest;      // 최신 패킷 보관용 버퍼

    std::mutex cv_mtx;
    std::condition_variable cv;

    // ───────────── push ─────────────
    inline void push(std::string&& pkt) noexcept {
        {
            std::lock_guard<std::mutex> lk(cv_mtx);
            latest = std::move(pkt);
            ++wseq;  // 단순 증가 (락으로 보호되므로 atomic 불필요)
        }
        cv.notify_one();  // 즉시 wake (락 해제 후 호출)
    }

    // ───────────── pop_latest ─────────────
    bool pop_latest(std::string& out, int timeout_ms) noexcept {
        using clock = std::chrono::steady_clock;
        const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
        std::unique_lock<std::mutex> lk(cv_mtx);

        if (wseq != rseq) {
            out = latest;
            rseq = wseq;
            return true;
        }

        if (!cv.wait_until(lk, deadline, [&]{ return wseq != rseq; })) {
            ::sched_yield(); // 타임아웃 후 양보 시도
            return false;
        }

        out = latest;
        rseq = wseq;
        return true;
    }

    // ───────────── clear ─────────────
    inline void clear() noexcept {
        std::lock_guard<std::mutex> lk(cv_mtx);
        rseq = wseq;     // 읽기 인덱스 최신화
        latest.clear();  // 문자열 버퍼 초기화
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

// [CHANGED] ─────────────────────────────────────────────
// 태그별 1-슬롯 버퍼 디멀티플렉싱 구조 추가
// MIT/REQ/STATUS 등 서로 다른 ACK가 같은 큐를 덮어쓰지 않도록 분리
// ──────────────────────────────────────────────────────
struct AckQueues { // [CHANGED]
    LatestBufferRT mit;
    LatestBufferRT req;
    LatestBufferRT status;
    LatestBufferRT ping, whoami, start_, stop_, estop_, setzero;

    void clear_all() { // [CHANGED]
        mit.clear(); req.clear(); status.clear();
        ping.clear(); whoami.clear();
        start_.clear(); stop_.clear(); estop_.clear(); setzero.clear();
    }

    // [CHANGED] 태그 문자열로 해당 큐 선택
    LatestBufferRT* select(const char* tag_upper) noexcept {
        if (!tag_upper) return nullptr;
        // strcmp는 <cstring> 필요 (이미 포함되어 있음)
        if (std::strcmp(tag_upper,"MIT")     == 0) return &mit;
        if (std::strcmp(tag_upper,"REQ")     == 0) return &req;
        if (std::strcmp(tag_upper,"STATUS")  == 0) return &status;
        if (std::strcmp(tag_upper,"PING")    == 0) return &ping;
        if (std::strcmp(tag_upper,"WHOAMI")  == 0) return &whoami;
        if (std::strcmp(tag_upper,"START")   == 0) return &start_;
        if (std::strcmp(tag_upper,"STOP")    == 0) return &stop_;
        if (std::strcmp(tag_upper,"ESTOP")   == 0) return &estop_;
        if (std::strcmp(tag_upper,"SETZERO") == 0) return &setzero;
        return nullptr;
    }

    // [CHANGED] 단일 태그만 비우기 (내부 select 사용)
    bool clear_tag(const char* tag_upper) noexcept {
        if (auto* q = select(tag_upper)) { q->clear(); return true; }
        std::cerr << "[AckQueues] clear_tag: unknown tag: "
                << (tag_upper ? tag_upper : "(null)") << "\n";
        return false;
    }

    // [CHANGED] 패킷 내용으로 큐 선택
    LatestBufferRT* select_by_packet(const std::string& pkt) noexcept {
        if (!begins_with_ok(pkt)) return nullptr;
        std::string tag;
        if (!extract_tag_word(pkt, tag)) return nullptr;

        if (tag_equals_ci(tag, "MIT"))     return &mit;
        if (tag_equals_ci(tag, "REQ"))     return &req;
        if (tag_equals_ci(tag, "STATUS"))  return &status;
        if (tag_equals_ci(tag, "PING"))    return &ping;
        if (tag_equals_ci(tag, "WHOAMI"))  return &whoami;
        if (tag_equals_ci(tag, "START"))   return &start_;
        if (tag_equals_ci(tag, "STOP"))    return &stop_;
        if (tag_equals_ci(tag, "ESTOP"))   return &estop_;
        if (tag_equals_ci(tag, "SETZERO")) return &setzero;
        return nullptr;
    }
};


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

            // --- 2) 수신 타임아웃 설정 (최대 1ms blocking) ---
            // struct timeval tv;
            // tv.tv_sec  = 0;
            // tv.tv_usec = 1000;  // 1ms
            // if (::setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0)
            //     perror("[WARN] setsockopt(SO_RCVTIMEO)");
        }

        int yes = 1;
        setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        setsockopt(sock_, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));

        int tos = 0x10; // 0x10 = IPTOS_LOWDELAY
        if (::setsockopt(sock_, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) != 0)
            perror("[WARN] setsockopt(IP_TOS)");

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
        run_rx_.store(false, std::memory_order_release);
        if (sock_ >= 0) {
            ::shutdown(sock_, SHUT_RDWR);
            ::close(sock_);
        }
        if (rx_thread_.joinable()) rx_thread_.join();
    }

    void create_socket_or_throw() {
        std::lock_guard<std::mutex> lk(sock_mtx_);
        // 기존 소켓이 살아 있으면 닫기
        if (sock_ >= 0) {
            ::shutdown(sock_, SHUT_RDWR);
            ::close(sock_);
            sock_ = -1;
            std::this_thread::sleep_for(std::chrono::microseconds(50));  // ✅ 50us 안정화 대기
        }

        // 새 소켓 생성
        sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);

        if (sock_ < 0)
            throw std::runtime_error("socket() failed: " + std::string(strerror(errno)));

        // 기존과 동일한 옵션 재적용
        int rcvbuf = 256 * 1024;
        if (::setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) != 0)
            perror("[WARN] setsockopt(SO_RCVBUF)");

        int yes = 1;
        setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        setsockopt(sock_, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));

        int tos = 0x10; // 0x10 = IPTOS_LOWDELAY
        if (::setsockopt(sock_, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) != 0)
            perror("[WARN] setsockopt(IP_TOS)");

        // struct timeval tv;
        // tv.tv_sec  = 0;
        // tv.tv_usec = 1000;  // 1ms timeout
        // if (::setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0)
        //     perror("[WARN] setsockopt(SO_RCVTIMEO)");

        int flags = ::fcntl(sock_, F_GETFL, 0);
        ::fcntl(sock_, F_SETFL, flags | O_NONBLOCK);

        if (::connect(sock_, reinterpret_cast<struct sockaddr*>(&addr_), sizeof(addr_)) < 0) {
            int err = errno;
            ::close(sock_);
            sock_ = -1;
            throw std::runtime_error("connect() failed: " + std::string(strerror(err)));
        }

        FXCLI_LOG("[UdpSocket] new socket created (fd=" << sock_ << ")");
    }

    void send(const char *data, size_t len) {
        int fd;
        {
            std::lock_guard<std::mutex> lk(sock_mtx_);
            fd = sock_;
        }

        if (fd < 0)
            throw std::runtime_error("send() failed: invalid socket descriptor");

        ssize_t n = ::send(fd, data, (int)len, 0);
        if (n < 0)
            throw std::runtime_error(std::string("send() failed: ") + strerror(errno));
        if ((size_t)n != len)
            throw std::runtime_error("partial send()");
    }

    // [CHANGED] 전체 큐 비우기 → 태그별 큐 전체 초기화
    void flush_queue() { q_.clear_all(); } // [CHANGED]

    // [CHANGED] 단일 태그 플러시
    void flush_tag(const char* tag_upper) { q_.clear_tag(tag_upper); }

    // [CHANGED] 태그별 큐에서 잔여시간 내 재시도 (deadline 기반)
    bool wait_for_ok_tag(const char* expect_tag_upper, std::string& out_ok, int timeout_ms) {
        using clock = std::chrono::steady_clock;
        auto* q = q_.select(expect_tag_upper);
        if (!q) {
            return false;
        }

        #ifdef DEBUG
        ElapsedTimerRT* t = timer_for_expect(expect_tag_upper);
        if (t) t->startTimer();
        #endif

        std::string data;
        if (!q->pop_latest(data, timeout_ms)) {
            #ifdef DEBUG
            FXCLI_LOG("[wait_for_ok_tag] pop_latest timeout, yielding");
            if (t) { t->stopTimer(); t->printLatest(); }
            #endif
            ::sched_yield(); // Yield to allow rx_thread to push data
            return false;
        }
        if (!begins_with_ok(data)) return false;

        std::string tag;
        if (!extract_tag_word(data, tag)) return false;
        if (!tag_equals_ci(tag, expect_tag_upper)) return false;

        uint64_t seq{};
        if (parse_seq_num(data, seq)) {
            std::lock_guard<std::mutex> lock(seq_mtx_);
            uint64_t& prev = seq_map_[std::string(expect_tag_upper)];
            if (prev != 0 && seq != prev + 1) {
                FXCLI_LOG(std::string("[DROP?] ") + expect_tag_upper +
                        " SEQ jump: prev=" + std::to_string(prev) +
                        " curr=" + std::to_string(seq));
            }
            prev = seq;
        }

        #ifdef DEBUG
        if (t) { t->stopTimer(); t->printLatest(); }
        #endif
        out_ok = std::move(data);
        return true;
    }

private:
    int sock_{-1};
    std::mutex sock_mtx_;
    struct sockaddr_in addr_{};
    std::atomic<bool> run_rx_{false};
    std::thread rx_thread_;

    AckQueues q_;  // [CHANGED] ✅ 태그별 최신 데이터만 유지

    // 태그별 SEQ 추적용 (예: "REQ", "STATUS", "MIT" 등)
    std::unordered_map<std::string, uint64_t> seq_map_;
    std::mutex seq_mtx_;

#ifdef DEBUG
    // [CHANGED - TIMER] 태그별 타이머 세트
    struct TagTimers {
        ElapsedTimerRT mit     {"ack_MIT"};
        ElapsedTimerRT req     {"ack_REQ"};
        ElapsedTimerRT status  {"ack_STATUS"};
        ElapsedTimerRT ping    {"ack_PING"};
        ElapsedTimerRT whoami  {"ack_WHOAMI"};
        ElapsedTimerRT start_  {"ack_START"};
        ElapsedTimerRT stop_   {"ack_STOP"};
        ElapsedTimerRT estop_  {"ack_ESTOP"};
        ElapsedTimerRT setzero {"ack_SETZERO"};
        ElapsedTimerRT other   {"ack_OTHER"};
    } timers_;  // 태그별 누적 통계/최근치 출력 가능
#endif

#ifdef DEBUG
    // [CHANGED - TIMER] 기대 태그에 해당하는 타이머 포인터 반환
    inline ElapsedTimerRT* timer_for_expect(const char* tag) noexcept {
        if (!tag) return &timers_.other;
        if (!strcmp(tag,"MIT"))     return &timers_.mit;
        if (!strcmp(tag,"REQ"))     return &timers_.req;
        if (!strcmp(tag,"STATUS"))  return &timers_.status;
        if (!strcmp(tag,"PING"))    return &timers_.ping;
        if (!strcmp(tag,"WHOAMI"))  return &timers_.whoami;
        if (!strcmp(tag,"START"))   return &timers_.start_;
        if (!strcmp(tag,"STOP"))    return &timers_.stop_;
        if (!strcmp(tag,"ESTOP"))   return &timers_.estop_;
        if (!strcmp(tag,"SETZERO")) return &timers_.setzero;
        return &timers_.other;
    }
#endif

    void rx_thread_entry() {
        // prio, cpu_index는 환경 맞춰 조정
        //set_thread_rt_and_affinity(/*fifo_prio=*/85, /*cpu_index=*/4);
        rx_loop_polling();
    }

    void rx_loop_polling() {
        struct pollfd pfd{ .fd = sock_, .events = POLLIN };
        std::array<char, 65536> buf;

        using clock = std::chrono::steady_clock;

        const auto RX_BUDGET = std::chrono::milliseconds(1);

        while (run_rx_.load(std::memory_order_acquire)) {
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
                    int err = errno;
                    if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR){
                        break;
                    }
                    if (err == EBADF || err == ENOTCONN || err == ENETDOWN ||
                        err == ECONNRESET || err == ECONNREFUSED || err == EPIPE) {

                        if (!run_rx_.load(std::memory_order_acquire)) {
                            std::cerr << "[FxCli::UdpSocket] Socket error during shutdown (errno=" << err << "), exiting...\n";
                            break;
                        }
                        std::cerr << "[FxCli::UdpSocket] Detected bad socket (errno=" << err
                                  << "), attempting recreate...\n";

                        // ──────────────────────────────
                        //  소켓 재생성 시간 측정 시작
                        // ──────────────────────────────
                        auto t_recreate_start = clock::now();

                        try {
                            create_socket_or_throw();   // ✅ 간단히 재호출
                            {
                                std::lock_guard<std::mutex> lk(sock_mtx_);
                                pfd.fd = sock_;  // ✅ 새 소켓 핸들 갱신
                            }

                            auto t_recreate_end = clock::now();
                            auto recreate_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                                    t_recreate_end - t_recreate_start).count();

                            std::cerr << "[FxCli::UdpSocket] Socket recreated successfully ("
                                      << recreate_us << " us elapsed)\n";
                        } catch (const std::exception &e) {
                            auto t_recreate_end = clock::now();
                            auto recreate_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                                    t_recreate_end - t_recreate_start).count();

                            std::cerr << "[FxCli::UdpSocket] Socket recreation failed after "
                                      << recreate_us << " us: " << e.what() << "\n";
                        }
                        // ──────────────────────────────

                        break;
                    }

                    std::cerr << "[FxCli::UdpSocket] recv() error " << err
                              << ": " << strerror(err) << "\n";
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    break;
                }
                if (n == 0) break; // UDP에선 거의 없음

                std::string pkt(buf.data(), buf.data() + n);

                // [CHANGED] 수신 즉시 태그 파싱 → 해당 태그 큐로 라우팅
                if (auto* qdst = q_.select_by_packet(pkt)) {
                    qdst->push(std::move(pkt));
                } else {
                    std::cerr << "[RX] drop unknown/invalid packet: " << pkt << "\n";
                    continue;
                }
            }
        }
    }
};


// ──────────────── FxCli ────────────────
FxCli::FxCli(const std::string &ip, uint16_t port)
: socket_(new UdpSocket(ip, port)) {}

FxCli::~FxCli() {
    delete socket_;
}

void FxCli::send_cmd(const std::string &cmd) {
    if (!socket_)
        throw std::runtime_error("socket not initialized");

    try {
        socket_->send(cmd.c_str(), cmd.size());
    }
    catch (const std::exception &e) {
        std::cerr << "[FxCli::send_cmd] send() failed: " << e.what() << std::endl;

        try {
            std::cerr << "[FxCli::send_cmd] Attempting socket recreate...\n";
            socket_->create_socket_or_throw();
            std::cerr << "[FxCli::send_cmd] Socket recreated successfully.\n";
        } catch (const std::exception &ex) {
            std::cerr << "[FxCli::send_cmd] Socket recreation failed: "
                      << ex.what() << std::endl;
        }
    }
}

// 비실시간 명령 전용: flush + 1s 안정화 sleep 유지
bool FxCli::send_cmd_wait_ok_tag(const std::string& cmd, const char* expect_tag, int timeout_ms) {
#ifdef DEBUG
    g_timer_ack_n.startTimer();
#endif
    // socket_->flush_tag(expect_tag);
    socket_->flush_queue();      // Non-RT만 사용 (이제 태그별 큐 전체 초기화)  // [CHANGED] 주석
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
#ifdef DEBUG
g_timer_ack_mit.startTimer();
#endif

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
    std::string out;
    send_cmd(cmd);
    bool ok = socket_->wait_for_ok_tag("MIT", out, timeout_ms_rt_);
#ifdef DEBUG
g_timer_ack_mit.stopTimer();
g_timer_ack_mit.printLatest();
#endif
    return ok;
}

std::string FxCli::req(const std::vector<uint8_t> &ids) {
#ifdef DEBUG
g_timer_ack_req.startTimer();
#endif
    std::string out;
    send_cmd("AT+REQ " + build_id_group(ids));
    bool ok = socket_->wait_for_ok_tag("REQ", out, timeout_ms_rt_);
#ifdef DEBUG
g_timer_ack_req.stopTimer();
g_timer_ack_req.printLatest();
#endif
    return ok ? out : std::string();
}

std::string FxCli::status() {
    std::string out;
    send_cmd("AT+STATUS");
    bool ok = socket_->wait_for_ok_tag("STATUS", out, timeout_ms_rt_);
    return ok ? out : std::string();
}

void FxCli::flush() {
    if (!socket_) return;
    socket_->flush_queue();
#ifdef DEBUG
    FXCLI_LOG("[FLUSH] queue cleared");
#endif
}
