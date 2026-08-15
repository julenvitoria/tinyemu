"""ELF symbol helpers, shared by every tier and both targets."""

import subprocess


def get_symbols(elf_file):
    """{name: address} for an ELF. Works for our core, the bare-metal harness,
    and the retro-go firmware alike."""
    res = subprocess.run(["arm-none-eabi-nm", elf_file],
                         capture_output=True, text=True)
    syms = {}
    for line in res.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3:
            try:
                syms[parts[2]] = int(parts[0], 16)
            except ValueError:
                pass
    return syms


def nearest_symbol(syms, addr):
    """'name+0xoff' for an address, for readable fault and PC reports."""
    best = None
    for name, a in syms.items():
        if a <= addr and (best is None or a > best[1]):
            best = (name, a)
    return f"{best[0]}+0x{addr - best[1]:x}" if best else "?"
