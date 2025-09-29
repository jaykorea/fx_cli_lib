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

    T = 5.0
    dt = 0.02
    steps = int(T / dt)

    # start = time.time()
    # for i in range(steps):
    #     cli.operation_control(mit_groups)
    #     time.sleep(dt)
    # end = time.time()

    # OBS
    start = time.time()
    for i in range(steps):
        state = cli.req(ids_specific)
        print(state)
        time.sleep(dt)
    end = time.time()

    # 진단 상태
    # start = time.time()
    # for i in range(steps):
    #     status = cli.status()
    #     print(status)
    #     time.sleep(dt)
    # end = time.time()

    # # STOP
    # if not cli.motor_stop(ids_specific):
    #     print("Motor stop failed")
    # else:
    #     print("Motor stop succeeded")

except Exception as e:
    print("Error during specific-ID flow:", e)

# 2) 브로드캐스트 제어 예시
#   - 빈 리스트 [] 를 사용하면 ID 그룹이 "<>"로 전송됩니다.
#   - 펌웨어가 "<>"를 브로드캐스트로 해석한다는 전제.
# ids_broadcast = []

# try:
#     cli.motor_stop(ids_broadcast)
#     print("motor_stop(broadcast) OK")
#
#     # 필요 시 E-STOP 브로드캐스트
#     # cli.motor_estop(ids_broadcast)
#     # print("motor_estop(broadcast) OK")
#
# except Exception as e:
#     print("Error during broadcast flow:", e)
