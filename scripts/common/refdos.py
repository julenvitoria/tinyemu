"""A minimal DOS/BIOS for the reference emulator.

`refx86` stops at the first software interrupt, which is far enough to validate
a self-unpacking program but stops exactly where our own HLE layer starts
mattering. This services the calls a DOS utility actually makes, so the
reference can run to completion and say what the program SHOULD do -- which
answers a question no amount of reading our x86_dos.c can: is the guest going
wrong because of an answer we gave, or one we did not give?

It is deliberately independent of our implementation. Mirroring x86_dos.c would
make the two agree on the same mistake.

Console output is captured two ways, because programs use both: text written
through INT 21h/INT 10h is appended to `.out`, and writes the program makes
directly to the B800 text buffer are read back out of memory by `screen_text`.
"""

import struct

VIDEO_SEG = 0xB800
TEXT_COLS, TEXT_ROWS = 80, 25


class RefDos:
    def __init__(self, uc, psp=0x71):
        self.uc = uc
        self.psp = psp
        self.out = bytearray()
        self.ticks = 0
        self.exited = False
        self.exit_code = None
        self.cursor = (0, 0)
        self.mode = 3
        self.unhandled = []
        self.calls = []

    # -- register helpers ------------------------------------------------
    def _r(self, name):
        from unicorn import x86_const as C
        return self.uc.reg_read(getattr(C, "UC_X86_REG_" + name))

    def _w(self, name, v):
        from unicorn import x86_const as C
        self.uc.reg_write(getattr(C, "UC_X86_REG_" + name), v & 0xFFFF)

    def _flags(self, cf):
        from unicorn import x86_const as C
        fl = self.uc.reg_read(C.UC_X86_REG_EFLAGS)
        fl = (fl | 1) if cf else (fl & ~1)
        self.uc.reg_write(C.UC_X86_REG_EFLAGS, fl)

    def _lin(self, seg, off):
        return ((seg << 4) + off) & 0xFFFFF

    def _emit(self, b):
        self.out += bytes([b])

    # -- the vectored services -------------------------------------------
    def service(self, vec):
        ah = (self._r("AX") >> 8) & 0xFF
        al = self._r("AX") & 0xFF
        self.calls.append((vec, ah))
        fn = getattr(self, f"int{vec:02x}", None)
        if fn is None or fn(ah, al) is False:
            self.unhandled.append((vec, ah))
        return not self.exited

    def int10(self, ah, al):
        if ah == 0x00:
            self.mode = al
        elif ah == 0x02:
            self.cursor = ((self._r("DX") >> 8) & 0xFF, self._r("DX") & 0xFF)
        elif ah == 0x03:
            self._w("CX", 0x0607)
            self._w("DX", (self.cursor[0] << 8) | self.cursor[1])
        elif ah == 0x0F:
            self._w("AX", (TEXT_COLS << 8) | self.mode)
            self._w("BX", 0)
        elif ah == 0x0E:
            self._emit(al)
        elif ah in (0x09, 0x0A):
            for _ in range(max(1, self._r("CX"))):
                self._emit(al)
        elif ah == 0x08:
            self._w("AX", 0x0720)
        elif ah == 0x06:
            self._emit(0x0A)
        elif ah == 0x11:
            # A CGA BIOS has no AH=11h. Answer AL=30h anyway, pointing at the
            # ROM font, because that is what our BIOS does and the point of
            # this shim is to test the guest, not to re-litigate the BIOS.
            if al == 0x30:
                self._w("ES", 0xF000)
                self._w("BP", 0xF000)
                self._w("CX", 8)
                self._w("DX", (self._r("DX") & 0xFF00) | 24)
            else:
                return False
        elif ah in (0x01, 0x05, 0x12, 0x1A):
            pass
        else:
            return False
        return True

    def int16(self, ah, al):
        if ah in (0x00, 0x10):
            self._w("AX", 0x1C0D)          # Enter, so a "press a key" returns
        elif ah in (0x01, 0x11):
            self._flags(0)                 # ZF handling is not modelled
        elif ah == 0x02:
            self._w("AX", self._r("AX") & 0xFF00)
        else:
            return False
        return True

    def int1a(self, ah, al):
        if ah == 0x00:
            self.ticks += 1
            self._w("CX", (self.ticks >> 16) & 0xFFFF)
            self._w("DX", self.ticks & 0xFFFF)
            self._w("AX", self._r("AX") & 0xFF00)
        else:
            return False
        return True

    def int20(self, ah, al):
        self.exited, self.exit_code = True, 0
        return True

    def int21(self, ah, al):
        uc = self.uc
        if ah in (0x00, 0x4C):
            self.exited, self.exit_code = True, al
        elif ah == 0x02:
            self._emit(self._r("DX") & 0xFF)
        elif ah == 0x09:
            addr = self._lin(self._r("DS"), self._r("DX"))
            while True:
                c = uc.mem_read(addr, 1)[0]
                if c == ord("$"):
                    break
                self._emit(c)
                addr += 1
        elif ah == 0x40:
            n = self._r("CX")
            data = uc.mem_read(self._lin(self._r("DS"), self._r("DX")), n)
            if self._r("BX") in (1, 2):
                self.out += bytes(data)
            self._w("AX", n)
            self._flags(0)
        elif ah == 0x30:
            self._w("AX", 0x0005)
            self._w("BX", 0)
            self._w("CX", 0)
        elif ah == 0x25:
            v = al * 4
            uc.mem_write(v, struct.pack("<HH", self._r("DX"), self._r("DS")))
        elif ah == 0x35:
            off, seg = struct.unpack("<HH", uc.mem_read(al * 4, 4))
            self._w("BX", off)
            self._w("ES", seg)
        elif ah == 0x44:
            if al == 0x00:
                self._w("DX", 0x80D3 if self._r("BX") <= 2 else 0)
                self._w("AX", self._r("DX"))
            self._flags(0)
        elif ah == 0x2C:
            self._w("CX", 0x0C00)
            self._w("DX", 0)
        elif ah == 0x2A:
            self._w("CX", 1990)
            self._w("DX", 0x0101)
            self._w("AX", self._r("AX") & 0xFF00)
        elif ah == 0x19:
            self._w("AX", (self._r("AX") & 0xFF00) | 0)
        elif ah in (0x1A, 0x33, 0x0B, 0x0C):
            self._flags(0)
        else:
            self._flags(1)
            return False
        return True

    # -- what the program left on the screen ------------------------------
    def screen_text(self):
        base = VIDEO_SEG << 4
        raw = self.uc.mem_read(base, TEXT_COLS * TEXT_ROWS * 2)
        lines = []
        for r in range(TEXT_ROWS):
            row = "".join(
                chr(raw[(r * TEXT_COLS + c) * 2]) if 32 <= raw[(r * TEXT_COLS + c) * 2] < 127
                else " " for c in range(TEXT_COLS))
            lines.append(row.rstrip())
        while lines and not lines[-1]:
            lines.pop()
        return lines
