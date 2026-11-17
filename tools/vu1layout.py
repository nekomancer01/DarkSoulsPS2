"""The VU1 memory contract (tools/vu1/layout.py) for the exporters that write VIF packets: `from vu1layout import L`.
Polaris 1.5: the exporters, gen.py and ps2/proto/src/vu1_layout.h read one table."""
import importlib.util
import os

_spec = importlib.util.spec_from_file_location(
    "vu1_layout", os.path.join(os.path.dirname(os.path.abspath(__file__)), "vu1", "layout.py"))
L = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(L)
