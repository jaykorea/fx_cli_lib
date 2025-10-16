#pragma once

#include <string>
#include <vector>
#include <cstdint>

/**
 * @brief Linux-only UDP client for the Fx protocol.
 *
 * The FxCli class encapsulates a UDP socket client designed for low-latency,
 * real-time communication with MCU-based motor controllers. Internally it
 * operates a dedicated background thread that continuously receives UDP packets
 * and stores only the most recent frame in a lock-free buffer (LatestBuffer).
 *
 * All command/response APIs follow the pattern:
 *   1. Flush old packets.
 *   2. Send a command string (e.g., "AT+REQ <1 2>").
 *   3. Wait for an OK<TAG> response within a defined timeout.
 *
 * Time-critical commands (REQ, STATUS) use a shorter timeout (≈5 ms),
 * while normal control or configuration commands use longer windows.
 *
 * This design guarantees deterministic latency and eliminates packet backlog.
 */
class FxCli {
public:
  /**
   * @brief Construct a UDP client and start the RX thread.
   * @param ip   Target IPv4 address (e.g., "192.168.10.2")
   * @param port Target UDP port number
   */
  FxCli(const std::string& ip, uint16_t port);

  FxCli(const FxCli&) = delete;
  FxCli& operator=(const FxCli&) = delete;

  /// Destructor automatically stops and joins the RX thread.
  ~FxCli();

  // ────────────────────────────────
  // Command API (Public Interface)
  // ────────────────────────────────

  /// @brief MCU alive check ("AT+PING")
  std::string mcu_ping();

  /// @brief Query device identifier ("AT+WHOAMI")
  std::string mcu_whoami();

  /// @brief Start / Stop / Emergency-Stop / Zeroing for specified motor IDs
  bool motor_start (const std::vector<uint8_t>& ids);
  bool motor_stop  (const std::vector<uint8_t>& ids);
  bool motor_estop (const std::vector<uint8_t>& ids);
  bool motor_setzero(const std::vector<uint8_t>& ids);

  /**
   * @brief Send MIT control frames (multi-motor command).
   *
   * @param ids  Motor IDs
   * @param pos  Desired positions
   * @param vel  Desired velocities
   * @param kp   Proportional gains
   * @param kd   Derivative gains
   * @param tau  Torque commands
   *
   * All vectors must have identical length N.
   */
  bool operation_control(const std::vector<uint8_t>& ids,
                         const std::vector<float>& pos,
                         const std::vector<float>& vel,
                         const std::vector<float>& kp,
                         const std::vector<float>& kd,
                         const std::vector<float>& tau);

  /// @brief Request real-time observation ("AT+REQ <ids>")
  std::string req(const std::vector<uint8_t>& ids);

  /// @brief Request status report ("AT+STATUS")
  std::string status();

  /// @brief Immediately discard all received packets.
  void flush();

private:
  // ────────────────────────────────
  // Internal helpers
  // ────────────────────────────────

  /**
   * @brief Transmit a raw command string over the UDP socket.
   *
   * The command is sent exactly as provided; no CR/LF is appended.
   * This internal helper wraps UdpSocket::send() and logs the command
   * when built with DEBUG defined.
   *
   * @param cmd command to transmit
   */
  void send_cmd(const std::string& cmd);

  /**
   * @brief Issue a command and wait for a matching OK<TAG> response.
   *
   * The transmit queue is flushed before the command is sent to avoid
   * matching stale responses. The method delegates the blocking wait
   * to UdpSocket::wait_for_ok_tag().
   *
   * @param cmd          Full AT command string
   * @param expect_tag   Expected OK<TAG> literal (e.g., "REQ")
   * @param timeout_ms   Timeout in milliseconds
   * @return true if matching tag was received before timeout
   */
  bool send_cmd_wait_ok_tag(const std::string& cmd,
                            const char* expect_tag,
                            int timeout_ms);

  // ────────────────────────────────
  // Timeout configurations
  // ────────────────────────────────
  int timeout_ms_    = 200;  ///< General command timeout (ms)
  int timeout_ms_rt_ = 5;    ///< Real-time command timeout (ms)

  // ────────────────────────────────
  // Internal UDP socket handler
  // ────────────────────────────────
  class UdpSocket;
  UdpSocket* socket_;
};
