import fx_cli
import time
"""
Example usage of the FX CLI Python bindings.

This script demonstrates how to send high level commands to the FX motor
controller using JSON-like inputs and receive structured responses over
the text-based AT command protocol via the `fx_cli` module.
"""

# Create a client; you can override the defaults by passing ip/port (port is int).
cli = fx_cli.FxCli()

# 1) 특정 ID 제어 예시
ids_specific = [1, 3, 2, 4]

try:
    result = cli.motor_setzero(ids_specific)
    print(result)

    # Motor Stop
    if not cli.motor_stop(ids_specific):
        print("Motor stop failed")
    else:
        print("Motor stop succeeded")
except Exception as e:
    print("Error:", e)

finally:
    # Motor Stop
    if not cli.motor_stop(ids_specific):
        print("Motor stop failed")
    else:
        print("Motor stop succeeded")