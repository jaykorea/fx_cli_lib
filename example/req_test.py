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
ids_specific = [1, 2, 3, 4, 5, 6, 7, 8]

try:
    # START (ACK: OK <START ...>)
    if not cli.motor_start(ids_specific):
        print("Motor start failed")
    else:
        print("Motor start succeeded")

    # Operation Control
    mit_groups = [
        {"id": ids_specific[0], "pos": 0.0, "vel": 0.0, "kp": 0.0, "kd": 0.01, "tau": 0.0},
        {"id": ids_specific[1], "pos": 0.0, "vel": 0.0, "kp": 0.0, "kd": 0.01, "tau": 0.0},
        {"id": ids_specific[2], "pos": 0.0, "vel": 0.0, "kp": 0.0, "kd": 0.01, "tau": 0.0},
        {"id": ids_specific[3], "pos": 0.0, "vel": 0.0, "kp": 0.0, "kd": 0.01, "tau": 0.0},
        {"id": ids_specific[4], "pos": 0.0, "vel": 0.0, "kp": 0.0, "kd": 0.01, "tau": 0.0},
        {"id": ids_specific[5], "pos": 0.0, "vel": 0.0, "kp": 0.0, "kd": 0.01, "tau": 0.0},
        {"id": ids_specific[6], "pos": 0.0, "vel": 0.0, "kp": 0.0, "kd": 0.01, "tau": 0.0},
        {"id": ids_specific[7], "pos": 0.0, "vel": 0.0, "kp": 0.0, "kd": 0.01, "tau": 0.0},
    ]

    T = 3600
    dt = 0.02
    steps = int(T / dt)

    TIMEOUT = 10

    a=1

    # Get Obs
    for i in range(steps):
        # 연산 시작 시간 기록 (단위: 초)
        t0 = time.monotonic()

        # 실제 작업 수행
        bool_state = cli.operation_control(mit_groups)
        print(f"Step {i}: {bool_state}")
        state = cli.req(ids_specific)
        print(f"Step {i}: {state}")

        # 연산 종료 시간 기록 (단위: 초)
        t1 = time.monotonic()

        # 경과 시간을 초(second)에서 밀리초(millisecond)로 변환
        step_ms = (t1 - t0) * 1000


        # 10ms를 초과했는지 확인하고, 초과 시 RuntimeError 발생
        if step_ms > TIMEOUT:
            raise RuntimeError(f"오류: Step {i}번이 너무 오래 걸렸습니다. 소요 시간: {step_ms:.3f} ms (제한: {TIMEOUT} ms)")

        print(f"Step {i} 소요 시간: {step_ms:.3f} ms")

        # 다음 루프 전 잠시 대기
        time.sleep(dt)

    # Motor Stop
    if not cli.motor_stop(ids_specific):
        print("Motor stop failed")
    else:
        print("Motor stop succeeded")

except Exception as e:
    print("Error during specific-ID flow:", e)

finally:
    # Motor Stop
    if not cli.motor_stop(ids_specific):
        print("Motor stop failed")
    else:
        print("Motor stop succeeded")
