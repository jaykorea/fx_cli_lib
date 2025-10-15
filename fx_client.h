#pragma once

#include <string>
#include <vector>
#include <cstdint>

// 리눅스 전용 UDP 클라이언트
// - 내부적으로 RX 전용 스레드와 링버퍼를 운영하여
//   측정 지연 편차를 최소화하고 안정적인 패킷 수신을 지원.
class FxCli {
public:
  FxCli(const std::string& ip, uint16_t port);
  FxCli(const FxCli&) = delete;
  FxCli& operator=(const FxCli&) = delete;
  ~FxCli();

  // ──────────────────────────
  // 공개 명령 API
  // ──────────────────────────
  std::string mcu_ping();
  std::string mcu_whoami();

  bool motor_start (const std::vector<uint8_t>& ids);
  bool motor_stop  (const std::vector<uint8_t>& ids);
  bool motor_estop (const std::vector<uint8_t>& ids);
  bool motor_setzero(const std::vector<uint8_t>& ids);

  // MIT 제어
  void operation_control(const std::vector<uint8_t>& ids,
                         const std::vector<float>& pos,
                         const std::vector<float>& vel,
                         const std::vector<float>& kp,
                         const std::vector<float>& kd,
                         const std::vector<float>& tau);

  // 데이터 질의
  std::string req   (const std::vector<uint8_t>& ids);
  std::string status();

  // 큐에 남아있는 모든 수신 패킷 즉시 폐기
  void flush();

private:
  // ──────────────────────────
  // 내부 I/O 유틸
  // ──────────────────────────
  void send_cmd(const std::string& cmd);

  // 원하는 TAG가 나올 때까지 큐에서 대기
  bool send_cmd_wait_ok_tag(const std::string& cmd,
                            const char* expect_tag,
                            int timeout_ms);

  // [MOD] RT/일반 명령의 대기시간(ms) 분리
  int timeout_ms_    = 200;  // 일반 명령
  int timeout_ms_rt_ = 5;    // 실시간 명령(REQ/STATUS)

  // ──────────────────────────
  // 내부 UDP 소켓 + 수신 스레드/큐 관리
  // ──────────────────────────
  class UdpSocket;
  UdpSocket* socket_;
};