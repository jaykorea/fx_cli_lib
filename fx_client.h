#pragma once

#include <string>
#include <vector>
#include <cstdint>

// 리눅스 전용 UDP AT-Command 클라이언트
// - 내부적으로 RX 전용 스레드와 링버퍼를 운영하여
//   측정 지연 편차를 최소화하고 안정적인 패킷 수신을 지원합니다.
class FxCli {
public:
  // ip와 port로 대상 MCU를 설정합니다.
  FxCli(const std::string& ip, uint16_t port);
  FxCli(const FxCli&) = delete;
  FxCli& operator=(const FxCli&) = delete;
  ~FxCli();

  // ──────────────────────────
  // 공개 명령 API
  // ──────────────────────────
  // 모터 구동/정지/비상정지 : MCU로 명령 전송 후 원하는 TAG가 올 때까지 큐에서 대기
  bool motor_start(const std::vector<uint8_t>& ids);
  bool motor_stop (const std::vector<uint8_t>& ids);
  bool motor_estop(const std::vector<uint8_t>& ids);

  // MIT 제어 : 동일하게 TAG 대기 방식으로 안정 처리
  void operation_control(const std::vector<uint8_t>& ids,
                         const std::vector<float>& pos,
                         const std::vector<float>& vel,
                         const std::vector<float>& kp,
                         const std::vector<float>& kd,
                         const std::vector<float>& tau);

  // 데이터 질의
  //  - req   : <REQ> 태그가 올 때까지 큐에서 대기 후 가장 최근 패킷 반환
  //  - status: <STATUS> 태그가 올 때까지 대기 후 패킷 반환
  //  - timeout_ms: 전체 대기 시간(ms)
  std::string req   (const std::vector<uint8_t>& ids);
  std::string status();

  // 큐에 남아있는 모든 수신 패킷을 즉시 폐기
  void flush();

private:
  // ──────────────────────────
  // 내부 I/O 유틸
  // ──────────────────────────
  // 단순 송신
  void send_cmd(const std::string& cmd);

  // 원하는 TAG가 나올 때까지 큐에서 대기
  bool send_cmd_wait_ok_tag(const std::string& cmd,
                         const char* expect_tag,
                         int timeout_ms);

  // 기본 대기시간(ms) – 필요 시 조정
  int timeout_ms_ = 2;

  // ──────────────────────────
  // 내부 UDP 소켓 + 수신 스레드/큐 관리
  // ──────────────────────────
  class UdpSocket;
  UdpSocket* socket_;
};