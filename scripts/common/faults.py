"""
Read the latched fault state out of the target.

Bare-metal firmware with no fault handlers wedges silently, and every harness
sees that as an indistinguishable timeout -- which is how an unconfigured
OCTOSPI hid behind a "benchmark did not complete" for the life of the project.
tests/main.c now latches the fault; this decodes it.
"""

# ARMv7-M CFSR: MMFSR[7:0] | BFSR[15:8] | UFSR[31:16]
_MMFSR = [(0x01, "IACCVIOL"), (0x02, "DACCVIOL"), (0x08, "MUNSTKERR"),
          (0x10, "MSTKERR"), (0x20, "MLSPERR"), (0x80, "MMARVALID")]
_BFSR = [(0x01, "IBUSERR"), (0x02, "PRECISERR"), (0x04, "IMPRECISERR"),
         (0x08, "UNSTKERR"), (0x10, "STKERR"), (0x20, "LSPERR"),
         (0x80, "BFARVALID")]
_UFSR = [(0x0001, "UNDEFINSTR"), (0x0002, "INVSTATE"), (0x0004, "INVPC"),
         (0x0008, "NOCP"), (0x0100, "UNALIGNED"), (0x0200, "DIVBYZERO")]

_VECTORS = {2: "NMI", 3: "HardFault", 4: "MemManage", 5: "BusFault",
            6: "UsageFault", 11: "SVCall", 12: "DebugMonitor",
            14: "PendSV", 15: "SysTick"}


def decode_cfsr(cfsr):
    bits = [n for m, n in _MMFSR if cfsr & m]
    bits += [n for m, n in _BFSR if (cfsr >> 8) & m]
    bits += [n for m, n in _UFSR if (cfsr >> 16) & m]
    return ", ".join(bits) or "none"


def check_fault(backend, syms, verbose=True):
    """Return True if the target is sitting in a latched fault."""
    if "fault_active" not in syms:
        return False
    active = backend.read_uint32(syms["fault_active"])
    if not active:
        return False
    if verbose:
        cfsr = backend.read_uint32(syms["fault_cfsr"])
        print("\n*** TARGET FAULTED ***")
        print(f"  vector : {active} ({_VECTORS.get(active, 'unknown')})")
        print(f"  CFSR   : 0x{cfsr:08x}  [{decode_cfsr(cfsr)}]")
        print(f"  HFSR   : 0x{backend.read_uint32(syms['fault_hfsr']):08x}")
        print(f"  BFAR   : 0x{backend.read_uint32(syms['fault_bfar']):08x}"
              "   <- faulting data address")
        pc = backend.read_uint32(syms["fault_pc"])
        lr = backend.read_uint32(syms["fault_lr"])
        print(f"  PC     : 0x{pc:08x} ({nearest_symbol(syms, pc)})")
        print(f"  LR     : 0x{lr:08x} ({nearest_symbol(syms, lr)})")
    return True


# nearest_symbol lives in elf.py -- one implementation, used by both the fault
# reporter and the target layer.
