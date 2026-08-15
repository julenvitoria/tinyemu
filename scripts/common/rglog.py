"""
Read retro-go's in-memory log ring buffer over any gnwmanager backend.

docs/04 §4: do NOT use `gnwmanager monitor` -- the device log is small and wraps
fast. Read it out of memory instead, which works identically under gwemu and on
silicon and needs no serial port.

Symbols come from the FIRMWARE elf (gw_retro_go.elf), not the core's.
"""

import subprocess


def firmware_symbols(elf):
    """{name: addr} from a firmware ELF."""
    out = subprocess.run(["arm-none-eabi-nm", elf],
                         capture_output=True, text=True).stdout
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            try:
                syms[parts[2]] = int(parts[0], 16)
            except ValueError:
                pass
    return syms


def read_log(backend, syms, max_bytes=16384):
    """Return the log as text. Reads log_idx bytes from logbuf."""
    if "logbuf" not in syms or "log_idx" not in syms:
        return None
    idx = backend.read_uint32(syms["log_idx"])
    if idx == 0:
        return ""
    n = min(idx, max_bytes)
    data = backend.read_memory(syms["logbuf"], n)
    return bytes(data).decode("utf-8", errors="replace")


class LogDrainer:
    """Incremental reader for the log ring, for use while a run is in flight.

    Reading the whole buffer every sample and diffing the text is wrong once the
    ring wraps. This tracks log_idx and reads only what was appended since the
    last call, so a long run streams instead of re-printing.

    Exists because probe.py used to run gwemu without ever reading this buffer:
    a guest hardfault was sitting in the log, unread, while the sampler happily
    printed PC values. Draining is not optional instrumentation -- it is the
    only channel the firmware has to say something went wrong.
    """

    def __init__(self, fw_elf, prefix="  | "):
        self.syms = firmware_symbols(fw_elf) if fw_elf else {}
        self.prefix = prefix
        self.pos = 0
        self.available = "logbuf" in self.syms and "log_idx" in self.syms

    def drain(self, backend, out=print):
        """Print and return whatever was appended since the last drain."""
        if not self.available:
            return ""
        idx = backend.read_uint32(self.syms["log_idx"])
        if idx < self.pos:          # ring wrapped: restart from the top
            self.pos = 0
        if idx <= self.pos:
            return ""
        data = backend.read_memory(self.syms["logbuf"] + self.pos,
                                   idx - self.pos)
        self.pos = idx
        text = bytes(data).decode("utf-8", errors="replace")
        for ln in text.splitlines():
            out(self.prefix + ln)
        return text


def dump_log(backend, elf, prefix="  | ", tail=None):
    """Print the firmware log; returns the text. tail=N prints last N lines."""
    syms = firmware_symbols(elf)
    text = read_log(backend, syms)
    if text is None:
        print("no logbuf/log_idx in firmware ELF -- wrong build?")
        return None
    lines = text.splitlines()
    if tail:
        lines = lines[-tail:]
    print(f"--- retro-go log ({len(text)} bytes) ---")
    for ln in lines:
        print(prefix + ln)
    return text
