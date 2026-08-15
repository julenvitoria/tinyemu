#!/usr/bin/env python3
"""
Generate retro-go's global /CONFIG (user-facing CLI).

Preselecting a ROM here is what removes launcher navigation from automated
testing, and main_menu_timeout_s=0 stops idle standby cutting a run short.
Logic lives in scripts/common/retrogo_config.py.

    python3 scripts/make_retrogo_config.py --out build/CONFIG --rom dos ALLEYCAT.dsk
"""
import os
import sys

sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
from scripts.common.retrogo_config import main   # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
