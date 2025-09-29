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
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>

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

static inline std::string upper_copy(std::string s) {
    for (char &c : s)
        c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
    return s;
}

// "OK <TAG>; ..." 혹은 "OK <TAG> ..." 에서 <TAG> 추출
static bool extract_tag_word(const std::string &resp, std::string &out_word) {
    auto l = resp.find('<');
    auto r = resp.find('>');
    if (l == std::string::npos || r == std::string::npos || r <= l + 1) return false;
    std::string inside = resp.substr(l + 1, r - l - 1);
    trim(inside);
    if (inside.empty()) return false;
    size_t cut = inside.find_first_of(" \t(");
    out_word = (cut == std::string::npos) ? inside : inside.substr(0, cut);
    trim(out_word);
    return !out_word.empty();
}

// OK 응답 검증 (단발용, 유지)
static void verify_ack_or_throw(const std::string &resp, const char *expect_tag) {
    if (resp.empty()) throw std::runtime_error("Timeout or empty reply from MCU");

    std::string s = resp;
    trim(s);
    std::string su = upper_copy(s);
    if (su.rfind("OK", 0) != 0)
        throw std::runtime_error("Unexpected reply (no OK): " + s);

    std::string tag;
    if (!extract_tag_word(s, tag))
        throw std::runtime_error("Missing <TAG> in reply: " + s);

    if (upper_copy(tag) != upper_copy(expect_tag))
        throw std::runtime_error("ACK TAG mismatch: expected '" +
                                 std::string(expect_tag) + "' got '" + tag + "'");
}

// float -> 문자열 변환
static std::string format_float(float v) {
    std::ostringstream ss;
    ss << std::setprecision(6) << std::fixed << v;
    std::string s = ss.str();
    size_t pos = s.find_last_not_of('0');
    if (pos != std::string::npos) {
        if (s[pos] == '.') s.erase(pos + 2);
        else s.erase(pos + 1);
    }
    return s;
}

// ID 그룹 빌드
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

} // namespace

// ========= UDP 소켓 + RX 스레드/링버퍼 =========
class FxCli::UdpSocket {
public:
    struct RxPacket {
        std::string data;
        std::chrono::steady_clock::time_point t_arrival;
    };

    UdpSocket(const std::string &ip, uint16_t port, size_t max_queue = 256)
    : max_queue_(max_queue)
    {
        sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ < 0) throw std::runtime_error("socket() failed");

        // 넉넉한 수신 버퍼(버스트 대비)
        int rcvbuf = 1 << 20; // 1MB
        ::setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

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

        // Rx 스레드 시작
        run_rx_.store(true);
        rx_thread_ = std::thread([this]{ this->rx_loop_blocking(); });
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
        std::lock_guard<std::mutex> lk(m_);
        q_.clear();
    }

    // OK <TAG>; ... 패킷이 올 때까지 대기
    bool wait_for_ok_tag(const std::string& expect_tag_upper,
                        std::string& out_ok,
                        int timeout_ms)
    {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 0);

        std::unique_lock<std::mutex> lk(m_);
        auto pred = [this, &expect_tag_upper, &out_ok]() -> bool {
            for (auto it = q_.rbegin(); it != q_.rend(); ++it) { // ★ 뒤에서부터
                if (upper_copy(it->data).rfind("OK", 0) == 0) {
                    std::string tag;
                    if (extract_tag_word(it->data, tag) &&
                        upper_copy(tag) == expect_tag_upper)
                    {
                        out_ok = it->data;
                        // 최신 패킷 이후는 남기고, 찾은 것 이전만 삭제
                        q_.erase(q_.begin(), it.base());
                        return true;
                    }
                }
            }
            return false;
        };

        if (timeout_ms <= 0) {
            return pred(); // 논블로킹
        }
        while (cv_.wait_until(lk, deadline, pred) == false) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
        }
        return true;
    }

    // 아무 패킷이나 하나(가장 최근) 대기 - 태그 검증 없이
    bool wait_for_any(std::string& out, int timeout_ms) {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 0);
        std::unique_lock<std::mutex> lk(m_);
        auto pred = [this, &out]() -> bool {
            if (!q_.empty()) {
                out = q_.back().data;
                q_.clear();
                return true;
            }
            return false;
        };
        if (timeout_ms <= 0) return pred();
        while (cv_.wait_until(lk, deadline, pred) == false) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
        }
        return true;
    }

private:
    int sock_{-1};
    struct sockaddr_in addr_{};

    std::atomic<bool> run_rx_{false};
    std::thread rx_thread_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<RxPacket> q_;
    size_t max_queue_;

    static void close_socket(int s) { ::close(s); }

    void rx_loop_blocking() {
        // 블로킹 recv 루프
        while (run_rx_.load()) {
            char buf[1024];
            ssize_t n = ::recv(sock_, buf, sizeof(buf) - 1, 0);
            if (n <= 0) {
                // 소켓 종료/에러 시 잠깐 쉼
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            buf[n] = '\0';

            RxPacket pkt;
            pkt.data = std::string(buf, (size_t)n);
            pkt.t_arrival = std::chrono::steady_clock::now();

            {
                std::lock_guard<std::mutex> lk(m_);
                if (q_.size() >= max_queue_) {
                    // 오래된 것부터 드랍
                    q_.pop_front();
                }
                q_.push_back(std::move(pkt));
            }
            cv_.notify_all();
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

// ★ NEW: 원하는 TAG가 나올 때까지 큐에서 대기
bool FxCli::send_cmd_wait_ok_tag(const std::string& cmd,
                              const char* expect_tag,
                              int timeout_ms)
{
#ifdef DEBUG
    g_timer_ack.startTimer();
#endif
    // 0) 직전 남은 큐 드레인
    socket_->flush_queue();

    // 1) 송신
    send_cmd(cmd);

    // 2) 기대 TAG 대기
    std::string out;
    bool ok = socket_->wait_for_ok_tag(upper_copy(expect_tag), out, timeout_ms);

#ifdef DEBUG
    g_timer_ack.stopTimer();
    if (ok) std::cout << "[DEBUG] " << expect_tag << " OK: " << out << std::endl;
    else    std::cerr << "[DEBUG] " << expect_tag << " FAIL: Timeout waiting correct tag" << std::endl;
#endif
    return ok;
}

// ---- 공개 API ----
// 제어 계열은 혼잡한 스트림에서 원하는 TAG를 기다리도록 변경
bool FxCli::motor_start(const std::vector<uint8_t> &ids) {
    std::string cmd = "AT+START " + build_id_group(ids);
    return send_cmd_wait_ok_tag(cmd, "START", timeout_ms_);
}

bool FxCli::motor_stop(const std::vector<uint8_t> &ids) {
    std::string cmd = "AT+STOP " + build_id_group(ids);
    return send_cmd_wait_ok_tag(cmd, "STOP", timeout_ms_);
}

bool FxCli::motor_estop(const std::vector<uint8_t> &ids) {
    std::string cmd = "AT+ESTOP " + build_id_group(ids);
    return send_cmd_wait_ok_tag(cmd, "ESTOP", timeout_ms_);
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

    std::ostringstream oss;
    oss << "AT+MIT ";
    for (size_t i = 0; i < n; ++i) {
        oss << '<' << static_cast<unsigned>(ids[i]) << ' '
            << format_float(pos[i]) << ' ' << format_float(vel[i]) << ' '
            << format_float(kp[i])  << ' ' << format_float(kd[i])  << ' '
            << format_float(tau[i]) << '>';
        if (i + 1 < n) oss << ' ';
    }
    send_cmd(oss.str());
}

// ---- 데이터 요청 ----
std::string FxCli::req(const std::vector<uint8_t> &ids)
{
    std::string cmd = "AT+REQ " + build_id_group(ids);

#ifdef DEBUG
    g_timer_ack.startTimer();
#endif
    send_cmd(cmd);

    std::string out;
    bool ok = socket_->wait_for_ok_tag("REQ", out, timeout_ms_);   // ★ 태그 검증 - ON
    // bool ok = socket_->wait_for_any(out, timeout_ms_);   // ★ 태그 검증 - OFF
    
#ifdef DEBUG
    g_timer_ack.stopTimer();
#endif
    return ok ? out : std::string();
}

std::string FxCli::status()
{
    std::string cmd = "AT+STATUS";

#ifdef DEBUG
    g_timer_ack.startTimer();
#endif
    socket_->flush_queue();
    send_cmd(cmd);

    std::string out;
    bool ok = socket_->wait_for_ok_tag("STATUS", out, timeout_ms_);   // ★ 태그 검증 - ON
    // bool ok = socket_->wait_for_any(out, timeout_ms_);   // ★ 태그 검증 없음
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