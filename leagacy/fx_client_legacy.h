#pragma once

#include <string>
#include <vector>
#include <cstdint>

// 리눅스 전용 UDP AT-Command 클라이언트
class FxCli {
public:
  // ip와 port로 대상 MCU를 설정합니다.
  FxCli(const std::string& ip, uint16_t port);
  FxCli(const FxCli&) = delete;
  FxCli& operator=(const FxCli&) = delete;
  ~FxCli();

  // 중요 명령: MCU로 명령 전송 후 원하는 TAG가 올 때까지 반복 수신 → 성공(true) / 실패(false)
  bool motor_start(const std::vector<uint8_t>& ids);
  bool motor_stop (const std::vector<uint8_t>& ids);
  bool motor_estop(const std::vector<uint8_t>& ids);

  // MIT 제어: 동일하게 TAG 대기 방식으로 안정 처리
  bool operation_control(const std::vector<uint8_t>& ids,
                         const std::vector<float>& pos,
                         const std::vector<float>& vel,
                         const std::vector<float>& kp,
                         const std::vector<float>& kd,
                         const std::vector<float>& tau);

  // 상태 질의: ACK 검증 없이 데이터 전체를 원샷 수신
  std::string req   (const std::vector<uint8_t>& ids, int timeout_ms = 200);
  std::string status(int timeout_ms = 200);

private:
  // 내부 전송 및 수신
  void        send_cmd(const std::string& cmd);
  std::string recv_cmd(int timeout_ms);

  // 단발 ACK 검증 (기존 유지, 필요 시 사용)
  bool        send_cmd_chk_ack(const std::string& cmd,
                               const char* expect_tag,
                               int timeout_ms);

  // ★ 추가: 원하는 TAG가 나올 때까지 반복 수신
  bool        send_cmd_wait_tag(const std::string& cmd,
                                const char* expect_tag,
                                int timeout_ms);

  // 데이터형: ACK 검증 없이 전체 패킷(ACK+DATA) 그대로 반환
  std::string send_cmd_recv_data(const std::string& cmd,
                                 const char* expect_tag,
                                 int timeout_ms);

  // 단순 송신 후 1회 수신 (ACK 검증 없음, RAW)
  std::string send_cmd_raw(const std::string& cmd, int timeout_ms);

  void flush();

  int timeout_ms_ = 1;

  // UDP 소켓 래퍼(중첩 클래스)
  class UdpSocket;
  UdpSocket* socket_;
};
