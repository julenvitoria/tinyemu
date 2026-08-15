"""
Preflight: does this firmware actually accept this core?

The firmware refuses to load a core when
    required_abi_version  > GW_FIRMWARE_ABI_VERSION      or
    required_abi_min_size > sizeof(g_firmware_abi)
and it does so SILENTLY -- no log line, the core simply never appears in the
launcher. That failure is indistinguishable from a bad SD layout, a bad CONFIG,
or broken navigation, and it cost a full debugging round here.

Root cause when it happened: the SDK vendored at sdk/ was newer than the built
firmware binary (identical to the firmware SOURCE, but the .elf on disk was two
days older), so the ABI struct had grown 524 -> 548 bytes underneath it.

Check it up front and say so plainly.
"""

import struct
import subprocess

CORE_MAGIC = b"CORE"


def core_requirements(packed_bin):
    """(required_abi_version, required_abi_min_size) from a packed .bin.

    Layout (sdk/tools/pack_core.py): "CORE" + header_version(u16) +
    header_length(u16) + header_data, where header_data begins with
    gnw_core_meta_t: required_abi_version(u32), required_abi_min_size(u32).
    """
    with open(packed_bin, "rb") as fh:
        head = fh.read(16)
    if head[:4] != CORE_MAGIC:
        raise ValueError(f"{packed_bin} is not a packed core (magic {head[:4]!r})")
    hdr_ver, hdr_len = struct.unpack_from("<HH", head, 4)
    abi_ver, abi_min = struct.unpack_from("<II", head, 8)
    return abi_ver, abi_min, hdr_ver, hdr_len


def firmware_abi_size(firmware_elf):
    """sizeof(g_firmware_abi) from the firmware ELF, or None."""
    out = subprocess.run(["arm-none-eabi-nm", "-S", firmware_elf],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[3] == "g_firmware_abi":
            return int(parts[1], 16)
    return None


def check(packed_bin, firmware_elf, verbose=True):
    """True if the firmware will accept the core. Explains itself if not."""
    abi_ver, abi_min, _, _ = core_requirements(packed_bin)
    fw_size = firmware_abi_size(firmware_elf)
    if fw_size is None:
        if verbose:
            print(f"WARNING: no g_firmware_abi in {firmware_elf}; cannot preflight")
        return True

    ok = abi_min <= fw_size
    if verbose:
        print(f"ABI preflight: core needs v{abi_ver} / {abi_min} bytes; "
              f"firmware provides {fw_size} bytes -- {'OK' if ok else 'REFUSED'}")
    if not ok and verbose:
        print("\n  The firmware will silently ignore this core: it asks for more\n"
              "  ABI than the firmware binary provides. The binary is older than\n"
              "  the SDK vendored at sdk/. Rebuild the firmware from its current\n"
              "  source, or re-vendor an SDK matching the firmware:\n"
              "      scripts/sync_from_firmware.sh <firmware-checkout>\n"
              "  and update SDK_VERSION.\n")
    return ok
