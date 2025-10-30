#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>     // [CHANGED] perror
#include <pthread.h>  // [CHANGED] pthread_* APIs
#include <sched.h>    // [CHANGED] SCHED_FIFO, sched_param
#include <sys/mman.h>

// RT 스레드/코어 고정/페이지 폴트 방지 유틸
static void set_thread_rt_and_affinity(int fifo_prio, int cpu_index) {
    pthread_t tid = pthread_self();

    // 1) 실시간 스케줄
    sched_param sp{}; sp.sched_priority = fifo_prio;   // 1~99
    if (pthread_setschedparam(tid, SCHED_FIFO, &sp) != 0) {
        perror("[WARN] pthread_setschedparam");
    }

    // 2) 코어 고정
    if (cpu_index >= 0) {
        cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(cpu_index, &cs);
        if (pthread_setaffinity_np(tid, sizeof(cs), &cs) != 0) {
            perror("[WARN] pthread_setaffinity_np");
        }
    }

    // 3) 페이지 폴트 방지
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        perror("[WARN] mlockall");
    }
}

/**
 * @brief Linux-only UDP client for the Fx protocol.
 *
 * The FxCli class encapsulates a UDP socket client designed for low-latency,
 * real-time communication with MCU-based motor controllers.
 *
 * [CHANGED] Internally it runs a dedicated RX thread that receives UDP packets
 * and performs *tag-based demultiplexing* into per-tag, single-slot, CV-protected
 * buffers (e.g., MIT / REQ / STATUS / ...). Each tag holds only the *latest*
 * frame, eliminating backlog and *preventing cross-tag overwrite* issues.
 *
 * All command/response APIs follow the pattern:
 *   1. (Non-RT only) Flush previously received packets (per-tag buffers).
 *   2. Send a command string (e.g., "AT+REQ <1 2>").
 *   3. Wait once for an OK<TAG> response within a defined timeout *from the
 *      corresponding tag buffer only*.
 *
 * Time-critical commands (e.g., REQ, STATUS) use a shorter timeout (≈5 ms),
 * while normal commands use a longer window.
 *
 * [CHANGED] This design keeps deterministic latency while avoiding “ACK
 * double-check” and “MIT/REQ cross-consumption” that could happen when sharing
 * a single latest buffer across tags.
 */
class FxCli {
public:
  /**
   * @brief Construct a UDP client and start the RX thread.
   * @param ip   Target IPv4 address (e.g., "192.168.10.10")
   * @param port Target UDP port number
   */
  FxCli(const std::string& ip = "192.168.10.10", uint16_t port = 5101);

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

  //Temp
  std::string req_simul();
  std::string status_simul();

  /// @brief Immediately discard all received packets. 
  /// [CHANGED] Clears *all per-tag* buffers (MIT/REQ/STATUS/...).
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
   * [CHANGED] The method flushes per-tag buffers (Non-RT flows), sends the command,
   * then waits *once* on the tag-specific buffer only—no internal retry loop.
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
  int timeout_ms_rt_ = 2;    ///< Real-time command timeout (ms)

  // ────────────────────────────────
  // Internal UDP socket handler
  // ────────────────────────────────
  class UdpSocket;
  UdpSocket* socket_;
};
