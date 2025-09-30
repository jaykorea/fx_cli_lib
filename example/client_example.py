import fx_cli
import time
"""
Example usage of the FX CLI Python bindings.

This script demonstrates how to send high level commands to the FX motor
controller using JSON-like inputs and receive structured responses over
the text-based AT command protocol via the `fx_cli` module.
"""

# Create a client; you can override the defaults by passing ip/port (port is int).
cli = fx_cli.FxCli("192.168.10.10", 5101)

# 1) 특정 ID 제어 예시
ids_specific = [1, 2, 3, 4]

try:
    # START (ACK: OK <START ...>)
    # if not cli.motor_start(ids_specific):
    #     print("Motor start failed")
    # else:
    #     print("Motor start succeeded")

    # Operation Control
    mit_groups = [
        {"id": ids_specific[0], "pos": 0.0, "vel": 0.0, "kp": 0.0, "kd": 0.1, "tau": 0.0},
        {"id": ids_specific[1], "pos": 0.0, "vel": 0.0, "kp": 0.0, "kd": 0.1, "tau": 0.0},
        {"id": ids_specific[2], "pos": 0.0, "vel": 0.0, "kp": 0.0, "kd": 0.1, "tau": 0.0},
        {"id": ids_specific[3], "pos": 0.0, "vel": 0.0, "kp": 0.0, "kd": 0.1, "tau": 0.0},
    ]

    T = 3600
    dt = 0.02
    steps = int(T / dt)

    last_seq_num = None  # 마지막 시퀀스 번호를 저장할 변수
    missed_packets_count = 0  # 놓친 패킷 수를 저장할 변수
    print(f"📊 테스트를 시작합니다. 예상 실행 시간: {T}초, 총 스텝: {steps}회")
    time.sleep(2)

    # MCU PING Test
    # result = cli.mcu_ping()
    # print(result)

    # # MCU Identify Test
    # result = cli.mcu_whoami()
    # print(result)

    # start = time.time()
    # for i in range(steps):
    #     cli.operation_control(mit_groups)
    #     time.sleep(dt)
    # end = time.time()

    # Get Obs
    start = time.time()
    for i in range(steps):
        try:
            state = cli.req(ids_specific)
            
            # 'SEQ_NUM' 또는 'cnt' 키가 없을 경우를 대비
            if 'SEQ_NUM' in state and 'cnt' in state['SEQ_NUM']:
                current_seq_num = state['SEQ_NUM']['cnt']
                
                # 첫 번째 패킷을 수신한 경우, last_seq_num을 초기화
                if last_seq_num is None:
                    last_seq_num = current_seq_num
                else:
                    # 예상되는 다음 시퀀스 번호
                    expected_seq_num = last_seq_num + 1
                    
                    # 시퀀스 번호가 순차적이지 않은 경우 (패킷 손실 발생)
                    if current_seq_num != expected_seq_num:
                        # 몇 개의 패킷을 놓쳤는지 계산
                        dropped_count = current_seq_num - expected_seq_num
                        if dropped_count > 0:
                            print(f"패킷 손실 감지! 예상: {expected_seq_num}, 수신: {current_seq_num} (손실: {dropped_count}개)")
                            missed_packets_count += dropped_count
                    
                # 다음 루프를 위해 현재 시퀀스 번호를 저장
                last_seq_num = current_seq_num
                
            else:
                print("경고: 응답에 'SEQ_NUM' 또는 'cnt' 키가 없습니다.")
                missed_packets_count += 1

        except Exception as e:
            print(f"에러 발생: {e}")
            missed_packets_count += 1 # 에러 발생도 하나의 패킷 손실로 간주
            
        time.sleep(dt)
        
    end = time.time()

    # ===================================================================
    # 3. 최종 결과 출력
    # ===================================================================
    total_time = end - start
    received_packets_count = steps
    total_expected_packets = received_packets_count + missed_packets_count
    loss_percentage = (missed_packets_count / total_expected_packets) * 100 if total_expected_packets > 0 else 0

    print("\n" + "="*40)
    print("📈 테스트 결과 요약")
    print("="*40)
    print(f"  - 총 실행 시간: {total_time:.2f} 초")
    print(f"  - 총 요청 횟수 (수신 시도): {received_packets_count} 회")
    print(f"  - 놓친 패킷 총계: {missed_packets_count} 개")
    print(f"  - 패킷 손실률: {loss_percentage:.4f} %")
    print("="*40)


    # Get Status
    # start = time.time()
    # for i in range(steps):
    #     status = cli.status()
    #     print(status)
    #     time.sleep(dt)
    # end = time.time()

    # Motor Stop
    if not cli.motor_stop(ids_specific):
        print("Motor stop failed")
    else:
        print("Motor stop succeeded")

except Exception as e:
    print("Error during specific-ID flow:", e)

# 2) 브로드캐스트 제어 예시
# ids_broadcast = [0xff]

# try:
#     cli.motor_stop(ids_broadcast)
#     print("motor_stop(broadcast) OK")

#     # 필요 시 E-STOP 브로드캐스트
#     # cli.motor_estop(ids_broadcast)
#     # print("motor_estop(broadcast) OK")

# except Exception as e:
#     print("Error during broadcast flow:", e)
