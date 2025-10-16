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

try:
    # MCU PING Test
    result = cli.mcu_ping()
    print(result)

    # MCU Identify Test
    result = cli.mcu_whoami()
    print(result)
except Exception as e:
    print("Error:", e)