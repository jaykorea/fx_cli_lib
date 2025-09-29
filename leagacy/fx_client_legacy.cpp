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
#include <chrono>   // ★ added

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/select.h>

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

// "OK <TAG>; ..." 에서 <TAG> 추출
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

// OK 응답 검증
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

// ========= UDP 소켓 =========
class FxCli::UdpSocket {
public:
    UdpSocket(const std::string &ip, uint16_t port) {
        sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ < 0) throw std::runtime_error("socket() failed");

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
    }

    ~UdpSocket() {
        if (sock_ >= 0) close_socket(sock_);
    }

    void send(const char *data, size_t len) {
        ssize_t n = ::send(sock_, data, (int)len, 0);
        if (n < 0 || (size_t)n != len) throw std::runtime_error("send() failed");
    }

    std::string recv(int timeout_ms) {
        if (timeout_ms < 0) timeout_ms = 0;
        fd_set fds; FD_ZERO(&fds); FD_SET(sock_, &fds);
        timeval tv; tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int r = ::select(sock_ + 1, &fds, nullptr, nullptr, &tv);
        if (r <= 0) return std::string();

        char buf[1024];
        ssize_t n = ::recv(sock_, buf, sizeof(buf) - 1, 0);
        if (n <= 0) return std::string();
        buf[n] = '\0';
        return std::string(buf, (size_t)n);
    }

private:
    int sock_{-1};
    struct sockaddr_in addr_{};
    static void close_socket(int s) { ::close(s); }
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

std::string FxCli::recv_cmd(int timeout_ms) {
    if (!socket_) throw std::runtime_error("socket not initialized");
    return socket_->recv(timeout_ms);
}

// ★ NEW: 원하는 TAG가 나올 때까지 짧게 반복 수신 (중간 잡음 패킷은 스킵)
bool FxCli::send_cmd_wait_tag(const std::string& cmd,
                              const char* expect_tag,
                              int timeout_ms)
{
#ifdef DEBUG
    g_timer_ack.startTimer();
#endif
    // 0) 직전 남은 잡음 드레인(논블로킹)
    for (;;) {
        std::string junk = recv_cmd(0);
        if (junk.empty()) break;
        FXCLI_LOG("[DRAIN] " << junk);
    }

    // 1) 송신
    send_cmd(cmd);

    // 2) 기대 TAG 나올 때까지 반복 수신
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 0);
    std::string resp;
    while (std::chrono::steady_clock::now() < deadline) {
        resp = recv_cmd(5); // 3ms 정도 짧게 폴링
        if (resp.empty()) continue;

        // OK 접두 + 태그 확인
        std::string tag;
        if (extract_tag_word(resp, tag) && upper_copy(tag) == upper_copy(expect_tag)) {
#ifdef DEBUG
            g_timer_ack.stopTimer();
            std::cout << "[DEBUG] " << expect_tag << " OK: " << resp << std::endl;
#endif
            return true;
        }

        // 기대 태그가 아니면 스킵
        FXCLI_LOG("[SKIP] " << resp);
    }

#ifdef DEBUG
    g_timer_ack.stopTimer();
    std::cerr << "[DEBUG] " << expect_tag << " FAIL: Timeout waiting correct tag" << std::endl;
#endif
    return false;
}

// (기존) 송신 + 단발 ACK 검증 (유지, 필요시 사용)
bool FxCli::send_cmd_chk_ack(const std::string &cmd,
                             const char *expect_tag,
                             int timeout_ms) {
#ifdef DEBUG
    g_timer_ack.startTimer();
#endif
    try {
        send_cmd(cmd);
        std::string resp = recv_cmd(timeout_ms);
        verify_ack_or_throw(resp, expect_tag);
#ifdef DEBUG
        g_timer_ack.stopTimer();
        std::cout << "[DEBUG] " << expect_tag << " OK: " << resp << std::endl;
#endif
        return true;
    } catch (const std::exception &e) {
#ifdef DEBUG
        g_timer_ack.stopTimer();
        std::cerr << "[DEBUG] " << expect_tag << " FAIL: " << e.what() << std::endl;
#endif
        return false;
    }
}

// (데이터형) 송신 + 전체 패킷(ACK+DATA) 그대로 반환 (ACK 검증 없음)
std::string FxCli::send_cmd_recv_data(const std::string& cmd,
                                      const char* /*expect_tag*/,
                                      int timeout_ms)
{
#ifdef DEBUG
    g_timer_ack.startTimer();
#endif
    send_cmd(cmd);
    std::string full = recv_cmd(timeout_ms);
#ifdef DEBUG
    g_timer_ack.stopTimer();
    // std::cout << "[DEBUG] RAW: " << full << std::endl;
#endif
    if (full.empty()) throw std::runtime_error("Timeout or empty reply from MCU");
    return full;
}

std::string FxCli::send_cmd_raw(const std::string& cmd, int timeout_ms)
{
#ifdef DEBUG
    g_timer_ack.startTimer();
#endif
    send_cmd(cmd);
    std::string full = recv_cmd(timeout_ms);
#ifdef DEBUG
    g_timer_ack.stopTimer();
    //std::cout << "[DEBUG] RAW: " << full << std::endl;
#endif
    return full;
}

// ---- 공개 API ----
// 제어 계열은 혼잡한 스트림에서 원하는 TAG를 기다리도록 변경
bool FxCli::motor_start(const std::vector<uint8_t> &ids) {
    std::string cmd = "AT+START " + build_id_group(ids);
    return send_cmd_wait_tag(cmd, "START", timeout_ms_);
}

bool FxCli::motor_stop(const std::vector<uint8_t> &ids) {
    std::string cmd = "AT+STOP " + build_id_group(ids);
    return send_cmd_wait_tag(cmd, "STOP", timeout_ms_);
}

bool FxCli::motor_estop(const std::vector<uint8_t> &ids) {
    std::string cmd = "AT+ESTOP " + build_id_group(ids);
    return send_cmd_wait_tag(cmd, "ESTOP", timeout_ms_);
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

    std::ostringstream oss;
    oss << "AT+MIT ";
    for (size_t i = 0; i < n; ++i) {
        oss << '<' << static_cast<unsigned>(ids[i]) << ' '
            << format_float(pos[i]) << ' ' << format_float(vel[i]) << ' '
            << format_float(kp[i])  << ' ' << format_float(kd[i])  << ' '
            << format_float(tau[i]) << '>';
        if (i + 1 < n) oss << ' ';
    }
    return send_cmd_wait_tag(oss.str(), "MIT", timeout_ms_);
}

// ---- 데이터 요청 ----
std::string FxCli::req(const std::vector<uint8_t> &ids, int timeout_ms) {
    std::string cmd = "AT+REQ " + build_id_group(ids);
    return send_cmd_raw(cmd, timeout_ms);          // ACK 검증 없이 원샷 수신
}

std::string FxCli::status(int timeout_ms) {
    std::string cmd = "AT+STATUS";
    return send_cmd_recv_data(cmd, "STATUS", timeout_ms);  // ACK 검증 없이 원샷 수신
}

void FxCli::flush() {
    while (true) {
        auto leftover = recv_cmd(0);
        if (leftover.empty()) break;
        FXCLI_LOG("[FLUSH] " << leftover);
    }
}
