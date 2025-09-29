"""fx_cli Python package

This package exposes the high level FX motor controller client to Python.
It re-exports the C++ class `FxCli` from the compiled module `fx_cli`.

Usage:

    import fx_cli
    cli = fx_cli.FxCli("192.168.10.10", 5101)
    cli.motor_start([1, 2])
    ...

"""

from .fx_cli import FxCli

__all__ = ["FxCli"]
