#!/usr/bin/env python3
"""
Per-handler assembly metrics for the Thumb-2 x86 core (user-facing CLI).

Logic lives in scripts/common/asm_metrics.py so other tooling can import it;
this is the entry point. See that module for what the tiers mean and why the
static baseline exists.

    python3 scripts/asm_metrics.py --update              # write the baseline
    python3 scripts/asm_metrics.py                       # compare against it
    python3 scripts/asm_metrics.py --top 20              # biggest handlers
    python3 scripts/asm_metrics.py --cycles --target hardware
"""
import os
import sys

sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
from scripts.common.asm_metrics import main   # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
