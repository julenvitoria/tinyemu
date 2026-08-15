#!/usr/bin/env python3

import os
import re
import sys
import subprocess
import time
import socket
import argparse
import struct

# Add the project root to sys.path to allow importing scripts.common
sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))
from scripts.common.runner import build_and_create_runner
from scripts.common.elf import get_symbols
from scripts.common.faults import check_fault

# x86 EFLAGS bit positions, for the flags_expected field on a test case.
# Offset of each segment's cached base within the CPU struct. Segments start at
# 48 with a 12-byte stride (sel 0, flags 2, base 4, limit 8) in x86 encoding
# order. Mirrors x86_offsets.inc, which is generated from the C struct.
SEG_BASE_OFFSETS = {
    "ES": 48 + 4, "CS": 60 + 4, "SS": 72 + 4,
    "DS": 84 + 4, "FS": 96 + 4, "GS": 108 + 4,
}

# CS.flags lives at segs[1]+2. The interpreter does a 32-bit load at SEG_CS_SEL
# (60) and tests bit 22, which is bit 6 of that halfword: DESC_D_MASK.
SEG_ORDER = ("ES", "CS", "SS", "DS", "FS", "GS")
CS_FLAGS_OFFSET = 60 + 2
# CR0 was never reset between cases either; bit 0 (PE) changes what a segment
# load means, so a case that sets it would silently alter every later one.
def _generated_offsets(path="x86_offsets.inc"):
    """Read the offsets the build generates from the C structs.

    Mirroring them by hand is how the CS 48/60 drift happened; there is one
    source of truth and it is this file."""
    out = {}
    for line in open(path):
        m = re.match(r"\s*\.equ\s+(\w+)\s*,\s*(\d+)", line)
        if m:
            out[m.group(1)] = int(m.group(2))
    return out


OFF = _generated_offsets()
CR0_OFFSET = OFF["CPU_CR0"]
EXIT_INFO_OFFSET = OFF["CPU_EXIT_INFO"]
EXIT_REASON_OFFSET = OFF["CPU_EXIT_REASON"]
HLE_ENABLED_OFFSET = OFF["CPU_HLE_ENABLED"]
IO_KBD_QUEUE = OFF["IO_KBD_QUEUE"]
IO_KBD_HEAD = OFF["IO_KBD_HEAD"]
PIT_COUNTER_OFFSET = OFF["CPU_PIT_COUNTER"]
HARD_INT_OFFSET = OFF["CPU_HARD_INT_PENDING"]
CS_FLAG_D = 1 << 6

FLAG_BITS = {
    "CF": 0, "PF": 2, "AF": 4, "ZF": 6, "SF": 7,
    "TF": 8, "IF": 9, "DF": 10, "OF": 11,
}


PIT_HZ = 1193182


def read_io_state(backend, addr):
    """Mirror of X86IOState. Field order is fixed by the struct in x86_cpu.h."""
    raw = bytes(backend.read_memory(addr, 12))
    reload_ = int.from_bytes(raw[0:2], "little")
    port61 = raw[4]
    if (port61 & 3) != 3:
        hz = 0
    else:
        hz = PIT_HZ // (reload_ if reload_ else 65536)
    return {
        "pit2_reload": reload_,
        "pit2_access": raw[2],
        "port61": port61,
        "port61_low2": port61 & 3,
        "speaker_hz": hz,
    }


def get_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(('', 0))
        return s.getsockname()[1]

def run_test(args):
    runner = build_and_create_runner(args)
    runner.start()
    
    backend = runner.backend
    
    syms = get_symbols("test_core.elf")
    dtcm_addr = syms.get("cpu_state_dtcm")
    ram_addr = syms.get("test_ram")
    run_flag_addr = syms.get("run_flag")
    page_table_addr = syms.get("guest_page_table")
    io_addr = syms.get("x86_io")
    
    if not all([dtcm_addr, ram_addr, run_flag_addr, page_table_addr, io_addr]):
        print("Failed to find necessary symbols in ELF!")
        return False
        
    print(f"Symbols -> cpu_state_dtcm: 0x{dtcm_addr:08x}, test_ram: 0x{ram_addr:08x}, run_flag: 0x{run_flag_addr:08x}")
    
    # Wait for C firmware init to complete (guest_page_table[0] will be set to test_ram)
    deadline = 50  # 50 * 0.1s = 5 seconds max
    while deadline > 0:
        pt0 = backend.read_uint32(page_table_addr)
        if pt0 == ram_addr:
            break
        time.sleep(0.1)
        deadline -= 1
    if deadline == 0:
        print(f"WARNING: guest_page_table[0]={pt0:#010x}, expected {ram_addr:#010x} -- firmware may not have initialized!")

    
    try:
        
        test_cases = [
            {
                "name": "NOP + INC EAX",
                "code": bytes([0x90, 0x40, 0xF4]),
                "regs_initial": {0: 5},
                "regs_expected": {0: 6},
                "expected_eip": 3,
            },
            {
                "name": "INC ECX (0x41)",
                "code": bytes([0x41, 0xF4]),
                "regs_initial": {1: 10},
                "regs_expected": {1: 11},
                "expected_eip": 2,
            },
            {
                "name": "DEC EBX (0x4B)",
                "code": bytes([0x4B, 0xF4]),
                "regs_initial": {3: 42},
                "regs_expected": {3: 41},
                "expected_eip": 2,
            },
            {
                "name": "INC EDI (0x47) x 3",
                "code": bytes([0x47, 0x47, 0x47, 0xF4]),
                "regs_initial": {7: 0},
                "regs_expected": {7: 3},
                "expected_eip": 4,
            },
            {
                "name": "MOV EDX, 0x11223344",
                "code": bytes([0xBA, 0x44, 0x33, 0x22, 0x11, 0xF4]), # 0xBA = MOV EDX
                "regs_initial": {2: 0},
                "regs_expected": {2: 0x11223344},
                "expected_eip": 6,
            },
            {
                "name": "PUSH EAX",
                "code": bytes([0x50, 0x5B, 0xF4]),
                "regs_initial": {0: 0xDEADBEEF, 3: 0, 4: 1000}, # ESP=1000
                "regs_expected": {0: 0xDEADBEEF, 3: 0xDEADBEEF, 4: 1000},
                "expected_eip": 3,
            },
            {
                "name": "ADD ECX, EDX (0x01)",
                "code": bytes([0x01, 0xD1, 0xF4]),
                "regs_initial": {1: 10, 2: 25}, # ECX=10, EDX=25
                "regs_expected": {1: 35, 2: 25}, # ECX=35, EDX=25
                "expected_eip": 3,
            },
            {
                "name": "ADD EAX, EBX (0x03)",
                "code": bytes([0x03, 0xC3, 0xF4]),
                "regs_initial": {0: 100, 3: 50}, # EAX=100, EBX=50
                "regs_expected": {0: 150, 3: 50}, # EAX=150, EBX=50
                "expected_eip": 3,
            },
            {
                "name": "OR ESI, EDI (0x09)",
                "code": bytes([0x09, 0xFE, 0xF4]), # ModR/M=11 111 110 = 0xFE (Dest=ESI, Src=EDI)
                "regs_initial": {6: 0x1010, 7: 0x0101},
                "regs_expected": {6: 0x1111, 7: 0x0101},
                "expected_eip": 3,
            },
            {
                "name": "AND EBP, EAX (0x23)",
                "code": bytes([0x23, 0xE8, 0xF4]), # ModR/M=11 101 000 = 0xE8 (Dest=EBP, Src=EAX)
                "regs_initial": {5: 0xF4FF0000, 0: 0xF400FF00},
                "regs_expected": {5: 0xF4000000, 0: 0xF400FF00},
                "expected_eip": 3,
            },
            {
                "name": "SUB EBX, ECX (0x2B)",
                "code": bytes([0x2B, 0xD9, 0xF4]), # ModR/M=11 011 001 = 0xD9 (Dest=EBX, Src=ECX)
                "regs_initial": {3: 100, 1: 30},
                "regs_expected": {3: 70, 1: 30},
                "expected_eip": 3,
            },
            {
                "name": "XOR EDX, EDX (0x33)",
                "code": bytes([0x33, 0xD2, 0xF4]), # ModR/M=11 010 010 = 0xD2 (Dest=EDX, Src=EDX)
                "regs_initial": {2: 0xBEEF},
                "regs_expected": {2: 0},
                "expected_eip": 3,
            },
            {
                "name": "ADD EAX, 0x1000 (0x05)",
                "code": bytes([0x05, 0x00, 0x10, 0x00, 0x00, 0xF4]),
                "regs_initial": {0: 0x500},
                "regs_expected": {0: 0x1500},
                "expected_eip": 6,
            },
            {
                "name": "ADD EAX, 0x1000 (0x05)",
                "code": bytes([0x05, 0x00, 0x10, 0x00, 0x00, 0xF4]),
                "regs_initial": {0: 0x500},
                "regs_expected": {0: 0x1500},
                "expected_eip": 6,
            },
            {
                "name": "SUB EAX, 0x100 (0x2D)",
                "code": bytes([0x2D, 0x00, 0x01, 0x00, 0x00, 0xF4]),
                "regs_initial": {0: 0x500},
                "regs_expected": {0: 0x400},
                "expected_eip": 6,
            },
            {
                "name": "ADD AL, 0x15 (0x04)",
                "code": bytes([0x04, 0x15, 0xF4]),
                "regs_initial": {0: 0x11223344},
                "regs_expected": {0: 0x11223359}, # 44 + 15 = 59
                "expected_eip": 3,
            },
            {
                "name": "ADD AL, BL (0x00 ModR/M=11 011 000 = 0xD8)",
                "code": bytes([0x00, 0xD8, 0xF4]),
                "regs_initial": {0: 0x100, 3: 0x55}, # AL=00, BL=55
                "regs_expected": {0: 0x155, 3: 0x55}, # AL=55
                "expected_eip": 3,
            },
            {
                "name": "OR CH, DH (0x0A ModR/M=11 101 110 = 0xEE)",
                "code": bytes([0x0A, 0xEE, 0xF4]), # Dest=CH (Reg=5), Src=DH (R/M=6). Mod=11.
                "regs_initial": {1: 0x1100, 2: 0x2200}, # CH=11, DH=22
                "regs_expected": {1: 0x3300, 2: 0x2200}, # CH=33
                "expected_eip": 3,
            },
            {
                "name": "ADD EBX, 0x1000 (0x81)",
                "code": bytes([0x81, 0xC3, 0x00, 0x10, 0x00, 0x00, 0xF4]), # ModR/M = 0xC3
                "regs_initial": {3: 0x500},
                "regs_expected": {3: 0x1500},
                "expected_eip": 7,
            },
            {
                "name": "AND EDX, 0xFFFFFFF4 (0x83 sign-extended)",
                "code": bytes([0x83, 0xE2, 0xF4, 0xF4]), # ModR/M = 0xE2, imm8 = 0xF4 (-12)
                "regs_initial": {2: 0x12345678},
                "regs_expected": {2: 0x12345670},
                "expected_eip": 4,
            },
            {
                "name": "SUB AL, 0x11 (0x80)",
                "code": bytes([0x80, 0xE8, 0x11, 0xF4]), # ModR/M = 0xE8, imm8 = 0x11
                "regs_initial": {0: 0x55},
                "regs_expected": {0: 0x44},
                "expected_eip": 4,
            },
            {
                "name": "TEST EAX, EBX (0x85)",
                "code": bytes([0x85, 0xD8, 0xF4]), # ModR/M = 11 011 000 (0xD8) -> EBX, EAX
                "regs_initial": {0: 0x5555, 3: 0xAAAA},
                "regs_expected": {0: 0x5555, 3: 0xAAAA}, # No writeback!
                "expected_eip": 3,
            },
            {
                "name": "XCHG ECX, EDX (0x87)",
                "code": bytes([0x87, 0xCA, 0xF4]), # ModR/M = 11 001 010 (0xCA) -> ECX, EDX
                "regs_initial": {1: 0x111, 2: 0x222},
                "regs_expected": {1: 0x222, 2: 0x111},
                "expected_eip": 3,
            },
            {
                "name": "MOV ESI, EDI (0x8B)",
                "code": bytes([0x8B, 0xF7, 0xF4]), # ModR/M = 11 110 111 (0xF7) -> ESI, EDI
                "regs_initial": {6: 0x111, 7: 0x999},
                "regs_expected": {6: 0x999, 7: 0x999}, # ESI = EDI
                "expected_eip": 3,
            },
            {
                "name": "MOV BL, BH (0x8A)",
                "code": bytes([0x8A, 0xDF, 0xF4]), # ModR/M = 11 011 111 (0xDF) -> BL(3), BH(7)
                "regs_initial": {3: 0x1234}, # BL=34, BH=12
                "regs_expected": {3: 0x1212}, # BL=12, BH=12
                "expected_eip": 3,
            },
            {
                "name": "XCHG EAX, EBX (0x93)",
                "code": bytes([0x93, 0xF4]),
                "regs_initial": {0: 0x111, 3: 0x333},
                "regs_expected": {0: 0x333, 3: 0x111},
                "expected_eip": 2,
            },
            {
                "name": "CWDE (0x98) - Positive",
                "code": bytes([0x98, 0xF4]),
                "regs_initial": {0: 0xDEAD0100}, # AX = 0x0100 -> Sign extend to 0x00000100
                "regs_expected": {0: 0x00000100},
                "expected_eip": 2,
            },
            {
                "name": "CWDE (0x98) - Negative",
                "code": bytes([0x98, 0xF4]),
                "regs_initial": {0: 0x00008000}, # AX = 0x8000 -> Sign extend to 0xFFFF8000
                "regs_expected": {0: 0xFFFF8000},
                "expected_eip": 2,
            },
            {
                "name": "CDQ (0x99) - Negative",
                "code": bytes([0x99, 0xF4]),
                "regs_initial": {0: 0x80000000, 2: 0}, # EAX is negative
                "regs_expected": {0: 0x80000000, 2: 0xFFFFFFFF}, # EDX becomes 0xFFFFFFFF
                "expected_eip": 2,
            },
            {
                "name": "POP EBX (0x8F)",
                "code": bytes([0x8F, 0xC3, 0xF4]), # ModR/M = 11 000 011 -> EBX
                "regs_initial": {3: 0},
                # We need to simulate memory since POP reads from memory. 
                # Let's set ESP to 0x1000 and write a value there in the test script, OR we can push first.
                # Let's just do PUSH EAX, POP EBX
            }
        ]
        
        # We need to fix the POP EBX test
        test_cases[-1]["code"] = bytes([0x50, 0x8F, 0xC3, 0xF4]) # PUSH EAX (0x50), POP EBX (0x8F 0xC3)
        test_cases[-1]["regs_initial"] = {0: 0x99887766, 3: 0, 4: 0x2000} # EAX=value, EBX=0, ESP=0x2000
        test_cases[-1]["regs_expected"] = {0: 0x99887766, 3: 0x99887766, 4: 0x2000} # ESP goes down then up
        test_cases[-1]["expected_eip"] = 4
        
        # PUSHFD and POPFD
        test_cases.append({
            "name": "PUSHFD / POPFD",
            # PUSHFD (0x9C), POPFD (0x9D)
            "code": bytes([0x9C, 0x9D, 0xF4]),
            "regs_initial": {4: 0x2000},
            "regs_expected": {4: 0x2000},
            "expected_eip": 3,
        })
        
        test_cases.extend([
            {
                "name": "MOV EAX, moffs32 (0xA1)",
                "code": bytes([0xA1, 0x00, 0x00, 0x00, 0x00, 0xF4]), # Read from guest addr 0x0
                "regs_initial": {0: 0},
                "regs_expected": {0: 0x000000A1}, # It reads the instruction bytes themselves! [A1 00 00 00]
                "expected_eip": 6,
            },
            {
                "name": "TEST EAX, imm32 (0xA9)",
                "code": bytes([0xA9, 0x11, 0x22, 0x33, 0x44, 0xF4]),
                "regs_initial": {0: 0x55555555},
                "regs_expected": {0: 0x55555555},
                "expected_eip": 6,
            },
            {
                "name": "MOV BL, imm8 (0xB3)",
                "code": bytes([0xB3, 0x99, 0xF4]),
                "regs_initial": {3: 0x11111111},
                "regs_expected": {3: 0x11111199},
                "expected_eip": 3,
            },
            {
                "name": "MOV CL, imm8 (0xC6)",
                "code": bytes([0xC6, 0xC1, 0x77, 0xF4]), # ModR/M = 11 000 001 -> CL
                "regs_initial": {1: 0x22222222},
                "regs_expected": {1: 0x22222277},
                "expected_eip": 4,
            },
            {
                "name": "MOV EDX, imm32 (0xC7)",
                "code": bytes([0xC7, 0xC2, 0xAA, 0xBB, 0xCC, 0xDD, 0xF4]), # ModR/M = 11 000 010 -> EDX
                "regs_initial": {2: 0},
                "regs_expected": {2: 0xDDCCBBAA},
                "expected_eip": 7,
            },
            {
                "name": "LEAVE (0xC9)",
                "code": bytes([0xC9, 0xF4]),
                # EBP is 0x100, ESP is 0x80. Stack at 0x100 has 0.
                "regs_initial": {4: 0x80, 5: 0x100},
                # ESP becomes EBP (0x100), then pops (reads 0 from memory) -> EBP=0
                # ESP becomes 0x104.
                "regs_expected": {4: 0x104, 5: 0},
                "expected_eip": 2,
            },
            {
                "name": "RET (0xC3)",
                "code": bytes([0xB8, 0x07, 0x00, 0x00, 0x00, 0x50, 0xC3, 0xF4]), # MOV EAX, 7; PUSH EAX; RET -> Jumps to 7
                "regs_initial": {0: 0xFFFFFFFF, 4: 0x104},
                "regs_expected": {0: 7, 4: 0x104},
                "expected_eip": 8,
            },
            {
                "name": "RET imm16 (0xC2)",
                "code": bytes([0xB8, 0x09, 0x00, 0x00, 0x00, 0x50, 0xC2, 0x10, 0x00, 0xF4]), # MOV EAX, 9; PUSH EAX; RET 0x10
                "regs_initial": {0: 0xFFFFFFFF, 4: 0x104},
                "regs_expected": {0: 9, 4: 0x114}, # ESP goes to 0x100, pops 9, adds 0x10 -> 0x114
                "expected_eip": 10,
            },
            {
                "name": "LEA EAX, [EBX + 0x12] (disp8)",
                "code": bytes([0x8D, 0x43, 0x12, 0xF4]), # 0x8D, Mod=01, Reg=000, R/M=011 -> 0x43
                "regs_initial": {0: 0, 3: 0x1000},
                "regs_expected": {0: 0x1012, 3: 0x1000},
                "expected_eip": 4,
            },
            {
                "name": "LEA ECX, [EDI + ESI*4 + 0x11223344] (SIB + disp32)",
                # 0x8D
                # ModR/M = 10 001 100 (0x8C) -> Reg=ECX, R/M=SIB, Mod=disp32
                # SIB = 10 110 111 (0xB7) -> Scale=4, Index=ESI(6), Base=EDI(7)
                # Disp32 = 0x11223344
                "code": bytes([0x8D, 0x8C, 0xB7, 0x44, 0x33, 0x22, 0x11, 0xF4]),
                "regs_initial": {1: 0, 6: 0x10, 7: 0x100},
                # EA = EDI + ESI*4 + Disp32 = 0x100 + 0x40 + 0x11223344 = 0x11223484
                "regs_expected": {1: 0x11223484},
                "expected_eip": 8,
            },
            {
                "name": "LEA EDX, [disp32] (Mod=00, R/M=5)",
                # 0x8D
                # ModR/M = 00 010 101 (0x15) -> Reg=EDX, R/M=5
                # Disp32 = 0x55555555
                "code": bytes([0x8D, 0x15, 0x55, 0x55, 0x55, 0x55, 0xF4]),
                "regs_initial": {2: 0},
                "regs_expected": {2: 0x55555555},
                "expected_eip": 7,
            },
            {
                "name": "ADD [EBX + 4], ECX (Memory Ev, Gv)",
                # 0x01
                # ModR/M = 01 001 011 (0x4B) -> Reg=ECX, R/M=EBX, Mod=disp8
                # disp8 = 4
                "code": bytes([0x01, 0x4B, 0x04, 0xF4]),
                "regs_initial": {1: 0x11223344, 3: 0},
                "regs_expected": {1: 0x11223344, 3: 0},
                # It should write to [4]! We can't easily check memory without modifying test script, but EIP=4 means it didn't crash.
                "expected_eip": 4,
            },
            {
                "name": "CLD, STOSD",
                "code": bytes([0xFC, 0xAB, 0xF4]), # CLD (0xFC), STOSD (0xAB)
                "regs_initial": {0: 0xDEADBEEF, 7: 0x100}, # EDI = 0x100
                "regs_expected": {0: 0xDEADBEEF, 7: 0x104}, # EDI should increment by 4
                "expected_eip": 3,
            },
            {
                "name": "STD, LODSD",
                "code": bytes([0xFD, 0xAD, 0xF4]), # STD (0xFD), LODSD (0xAD)
                "regs_initial": {6: 0x100}, # ESI = 0x100
                "regs_expected": {6: 0x0FC}, # ESI should decrement by 4
                "expected_eip": 3,
            },
            {
                "name": "CLD, MOVSB",
                "code": bytes([0xFC, 0xA4, 0xF4]), # CLD (0xFC), MOVSB (0xA4)
                "regs_initial": {6: 0x100, 7: 0x200}, # ESI = 0x100, EDI = 0x200
                "regs_expected": {6: 0x101, 7: 0x201}, # Both should increment by 1
                "expected_eip": 3,
            },
            {
                "name": "SHL EAX, 1 (0xD1)",
                # 0xD1, Mod=11, Reg=4 (SHL), R/M=0 (EAX) -> 0xE0
                "code": bytes([0xD1, 0xE0, 0xF4]),
                "regs_initial": {0: 1},
                "regs_expected": {0: 2},
                "expected_eip": 3,
            },
            {
                "name": "SHR EBX, 4 (0xC1)",
                # 0xC1, Mod=11, Reg=5 (SHR), R/M=3 (EBX) -> 0xEB
                # imm8 = 4
                "code": bytes([0xC1, 0xEB, 0x04, 0xF4]),
                "regs_initial": {3: 0x40},
                "regs_expected": {3: 4},
                "expected_eip": 4,
            },
            {
                "name": "SAR ECX, CL (0xD3)",
                # 0xD3, Mod=11, Reg=7 (SAR), R/M=1 (ECX) -> 0xF9
                "code": bytes([0xD3, 0xF9, 0xF4]),
                "regs_initial": {1: 0x80000002}, # Negative number, CL=2
                "regs_expected": {1: 0xE0000000}, # Arithmetic shift right by 2 -> 0xE0000000
                "expected_eip": 3,
            },
            {
                "name": "SHR AL, 2 (0xC0)",
                # 0xC0, Mod=11, Reg=5 (SHR), R/M=0 (AL) -> 0xE8, imm8=2
                "code": bytes([0xC0, 0xE8, 0x02, 0xF4]),
                "regs_initial": {0: 0xFF}, # AL = 0xFF
                "regs_expected": {0: 0x3F}, # 0xFF >> 2 = 0x3F
                "expected_eip": 4,
            },
            {
                "name": "ROL BL, 1 (0xD0)",
                # 0xD0, Mod=11, Reg=0 (ROL), R/M=3 (BL) -> 0xC3
                "code": bytes([0xD0, 0xC3, 0xF4]),
                "regs_initial": {3: 0x81}, # 1000 0001
                "regs_expected": {3: 0x03}, # 0000 0011
                "expected_eip": 3,
            },
            {
                "name": "JMP rel8 (0xEB)",
                "code": bytes([0xEB, 0x05, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0x90, 0xF4]),
                "regs_initial": {},
                "regs_expected": {},
                "expected_eip": 9, # 0 + 2 + 5 (rel8) + 1 (NOP) + 1 (term) = 9
            },
            {
                "name": "CALL rel32 (0xE8)",
                "code": bytes([0xE8, 0x04, 0x00, 0x00, 0x00, 0xF4, 0xF4, 0xF4, 0x90, 0xF4]),
                "regs_initial": {4: 0x104},
                "regs_expected": {4: 0x100},
                "expected_eip": 10, # 0 + 5 + 4 + 1 = 10
            },
            {
                "name": "INC [EBX] (0xFE Group 4)",
                # 0xFE, Mod=00, Reg=0 (INC), R/M=3 (EBX) -> 0x03
                "code": bytes([0xFE, 0x03, 0xF4]),
                "regs_initial": {3: 0x1000},
                "regs_expected": {3: 0x1000},
                "expected_eip": 3,
            },
            {
                "name": "DEC EAX (0xFF Group 5)",
                # 0xFF, Mod=11, Reg=1 (DEC), R/M=0 (EAX) -> 0xC8
                "code": bytes([0xFF, 0xC8, 0xF4]),
                "regs_initial": {0: 0x500},
                "regs_expected": {0: 0x4FF},
                "expected_eip": 3,
            },
            {
                "name": "JZ rel8 (0x74) - Not Taken (ZF=0)",
                "code": bytes([0x74, 0x05, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0x90, 0xF4]),
                "regs_initial": {},
                "regs_expected": {},
                "expected_eip": 3, # 0 + 2 (rel8) + 1 (term) = 3
            },
            {
                "name": "JNZ rel8 (0x75) - Taken (ZF=0)",
                "code": bytes([0x75, 0x05, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0x90, 0xF4]),
                "regs_initial": {},
                "regs_expected": {},
                "expected_eip": 9, # 0 + 2 + 5 + 1 (NOP) + 1 (term) = 9
            },
            {
                "name": "PUSHA / POPA (0x60 / 0x61)",
                # 0x60 = PUSHA, 0x61 = POPA
                # EAX=1, ECX=2, EDX=3, EBX=4, ESP=0x1000, EBP=6, ESI=7, EDI=8
                # We can PUSHA, clear registers, then POPA to see if they come back!
                "code": bytes([0x60, 0x33, 0xC0, 0x33, 0xC9, 0x61, 0xF4]), # PUSHA (1), XOR EAX, EAX (2), XOR ECX, ECX (2), POPA (1), HLT (1)
                "regs_initial": {0: 1, 1: 2, 2: 3, 3: 4, 4: 0x1000, 5: 6, 6: 7, 7: 8},
                "regs_expected": {0: 1, 1: 2, 2: 3, 3: 4, 4: 0x1000, 5: 6, 6: 7, 7: 8},
                "expected_eip": 7,
            },
            {
                "name": "PUSH imm32 (0x68)",
                "code": bytes([0x68, 0x78, 0x56, 0x34, 0x12, 0x58, 0xF4]), # PUSH 0x12345678 (5), POP EAX (1), HLT (1)
                "regs_initial": {0: 0, 4: 0x1000},
                "regs_expected": {0: 0x12345678, 4: 0x1000},
                "expected_eip": 7,
            },
            {
                "name": "IMUL EAX, EBX, imm32 (0x69)",
                # 0x69, Mod=11, Reg=0 (EAX), R/M=3 (EBX) -> 0xC3
                "code": bytes([0x69, 0xC3, 0x03, 0x00, 0x00, 0x00, 0xF4]), # IMUL EAX, EBX, 3
                "regs_initial": {0: 0, 3: 0x1000},
                "regs_expected": {0: 0x3000, 3: 0x1000},
                "expected_eip": 7,
            },
            {
                "name": "ENTER 0x10, 0 (0xC8)",
                "code": bytes([0xC8, 0x10, 0x00, 0x00, 0xF4]),
                "regs_initial": {4: 0x1000, 5: 0x2000},
                "regs_expected": {4: 0x0FEC, 5: 0x0FFC}, # ESP=0xFFC, pushes 0x2000, then ESP -= 0x10 = 0xFEC
                "expected_eip": 5,
            },
            {
                "name": "RETF (0xCB)",
                # push CS=0, push IP=0x0B (the 0xF4), then RETF.
                # CS was 0x0023 here originally, which only worked because RETF
                # popped CS and discarded it. It now installs CS for real, so a
                # non-zero selector moves CS.base and the linear EIP with it --
                # see the next case.
                "code": bytes([0x68, 0x00, 0x00, 0x00, 0x00, 0x68, 0x0B, 0x00, 0x00, 0x00, 0xCB, 0xF4]),
                "regs_initial": {4: 0x1000},
                "regs_expected": {4: 0x1000},
                "expected_eip": 12, # Jump to 11, fetch 0xF4, EIP = 12
            },
            {
                "name": "RETF installs CS, moving CS.base",
                # The stack is planted directly: IP=0x0002 then CS=0x0020 above
                # it. CS.base becomes 0x200, so execution resumes at linear
                # 0x202 where a HLT sits. Before the fix RETF discarded CS and
                # carried on in the caller's segment.
                "code": bytes([0xCB]),
                "mode": 16,
                "segs_initial": {"SS": 0x0000},
                "ram_setup": [(0x1000, bytes([0x02, 0x00, 0x20, 0x00])),
                              (0x202, bytes([0xF4]))],
                "regs_initial": {4: 0x1000},
                "regs_expected": {4: 0x1004},
                "expected_eip": 3,   # IP 2 -> fetch HLT -> IP 3
            },
            {
                "name": "AAM (0xD4)",
                "code": bytes([0xD4, 0x0A, 0xF4]), # AL = AL % 10, AH = AL / 10
                "regs_initial": {0: 0x17}, # AL = 23
                "regs_expected": {0: 0x0203}, # AH = 2, AL = 3
                "expected_eip": 3,
            },
            {
                "name": "AAD (0xD5)",
                "code": bytes([0xD5, 0x0A, 0xF4]), # AL = AH * 10 + AL, AH = 0
                "regs_initial": {0: 0x0203}, # AH = 2, AL = 3
                "regs_expected": {0: 0x17}, # AL = 23, AH = 0
                "expected_eip": 3,
            },
            {
                "name": "XLAT (0xD7)",
                "code": bytes([0xD7, 0xF4]), # AL = [EBX + AL]
                "regs_initial": {3: 0, 0: 0}, # EBX = 0, AL = 0
                "regs_expected": {3: 0, 0: 0xD7}, # It reads the instruction byte at 0 (0xD7)
                "expected_eip": 2,
            },
            {
                "name": "LOOP (0xE2) - Taken",
                "code": bytes([0xE2, 0x05, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0x90, 0xF4]),
                "regs_initial": {1: 2}, # ECX = 2
                "regs_expected": {1: 1}, # ECX = 1
                "expected_eip": 9,
            },
            {
                "name": "LOOP (0xE2) - Not Taken",
                "code": bytes([0xE2, 0x05, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0x90, 0xF4]),
                "regs_initial": {1: 1}, # ECX = 1
                "regs_expected": {1: 0}, # ECX = 0
                "expected_eip": 3,
            },
            {
                "name": "JCXZ (0xE3) - Taken",
                "code": bytes([0xE3, 0x05, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0x90, 0xF4]),
                "regs_initial": {1: 0}, # ECX = 0
                "regs_expected": {1: 0}, # ECX = 0
                "expected_eip": 9,
            },
            {
                "name": "STC / CLC / CMC (0xF9, 0xF8, 0xF5)",
                # STC (F9) -> CF=1
                # CMC (F5) -> CF=0
                # CLC (F8) -> CF=0
                "code": bytes([0xF9, 0xF5, 0xF8, 0xF4]),
                "regs_initial": {},
                "regs_expected": {},
                "expected_eip": 4, # Just check if they execute without crashing since we don't mock EFLAGS directly
            },
            {
                "name": "STI / CLI / LOCK (0xFB, 0xFA, 0xF0)",
                "code": bytes([0xFB, 0xFA, 0xF0, 0x90, 0xF4]),
                "regs_initial": {},
                "regs_expected": {},
                "expected_eip": 5,
            },
            {
                "name": "SAHF / LAHF (0x9E, 0x9F)",
                "code": bytes([0x9E, 0x9F, 0xF4]), # SAHF, LAHF
                "regs_initial": {0: 0x5500}, # AH = 0x55
                "regs_expected": {0: 0x5500}, # AH = 0x55
                "expected_eip": 3,
            },
            {
                "name": "Grp3 F6 NOT / NEG AL",
                # F6 D0 -> NOT AL (Reg=2, R/M=0 -> Mod=11, Reg=010, R/M=000 -> 11 010 000 = 0xD0)
                # F6 D8 -> NEG AL (Reg=3, R/M=0 -> Mod=11, Reg=011, R/M=000 -> 11 011 000 = 0xD8)
                "code": bytes([0xF6, 0xD0, 0xF6, 0xD8, 0xF4]), 
                "regs_initial": {0: 0x05}, # AL = 5
                "regs_expected": {0: 0x06}, # NOT(5) = 0xFA, NEG(0xFA) = 6
                "expected_eip": 5,
            },
            {
                "name": "0x0F JZ rel32",
                "code": bytes([0x0F, 0x84, 0x03, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90, 0xF4]),
                "regs_initial": {2: 0},
                "regs_expected": {},
                "expected_eip": 10,
                "eflags_initial": (1 << 6), # ZF=1
            },
            {
                "name": "0x0F SETZ AL",
                "code": bytes([0x0F, 0x94, 0xC0, 0xF4]), # SETZ AL
                "regs_initial": {0: 0},
                "regs_expected": {0: 1},
                "expected_eip": 4,
                "eflags_initial": (1 << 6), # ZF=1
            },
            {
                "name": "0x0F MOVZX EAX, BL",
                "code": bytes([0x0F, 0xB6, 0xC3, 0xF4]),
                "regs_initial": {0: 0, 3: 0x88},
                "regs_expected": {0: 0x88},
                "expected_eip": 4,
            },
            {
                "name": "0x0F MOVSX EAX, BL",
                "code": bytes([0x0F, 0xBE, 0xC3, 0xF4]),
                "regs_initial": {0: 0, 3: 0x88},
                "regs_expected": {0: 0xFFFFFF88},
                "expected_eip": 4,
            },
            {
                "name": "0x0F MOVZX EAX, BX",
                "code": bytes([0x0F, 0xB7, 0xC3, 0xF4]),
                "regs_initial": {0: 0, 3: 0x8888},
                "regs_expected": {0: 0x00008888},
                "expected_eip": 4,
            },
            {
                "name": "0x0F MOVSX EAX, BX",
                "code": bytes([0x0F, 0xBF, 0xC3, 0xF4]),
                "regs_initial": {0: 0, 3: 0x8888},
                "regs_expected": {0: 0xFFFF8888},
                "expected_eip": 4,
            },
            {
                "name": "Grp3 F7 MUL EBX",
                # F7 E3 -> MUL EBX (Reg=4, R/M=3 -> Mod=11, Reg=100, R/M=011 -> 11 100 011 = 0xE3)
                "code": bytes([0xF7, 0xE3, 0xF4]), 
                "regs_initial": {0: 0x10, 3: 0x03}, # EAX = 16, EBX = 3
                "regs_expected": {0: 0x30, 2: 0x00, 3: 0x03}, # EAX = 48, EDX = 0
                "expected_eip": 3,
            },
            {
                "name": "LIDT test",
                "code": bytes([
                    0x0F, 0x01, 0x18, # LIDT [EAX]
                    0xF4              # HLT
                ]),
                "regs_initial": {0: 0x1000},
                "regs_expected": {0: 0x1000},
                "expected_eip": 4,
                "ram_setup": [
                    (0x1000, bytes([0xFF, 0xFF, 0x00, 0x20, 0x00, 0x00]))
                ]
            },
            {
                "name": "INT 0x80 / IRET test",
                "code": bytes([
                    0xCD, 0x80,       # INT 0x80
                    0xF4              # HLT (at EIP=3)
                ]),
                "regs_initial": {0: 0x1000, 4: 0x3000},
                "regs_expected": {0: 0x1000, 4: 0x3000},
                "expected_eip": 3,
                "ram_setup": [
                    # Real-mode IVT: 4-byte entries, offset then selector.
                    # Vector 0x80 -> 0x80*4 = 0x200. This used to be written as
                    # an 8-byte protected-mode gate at 0x400, which no real
                    # machine would have read.
                    (0x200, bytes([0x34, 0x12, 0x00, 0x00])),
                    (0x1234, bytes([0xCF]))
                ]
            },
            {
                "name": "INT pushes FLAGS/CS/IP and clears IF",
                # STI ; INT 0x80 ; HLT   handler stores nothing, just IRETs.
                # Checked through the stack image: at SP-6 sits IP=4, then CS,
                # then FLAGS with IF still set (the pushed copy predates the
                # clear).
                "code": bytes([0xFB, 0xCD, 0x80, 0xF4]),
                "mode": 16,
                "segs_initial": {"SS": 0x0000},
                "regs_initial": {4: 0x3000},
                "regs_expected": {4: 0x3000},
                "ram_setup": [
                    (0x200, bytes([0x34, 0x12, 0x00, 0x00])),
                    (0x1234, bytes([0xCF])),
                ],
                "mem_expected": [
                    # STI is at 0 and INT 0x80 occupies 1-2, so the pushed IP
                    # is 3. CS follows it as 0x0000.
                    (0x2FFA, bytes([0x03, 0x00, 0x00, 0x00])),
                ],
                "expected_eip": 4,
            },
            {
                "name": "INT clears IF for the handler",
                # STI ; INT 0x80 ; HLT with a handler that HLTs instead of
                # returning, so EFLAGS is observed INSIDE the handler.
                "code": bytes([0xFB, 0xCD, 0x80, 0xF4]),
                "mode": 16,
                "segs_initial": {"SS": 0x0000},
                "regs_initial": {4: 0x3000},
                "ram_setup": [
                    (0x200, bytes([0x34, 0x12, 0x00, 0x00])),
                    (0x1234, bytes([0xF4])),
                ],
                "flags_expected": {"IF": 0},
                "regs_expected": {},
                "expected_eip": 0x1235,
            },
            {
                "name": "INT vector lookup uses a 4-byte stride",
                # Vector 0x21 -> 0x84. An 8-byte stride would read 0x108.
                "code": bytes([0xCD, 0x21, 0xF4]),
                "mode": 16,
                "segs_initial": {"SS": 0x0000},
                "regs_initial": {4: 0x3000},
                "ram_setup": [
                    (0x84, bytes([0x00, 0x30, 0x00, 0x00])),   # IP=0x3000 CS=0
                    (0x108, bytes([0x00, 0x40, 0x00, 0x00])),  # decoy
                    (0x3000, bytes([0xF4])),
                ],
                "regs_expected": {},
                "expected_eip": 0x3001,
            },
            {
                "name": "INT installs CS from the IVT, moving CS.base",
                # Handler at CS=0x0100:IP=0x0010 -> linear 0x1010
                "code": bytes([0xCD, 0x80, 0xF4]),
                "mode": 16,
                "segs_initial": {"SS": 0x0000},
                "regs_initial": {4: 0x3000},
                "ram_setup": [
                    (0x200, bytes([0x10, 0x00, 0x00, 0x01])),
                    (0x1010, bytes([0xF4])),
                ],
                "regs_expected": {},
                "expected_eip": 0x0011,
            },
            {
                "name": "INTO does not fire when OF is clear",
                "code": bytes([0xCE, 0xF4]),
                "mode": 16,
                "segs_initial": {"SS": 0x0000},
                "regs_initial": {4: 0x3000},
                "eflags_initial": 0,
                "ram_setup": [(0x10, bytes([0x00, 0x30, 0x00, 0x00])),
                              (0x3000, bytes([0xF4]))],
                "regs_expected": {},
                "expected_eip": 2,
            },
            {
                "name": "IN AL, imm8 (0xE4)",
                "code": bytes([
                    0xE4, 0x1F,       # IN AL, 0x1F
                    0xF4              # HLT
                ]),
                "regs_initial": {0: 0},
                "regs_expected": {0: 255}, # Since port 0x1F is unassigned, AL becomes 0xFF
                "expected_eip": 3
            }
        ])

        # ------------------------------------------------------------------
        # Flag tests.
        #
        # Every case above checks only registers and EIP, which is why the
        # entirely-missing arithmetic flag layer passed 82/82: CMP and TEST
        # "pass" by correctly not touching registers, and the Jcc cases pass by
        # reading an eflags_initial no instruction had to produce.
        #
        # These assert EFLAGS directly. Only the named flags are compared, so a
        # case says exactly what it means to pin down.
        # ------------------------------------------------------------------
        test_cases.extend([
            {
                "name": "FLAGS: CMP equal (ZF=1, CF=0)",
                "code": bytes([0x39, 0xD8, 0xF4]),          # CMP EAX, EBX
                "regs_initial": {0: 100, 3: 100},
                "regs_expected": {0: 100, 3: 100},           # CMP must not write
                "flags_expected": {"ZF": 1, "CF": 0, "SF": 0, "OF": 0},
                "expected_eip": 3,
            },
            {
                "name": "FLAGS: CMP below (CF=1, ZF=0)",
                "code": bytes([0x39, 0xD8, 0xF4]),          # CMP EAX, EBX
                "regs_initial": {0: 5, 3: 9},
                "regs_expected": {0: 5, 3: 9},
                "flags_expected": {"ZF": 0, "CF": 1, "SF": 1, "OF": 0},
                "expected_eip": 3,
            },
            {
                "name": "FLAGS: CMP above (CF=0, ZF=0)",
                # 0x100 - 1 = 0xFF: low byte has 8 bits set, so PF=1. Asserting a
                # flag that must be *set* keeps this case from passing vacuously
                # against an all-zero EFLAGS.
                "code": bytes([0x39, 0xD8, 0xF4]),          # CMP EAX, EBX
                "regs_initial": {0: 0x100, 3: 1},
                "regs_expected": {0: 0x100, 3: 1},
                "flags_expected": {"ZF": 0, "CF": 0, "SF": 0, "OF": 0, "PF": 1},
                "expected_eip": 3,
            },
            {
                "name": "FLAGS: SUB to zero (ZF=1)",
                "code": bytes([0x29, 0xD8, 0xF4]),          # SUB EAX, EBX
                "regs_initial": {0: 42, 3: 42},
                "regs_expected": {0: 0, 3: 42},
                "flags_expected": {"ZF": 1, "SF": 0, "CF": 0, "OF": 0},
                "expected_eip": 3,
            },
            {
                "name": "FLAGS: ADD unsigned carry out (CF=1)",
                "code": bytes([0x01, 0xD8, 0xF4]),          # ADD EAX, EBX
                "regs_initial": {0: 0xFFFFFFFF, 3: 1},
                "regs_expected": {0: 0, 3: 1},
                "flags_expected": {"CF": 1, "ZF": 1, "SF": 0, "OF": 0},
                "expected_eip": 3,
            },
            {
                "name": "FLAGS: ADD signed overflow (OF=1)",
                "code": bytes([0x01, 0xD8, 0xF4]),          # ADD EAX, EBX
                "regs_initial": {0: 0x7FFFFFFF, 3: 1},
                "regs_expected": {0: 0x80000000, 3: 1},
                "flags_expected": {"OF": 1, "SF": 1, "ZF": 0, "CF": 0},
                "expected_eip": 3,
            },
            {
                "name": "FLAGS: XOR self clears (ZF=1, CF=0, OF=0)",
                "code": bytes([0x31, 0xC0, 0xF4]),          # XOR EAX, EAX
                "regs_initial": {0: 0xDEADBEEF},
                "regs_expected": {0: 0},
                "flags_expected": {"ZF": 1, "SF": 0, "CF": 0, "OF": 0},
                "expected_eip": 3,
            },
            {
                "name": "FLAGS: AND negative result (SF=1)",
                "code": bytes([0x21, 0xD8, 0xF4]),          # AND EAX, EBX
                "regs_initial": {0: 0xF0000000, 3: 0x90000000},
                "regs_expected": {0: 0x90000000, 3: 0x90000000},
                "flags_expected": {"SF": 1, "ZF": 0, "CF": 0, "OF": 0},
                "expected_eip": 3,
            },
            {
                "name": "FLAGS: TEST Ev,Gv (0x85) sets ZF without writing",
                "code": bytes([0x85, 0xD8, 0xF4]),          # TEST EAX, EBX
                "regs_initial": {0: 0x0F0F0F0F, 3: 0xF0F0F0F0},
                "regs_expected": {0: 0x0F0F0F0F, 3: 0xF0F0F0F0},
                "flags_expected": {"ZF": 1, "SF": 0, "CF": 0, "OF": 0},
                "expected_eip": 3,
            },
            {
                "name": "FLAGS: TEST Ev,Iv (F7 /0) sets ZF without writing",
                # Test a bit that is clear, so the AND is zero and ZF must be 1.
                "code": bytes([0xF7, 0xC0, 0x02, 0x00, 0x00, 0x00, 0xF4]),  # TEST EAX, 2
                "regs_initial": {0: 0x01000000},
                "regs_expected": {0: 0x01000000},
                "flags_expected": {"ZF": 1, "SF": 0, "CF": 0, "OF": 0},
                "expected_eip": 7,
            },
            {
                "name": "FLAGS: parity even (PF=1)",
                "code": bytes([0x21, 0xD8, 0xF4]),          # AND EAX, EBX -> 0x03, two bits set
                "regs_initial": {0: 0xFF, 3: 0x03},
                "regs_expected": {0: 0x03, 3: 0x03},
                "flags_expected": {"PF": 1, "ZF": 0, "SF": 0},
                "expected_eip": 3,
            },
            {
                "name": "FLAGS: parity odd (PF=0), negative result (SF=1)",
                # Result 0x80000007: low byte has 3 bits set so PF=0, and bit 31
                # is set so SF=1 -- the SF assertion is what stops this case from
                # passing vacuously.
                "code": bytes([0x21, 0xD8, 0xF4]),          # AND EAX, EBX
                "regs_initial": {0: 0xFFFFFFFF, 3: 0x80000007},
                "regs_expected": {0: 0x80000007, 3: 0x80000007},
                "flags_expected": {"PF": 0, "ZF": 0, "SF": 1},
                "expected_eip": 3,
            },
            {
                "name": "FLAGS: INC does not touch CF",
                "code": bytes([0x40, 0xF4]),                # INC EAX
                "regs_initial": {0: 0xFFFFFFFF},
                "regs_expected": {0: 0},
                # INC sets ZF but must leave CF exactly as it was (1 here).
                "eflags_initial": (1 << 0),
                "flags_expected": {"ZF": 1, "CF": 1, "SF": 0, "OF": 0},
                "expected_eip": 2,
            },
            {
                "name": "FLAGS: SHL CF from last bit shifted out",
                "code": bytes([0xC1, 0xE0, 0x01, 0xF4]),    # SHL EAX, 1
                "regs_initial": {0: 0x80000000},
                "regs_expected": {0: 0},
                "flags_expected": {"CF": 1, "ZF": 1, "SF": 0},
                "expected_eip": 4,
            },
            {
                "name": "FLAGS: SHR CF from last bit shifted out",
                "code": bytes([0xC1, 0xE8, 0x01, 0xF4]),    # SHR EAX, 1
                "regs_initial": {0: 0x00000003},
                "regs_expected": {0: 1},
                "flags_expected": {"CF": 1, "ZF": 0, "SF": 0},
                "expected_eip": 4,
            },
            {
                "name": "FLAGS: SAR keeps sign, sets CF",
                "code": bytes([0xC1, 0xF8, 0x01, 0xF4]),    # SAR EAX, 1
                "regs_initial": {0: 0xFFFFFFFF},
                "regs_expected": {0: 0xFFFFFFFF},
                "flags_expected": {"CF": 1, "SF": 1, "ZF": 0},
                "expected_eip": 4,
            },
            {
                "name": "FLAGS: shift by zero leaves flags alone",
                "code": bytes([0xC1, 0xE0, 0x00, 0xF4]),    # SHL EAX, 0
                "regs_initial": {0: 0x00000001},
                "regs_expected": {0: 1},
                # CF starts set and must survive a zero-count shift untouched.
                "eflags_initial": (1 << 0),
                "flags_expected": {"CF": 1},
                "expected_eip": 4,
            },
            {
                "name": "FLAGS: NEG sets CF when operand nonzero",
                "code": bytes([0xF7, 0xD8, 0xF4]),          # NEG EAX
                "regs_initial": {0: 1},
                "regs_expected": {0: 0xFFFFFFFF},
                "flags_expected": {"CF": 1, "SF": 1, "ZF": 0},
                "expected_eip": 3,
            },
            {
                "name": "FLAGS: NEG of zero clears CF",
                "code": bytes([0xF7, 0xD8, 0xF4]),          # NEG EAX
                "regs_initial": {0: 0},
                "regs_expected": {0: 0},
                "flags_expected": {"CF": 0, "ZF": 1, "SF": 0},
                "expected_eip": 3,
            },
            {
                "name": "FLAGS: ADC consumes CF",
                "code": bytes([0x11, 0xD8, 0xF4]),          # ADC EAX, EBX
                "regs_initial": {0: 1, 3: 1},
                "regs_expected": {0: 3, 3: 1},              # 1 + 1 + CF(1)
                "eflags_initial": (1 << 0),
                "flags_expected": {"CF": 0, "ZF": 0, "SF": 0},
                "expected_eip": 3,
            },
            {
                "name": "FLAGS: SBB consumes CF",
                "code": bytes([0x19, 0xD8, 0xF4]),          # SBB EAX, EBX
                "regs_initial": {0: 10, 3: 1},
                "regs_expected": {0: 8, 3: 1},              # 10 - 1 - CF(1)
                "eflags_initial": (1 << 0),
                "flags_expected": {"CF": 0, "ZF": 0, "SF": 0},
                "expected_eip": 3,
            },
            {
                "name": "FLAGS: 8-bit CMP uses byte width for SF",
                # CMP AL, BL with AL=0x00, BL=0x01 -> 0xFF: SF must come from
                # bit 7, not bit 31, so this fails without the LSL #24 normalize.
                "code": bytes([0x38, 0xD8, 0xF4]),          # CMP AL, BL
                "regs_initial": {0: 0x00, 3: 0x01},
                "regs_expected": {0: 0x00, 3: 0x01},
                "flags_expected": {"SF": 1, "CF": 1, "ZF": 0},
                "expected_eip": 3,
            },
            {
                "name": "FLAGS: HLT alone preserves flags",
                # Control: proves eflags_initial survives a run untouched, so a
                # failure elsewhere is the handler and not the harness.
                "code": bytes([0xF4]),
                "regs_initial": {0: 1},
                "regs_expected": {0: 1},
                "eflags_initial": (1 << 0),
                "flags_expected": {"CF": 1},
                "expected_eip": 1,
            },
            {
                "name": "FLAGS: CMP then JZ taken (end-to-end)",
                # CMP EAX,EBX ; JZ +2 ; HLT ; HLT  -- equal, so the branch is taken
                "code": bytes([0x39, 0xD8, 0x74, 0x01, 0xF4, 0xF4]),
                "regs_initial": {0: 7, 3: 7},
                "regs_expected": {0: 7, 3: 7},
                "flags_expected": {"ZF": 1},
                "expected_eip": 6,
            },

            # --- Segmentation -------------------------------------------------
            # Every case here is built so it FAILS if the base is not applied:
            # the value the instruction must load sits only at the segmented
            # address, and a decoy sits at the unsegmented one.
            {
                "name": "SEG: DS.base is added to a disp32 operand",
                # MOV EAX, [0x400] ; HLT   with DS.base = 0x200
                # -> must read linear 0x600, not 0x400.
                "code": bytes([0x8B, 0x05, 0x00, 0x04, 0x00, 0x00, 0xF4]),
                "segs_initial": {"DS": 0x200},
                "ram_setup": [(0x400, (0xDEADBEEF).to_bytes(4, "little")),
                              (0x600, (0xCAFEF00D).to_bytes(4, "little"))],
                "regs_initial": {},
                "regs_expected": {0: 0xCAFEF00D},
                "expected_eip": 7,
            },
            {
                "name": "SEG: [EBP+disp8] defaults to SS, not DS",
                # MOV EAX, [EBP+0x10] ; HLT   EBP=0x40, SS.base=0x200, DS.base=0x800
                # -> must read 0x250 (SS), not 0x850 (DS).
                "code": bytes([0x8B, 0x45, 0x10, 0xF4]),
                "segs_initial": {"SS": 0x200, "DS": 0x800},
                "ram_setup": [(0x250, (0x11112222).to_bytes(4, "little")),
                              (0x850, (0x33334444).to_bytes(4, "little"))],
                "regs_initial": {5: 0x40},
                "regs_expected": {0: 0x11112222, 5: 0x40},
                "expected_eip": 4,
            },
            {
                "name": "SEG: 3E override beats the SS default on [EBP+disp8]",
                # DS: MOV EAX, [EBP+0x10] ; HLT  -- same state as above, so this
                # case and the previous one disagree only about the prefix. That
                # is exactly the distinction a DS-pointer-as-default design
                # cannot make, which is why R6's sentinel is R8.
                "code": bytes([0x3E, 0x8B, 0x45, 0x10, 0xF4]),
                "segs_initial": {"SS": 0x200, "DS": 0x800},
                "ram_setup": [(0x250, (0x11112222).to_bytes(4, "little")),
                              (0x850, (0x33334444).to_bytes(4, "little"))],
                "regs_initial": {5: 0x40},
                "regs_expected": {0: 0x33334444, 5: 0x40},
                "expected_eip": 5,
            },
            {
                "name": "SEG: 26 ES override on a disp32 operand",
                # ES: MOV EAX, [0x400] ; HLT   ES.base=0x100, DS.base=0x200
                "code": bytes([0x26, 0x8B, 0x05, 0x00, 0x04, 0x00, 0x00, 0xF4]),
                "segs_initial": {"ES": 0x100, "DS": 0x200},
                "ram_setup": [(0x500, (0x55556666).to_bytes(4, "little")),
                              (0x600, (0x77778888).to_bytes(4, "little"))],
                "regs_initial": {},
                "regs_expected": {0: 0x55556666},
                "expected_eip": 8,
            },
            {
                "name": "SEG: an override does not leak into the next instruction",
                # ES: NOP ; MOV EAX, [0x400] ; HLT  -- the prefix is consumed by
                # an instruction with no memory operand, so the load that
                # follows must still be DS. Catches a self-clearing scheme that
                # only resets R6 where the override is actually used.
                "code": bytes([0x26, 0x90, 0x8B, 0x05, 0x00, 0x04, 0x00, 0x00, 0xF4]),
                "segs_initial": {"ES": 0x100, "DS": 0x200},
                "ram_setup": [(0x500, (0x55556666).to_bytes(4, "little")),
                              (0x600, (0x77778888).to_bytes(4, "little"))],
                "regs_initial": {},
                "regs_expected": {0: 0x77778888},
                "expected_eip": 9,
            },
            {
                "name": "SEG: a store applies DS.base too",
                # MOV [0x400], EAX ; HLT   with DS.base = 0x200
                "code": bytes([0x89, 0x05, 0x00, 0x04, 0x00, 0x00, 0xF4]),
                "segs_initial": {"DS": 0x200},
                "regs_initial": {0: 0x99990000},
                "regs_expected": {0: 0x99990000},
                "mem_expected": [(0x600, (0x99990000).to_bytes(4, "little")),
                                 (0x400, bytes(4))],
                "expected_eip": 7,
            },
            {
                "name": "SEG: SIB with ESP base defaults to SS",
                # MOV EAX, [ESP] ; HLT   ESP=0x40, SS.base=0x200, DS.base=0x800
                "code": bytes([0x8B, 0x04, 0x24, 0xF4]),
                "segs_initial": {"SS": 0x200, "DS": 0x800},
                "ram_setup": [(0x240, (0xAAAA1111).to_bytes(4, "little")),
                              (0x840, (0xBBBB2222).to_bytes(4, "little"))],
                "regs_initial": {4: 0x40},
                "regs_expected": {0: 0xAAAA1111, 4: 0x40},
                "expected_eip": 4,
            },

            # --- 16-bit ------------------------------------------------------
            # These run through opcode_table_16 with CS.D clear. Before this
            # work that table was a byte-for-byte duplicate of the 32-bit
            # handlers, so every one of these would have behaved as 32-bit.
            {
                "name": "O16: MOV AX,imm16 preserves the high half of EAX",
                "code": bytes([0xB8, 0x34, 0x12, 0xF4]),   # MOV AX, 0x1234
                "mode": 16,
                "regs_initial": {0: 0xAAAA0000},
                "regs_expected": {0: 0xAAAA1234},
                "expected_eip": 4,
            },
            {
                "name": "O16: immediate is 2 bytes, not 4",
                # MOV AX,0x1234 ; MOV BX,0x5678 ; HLT -- if the first
                # instruction ate a 4-byte immediate, the second is never
                # decoded and EIP lands elsewhere.
                "code": bytes([0xB8, 0x34, 0x12, 0xBB, 0x78, 0x56, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x1234, 3: 0x5678},
                "expected_eip": 7,
            },
            {
                "name": "O16: PUSH/POP move SP by 2 and go through SS.base",
                # MOV AX,0x1234 ; PUSH AX ; POP BX ; HLT   SP=0x100, SS.base=0x2000
                "code": bytes([0xB8, 0x34, 0x12, 0x50, 0x5B, 0xF4]),
                "mode": 16,
                "segs_initial": {"SS": 0x2000},
                "regs_initial": {4: 0x100},
                "regs_expected": {0: 0x1234, 3: 0x1234, 4: 0x100},
                # the word must actually land at SS.base + (SP-2)
                # exactly two bytes: the word below must stay clear, or the
                # push wrote 4.
                "mem_expected": [(0x20FC, bytes([0x00, 0x00, 0x34, 0x12]))],
                "expected_eip": 6,
            },
            {
                "name": "A16: [BX] addressing",
                # MOV AX,[BX] ; HLT   BX=0x300, DS.base=0x1000 -> linear 0x1300
                "code": bytes([0x8B, 0x07, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x1000},
                # 0xDEAD sits in the next word: a 32-bit read yields
                # 0xDEADBEEF, and the dirty high half of EAX must survive.
                "ram_setup": [(0x1300, (0xDEADBEEF).to_bytes(4, "little"))],
                "regs_initial": {0: 0xAAAA0000, 3: 0x300},
                "regs_expected": {0: 0xAAAABEEF, 3: 0x300},
                "expected_eip": 3,
            },
            {
                "name": "A16: [BX+SI+disp8] adds both index registers",
                # MOV AX,[BX+SI+0x10] ; HLT  BX=0x300 SI=0x40 -> 0x350 + DS 0x1000
                "code": bytes([0x8B, 0x40, 0x10, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x1000},
                "ram_setup": [(0x1350, (0xDEADC0DE).to_bytes(4, "little"))],
                "regs_initial": {0: 0xAAAA0000, 3: 0x300, 6: 0x40},
                "regs_expected": {0: 0xAAAAC0DE, 3: 0x300, 6: 0x40},
                "expected_eip": 4,
            },
            {
                "name": "A16: [BP+disp8] defaults to SS",
                # MOV AX,[BP+0x10] ; HLT  BP=0x40, SS.base=0x2000, DS.base=0x1000
                "code": bytes([0x8B, 0x46, 0x10, 0xF4]),
                "mode": 16,
                "segs_initial": {"SS": 0x2000, "DS": 0x1000},
                "ram_setup": [(0x2050, (0xDEAD4444).to_bytes(4, "little")),
                              (0x1050, (0x5555).to_bytes(2, "little"))],
                "regs_initial": {0: 0xAAAA0000, 5: 0x40},
                "regs_expected": {0: 0xAAAA4444, 5: 0x40},
                "expected_eip": 4,
            },
            {
                "name": "A16: mod=00 rm=110 is disp16, not [BP]",
                # MOV AX,[0x0300] ; HLT   -- BP is set to a decoy value
                "code": bytes([0x8B, 0x06, 0x00, 0x03, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x1000},
                "ram_setup": [(0x1300, (0xDEAD7777).to_bytes(4, "little"))],
                "regs_initial": {0: 0xAAAA0000, 5: 0xFFFF},
                "regs_expected": {0: 0xAAAA7777},
                "expected_eip": 5,
            },
            {
                "name": "A16: effective address wraps at 16 bits",
                # MOV AX,[BX+0x10] with BX=0xFFFF -> offset 0x0F, not 0x1000F
                "code": bytes([0x8B, 0x47, 0x10, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x1000},
                "ram_setup": [(0x100F, (0xDEAD8888).to_bytes(4, "little"))],
                "regs_initial": {0: 0xAAAA0000, 3: 0xFFFF},
                "regs_expected": {0: 0xAAAA8888, 3: 0xFFFF},
                "expected_eip": 4,
            },
            {
                "name": "O16: SF/OF come from bit 15",
                # MOV AX,0x7FFF ; ADD AX,1 ; HLT -> 0x8000: SF=1 OF=1 CF=0
                "code": bytes([0xB8, 0xFF, 0x7F, 0x83, 0xC0, 0x01, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x8000},
                "flags_expected": {"SF": 1, "OF": 1, "CF": 0, "ZF": 0},
                "expected_eip": 7,
            },
            {
                "name": "O16: CF comes from bit 16",
                # MOV AX,0xFFFF ; ADD AX,1 ; HLT -> 0x0000: CF=1 ZF=1 SF=0 OF=0
                "code": bytes([0xB8, 0xFF, 0xFF, 0x83, 0xC0, 0x01, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x0000},
                "flags_expected": {"CF": 1, "ZF": 1, "SF": 0, "OF": 0},
                "expected_eip": 7,
            },
            {
                "name": "O16: ADC consumes CF at the 16-bit width",
                # STC ; MOV AX,0x00FF ; ADC AX,0x0001 ; HLT -> 0x0101
                # The normalized-carry bug made narrow ADC ignore CF entirely.
                "code": bytes([0xF9, 0xB8, 0xFF, 0x00, 0x83, 0xD0, 0x01, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x0101},
                "expected_eip": 8,
            },

            # --- 66/67 prefixes: the two size axes, dynamically -----------------
            # The point of these is that operand size and address size are
            # INDEPENDENT and switch per instruction, in either default mode --
            # not chosen once from CS.D.
            {
                "name": "66: 32-bit operand inside a 16-bit segment",
                # 66 B8 imm32 -> MOV EAX, 0x11223344 ; HLT
                "code": bytes([0x66, 0xB8, 0x44, 0x33, 0x22, 0x11, 0xF4]),
                "mode": 16,
                "regs_initial": {0: 0xAAAA0000},
                "regs_expected": {0: 0x11223344},
                "expected_eip": 7,
            },
            {
                "name": "66: 16-bit operand inside a 32-bit segment",
                # 66 B8 imm16 -> MOV AX, 0x1234 ; HLT, high half preserved
                "code": bytes([0x66, 0xB8, 0x34, 0x12, 0xF4]),
                "mode": 32,
                "regs_initial": {0: 0xAAAA0000},
                "regs_expected": {0: 0xAAAA1234},
                "expected_eip": 5,
            },
            {
                "name": "66: the override dies at the instruction boundary",
                # 66 B8 imm32 (MOV EAX) ; B8 imm16 (MOV BX.. no: MOV AX) ; HLT
                # The second MOV must be 16-bit again.
                "code": bytes([0x66, 0xB8, 0x44, 0x33, 0x22, 0x11,
                               0xBB, 0x78, 0x56, 0xF4]),
                "mode": 16,
                "regs_initial": {3: 0xBBBB0000},
                "regs_expected": {0: 0x11223344, 3: 0xBBBB5678},
                "expected_eip": 10,
            },
            {
                "name": "66 twice cancels",
                "code": bytes([0x66, 0x66, 0xB8, 0x34, 0x12, 0xF4]),
                "mode": 16,
                "regs_initial": {0: 0xAAAA0000},
                "regs_expected": {0: 0xAAAA1234},
                "expected_eip": 6,
            },
            {
                "name": "67: 32-bit addressing inside a 16-bit segment",
                # 67 8B 05 disp32 -> MOV AX, [disp32], 16-bit operand but a
                # 32-bit address form. A 16-bit decoder would read the disp as
                # 2 bytes and take rm=101 as [DI].
                "code": bytes([0x67, 0x8B, 0x05, 0x00, 0x04, 0x00, 0x00, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x1000},
                "ram_setup": [(0x1400, (0xDEAD9999).to_bytes(4, "little"))],
                "regs_initial": {0: 0xAAAA0000, 7: 0xFFFF},
                "regs_expected": {0: 0xAAAA9999},
                "expected_eip": 8,
            },
            {
                "name": "67: 16-bit addressing inside a 32-bit segment",
                # 67 8B 07 -> MOV EAX, [BX] using the 16-bit grammar.
                # Without 67 this ModRM means [EDI].
                "code": bytes([0x67, 0x8B, 0x07, 0xF4]),
                "mode": 32,
                "segs_initial": {"DS": 0x1000},
                "ram_setup": [(0x1300, (0x12345678).to_bytes(4, "little"))],
                "regs_initial": {3: 0x300, 7: 0xFFFF},
                "regs_expected": {0: 0x12345678, 3: 0x300},
                "expected_eip": 4,
            },
            {
                "name": "66+67: both axes overridden at once",
                # 66 67 8B 07 in a 16-bit segment -> 32-bit operand, and 67
                # flips addressing to 32-bit, so ModRM 07 means [EDI].
                "code": bytes([0x66, 0x67, 0x8B, 0x07, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x1000},
                "ram_setup": [(0x1300, (0xCAFEBABE).to_bytes(4, "little"))],
                "regs_initial": {7: 0x300, 3: 0xFFFF},
                "regs_expected": {0: 0xCAFEBABE, 7: 0x300},
                "expected_eip": 5,
            },
            {
                "name": "67 does not disturb operand size",
                # 67 in a 16-bit segment: addressing becomes 32-bit, the
                # operand must stay 16-bit and preserve the high half.
                "code": bytes([0x67, 0x8B, 0x05, 0x00, 0x04, 0x00, 0x00, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x1000},
                "ram_setup": [(0x1400, (0xDEAD9999).to_bytes(4, "little"))],
                "regs_initial": {0: 0x5555FFFF},
                "regs_expected": {0: 0x55559999},
                "expected_eip": 8,
            },
            {
                "name": "66 with a segment override, both prefixes live",
                # 26 66 A1 disp -> ES: MOV EAX, [disp16] in a 16-bit segment
                "code": bytes([0x26, 0x66, 0x8B, 0x06, 0x00, 0x03, 0xF4]),
                "mode": 16,
                "segs_initial": {"ES": 0x1000, "DS": 0x8000},
                "ram_setup": [(0x1300, (0x0BADF00D).to_bytes(4, "little"))],
                "regs_initial": {},
                "regs_expected": {0: 0x0BADF00D},
                "expected_eip": 7,
            },

            {
                # F6 /0 fetched its immediate and set no flags at all, so the
                # NEXT conditional branch ran on the previous instruction's
                # result. Seeded here with an AND that leaves ZF clear, which
                # is exactly how the bug stayed invisible: the flags looked
                # plausible, they were just the wrong instruction's.
                "name": "TEST r/m8,imm8 (F6 /0) sets ZF",
                "code": bytes([0xB0, 0x31,        # mov al, 0x31 -> AND clears ZF
                               0x24, 0x7F,        # and al, 0x7F
                               0xB4, 0x31,        # mov ah, 0x31
                               0xF6, 0xC4, 0x80,  # test ah, 0x80 -> ZF must be 1
                               0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x3131},
                "flags_expected": {"ZF": 1, "SF": 0, "CF": 0, "OF": 0},
                "expected_eip": 10,
            },
            {
                "name": "TEST r/m8,imm8 (F6 /0) clears ZF when bits match",
                "code": bytes([0xB4, 0xB1,        # mov ah, 0xB1 (a break code)
                               0xF6, 0xC4, 0x80,  # test ah, 0x80 -> ZF 0, SF 1
                               0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0xB100},
                "flags_expected": {"ZF": 0, "SF": 1, "CF": 0, "OF": 0},
                "expected_eip": 6,
            },
            {
                # Alley Cat's INT 09h handler, byte for byte. It sets ES to its
                # data segment, reads the scancode, and bumps a counter that the
                # game's wait loops poll -- through an ES override on the
                # read-modify-write form of INC. If any part of that is wrong
                # the game takes the keystroke and does nothing with it, which
                # is exactly what "the guest ignores the keyboard" looked like.
                "name": "INC word ES:[disp16] (Alley Cat's INT 09h counter)",
                "code": bytes([0xBF, 0x91, 0x00,              # mov di, 0x0091
                               0x8E, 0xC7,                    # mov es, di
                               0xB0, 0x31,                    # mov al, 0x31 (N)
                               0x8A, 0xE0,                    # mov ah, al
                               0x24, 0x7F,                    # and al, 0x7F
                               0xF6, 0xC4, 0x80,              # test ah, 0x80
                               0x75, 0x05,                    # jnz over the INC
                               0x26, 0xFF, 0x06, 0x93, 0x06,  # inc es:[0x0693]
                               0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x8000},
                "ram_setup": [(0x0FA3, bytes([0x07, 0x00]))],
                "regs_initial": {},
                "regs_expected": {},
                "mem_expected": [(0x0FA3, bytes([0x08, 0x00]))],
                "expected_eip": 22,
            },
            {
                # The break code must NOT bump it: the JNZ has to be taken. A
                # TEST that got ZF backwards passes the make-code case above and
                # fails only here.
                "name": "INC skipped on a break code (TEST ah,0x80 / JNZ)",
                "code": bytes([0xBF, 0x91, 0x00,
                               0x8E, 0xC7,
                               0xB0, 0xB1,                    # mov al, 0xB1
                               0x8A, 0xE0,
                               0x24, 0x7F,
                               0xF6, 0xC4, 0x80,
                               0x75, 0x05,
                               0x26, 0xFF, 0x06, 0x93, 0x06,
                               0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x8000},
                "ram_setup": [(0x0FA3, bytes([0x07, 0x00]))],
                "regs_initial": {},
                "regs_expected": {},
                "mem_expected": [(0x0FA3, bytes([0x07, 0x00]))],
                "expected_eip": 22,
            },

            # --- 16-bit control flow ------------------------------------------
            {
                "name": "O16: JMP rel16 displacement is 2 bytes",
                # E9 rel16 -> jump over a MOV, land on the second
                "code": bytes([0xE9, 0x03, 0x00,          # JMP +3
                               0xB8, 0xFF, 0xFF,          # (skipped) MOV AX,0xFFFF
                               0xB8, 0x11, 0x22,          # MOV AX,0x2211
                               0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x2211},
                "expected_eip": 10,
            },
            {
                "name": "O16: CALL rel16 pushes a 2-byte return address",
                # CALL +1 ; HLT ; POP BX ; HLT   SP=0x100, SS.base=0x2000
                # The pushed IP is 3 (the instruction after the CALL).
                "code": bytes([0xE8, 0x01, 0x00,   # CALL -> 4
                               0xF4,               # (not reached)
                               0x5B,               # POP BX
                               0xF4]),
                "mode": 16,
                "segs_initial": {"SS": 0x2000},
                "regs_initial": {4: 0x100},
                "regs_expected": {3: 0x0003, 4: 0x100},
                "expected_eip": 6,
            },
            {
                "name": "O16: CALL/RET round trip",
                # CALL +2 ; JMP +3 (skipped path) ; RET target...
                #  0: E8 02 00   CALL 5
                #  3: EB 02      JMP 7
                #  5: C3         RET  -> back to 3
                #  6: F4
                #  7: F4
                "code": bytes([0xE8, 0x02, 0x00, 0xEB, 0x02, 0xC3, 0xF4, 0xF4]),
                "mode": 16,
                "segs_initial": {"SS": 0x2000},
                "regs_initial": {4: 0x100},
                "regs_expected": {4: 0x100},
                "expected_eip": 8,
            },
            {
                "name": "O16: Jcc rel8 taken on a 16-bit compare",
                # MOV AX,0x8000 ; CMP AX,0x8000 ; JZ +3 ; MOV AX,0xFFFF ; HLT
                "code": bytes([0xB8, 0x00, 0x80,          # MOV AX,0x8000
                               0x3D, 0x00, 0x80,          # CMP AX,0x8000
                               0x74, 0x03,                # JZ +3
                               0xB8, 0xFF, 0xFF,          # (skipped)
                               0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x8000},
                "flags_expected": {"ZF": 1},
                "expected_eip": 12,
            },
            {
                "name": "O16: rel16 displacement is sign-extended",
                #  0: E9 03 00   JMP forward to 6
                #  3: F4         HLT            <- backward target, ends the run
                #  4: 90 90
                #  6: B8 22 22   MOV AX,0x2222
                #  9: E9 F7 FF   JMP -9 -> 3
                # A zero-extended displacement lands at 3 + 0xFFF7 instead.
                "code": bytes([0xE9, 0x03, 0x00,
                               0xF4,
                               0x90, 0x90,
                               0xB8, 0x22, 0x22,
                               0xE9, 0xF7, 0xFF]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x2222},
                "expected_eip": 4,
            },
            {
                "name": "O16: CALL pushes IP, not the linear EIP",
                # Same CALL as before but with CS.base non-zero. Pushing R10
                # directly would store base+3 and RET would then add the base a
                # second time.
                "code": bytes([0xE8, 0x01, 0x00, 0xF4, 0x5B, 0xF4]),
                "mode": 16,
                "segs_initial": {"SS": 0x2000, "CS": 0x0000},
                "regs_initial": {4: 0x100},
                "regs_expected": {3: 0x0003, 4: 0x100},
                "expected_eip": 6,
            },

            # --- string ops: segments and address size -------------------------
            {
                "name": "A16: MOVSB uses DS:SI -> ES:DI, not raw ESI/EDI",
                # MOVSB ; HLT   DS.base=0x1000 SI=0x10, ES.base=0x2000 DI=0x20
                "code": bytes([0xA4, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x1000, "ES": 0x2000},
                "ram_setup": [(0x1010, bytes([0x5A]))],
                "regs_initial": {6: 0x10, 7: 0x20},
                "regs_expected": {6: 0x11, 7: 0x21},
                "mem_expected": [(0x2020, bytes([0x5A]))],
                "expected_eip": 2,
            },
            {
                "name": "O16: STOSW writes 2 bytes and advances DI by 2",
                "code": bytes([0xAB, 0xF4]),
                "mode": 16,
                "segs_initial": {"ES": 0x2000},
                "regs_initial": {0: 0xFFFF1234, 7: 0x30},
                "regs_expected": {7: 0x32},
                "mem_expected": [(0x2030, bytes([0x34, 0x12, 0x00, 0x00]))],
                "expected_eip": 2,
            },
            {
                "name": "A16: STD makes MOVSB decrement both indices",
                # STD ; MOVSB ; HLT
                "code": bytes([0xFD, 0xA4, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x1000, "ES": 0x2000},
                "ram_setup": [(0x1010, bytes([0x77]))],
                "regs_initial": {6: 0x10, 7: 0x20},
                "regs_expected": {6: 0x0F, 7: 0x1F},
                "mem_expected": [(0x2020, bytes([0x77]))],
                "expected_eip": 3,
            },
            {
                "name": "A16: SI wraps at 16 bits, it does not run into ESI",
                # SI = 0xFFFF -> source is DS.base + 0xFFFF = 0x10FFF (the wrap
                # is on the OFFSET, not on the linear sum). After the move SI
                # must be 0x0000, not 0x10000.
                "code": bytes([0xA4, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x1000, "ES": 0x2000},
                "ram_setup": [(0x10FFF, bytes([0x33]))],
                "regs_initial": {6: 0xFFFF, 7: 0x40},
                "regs_expected": {6: 0x0000, 7: 0x41},
                "mem_expected": [(0x2040, bytes([0x33]))],
                "expected_eip": 2,
            },
            {
                "name": "A16: a segment override redirects LODSB's source",
                # ES: LODSB -- source becomes ES:SI instead of DS:SI
                "code": bytes([0x26, 0xAC, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x1000, "ES": 0x2000},
                "ram_setup": [(0x1050, bytes([0x11])), (0x2050, bytes([0x99]))],
                "regs_initial": {0: 0xAAAAAA00, 6: 0x50},
                "regs_expected": {0: 0xAAAAAA99, 6: 0x51},
                "expected_eip": 3,
            },
            {
                "name": "A16: SCASB compares against ES:DI and sets ZF",
                "code": bytes([0xAE, 0xF4]),
                "mode": 16,
                "segs_initial": {"ES": 0x2000},
                "ram_setup": [(0x2060, bytes([0x42]))],
                "regs_initial": {0: 0x42, 7: 0x60},
                "regs_expected": {7: 0x61},
                "flags_expected": {"ZF": 1},
                "expected_eip": 2,
            },

            # --- REP ----------------------------------------------------------
            {
                "name": "REP MOVSB copies CX bytes and lands CX at 0",
                # REP MOVSB ; HLT   CX=4, DS:SI=0x1000:0x10, ES:DI=0x2000:0x20
                "code": bytes([0xF3, 0xA4, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x1000, "ES": 0x2000},
                "ram_setup": [(0x1010, bytes([0xDE, 0xAD, 0xBE, 0xEF]))],
                "regs_initial": {1: 4, 6: 0x10, 7: 0x20},
                "regs_expected": {1: 0, 6: 0x14, 7: 0x24},
                "mem_expected": [(0x2020, bytes([0xDE, 0xAD, 0xBE, 0xEF, 0x00]))],
                "expected_eip": 3,
            },
            # --- INS / OUTS (6C-6F) -------------------------------------------
            # These reached .exit_loop until now, which is what halted WOLF3D:
            # its VL_SetPalette is `mov cx,768 / rep outsb` to the VGA DAC, so
            # a whole 256-colour palette went through one missing opcode.
            # Port 0x1F is unassigned and reads back 0xFF, which is what makes
            # the INS side checkable without a device model.
            {
                "name": "REP INSB fills ES:DI from a port, CX to 0",
                # MOV DX,0x1F ; REP INSB ; HLT
                "code": bytes([0xBA, 0x1F, 0x00, 0xF3, 0x6C, 0xF4]),
                "mode": 16,
                "segs_initial": {"ES": 0x2000},
                "regs_initial": {1: 4, 2: 0x1F, 7: 0x60},
                "regs_expected": {1: 0, 7: 0x64},
                "mem_expected": [(0x2060, bytes([0xFF, 0xFF, 0xFF, 0xFF, 0x00]))],
                "expected_eip": 6,
            },
            {
                "name": "INSW advances DI by 2, not by 1",
                "code": bytes([0xBA, 0x1F, 0x00, 0x6D, 0xF4]),
                "mode": 16,
                "segs_initial": {"ES": 0x2000},
                "regs_initial": {2: 0x1F, 7: 0x70},
                "regs_expected": {7: 0x72},
                "mem_expected": [(0x2070, bytes([0xFF, 0xFF, 0x00]))],
                "expected_eip": 5,
            },
            {
                "name": "STD makes INSB decrement DI",
                "code": bytes([0xFD, 0xBA, 0x1F, 0x00, 0x6C, 0xFC, 0xF4]),
                "mode": 16,
                "segs_initial": {"ES": 0x2000},
                "regs_initial": {2: 0x1F, 7: 0x80},
                "regs_expected": {7: 0x7F},
                "mem_expected": [(0x2080, bytes([0xFF]))],
                "expected_eip": 7,
            },
            {
                # OUTSB reads DS:SI and advances SI. The port sink is not
                # observable here, so what is asserted is the MEMORY SIDE and
                # the count -- exactly the half that a "write one byte and stop"
                # regression would break.
                "name": "REP OUTSB walks DS:SI and lands CX at 0",
                "code": bytes([0xBA, 0x1F, 0x00, 0xF3, 0x6E, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x1000},
                "ram_setup": [(0x1090, bytes([0x11, 0x22, 0x33, 0x44]))],
                "regs_initial": {1: 4, 2: 0x1F, 6: 0x90},
                "regs_expected": {1: 0, 6: 0x94},
                "expected_eip": 6,
            },
            {
                "name": "OUTSW advances SI by 2",
                "code": bytes([0xBA, 0x1F, 0x00, 0x6F, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x1000},
                "ram_setup": [(0x10A0, bytes([0x11, 0x22]))],
                "regs_initial": {2: 0x1F, 6: 0xA0},
                "regs_expected": {6: 0xA2},
                "expected_eip": 5,
            },
            {
                "name": "REP with CX=0 does nothing and skips the string op",
                "code": bytes([0xF3, 0xA4, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x1000, "ES": 0x2000},
                "ram_setup": [(0x1010, bytes([0xDE]))],
                "regs_initial": {1: 0, 6: 0x10, 7: 0x20},
                "regs_expected": {1: 0, 6: 0x10, 7: 0x20},
                "mem_expected": [(0x2020, bytes([0x00]))],
                "expected_eip": 3,
            },
            {
                "name": "REP STOSW fills words, DI advances by 2 each",
                "code": bytes([0xF3, 0xAB, 0xF4]),
                "mode": 16,
                "segs_initial": {"ES": 0x2000},
                "regs_initial": {0: 0x1234, 1: 3, 7: 0x40},
                "regs_expected": {1: 0, 7: 0x46},
                "mem_expected": [(0x2040, bytes([0x34, 0x12, 0x34, 0x12,
                                                 0x34, 0x12, 0x00, 0x00]))],
                "expected_eip": 3,
            },
            {
                "name": "REPNE SCASB stops on a match, before CX runs out",
                # Scan for 0x42 in 'A','B','C',0x42,... CX=8
                "code": bytes([0xF2, 0xAE, 0xF4]),
                "mode": 16,
                "segs_initial": {"ES": 0x2000},
                "ram_setup": [(0x2050, bytes([0x41, 0x41, 0x41,
                                              0x42, 0x41, 0x41]))],
                "regs_initial": {0: 0x42, 1: 8, 7: 0x50},
                # stops with DI just past the match at offset 0x53
                "regs_expected": {1: 4, 7: 0x54},
                "flags_expected": {"ZF": 1},
                "expected_eip": 3,
            },
            {
                "name": "REPE CMPSB stops at the first difference",
                # "ABCX" vs "ABCY" -- equal for 3, differs at the 4th
                "code": bytes([0xF3, 0xA6, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x1000, "ES": 0x2000},
                "ram_setup": [(0x1010, b"ABCX"), (0x2020, b"ABCY")],
                "regs_initial": {1: 4, 6: 0x10, 7: 0x20},
                "regs_expected": {1: 0, 6: 0x14, 7: 0x24},
                "flags_expected": {"ZF": 0},
                "expected_eip": 3,
            },
            {
                "name": "REP MOVSW copies words across a granule boundary",
                # REP MOVSW ; HLT. 0x40 words from DS:SI to ES:DI, arranged so
                # the copy crosses a 4 KB fold granule -- each element is a
                # 2-byte access and the fold answers per granule, so a word
                # straddling the boundary is the case that breaks.
                "code": bytes([0xF3, 0xA5, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x1000, "ES": 0x2000},
                "ram_setup": [(0x1FC0, bytes(range(0x40)) * 2)],
                "regs_initial": {1: 0x40, 6: 0xFC0, 7: 0xFC0},
                "regs_expected": {1: 0, 6: 0x1040, 7: 0x1040},
                "mem_expected": [(0x2FC0, bytes(range(0x40)) * 2)],
                "expected_eip": 3,
            },
            {
                "name": "REP MOVSW with a large count runs to completion",
                # 0x400 words = 2 KB. The rewind implementation re-enters the
                # fetch loop per element, so a big count is where a per-element
                # bookkeeping error shows up rather than at CX=4.
                "code": bytes([0xF3, 0xA5, 0xF4]),
                "mode": 16,
                # segs_initial sets the BASE, not the selector.
                "segs_initial": {"DS": 0x10000, "ES": 0x30000},
                "ram_setup": [(0x10000, bytes([0x5A, 0xA5]) * 0x400)],
                "regs_initial": {1: 0x400, 6: 0x0000, 7: 0x0000},
                "regs_expected": {1: 0, 6: 0x800, 7: 0x800},
                "mem_expected": [(0x30000, bytes([0x5A, 0xA5]) * 8)],
                "expected_eip": 3,
            },
            {
                "name": "STD + REP MOVSW copies backwards",
                # STD ; REP MOVSW ; HLT. SI/DI start at the TOP of the block and
                # walk down, which is how a relocator moves an image without
                # overwriting itself. Getting the direction wrong here does not
                # corrupt the copy, it corrupts whatever sits ABOVE the
                # destination -- which for a self-relocating program is its own
                # code.
                "code": bytes([0xFD, 0xF3, 0xA5, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x10000, "ES": 0x30000},
                # Clear the destination first: the scratch wipe only covers
                # 0x200-0x3000, so an earlier case's data lingers here and would
                # satisfy the "untouched" check by accident.
                "ram_setup": [(0x30000, bytes(0x40)),
                              (0x10000, bytes(range(16)) * 2)],
                "regs_initial": {1: 8, 6: 0x1E, 7: 0x1E},
                # 8 words from offset 0x1E downward -> offsets 0x10..0x1F moved,
                # SI/DI end two below the last word touched.
                "regs_expected": {1: 0, 6: 0x0E, 7: 0x0E},
                "mem_expected": [(0x30010, bytes(range(16))),
                                 # nothing above the block may be touched
                                 (0x30020, bytes(8))],
                "expected_eip": 4,
            },
            {
                "name": "REP is ignored on a non-string opcode",
                # F3 B8 imm16 -> just MOV AX,imm16
                "code": bytes([0xF3, 0xB8, 0x34, 0x12, 0xF4]),
                "mode": 16,
                "regs_initial": {1: 5},
                "regs_expected": {0: 0x1234, 1: 5},
                "expected_eip": 5,
            },

            # --- REP: the batched fast path's boundaries ----------------------
            # .str_exit re-enters the string handler directly for up to
            # REP_BATCH_MAX (256) elements instead of rewinding EIP to the
            # prefix. These cases pin the edges of that batch: the fold granule
            # (the handler must still re-walk per element), the cap itself, and
            # the data-driven early exit, which must land on exactly the element
            # the per-element path would have stopped on.
            {
                "name": "REP MOVSW ends exactly on a fold granule boundary",
                # 0x40 words from 0x10F80 to 0x10FFF -- the last word ends on
                # 0x11000, so the batch finishes flush with the granule.
                "code": bytes([0xF3, 0xA5, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x10000, "ES": 0x30000},
                "ram_setup": [(0x30F80, bytes(0x80)),
                              (0x10F80, bytes(range(0x80)))],
                "regs_initial": {1: 0x40, 6: 0xF80, 7: 0xF80},
                "regs_expected": {1: 0, 6: 0x1000, 7: 0x1000},
                "mem_expected": [(0x30F80, bytes(range(0x80))),
                                 (0x31000, bytes(4))],
                "expected_eip": 3,
            },
            {
                "name": "STD + REP MOVSW walks backwards across a granule",
                # DF=1, starting above 0x11000 and walking down through it. The
                # batch must re-walk the fold per element in this direction too.
                "code": bytes([0xFD, 0xF3, 0xA5, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x10000, "ES": 0x30000},
                "ram_setup": [(0x30FF0, bytes(0x20)),
                              (0x10FF0, bytes(range(0x20)))],
                "regs_initial": {1: 0x10, 6: 0x100E, 7: 0x100E},
                "regs_expected": {1: 0, 6: 0xFEE, 7: 0xFEE},
                "mem_expected": [(0x30FF0, bytes(range(0x20)))],
                "expected_eip": 4,
            },
            {
                "name": "REP MOVSB with CX=1 copies exactly one byte",
                "code": bytes([0xF3, 0xA4, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x1000, "ES": 0x2000},
                "ram_setup": [(0x2020, bytes(4)),
                              (0x1010, bytes([0x5A, 0xA5]))],
                "regs_initial": {1: 1, 6: 0x10, 7: 0x20},
                "regs_expected": {1: 0, 6: 0x11, 7: 0x21},
                "mem_expected": [(0x2020, bytes([0x5A, 0x00]))],
                "expected_eip": 3,
            },
            {
                "name": "REP MOVSB spans several batches on a non-multiple count",
                # 300 > 256, so the cap forces a rewind mid-run, and 300 is not
                # a multiple of it -- an off-by-one in the cap shows up as a
                # short or long copy rather than as nothing at all.
                "code": bytes([0xF3, 0xA4, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x10000, "ES": 0x30000},
                "ram_setup": [(0x30000, bytes(320)),
                              (0x10000, bytes([(i * 7) & 0xFF
                                               for i in range(300)]))],
                "regs_initial": {1: 300, 6: 0, 7: 0},
                "regs_expected": {1: 0, 6: 300, 7: 300},
                "mem_expected": [(0x30000, bytes([(i * 7) & 0xFF
                                                  for i in range(300)])),
                                 (0x3012C, bytes(4))],   # nothing past the end
                "expected_eip": 3,
            },
            {
                "name": "REPNE SCASB terminates mid-batch on the match",
                # CX=512, match at the 300th element -- inside a batch, so the
                # ZF test between batched elements has to stop it there rather
                # than at the cap or at CX=0.
                "code": bytes([0xF2, 0xAE, 0xF4]),
                "mode": 16,
                "segs_initial": {"ES": 0x30000},
                "ram_setup": [(0x30000,
                               b"\x41" * 299 + b"\x42" + b"\x41" * 212)],
                "regs_initial": {0: 0x42, 1: 512, 7: 0},
                "regs_expected": {1: 512 - 300, 7: 300},
                "flags_expected": {"ZF": 1},
                "expected_eip": 3,
            },
            {
                "name": "REPE CMPSB stops on the first element after the cap",
                # The difference is at element 258, the first one that runs
                # after the 256-element cap has forced a rewind through the
                # prefix -- the seam between the fast path and the slow one.
                "code": bytes([0xF3, 0xA6, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x10000, "ES": 0x30000},
                "ram_setup": [(0x10000, b"A" * 512),
                              (0x30000, b"A" * 257 + b"B" + b"A" * 254)],
                "regs_initial": {1: 512, 6: 0, 7: 0},
                "regs_expected": {1: 512 - 258, 6: 258, 7: 258},
                "flags_expected": {"ZF": 0},
                "expected_eip": 3,
            },

            # --- hypercall ----------------------------------------------------
            {
                "name": "HLE: 0F 0B imm8 exits with reason and selector",
                "code": bytes([0x0F, 0x0B, 0x2A, 0xF4]),
                "mode": 16,
                "hle_enabled": 1,
                "regs_initial": {},
                "regs_expected": {},
                "expected_exit_reason": 2,   # X86_EXIT_HLE_INT
                "expected_exit_info": 0x2A,
                "expected_eip": 3,           # past the imm8: the stub's IRET runs on re-entry
            },
            {
                "name": "HLE: disabled, 0F 0B raises a real #UD",
                # Vector 6 -> 0x0000:0x5000, which halts. The pushed IP must
                # point AT the 0F 0B, not past it.
                "code": bytes([0x0F, 0x0B, 0x2A, 0xF4]),
                "mode": 16,
                "hle_enabled": 0,
                "segs_initial": {"SS": 0x0000},
                "regs_initial": {4: 0x3000},
                "regs_expected": {},
                "ram_setup": [(0x18, bytes([0x00, 0x50, 0x00, 0x00])),
                              (0x5000, bytes([0xF4]))],
                "mem_expected": [(0x2FFA, bytes([0x00, 0x00]))],
                "expected_eip": 0x5001,
            },

            # --- flag coverage: MUL / DIV -------------------------------------
            # CF and OF on a multiply mean "the result did not fit in the
            # destination", which is the only way a program can tell. SF/ZF/AF/PF
            # are architecturally undefined here, so nothing asserts them.
            {
                "name": "FLAGS: MUL r/m8 sets CF+OF when AH is non-zero",
                # MOV AL,0x10 ; MOV BL,0x10 ; MUL BL -> AX=0x0100
                "code": bytes([0xB0, 0x10, 0xB3, 0x10, 0xF6, 0xE3, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x0100, 3: 0x10},
                "flags_expected": {"CF": 1, "OF": 1},
                "expected_eip": 7,
            },
            {
                "name": "FLAGS: MUL r/m8 clears CF+OF when the result fits",
                # MOV AL,0x10 ; MOV BL,0x0F ; MUL BL -> AX=0x00F0
                "code": bytes([0xB0, 0x10, 0xB3, 0x0F, 0xF6, 0xE3, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x00F0, 3: 0x0F},
                "flags_expected": {"CF": 0, "OF": 0},
                "expected_eip": 7,
            },
            {
                "name": "FLAGS: MUL r/m16 reports overflow through DX",
                # MOV AX,0x1000 ; MOV BX,0x0010 ; MUL BX -> DX:AX = 0x0001_0000
                "code": bytes([0xB8, 0x00, 0x10, 0xBB, 0x10, 0x00,
                               0xF7, 0xE3, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x0000, 2: 0x0001, 3: 0x0010},
                "flags_expected": {"CF": 1, "OF": 1},
                "expected_eip": 9,
            },
            {
                "name": "IMUL r/m8 sign-extends the product",
                # MOV AL,0xFF (-1) ; MOV BL,0x02 ; IMUL BL -> AX = 0xFFFE (-2)
                "code": bytes([0xB0, 0xFF, 0xB3, 0x02, 0xF6, 0xEB, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0xFFFE, 3: 0x02},
                "expected_eip": 7,
            },
            {
                "name": "DIV r/m8 gives quotient in AL, remainder in AH",
                # MOV AX,23 ; MOV BL,5 ; DIV BL -> AL=4, AH=3
                "code": bytes([0xB8, 0x17, 0x00, 0xB3, 0x05, 0xF6, 0xF3, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x0304, 3: 0x05},
                "expected_eip": 8,
            },
            {
                "name": "IDIV r/m8 handles a negative dividend",
                # MOV AX,0xFFF6 (-10) ; MOV BL,3 ; IDIV BL -> AL=-3, AH=-1
                "code": bytes([0xB8, 0xF6, 0xFF, 0xB3, 0x03, 0xF6, 0xFB, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0xFFFD, 3: 0x03},
                "expected_eip": 8,
            },

            # --- flag coverage: NEG -------------------------------------------
            {
                "name": "FLAGS: 8-bit NEG sets CF for a non-zero operand",
                # MOV AL,1 ; NEG AL -> 0xFF
                "code": bytes([0xB0, 0x01, 0xF6, 0xD8, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x00FF},
                "flags_expected": {"CF": 1, "SF": 1, "ZF": 0},
                "expected_eip": 5,
            },
            {
                "name": "FLAGS: 8-bit NEG of zero clears CF",
                "code": bytes([0xB0, 0x00, 0xF6, 0xD8, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x0000},
                "flags_expected": {"CF": 0, "ZF": 1, "SF": 0},
                "expected_eip": 5,
            },

            # --- flag coverage: rotates ---------------------------------------
            {
                "name": "FLAGS: ROL r/m8,1 puts the rotated-out bit in CF",
                # MOV AL,0x81 ; ROL AL,1 -> 0x03, CF=1, OF = MSB(res)^CF = 1
                "code": bytes([0xB0, 0x81, 0xD0, 0xC0, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x0003},
                "flags_expected": {"CF": 1, "OF": 1},
                "expected_eip": 5,
            },
            {
                "name": "FLAGS: ROR r/m8,1 puts the rotated-out bit in CF",
                # MOV AL,0x01 ; ROR AL,1 -> 0x80, CF=1
                "code": bytes([0xB0, 0x01, 0xD0, 0xC8, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x0080},
                "flags_expected": {"CF": 1},
                "expected_eip": 5,
            },
            {
                "name": "FLAGS: RCL r/m8,1 rotates CF into the low bit",
                # STC ; MOV AL,0 ; RCL AL,1 -> 0x01, CF=0
                "code": bytes([0xF9, 0xB0, 0x00, 0xD0, 0xD0, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x0001},
                "flags_expected": {"CF": 0},
                "expected_eip": 6,
            },
            {
                "name": "FLAGS: RCR r/m8,1 rotates CF into the high bit",
                # STC ; MOV AL,0 ; RCR AL,1 -> 0x80, CF=0
                "code": bytes([0xF9, 0xB0, 0x00, 0xD0, 0xD8, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x0080},
                "flags_expected": {"CF": 0},
                "expected_eip": 6,
            },

            # --- flag coverage: shifts ----------------------------------------
            {
                "name": "FLAGS: SHL r/m8,1 CF and OF",
                # MOV AL,0x40 ; SHL AL,1 -> 0x80. CF=0, OF = MSB(res)^CF = 1
                "code": bytes([0xB0, 0x40, 0xD0, 0xE0, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x0080},
                "flags_expected": {"CF": 0, "OF": 1, "SF": 1},
                "expected_eip": 5,
            },
            {
                "name": "FLAGS: SHR r/m8,1 OF is the ORIGINAL msb",
                # MOV AL,0x80 ; SHR AL,1 -> 0x40, CF=0, OF=1
                "code": bytes([0xB0, 0x80, 0xD0, 0xE8, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x0040},
                "flags_expected": {"CF": 0, "OF": 1, "SF": 0},
                "expected_eip": 5,
            },
            {
                "name": "FLAGS: SAR r/m8,1 keeps the sign and clears OF",
                # MOV AL,0x80 ; SAR AL,1 -> 0xC0, CF=0, OF=0
                "code": bytes([0xB0, 0x80, 0xD0, 0xF8, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x00C0},
                "flags_expected": {"CF": 0, "OF": 0, "SF": 1},
                "expected_eip": 5,
            },
            {
                "name": "FLAGS: SHR r/m8,1 CF is the bit shifted out",
                # MOV AL,0x03 ; SHR AL,1 -> 0x01, CF=1
                "code": bytes([0xB0, 0x03, 0xD0, 0xE8, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x0001},
                "flags_expected": {"CF": 1},
                "expected_eip": 5,
            },
            {
                "name": "FLAGS: a shift of zero must not touch any flag",
                # STC ; MOV AL,0 ; MOV CL,0 ; SHL AL,CL
                # CF must survive, and ZF must NOT be set from the zero result.
                # This is the case the shift-dispatch bug hid behind for months.
                "code": bytes([0xF9, 0xB0, 0x00, 0xB1, 0x00, 0xD2, 0xE0, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x0000},
                "flags_expected": {"CF": 1, "ZF": 0},
                "expected_eip": 8,
            },
            {
                "name": "FLAGS: a rotate of zero must not touch any flag",
                # STC ; MOV AL,0x01 ; MOV CL,0 ; ROR AL,CL
                "code": bytes([0xF9, 0xB0, 0x01, 0xB1, 0x00, 0xD2, 0xC8, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x0001},
                "flags_expected": {"CF": 1},
                "expected_eip": 8,
            },
            {
                "name": "Shift counts are masked to 5 bits",
                # MOV AL,1 ; MOV CL,33 ; SHL AL,CL -> count 1, so AL=2
                "code": bytes([0xB0, 0x01, 0xB1, 0x21, 0xD2, 0xE0, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x0002},
                "expected_eip": 7,
            },
            {
                "name": "FLAGS: 32-bit SHL CF, ZF and OF together",
                # MOV EAX,0x80000000 ; SHL EAX,1 -> 0, CF=1, ZF=1, OF=0^1=1
                "code": bytes([0xB8, 0x00, 0x00, 0x00, 0x80, 0xD1, 0xE0, 0xF4]),
                "mode": 32,
                "regs_initial": {},
                "regs_expected": {0: 0x00000000},
                "flags_expected": {"CF": 1, "ZF": 1, "OF": 1, "SF": 0},
                "expected_eip": 8,
            },

            # --- PC speaker ---------------------------------------------------
            # The classic sequence: program PIT channel 2, then enable the gate
            # and data line on port 0x61. Checked through x86_io, which is what
            # the audio renderer reads.
            {
                "name": "SPK: OUT programs PIT ch2 and the speaker gate",
                # MOV AL,0xB6 ; OUT 0x43,AL      (ch2, lo/hi, square wave)
                # MOV AL,0x97 ; OUT 0x42,AL      (divisor lo)
                # MOV AL,0x04 ; OUT 0x42,AL      (divisor hi -> 0x0497 = 1175)
                # IN AL,0x61  ; OR AL,3 ; OUT 0x61,AL
                "code": bytes([0xB0, 0xB6, 0xE6, 0x43,
                               0xB0, 0x97, 0xE6, 0x42,
                               0xB0, 0x04, 0xE6, 0x42,
                               0xE4, 0x61, 0x0C, 0x03, 0xE6, 0x61,
                               0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {},
                "io_expected": {"pit2_reload": 0x0497, "port61_low2": 3,
                                "speaker_hz": 1193182 // 0x0497},
                "expected_eip": 19,
            },
            {
                "name": "SPK: clearing the gate silences it",
                # Program a divisor but leave port 0x61 at 0 -> no tone.
                "code": bytes([0xB0, 0xB6, 0xE6, 0x43,
                               0xB0, 0x97, 0xE6, 0x42,
                               0xB0, 0x04, 0xE6, 0x42,
                               0xB0, 0x00, 0xE6, 0x61,
                               0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {},
                "io_expected": {"pit2_reload": 0x0497, "speaker_hz": 0},
                "expected_eip": 17,
            },
            # The 8253's access modes are not "write the whole divisor three
            # different ways". A lo-byte-only write leaves the HIGH byte
            # standing and vice versa; zeroing the other half moves the tone by
            # octaves, which is what "the beeps are the wrong pitch" was.
            {
                "name": "SPK: lo-byte-only write keeps the high byte",
                # 0xB6 lo/hi -> 0x0497, then 0x90 (ch2, lo only) + 0x33.
                "code": bytes([0xB0, 0xB6, 0xE6, 0x43,
                               0xB0, 0x97, 0xE6, 0x42,
                               0xB0, 0x04, 0xE6, 0x42,
                               0xB0, 0x90, 0xE6, 0x43,
                               0xB0, 0x33, 0xE6, 0x42,
                               0xB0, 0x03, 0xE6, 0x61,
                               0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {},
                "io_expected": {"pit2_reload": 0x0433,
                                "speaker_hz": 1193182 // 0x0433},
                "expected_eip": 25,
            },
            {
                "name": "SPK: hi-byte-only write keeps the low byte",
                # 0xA0 is ch2, access mode 2 (high byte only).
                "code": bytes([0xB0, 0xB6, 0xE6, 0x43,
                               0xB0, 0x97, 0xE6, 0x42,
                               0xB0, 0x04, 0xE6, 0x42,
                               0xB0, 0xA0, 0xE6, 0x43,
                               0xB0, 0x02, 0xE6, 0x42,
                               0xB0, 0x03, 0xE6, 0x61,
                               0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {},
                "io_expected": {"pit2_reload": 0x0297,
                                "speaker_hz": 1193182 // 0x0297},
                "expected_eip": 25,
            },
            {
                "name": "SPK: a counter-latch command is not an access mode",
                # 0x80 has access field 0, which LATCHES the count for reading.
                # Storing it as the access mode reset the lo/hi pair phase, so
                # the high byte that followed landed in the low half and the
                # divisor came out 0x0404 instead of 0x0497.
                "code": bytes([0xB0, 0xB6, 0xE6, 0x43,
                               0xB0, 0x97, 0xE6, 0x42,
                               0xB0, 0x80, 0xE6, 0x43,
                               0xB0, 0x04, 0xE6, 0x42,
                               0xB0, 0x03, 0xE6, 0x61,
                               0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {},
                "io_expected": {"pit2_reload": 0x0497,
                                "speaker_hz": 1193182 // 0x0497},
                "expected_eip": 21,
            },
            {
                "name": "SPK: OUT DX,AL uses DX, not ESP",
                # DX=0x61, ESP=0x43 -- reading the wrong register would send the
                # byte to the PIT control port and leave the gate alone.
                "code": bytes([0xEE, 0xF4]),
                "mode": 16,
                "regs_initial": {0: 0x03, 2: 0x61, 4: 0x43},
                "regs_expected": {},
                "io_expected": {"port61_low2": 3},
                "expected_eip": 2,
            },

            # --- keyboard ------------------------------------------------------
            # The controller is modelled at the port level, so these work the
            # same in protected mode. A game reading 0x60 directly needs none of
            # the BIOS layer above it.
            {
                "name": "KBD: port 0x60 returns the queued scancode",
                # IN AL,0x60 ; HLT
                "code": bytes([0xE4, 0x60, 0xF4]),
                "mode": 16,
                "kbd_queue": [0x1E],          # 'a' make code
                "regs_initial": {},
                "regs_expected": {0: 0x001E},
                "expected_eip": 3,
            },
            {
                "name": "KBD: port 0x64 reports the output buffer full",
                # IN AL,0x64 ; HLT -> bit0 set (data waiting) and bit2 set
                "code": bytes([0xE4, 0x64, 0xF4]),
                "mode": 16,
                "kbd_queue": [0x1E],
                "regs_initial": {},
                "regs_expected": {0: 0x0005},
                "expected_eip": 3,
            },
            {
                "name": "KBD: port 0x64 reports empty when nothing is queued",
                "code": bytes([0xE4, 0x64, 0xF4]),
                "mode": 16,
                "kbd_queue": [],
                "regs_initial": {},
                "regs_expected": {0: 0x0004},
                "expected_eip": 3,
            },
            {
                "name": "KBD: reads drain the queue in order",
                # IN AL,0x60 ; MOV BL,AL ; IN AL,0x60 ; HLT
                "code": bytes([0xE4, 0x60, 0x88, 0xC3, 0xE4, 0x60, 0xF4]),
                "mode": 16,
                "kbd_queue": [0x1E, 0x30],
                "regs_initial": {},
                "regs_expected": {0: 0x0030, 3: 0x001E},
                "expected_eip": 7,
            },
            {
                "name": "IRQ: a pending interrupt is delivered in-guest",
                # NOP ; NOP ; HLT, with IRQ 1 pending and the boundary pulled
                # forward. Vector 9 points at 0000:0x3000, which halts, so
                # landing there proves delivery rather than the fallthrough.
                "code": bytes([0x90, 0x90, 0xF4]),
                "mode": 16,
                "segs_initial": {"SS": 0x0000},
                "regs_initial": {4: 0x2000},
                "regs_expected": {},
                "eflags_initial": 0x0200,          # IF set
                "irq_pending": 0x09,
                "pit_counter": 1,
                "ram_setup": [(0x24, bytes([0x00, 0x30, 0x00, 0x00])),
                              (0x3000, bytes([0xF4]))],
                "expected_eip": 0x3001,
            },
            {
                "name": "IRQ: delivery waits while IF is clear",
                # Same, but interrupts masked. The boundary still arrives, and
                # the interpreter exits on the timer with EIP untouched instead
                # of vectoring. What this pins down is that it did NOT reach
                # 0x3001, which is where an implementation ignoring IF lands.
                "code": bytes([0x90, 0x90, 0xF4]),
                "mode": 16,
                "segs_initial": {"SS": 0x0000},
                "regs_initial": {4: 0x2000},
                "regs_expected": {},
                "eflags_initial": 0x0000,          # IF clear
                "irq_pending": 0x09,
                "pit_counter": 1,
                "ram_setup": [(0x24, bytes([0x00, 0x30, 0x00, 0x00])),
                              (0x3000, bytes([0xF4]))],
                "expected_exit_reason": 3,      # X86_EXIT_TIMER, not a vector
                "expected_eip": 0,
            },
            {
                "name": "TOPBENCH relocator prologue computes its copy state",
                # The literal 42 bytes of TOPBENCH's Borland self-relocator,
                # from the STD up to (not including) the REP MOVSW, run with
                # the exact state the real program reaches: DS=PSP=0x71,
                # CS=0x2E92 (the relocated copy), DX=0x1529 (image paragraphs).
                #
                # This is where the program dies on the target, and every
                # instruction in it is a candidate: 8C reading a selector, a
                # shift by CL at 16 bits, INC/DEC on a HIGH byte register, and
                # 16-bit SUB feeding a segment load. Running the real sequence
                # pins all of them at once, and DF has to survive to the end or
                # the copy runs forwards and eats the code above its
                # destination -- which is exactly the observed corruption.
                #
                #   fd        std
                #   8c db     mov bx,ds       53      push bx
                #   83 c3 39  add bx,0x39     03 da   add bx,dx
                #   8c cd     mov bp,cs
                #   8b c2     mov ax,dx       80 e4 0f and ah,0x0f
                #   b1 04     mov cl,4
                #   8b f2     mov si,dx       d3 e6   shl si,cl
                #   d3 ea     shr dx,cl       fe c6   inc dh
                #   8b ce     mov cx,si       d1 e9   shr cx,1
                #   4e 4e     dec si; dec si  8b fe   mov di,si
                #   2b e8     sub bp,ax       2b d8   sub bx,ax
                #   8e c5     mov es,bp       8e db   mov ds,bx
                "code": bytes([
                    0xFD, 0x8C, 0xDB, 0x53, 0x83, 0xC3, 0x39, 0x03, 0xDA,
                    0x8C, 0xCD, 0x8B, 0xC2, 0x80, 0xE4, 0x0F, 0xB1, 0x04,
                    0x8B, 0xF2, 0xD3, 0xE6, 0xD3, 0xEA, 0xFE, 0xC6, 0x8B,
                    0xCE, 0xD1, 0xE9, 0x4E, 0x4E, 0x8B, 0xFE, 0x2B, 0xE8,
                    0x2B, 0xD8, 0x8E, 0xC5, 0x8E, 0xDB, 0xF4]),
                "mode": 16,
                "sels_initial": {"DS": 0x0071, "CS": 0x2E92},
                "regs_initial": {2: 0x1529, 4: 0x2000},
                "regs_expected": {
                    0: 0x0529,      # AX  = DX & 0x0FFF
                    1: 0x2948,      # CX  = (DX<<4)>>1, all at 16 bits
                    2: 0x0252,      # DX  = (DX>>4) with DH incremented
                    3: 0x10AA,      # BX  = DS+0x39+DX - AX
                    5: 0x2969,      # BP  = CS - AX
                    6: 0x528E,      # SI  = (DX<<4) - 2
                    7: 0x528E,      # DI  = SI
                },
                "flags_expected": {"DF": 1},
                "expected_eip": 43,
            },
            {
                "name": "FLAGS: INC r/m8 wrapping to zero sets ZF and keeps CF",
                # STC ; MOV DH,0xFF ; INC DH ; HLT. The FE group is the only
                # INC/DEC that takes a ModRM, and it set no flags at all --
                # which is invisible until something branches on the result.
                "code": bytes([0xF9, 0xB6, 0xFF, 0xFE, 0xC6, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {2: 0x0000},
                "flags_expected": {"ZF": 1, "SF": 0, "OF": 0, "PF": 1,
                                   "AF": 1, "CF": 1},
                "expected_eip": 6,
            },
            {
                "name": "FLAGS: DEC r/m8 to zero sets ZF, and below it sets SF",
                # MOV DH,1 ; DEC DH ; HLT
                "code": bytes([0xB6, 0x01, 0xFE, 0xCE, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {2: 0x0000},
                "flags_expected": {"ZF": 1, "SF": 0, "OF": 0},
                "expected_eip": 5,
            },
            {
                "name": "FLAGS: DEC r/m8 through zero sets SF and clears ZF",
                # MOV DH,0 ; DEC DH ; HLT -> DH = 0xFF
                "code": bytes([0xB6, 0x00, 0xFE, 0xCE, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {2: 0xFF00},
                "flags_expected": {"ZF": 0, "SF": 1, "OF": 0},
                "expected_eip": 5,
            },
            {
                "name": "DEC r/m8 terminates the loop it controls",
                # MOV DH,3 ; (loop) DEC DH ; JNE loop ; HLT -- TOPBENCH's
                # relocator counts its 64K chunks exactly like this. Without
                # flags on DEC the branch reads a stale ZF and the loop never
                # ends, which is how a relocator walks off into memory it does
                # not own.
                "code": bytes([0xB6, 0x03, 0xFE, 0xCE, 0x75, 0xFC, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {2: 0x0000},
                "flags_expected": {"ZF": 1},
                "expected_eip": 7,
            },

            # ---- The stack, through a NON-ZERO SS.base -----------------------
            # Every case below puts SS.base at 0x20000 and the data at a small
            # SP, so a handler that uses raw ESP as a linear address reads and
            # writes near zero instead. That was the state of thirteen of them:
            # correct only on a flat stack at base 0, which is the one case a
            # real-mode DOS program never has. They also hard-coded a 4-byte
            # slot, so a 16-bit push moved SP by twice what it should.
            {
                "name": "STACK: PUSH Ev (FF /6) goes through SS.base at 16 bits",
                # MOV BX,0x1234 ; PUSH BX (FF F3) ; HLT
                "code": bytes([0xBB, 0x34, 0x12, 0xFF, 0xF3, 0xF4]),
                "mode": 16,
                "segs_initial": {"SS": 0x20000},
                "ram_setup": [(0x20000, bytes(0x20))],
                "regs_initial": {4: 0x0010},
                "regs_expected": {3: 0x1234, 4: 0x000E},
                "mem_expected": [(0x2000E, bytes([0x34, 0x12]))],
                "expected_eip": 6,
            },
            {
                "name": "STACK: CALL near indirect (FF /2) is CS-relative",
                # CS.base is 0x1000 here, so a handler that treats the operand
                # as a LINEAR address lands 0x1000 bytes short, and one that
                # pushes the linear EIP makes the matching RET land 0x1000 too
                # far. MOV BX,8 ; CALL BX ; (at CS:8) HLT
                "code": bytes([0xF4]),
                "mode": 16,
                "segs_initial": {"SS": 0x20000, "CS": 0x1000},
                "ram_setup": [(0x20000, bytes(0x20)),
                              (0x1000, (bytes([0xBB, 0x08, 0x00, 0xFF, 0xD3])
                                        + bytes(3) + bytes([0xF4])))],
                "regs_initial": {4: 0x0010},
                "regs_expected": {3: 0x0008, 4: 0x000E},
                # the pushed return address is the OFFSET after the CALL (5),
                # not the linear EIP
                "mem_expected": [(0x2000E, bytes([0x05, 0x00]))],
                "expected_eip": 9,
            },
            {
                "name": "STACK: PUSHF/POPF round-trip on a based stack",
                # STC ; PUSHF ; CLC ; POPF ; HLT -> CF comes back set, and SP
                # returns to where it started.
                "code": bytes([0xF9, 0x9C, 0xF8, 0x9D, 0xF4]),
                "mode": 16,
                "segs_initial": {"SS": 0x20000},
                "ram_setup": [(0x20000, bytes(0x20))],
                "regs_initial": {4: 0x0010},
                "regs_expected": {4: 0x0010},
                "flags_expected": {"CF": 1},
                "expected_eip": 5,
            },
            {
                "name": "STACK: PUSH imm16 (68) and POP Ev (8F)",
                # PUSH 0xBEEF ; POP BX (8F C3) ; HLT
                "code": bytes([0x68, 0xEF, 0xBE, 0x8F, 0xC3, 0xF4]),
                "mode": 16,
                "segs_initial": {"SS": 0x20000},
                "ram_setup": [(0x20000, bytes(0x20))],
                "regs_initial": {4: 0x0010},
                "regs_expected": {3: 0xBEEF, 4: 0x0010},
                "expected_eip": 6,
            },
            {
                "name": "STACK: RET imm16 (C2) returns and unwinds arguments",
                # The return offset and two argument words are planted by hand;
                # C2 0004 must return to 0x0008 and leave SP past both.
                "code": bytes([0xC2, 0x04, 0x00]) + bytes(5) + bytes([0xF4]),
                "mode": 16,
                "segs_initial": {"SS": 0x20000},
                "ram_setup": [(0x20000, bytes(0x20)),
                              (0x2000A, bytes([0x08, 0x00,      # return offset
                                               0x11, 0x11,      # arg
                                               0x22, 0x22]))],
                "regs_initial": {4: 0x000A},
                "regs_expected": {4: 0x0010},
                "expected_eip": 9,
            },
            {
                "name": "STACK: PUSH imm8 (6A) sign-extends into one word",
                # PUSH -2 ; POP BX ; HLT
                "code": bytes([0x6A, 0xFE, 0x8F, 0xC3, 0xF4]),
                "mode": 16,
                "segs_initial": {"SS": 0x20000},
                "ram_setup": [(0x20000, bytes(0x20))],
                "regs_initial": {4: 0x0010},
                "regs_expected": {3: 0xFFFE, 4: 0x0010},
                "expected_eip": 5,
            },
            {
                "name": "STACK: RETF imm16 (CA) installs CS and unwinds",
                # IP=0x0008, CS=0x0300, then one argument word. CB does this
                # correctly; CA popped CS and discarded it.
                "code": bytes([0xCA, 0x02, 0x00]),
                "mode": 16,
                "segs_initial": {"SS": 0x20000},
                "ram_setup": [(0x20000, bytes(0x20)),
                              (0x2000A, bytes([0x08, 0x00,      # IP
                                               0x00, 0x03,      # CS
                                               0x99, 0x99])),   # argument
                              (0x3008, bytes([0xF4]))],
                "regs_initial": {4: 0x000A},
                "regs_expected": {4: 0x0010},
                "expected_eip": 9,
            },

            # ---- 16-bit width on the operand FETCH, not just the core -------
            {
                "name": "SHR at 16 bits must not pull in the register's high half",
                # MOV CL,1 ; SHR BP,CL ; HLT, with EBP dirty above bit 15.
                # A 32-bit LDR of the operand shifts bit 16 down into bit 15,
                # so the low half comes out wrong even though the shift core
                # itself is width-aware. 0x00011234 >> 1 is 0x891A at 32 bits
                # and 0x091A at 16.
                "code": bytes([0xB1, 0x01, 0xD3, 0xED, 0xF4]),
                "mode": 16,
                "regs_initial": {5: 0x00011234},
                "regs_expected": {5: 0x0001091A},
                "expected_eip": 5,
            },
            {
                "name": "SHR r/m16,1 (D1 /5) likewise, and CF is the bit lost",
                "code": bytes([0xD1, 0xED, 0xF4]),
                "mode": 16,
                "regs_initial": {5: 0xFFFF8001},
                "regs_expected": {5: 0xFFFF4000},
                "flags_expected": {"CF": 1},
                "expected_eip": 3,
            },
            {
                "name": "SHR r/m16,imm8 (C1 /5) likewise",
                "code": bytes([0xC1, 0xED, 0x04, 0xF4]),
                "mode": 16,
                "regs_initial": {5: 0x00011234},
                "regs_expected": {5: 0x00010123},
                "expected_eip": 4,
            },
            {
                "name": "XCHG AX,r16 (0x95) swaps only the low halves",
                # This is the decompressor's own idiom -- LODSW then XCHG BP,AX
                # to refill a 16-bit bit buffer. Swapping 32 bits moves EAX's
                # high half into EBP, and the SHR that reads the buffer then
                # shifts that garbage down into BP.
                "code": bytes([0x95, 0xF4]),
                "mode": 16,
                "regs_initial": {0: 0x11112222, 5: 0x33334444},
                "regs_expected": {0: 0x11114444, 5: 0x33332222},
                "expected_eip": 2,
            },
            {
                "name": "XCHG Ev,Gv (0x87) swaps only the low halves",
                # 87 D9 = XCHG CX,BX
                "code": bytes([0x87, 0xD9, 0xF4]),
                "mode": 16,
                "regs_initial": {1: 0xAAAA1111, 3: 0xBBBB2222},
                "regs_expected": {1: 0xAAAA2222, 3: 0xBBBB1111},
                "expected_eip": 3,
            },
            {
                "name": "STD + REP MOVSW walks SI/DI through the 16-bit wrap",
                # The relocator's second pass starts at SI=DI=0xFFFE and copies
                # DOWN through offset 0, so the index registers wrap from 0x0000
                # back to 0xFFFE mid-run. Existing coverage never crossed that
                # point. STD ; REP MOVSW ; HLT with the segments based at
                # 0x10000/0x30000 and six words to move from offset 4.
                "code": bytes([0xFD, 0xF3, 0xA5, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x10000, "ES": 0x30000},
                "ram_setup": [(0x30000, bytes(8)), (0x3FFF8, bytes(8)),
                              (0x10000, bytes([0x11, 0x11, 0x22, 0x22,
                                               0x33, 0x33])),
                              (0x1FFFA, bytes([0x44, 0x44, 0x55, 0x55,
                                               0x66, 0x66]))],
                "regs_initial": {1: 6, 6: 0x0004, 7: 0x0004},
                # six words from offset 4 downward: 4, 2, 0, then 0xFFFE,
                # 0xFFFC, 0xFFFA. The run must not stop at the wrap.
                "regs_expected": {1: 0, 6: 0xFFF8, 7: 0xFFF8},
                "mem_expected": [(0x30000, bytes([0x11, 0x11, 0x22, 0x22,
                                                  0x33, 0x33])),
                                 (0x3FFFA, bytes([0x44, 0x44, 0x55, 0x55,
                                                  0x66, 0x66]))],
                "expected_eip": 4,
            },

            # ---- TEST, and the two 16-bit sign extensions -------------------
            {
                "name": "FLAGS: TEST AL,imm8 (A8) sets ZF/PF and clears CF/OF",
                # STC ; MOV AL,0x0F ; TEST AL,0xF0 ; HLT. A8 set no flags at
                # all, so anything branching on a bit test fell through.
                "code": bytes([0xF9, 0xB0, 0x0F, 0xA8, 0xF0, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x0F},   # TEST must not write the operand
                "flags_expected": {"ZF": 1, "SF": 0, "CF": 0, "OF": 0, "PF": 1},
                "expected_eip": 6,
            },
            {
                "name": "FLAGS: TEST AL,imm8 sets SF on a set high bit",
                "code": bytes([0xB0, 0x80, 0xA8, 0x80, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x80},
                "flags_expected": {"ZF": 0, "SF": 1},
                "expected_eip": 5,
            },
            {
                "name": "FLAGS: TEST Eb,Gb (84) sets ZF",
                # MOV AL,0x0F ; MOV BL,0xF0 ; TEST AL,BL ; HLT
                "code": bytes([0xB0, 0x0F, 0xB3, 0xF0, 0x84, 0xD8, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x0F, 3: 0xF0},
                "flags_expected": {"ZF": 1, "CF": 0, "OF": 0},
                "expected_eip": 7,
            },
            {
                "name": "FLAGS: TEST AX,imm16 (A9) sets ZF at 16 bits",
                "code": bytes([0xB8, 0x0F, 0x0F, 0xA9, 0xF0, 0xF0, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x0F0F},
                "flags_expected": {"ZF": 1, "CF": 0, "OF": 0},
                "expected_eip": 7,
            },
            {
                "name": "CBW (98) sign-extends AL into AX, not AX into EAX",
                # 0x98/0x99 are emitted for both passes but had fixed 32-bit
                # bodies, so 16-bit CBW/CWD executed CWDE/CDQ.
                "code": bytes([0xB8, 0x80, 0x00, 0x98, 0xF4]),
                "mode": 16,
                "regs_initial": {0: 0},
                "regs_expected": {0: 0xFF80},
                "expected_eip": 5,
            },
            {
                "name": "CWD (99) fills DX from the sign of AX",
                "code": bytes([0xB8, 0x00, 0x80, 0x99, 0xF4]),
                "mode": 16,
                "regs_initial": {2: 0},
                "regs_expected": {0: 0x8000, 2: 0xFFFF},
                "expected_eip": 5,
            },

            # ---- near Jcc and the LOOP family at 16 bits -------------------
            {
                "name": "0F 8x near Jcc takes a rel16 at 16-bit operand size",
                # XOR AX,AX (sets ZF) ; JZ rel16 +3 ; 3 filler ; HLT.
                # The handler read a hard-coded 4-byte displacement, so it
                # consumed two bytes too many and jumped by a number assembled
                # from the following instruction.
                "code": bytes([0x31, 0xC0, 0x0F, 0x84, 0x03, 0x00,
                               0x90, 0x90, 0x90, 0xF4]),
                "mode": 16,
                "regs_initial": {0: 1},
                "regs_expected": {0: 0},
                "expected_eip": 10,
            },
            {
                "name": "0F 8x not taken advances by exactly four bytes",
                # XOR AX,AX ; JNZ rel16 (not taken) ; HLT at offset 6
                "code": bytes([0x31, 0xC0, 0x0F, 0x85, 0x40, 0x00, 0xF4]),
                "mode": 16,
                "regs_initial": {0: 1},
                "regs_expected": {0: 0},
                "expected_eip": 7,
            },
            {
                "name": "LOOP counts in CX, not ECX, at 16-bit address size",
                # E2 FE loops to itself. ECX carries a dirty high half: a
                # 32-bit counter runs 0x10003 times and leaves ECX zero, a
                # correct one runs three and leaves the high half alone.
                "code": bytes([0xE2, 0xFE, 0xF4]),
                "mode": 16,
                "regs_initial": {1: 0x00010003},
                "regs_expected": {1: 0x00010000},
                "expected_eip": 3,
            },
            {
                "name": "JCXZ tests CX, not ECX",
                # ECX high half set, CX zero -> the branch must be TAKEN and
                # skip the HLT sitting in the shadow.
                "code": bytes([0xE3, 0x01, 0xF4, 0x90, 0xF4]),
                "mode": 16,
                "regs_initial": {1: 0x00010000},
                "regs_expected": {1: 0x00010000},
                "expected_eip": 5,
            },
            {
                "name": "FLAGS: DEC r16 AF is the borrow out of bit 3",
                # MOV DX,0x10 ; DEC DX ; HLT -> 0x0F, which borrows from bit 4.
                # The AF aux was ~a rather than a^b, which inverts the bit the
                # macro reads, so AF came out exactly backwards.
                "code": bytes([0xBA, 0x10, 0x00, 0x4A, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {2: 0x000F},
                "flags_expected": {"AF": 1, "ZF": 0, "SF": 0, "OF": 0, "PF": 1},
                "expected_eip": 5,
            },
            {
                "name": "FLAGS: DEC r16 leaves AF clear when bit 3 does not borrow",
                # 0x12 -> 0x11 borrows nothing out of bit 3.
                "code": bytes([0xBA, 0x12, 0x00, 0x4A, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {2: 0x0011},
                "flags_expected": {"AF": 0},
                "expected_eip": 5,
            },
            {
                "name": "FLAGS: INC r16 AF is the carry out of bit 3",
                # 0x0F + 1 carries out of bit 3.
                "code": bytes([0xB8, 0x0F, 0x00, 0x40, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x0010},
                "flags_expected": {"AF": 1, "ZF": 0},
                "expected_eip": 5,
            },
            {
                "name": "FLAGS: SHR r/m16,1 PF is the parity of the LOW BYTE",
                # The exact values TOPBENCH's bit-stream decoder reaches:
                # BP = 0xA05B, SHR BP,1 -> 0x502D. Low byte 0x2D has four bits
                # set, so PF is 1; CF is the bit shifted out; OF is the
                # ORIGINAL msb.
                "code": bytes([0xBD, 0x5B, 0xA0, 0xD1, 0xED, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {5: 0x502D},
                "flags_expected": {"CF": 1, "PF": 1, "SF": 0, "ZF": 0, "OF": 1},
                "expected_eip": 6,
            },
            {
                "name": "LEA yields the OFFSET, not the linear address",
                # MOV DI,0x7FEB ; LEA BX,[DI-0x2000] ; HLT, with DS based at
                # 0x19690. The ModRM decoder folds the segment base into the
                # address it returns because that is what a memory operand
                # needs; LEA must take it back out. TOPBENCH's unpacker uses
                # exactly this to turn an overflowing DI into a segment bump,
                # and with the base left in it advanced ES by 0x967 paragraphs
                # instead of 0x5FE.
                "code": bytes([0xBF, 0xEB, 0x7F, 0x8D, 0x9D, 0x00, 0xE0, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x19690},
                "regs_initial": {},
                "regs_expected": {3: 0x5FEB, 7: 0x7FEB},
                "expected_eip": 8,
            },
            {
                "name": "LEA wraps its result at the address size",
                # MOV DI,4 ; LEA BX,[DI-0x10] ; HLT -> 0xFFF4, not 0x0FFFF4.
                "code": bytes([0xBF, 0x04, 0x00, 0x8D, 0x9D, 0xF0, 0xFF, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x19690},
                "regs_initial": {3: 0xAAAA0000},
                "regs_expected": {3: 0xAAAAFFF4},
                "expected_eip": 8,
            },

            # ---- MOV moffs: address size, and the segment base --------------
            {
                "name": "MOV AL,moffs8 (A0) is DS-relative and takes a moffs16",
                # A0 reads the offset with a fixed 32-bit load and uses it as a
                # LINEAR address. At 16-bit address size it must read two bytes
                # and add DS.base -- the old form both mis-decoded the
                # instruction length and indexed the fold table with garbage.
                "code": bytes([0xA0, 0x40, 0x00, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x10000},
                "ram_setup": [(0x10040, bytes([0x5A]))],
                "regs_initial": {0: 0x1100},
                "regs_expected": {0: 0x115A},
                "expected_eip": 4,
            },
            {
                "name": "MOV moffs8,AL (A2) writes through DS",
                "code": bytes([0xA2, 0x50, 0x00, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x10000},
                "ram_setup": [(0x10050, bytes(4))],
                "regs_initial": {0: 0x0099},
                "regs_expected": {0: 0x0099},
                "mem_expected": [(0x10050, bytes([0x99, 0x00]))],
                "expected_eip": 4,
            },
            {
                "name": "MOV AX,moffs16 (A1) is DS-relative",
                "code": bytes([0xA1, 0x60, 0x00, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x10000},
                "ram_setup": [(0x10060, bytes([0x34, 0x12]))],
                "regs_initial": {0: 0xAAAA0000},
                "regs_expected": {0: 0xAAAA1234},
                "expected_eip": 4,
            },
            {
                "name": "MOV moffs16,AX (A3) writes through DS",
                "code": bytes([0xA3, 0x70, 0x00, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x10000},
                "ram_setup": [(0x10070, bytes(4))],
                "regs_initial": {0: 0xBEEF},
                "regs_expected": {0: 0xBEEF},
                "mem_expected": [(0x10070, bytes([0xEF, 0xBE, 0x00, 0x00]))],
                "expected_eip": 4,
            },
            {
                "name": "MOV AL,moffs8 honours a segment override",
                # 26 A0 40 00 -> ES:[0x0040]
                "code": bytes([0x26, 0xA0, 0x40, 0x00, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x10000, "ES": 0x30000},
                "ram_setup": [(0x10040, bytes([0x5A])),
                              (0x30040, bytes([0xC7]))],
                "regs_initial": {},
                "regs_expected": {0: 0x00C7},
                "expected_eip": 5,
            },

            # ---- the register-form-only handlers ---------------------------
            # Six handlers decoded a ModRM and then exited if it named memory.
            # Every case here uses mod=00 rm=111, i.e. [BX], through a DS based
            # at 0x10000. `MOV WORD PTR [BX],imm` is what stopped TOPBENCH once
            # it reached its real entry point.
            {
                "name": "MOV Ev,imm16 (C7) memory form",
                # MOV BX,0x400 ; MOV WORD PTR [BX],0x1234 ; HLT
                "code": bytes([0xBB, 0x00, 0x04, 0xC7, 0x07, 0x34, 0x12, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x10000},
                "ram_setup": [(0x10400, bytes(4))],
                "regs_initial": {},
                "regs_expected": {3: 0x400},
                "mem_expected": [(0x10400, bytes([0x34, 0x12, 0x00, 0x00]))],
                "expected_eip": 8,
            },
            {
                "name": "MOV Eb,imm8 (C6) memory form",
                "code": bytes([0xBB, 0x10, 0x04, 0xC6, 0x07, 0x5A, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x10000},
                "ram_setup": [(0x10410, bytes(4))],
                "regs_initial": {},
                "regs_expected": {3: 0x410},
                "mem_expected": [(0x10410, bytes([0x5A, 0x00]))],
                "expected_eip": 7,
            },
            {
                "name": "TEST Ev,Gv (85) memory form sets ZF",
                # MOV BX,0x420 ; MOV AX,0xF0F0 ; TEST [BX],AX ; HLT
                "code": bytes([0xBB, 0x20, 0x04, 0xB8, 0xF0, 0xF0,
                               0x85, 0x07, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x10000},
                "ram_setup": [(0x10420, bytes([0x0F, 0x0F]))],
                "regs_initial": {},
                "regs_expected": {0: 0xF0F0, 3: 0x420},
                "flags_expected": {"ZF": 1, "CF": 0, "OF": 0},
                "mem_expected": [(0x10420, bytes([0x0F, 0x0F]))],
                "expected_eip": 9,
            },
            {
                "name": "XCHG Ev,Gv (87) memory form",
                "code": bytes([0xBB, 0x30, 0x04, 0xB8, 0xEF, 0xBE,
                               0x87, 0x07, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x10000},
                "ram_setup": [(0x10430, bytes([0x11, 0x22]))],
                "regs_initial": {},
                "regs_expected": {0: 0x2211, 3: 0x430},
                "mem_expected": [(0x10430, bytes([0xEF, 0xBE]))],
                "expected_eip": 9,
            },
            {
                "name": "XCHG Eb,Gb (86) memory form",
                "code": bytes([0xBB, 0x40, 0x04, 0xB0, 0x99,
                               0x86, 0x07, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x10000},
                "ram_setup": [(0x10440, bytes([0x77, 0x00]))],
                "regs_initial": {},
                "regs_expected": {0: 0x0077, 3: 0x440},
                "mem_expected": [(0x10440, bytes([0x99, 0x00]))],
                "expected_eip": 8,
            },
            {
                "name": "POP Ev (8F) memory form",
                # MOV BX,0x450 ; POP WORD PTR [BX] ; HLT, with 0xCAFE on a
                # stack based at 0x20000.
                "code": bytes([0xBB, 0x50, 0x04, 0x8F, 0x07, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x10000, "SS": 0x20000},
                "ram_setup": [(0x10450, bytes(4)),
                              (0x20010, bytes([0xFE, 0xCA]))],
                "regs_initial": {4: 0x0010},
                "regs_expected": {3: 0x450, 4: 0x0012},
                "mem_expected": [(0x10450, bytes([0xFE, 0xCA, 0x00, 0x00]))],
                "expected_eip": 6,
            },
            {
                "name": "MOV Ew,Sreg (8C) memory form stores the SELECTOR",
                # MOV BX,0x460 ; MOV [BX],DS ; HLT. The handler read the
                # selector into R5 before calling the ModRM decoder, which uses
                # R4 and R5 as scratch -- so it stored the decoder's leftovers.
                # Exactly the defect MOV Ev,Gv had.
                "code": bytes([0xBB, 0x60, 0x04, 0x8C, 0x1F, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x10000},
                "sels_initial": {"DS": 0x1234},
                "ram_setup": [(0x10460, bytes(4))],
                "regs_initial": {},
                "regs_expected": {3: 0x460},
                "mem_expected": [(0x10460, bytes([0x34, 0x12, 0x00, 0x00]))],
                "expected_eip": 6,
            },
            {
                "name": "TEST Eb,Gb (84) memory form sets ZF",
                # MOV BX,0x470 ; MOV AL,0xF0 ; TEST [BX],AL ; HLT. 0x85 got its
                # memory form; its 8-bit twin did not, and TOPBENCH stops dead
                # on it right after printing its banner.
                "code": bytes([0xBB, 0x70, 0x04, 0xB0, 0xF0, 0x84, 0x07, 0xF4]),
                "mode": 16,
                "segs_initial": {"DS": 0x10000},
                "ram_setup": [(0x10470, bytes([0x0F, 0x00]))],
                "regs_initial": {},
                "regs_expected": {0: 0x00F0, 3: 0x470},
                "flags_expected": {"ZF": 1, "CF": 0, "OF": 0},
                "mem_expected": [(0x10470, bytes([0x0F, 0x00]))],
                "expected_eip": 8,
            },
            {
                "name": "MOV CRn,r32 and MOV r32,CRn (0F 22 / 0F 20)",
                # 0F 22 C0 = MOV CR0,EAX ; 0F 20 C3 = MOV EBX,CR0 ; HLT.
                # A 386 is detected by reading CR0 -- a 286 has no such
                # instruction -- so a CPU benchmark runs this before it runs
                # anything else. Both are always 32-bit whatever the operand
                # size, and the ModRM's mod field is ignored. EAX is 0x10 (ET),
                # deliberately not 0x11: setting PE would switch the machine to
                # protected mode, which segment loads still trap on.
                "code": bytes([0x0F, 0x22, 0xC0, 0x0F, 0x20, 0xC3, 0xF4]),
                "mode": 16,
                "regs_initial": {0: 0x00000010, 3: 0xDEADBEEF},
                "regs_expected": {0: 0x00000010, 3: 0x00000010},
                "expected_eip": 7,
            },
            {
                "name": "MOV r32,CR0 reads the whole 32 bits at 16-bit size",
                # No operand-size prefix, 16-bit code: the destination is still
                # written at 32 bits, so the high half of ECX must be replaced,
                # not preserved.
                "code": bytes([0x0F, 0x22, 0xC0, 0x0F, 0x20, 0xC1, 0xF4]),
                "mode": 16,
                "regs_initial": {0: 0x00000010, 1: 0xFFFFFFFF},
                "regs_expected": {1: 0x00000010},
                "expected_eip": 7,
            },

            # ---- BCD adjust: DAA, DAS, AAA, AAS ----------------------------
            {
                "name": "DAA adjusts the low nibble",
                # MOV AL,0x1A ; DAA ; HLT -> 0x20, AF set, no carry.
                "code": bytes([0xB0, 0x1A, 0x27, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x0020},
                "flags_expected": {"AF": 1, "CF": 0, "ZF": 0},
                "expected_eip": 4,
            },
            {
                "name": "DAA carries out of the high nibble",
                # 0x9A adjusts to 0xA0, then past 0x99 to 0x00 with CF set.
                "code": bytes([0xB0, 0x9A, 0x27, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x0000},
                "flags_expected": {"CF": 1, "AF": 1, "ZF": 1},
                "expected_eip": 4,
            },
            {
                "name": "DAS adjusts the low nibble down",
                "code": bytes([0xB0, 0x1B, 0x2F, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x0015},
                "flags_expected": {"AF": 1, "CF": 0},
                "expected_eip": 4,
            },
            {
                "name": "DAS borrows into the high nibble",
                # 0x05 with AF set: 0x05-6 borrows, so CF comes out set.
                "code": bytes([0xB0, 0x05, 0x2F, 0xF4]),
                "mode": 16,
                "eflags_initial": 0x0010,       # AF
                "regs_initial": {},
                "regs_expected": {0: 0x00FF},
                "flags_expected": {"CF": 1, "AF": 1},
                "expected_eip": 4,
            },
            {
                "name": "AAA carries the unpacked digit into AH",
                # MOV AX,0x000B ; AAA ; HLT -> AH=1, AL=1
                "code": bytes([0xB8, 0x0B, 0x00, 0x37, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x0101},
                "flags_expected": {"CF": 1, "AF": 1},
                "expected_eip": 5,
            },
            {
                "name": "AAA leaves AH alone when no adjust is needed",
                "code": bytes([0xB8, 0x05, 0x03, 0x37, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0x0305},
                "flags_expected": {"CF": 0, "AF": 0},
                "expected_eip": 5,
            },
            {
                "name": "AAS borrows the unpacked digit from AH",
                # MOV AX,0x000B ; AAS ; HLT -> AX - 6 = 0x0005, AH - 1 = 0xFF
                "code": bytes([0xB8, 0x0B, 0x00, 0x3F, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {0: 0xFF05},
                "flags_expected": {"CF": 1, "AF": 1},
                "expected_eip": 5,
            },

            # ---- FF /3 far CALL and FF /5 far JMP through memory ------------
            # The pointer is IN MEMORY: offset first, then selector, each at
            # the ADDRESS the ModRM decode produced. In a 16-bit segment that
            # is m16:16; in a 32-bit one it is m16:32, so the offset is four
            # bytes wide and the selector sits at +4, not at +2.
            {
                "name": "FAR: CALL FAR m16:16 (FF /3) switches CS and pushes CS:IP",
                # FF 1E 00 03 = CALL FAR [0x0300]; the pointer there is
                # 0100:0000, whose linear target 0x1000 holds a HLT.
                "code": bytes([0xFF, 0x1E, 0x00, 0x03, 0xF4]),
                "mode": 16,
                "segs_initial": {"SS": 0x20000},
                "ram_setup": [(0x20000, bytes(0x20)),
                              (0x300, bytes([0x00, 0x00, 0x00, 0x01])),
                              (0x1000, bytes([0xF4]))],
                "regs_initial": {4: 0x0010},
                "regs_expected": {4: 0x000C},
                # SP 0x10 -> push CS at 0x0E -> push IP at 0x0C
                "mem_expected": [(0x2000C, bytes([0x04, 0x00, 0x00, 0x00]))],
                "expected_eip": 1,
            },
            {
                "name": "FAR: JMP FAR m16:16 (FF /5) switches CS and leaves SP alone",
                # FF 2E 00 03 = JMP FAR [0x0300]
                "code": bytes([0xFF, 0x2E, 0x00, 0x03, 0xF4]),
                "mode": 16,
                "segs_initial": {"SS": 0x20000},
                "ram_setup": [(0x20000, bytes(0x20)),
                              (0x300, bytes([0x00, 0x00, 0x00, 0x01])),
                              (0x1000, bytes([0xF4]))],
                "regs_initial": {4: 0x0010},
                "regs_expected": {4: 0x0010},
                "mem_expected": [(0x2000C, bytes(4))],
                "expected_eip": 1,
            },
            {
                "name": "FAR: CALL FAR m16:16 to F000:FFF0, the BIOS entry point",
                # The exact transfer TOPBENCH's detection unit makes. Guest
                # page 0xFF is aliased into test_ram so the target is real
                # memory rather than a fault, and it holds a HLT.
                "code": bytes([0xFF, 0x1E, 0x00, 0x03, 0xF4]),
                "mode": 16,
                "segs_initial": {"SS": 0x20000},
                "page_map": [(0xFF, 0x70000)],
                "ram_setup": [(0x20000, bytes(0x20)),
                              (0x300, bytes([0xF0, 0xFF, 0x00, 0xF0])),
                              (0x70FF0, bytes([0xF4]))],
                "regs_initial": {4: 0x0010},
                "regs_expected": {4: 0x000C},
                "mem_expected": [(0x2000C, bytes([0x04, 0x00, 0x00, 0x00]))],
                "expected_eip": 0xFFF1,
            },
            {
                "name": "FAR: JMP FAR m16:16 to F000:FFF0, the BIOS entry point",
                "code": bytes([0xFF, 0x2E, 0x00, 0x03, 0xF4]),
                "mode": 16,
                "segs_initial": {"SS": 0x20000},
                "page_map": [(0xFF, 0x70000)],
                "ram_setup": [(0x20000, bytes(0x20)),
                              (0x300, bytes([0xF0, 0xFF, 0x00, 0xF0])),
                              (0x70FF0, bytes([0xF4]))],
                "regs_initial": {4: 0x0010},
                "regs_expected": {4: 0x0010},
                "expected_eip": 0xFFF1,
            },
            {
                "name": "FAR: CALL FAR m16:32 (FF /3) takes a 32-BIT offset",
                # 32-bit segment: FF 1D 00 03 00 00 = CALL FAR [0x00000300].
                # The pointer there is 00012000 / 0000: a FOUR-byte offset with
                # the selector at +4. An LDRH offset keeps only 0x2000, and a
                # selector read at +2 picks up 0x0001 from the offset's own
                # high half -- CS.base 0x10, EIP 0x2000, i.e. control taken to
                # an address built out of the target rather than from it. The
                # high half is deliberately non-zero so the two cannot agree.
                "code": bytes([0xF4]),
                "mode": 32,
                "segs_initial": {"SS": 0x20000, "CS": 0x1000},
                "sels_initial": {"CS": 0x0100},
                "ram_setup": [(0x20000, bytes(0x20)),
                              (0x300, bytes([0x00, 0x20, 0x01, 0x00,
                                             0x00, 0x00])),
                              (0x1000, bytes([0xFF, 0x1D, 0x00, 0x03,
                                              0x00, 0x00])),
                              (0x12000, bytes([0xF4]))],
                "regs_initial": {4: 0x00000020},
                "regs_expected": {4: 0x00000018},
                # push CS (dword) at 0x1C, push EIP (dword) at 0x18
                "mem_expected": [(0x20018, bytes([0x06, 0x00, 0x00, 0x00,
                                                  0x00, 0x01, 0x00, 0x00]))],
                "expected_eip": 0x12001,
            },
            {
                "name": "FAR: JMP FAR m16:32 (FF /5) takes a 32-BIT offset",
                "code": bytes([0xF4]),
                "mode": 32,
                "segs_initial": {"SS": 0x20000, "CS": 0x1000},
                "sels_initial": {"CS": 0x0100},
                "ram_setup": [(0x20000, bytes(0x20)),
                              (0x300, bytes([0x00, 0x20, 0x01, 0x00,
                                             0x00, 0x00])),
                              (0x1000, bytes([0xFF, 0x2D, 0x00, 0x03,
                                              0x00, 0x00])),
                              (0x12000, bytes([0xF4]))],
                "regs_initial": {4: 0x00000020},
                "regs_expected": {4: 0x00000020},
                "mem_expected": [(0x20018, bytes(8))],
                "expected_eip": 0x12001,
            },
            {
                # The register form of /3 and /5 does not exist: the operand is
                # a pointer IN MEMORY. Dispatching it anyway made the far
                # handlers read CS:IP through R7, the effective address left
                # over from an EARLIER instruction's ModRM decode -- here the
                # MOV BX,[0x0300] in front of it, so the far pointer would come
                # from 0x0300 and the run would continue at 0100:0000 as if the
                # encoding had been valid. It must trap instead.
                "name": "FAR: FF /3 register form (FF DB) is #UD, not a far call",
                # MOV BX,[0x0300] ; FF DB ; HLT
                "code": bytes([0x8B, 0x1E, 0x00, 0x03, 0xFF, 0xDB, 0xF4]),
                "mode": 16,
                "segs_initial": {"SS": 0x20000},
                "ram_setup": [(0x20000, bytes(0x20)),
                              (0x300, bytes([0x00, 0x00, 0x00, 0x01])),
                              (0x1000, bytes([0xF4]))],
                "regs_initial": {4: 0x0010},
                "regs_expected": {4: 0x0010},
                "expected_exit_reason": 4,
                "expected_eip": 6,
            },
            {
                "name": "FAR: FF /5 register form (FF EB) is #UD, not a far jump",
                "code": bytes([0x8B, 0x1E, 0x00, 0x03, 0xFF, 0xEB, 0xF4]),
                "mode": 16,
                "segs_initial": {"SS": 0x20000},
                "ram_setup": [(0x20000, bytes(0x20)),
                              (0x300, bytes([0x00, 0x00, 0x00, 0x01])),
                              (0x1000, bytes([0xF4]))],
                "regs_initial": {4: 0x0010},
                "regs_expected": {4: 0x0010},
                "expected_exit_reason": 4,
                "expected_eip": 6,
            },
        ])

        # ------------------------------------------------------------------
        # THE CPU-DETECTION PREAMBLE every C runtime runs before main().
        #
        # MORTAL_KOMBAT died on three consecutive walls of it (docs/13): SMSW,
        # the x87 escapes, and 0F BA / CPUID. None is exotic; all three are what
        # a Microsoft or Borland startup issues in its first few hundred
        # instructions, so these cases guard a path every future title takes.
        #
        # expected_eip is measured PAST the trailing HLT, as everywhere else in
        # this file -- except the #UD case, which stops at the offending opcode.
        # ------------------------------------------------------------------
        test_cases.extend([
            {
                # The REGISTER form is the whole point: 0F 01's ModRM walk was
                # unconditional, so mod=11 fell through the group into #UD.
                "name": "SMSW: 0F 01 E0 reads CR0 into AX, high half untouched",
                "code": bytes([0x0F, 0x01, 0xE0, 0xF4]),
                "regs_initial": {0: 0xFFFFFFFF},
                "regs_expected": {0: 0xFFFF0000},   # real mode: CR0 == 0
                "expected_eip": 4,
            },
            {
                "name": "SMSW: memory form stores a HALFWORD, disp16",
                # SMSW [0x0300] ; HLT
                "code": bytes([0x0F, 0x01, 0x26, 0x00, 0x03, 0xF4]),
                "mode": 16,
                "ram_setup": [(0x300, bytes([0xFF, 0xFF, 0xFF, 0xFF]))],
                "mem_expected": [(0x300, bytes([0x00, 0x00, 0xFF, 0xFF]))],
                "regs_initial": {},
                "regs_expected": {},
                "expected_eip": 6,
            },
            # --------------------------------------------------------------
            # LMSW, and the EXIT REPORTING that hid it.
            #
            # Three unrelated titles (Dark Forces, Mortal Kombat, Epic
            # Pinball) were recorded as dying on "unimplemented opcode 0x0F"
            # at one shared address. Two reporting defects made that reading:
            # the 0F escape handler left R0 = 0x0F, so .exit_loop named the
            # ESCAPE rather than the opcode; and 0F 01 is a GROUP, so even the
            # right byte would not have said which member. What two of the
            # three were really executing is 0F 01 F0, LMSW AX -- the 286
            # entry into protected mode. docs/13.
            #
            # These arms therefore grade BOTH: that LMSW does what the
            # references say, and that an exit NAMES what it refused.
            # --------------------------------------------------------------
            {
                "name": "LMSW: 0F 01 F0 writes the low 4 bits of CR0",
                # MOV AX,0x000A ; LMSW AX ; SMSW BX ; HLT
                "code": bytes([0xB8, 0x0A, 0x00, 0x0F, 0x01, 0xF0,
                               0x0F, 0x01, 0xE3, 0xF4]),
                "mode": 16,
                "regs_initial": {0: 0, 3: 0},
                "regs_expected": {0: 0x000A, 3: 0x000A},
                "expected_eip": 10,
            },
            {
                # tiny386 i386.c:3292 and DOSBox-X cpu.cpp:2790 both mask the
                # source to 4 bits. 0xFFFE, not 0xFFFF, so this arm does not
                # also turn PE on -- that is the arm below.
                "name": "LMSW: everything above bit 3 of the source is discarded",
                # MOV AX,0xFFFE ; LMSW AX ; SMSW BX ; HLT
                "code": bytes([0xB8, 0xFE, 0xFF, 0x0F, 0x01, 0xF0,
                               0x0F, 0x01, 0xE3, 0xF4]),
                "mode": 16,
                "regs_initial": {0: 0, 3: 0},
                "regs_expected": {0: 0xFFFE, 3: 0x000E},
                "expected_eip": 10,
            },
            {
                # "PE bit, stuck on, cannot exit protected mode, 286 style"
                # (DOSBox-X cpu.cpp:2792). tiny386's mask `(~0xf)|1` says the
                # same thing. Clearing PE is MOV CR0's job, not LMSW's.
                "name": "LMSW: PE is SET-ONLY -- a second LMSW of 0 cannot clear it",
                # MOV AX,1 ; LMSW AX ; XOR AX,AX ; LMSW AX ; SMSW BX ; HLT
                "code": bytes([0xB8, 0x01, 0x00, 0x0F, 0x01, 0xF0,
                               0x31, 0xC0, 0x0F, 0x01, 0xF0,
                               0x0F, 0x01, 0xE3, 0xF4]),
                "mode": 16,
                "regs_initial": {0: 0, 3: 0},
                "regs_expected": {3: 0x0001},
                "expected_eip": 15,
            },
            {
                # THE REPORTING ARM. Without the fix this reads 0x0F for every
                # unimplemented two-byte opcode there is, which is exactly how
                # three titles appeared to share one blocker.
                "name": "EXIT NAMES THE OPCODE: an unimplemented 0F nn reports 0x0Fnn",
                "code": bytes([0x0F, 0x0A, 0xF4]),
                "regs_initial": {},
                "regs_expected": {},
                "expected_exit_reason": 4,
                "expected_exit_info": 0x0F0A,
                "expected_eip": 2,
            },
            {
                # ...and a GROUP member carries its ModRM, because "0F 01 is
                # unimplemented" is false -- /2, /3, /4 and now /6 are not.
                "name": "EXIT NAMES THE GROUP MEMBER: 0F 01 /7 reports the ModRM too",
                # INVLPG [0x0300] -- mod=00 rm=110, still unimplemented
                "code": bytes([0x0F, 0x01, 0x3E, 0x00, 0x03, 0xF4]),
                "mode": 16,
                "regs_initial": {},
                "regs_expected": {},
                "expected_exit_reason": 4,
                "expected_exit_info": 0x3E0F01,
                "expected_eip": 5,
            },
            # --------------------------------------------------------------
            # 0F AF -- IMUL Gv,Ev. EPIC PINBALL's actual wall, and the proof
            # that the three titles never shared one: with the exit naming the
            # opcode it reports 0x0FAF where the other two report 0x0F01.
            # --------------------------------------------------------------
            {
                "name": "IMUL Gv,Ev: 0F AF register form multiplies into the destination",
                # MOV AX,7 ; MOV BX,6 ; IMUL AX,BX ; HLT
                "code": bytes([0xB8, 0x07, 0x00, 0xBB, 0x06, 0x00,
                               0x0F, 0xAF, 0xC3, 0xF4]),
                "mode": 16,
                "regs_initial": {0: 0, 3: 0},
                "regs_expected": {0: 42, 3: 6},
                "expected_eip": 10,
            },
            {
                "name": "IMUL Gv,Ev: 0F AF memory form, disp16",
                # MOV AX,9 ; IMUL AX,[0x0300] ; HLT
                "code": bytes([0xB8, 0x09, 0x00, 0x0F, 0xAF, 0x06,
                               0x00, 0x03, 0xF4]),
                "mode": 16,
                "ram_setup": [(0x300, bytes([0x05, 0x00]))],
                "regs_initial": {0: 0},
                "regs_expected": {0: 45},
                "expected_eip": 9,
            },
            {
                # Two's complement through the low half: -3 * 5 = -15 = 0xFFF1
                # in 16 bits. The handler zero-extends its loads, which is
                # correct precisely because only the LOW half is written back.
                "name": "IMUL Gv,Ev: 0F AF is correct in the low half for a NEGATIVE result",
                # MOV AX,0xFFFD ; MOV BX,5 ; IMUL AX,BX ; HLT
                "code": bytes([0xB8, 0xFD, 0xFF, 0xBB, 0x05, 0x00,
                               0x0F, 0xAF, 0xC3, 0xF4]),
                "mode": 16,
                "regs_initial": {0: 0, 3: 0},
                "regs_expected": {0: 0xFFF1, 3: 5},
                "expected_eip": 10,
            },
            {
                "name": "x87 ABSENT: DB E3 (FNINIT) is ignored, not #UD",
                "code": bytes([0xDB, 0xE3, 0xF4]),
                "regs_initial": {0: 0x12345678},
                "regs_expected": {0: 0x12345678},
                "expected_eip": 3,
            },
            {
                # The only real work in .op_esc_x87 is stepping over the
                # operand. If the ModRM+disp16 is not consumed the INC never
                # runs and EIP lands mid-instruction.
                "name": "x87 ABSENT: DD /6 [disp16] consumes its operand, stores nothing",
                # FNSTSW [0x0300] ; INC AX ; HLT
                "code": bytes([0xDD, 0x36, 0x00, 0x03, 0x40, 0xF4]),
                "mode": 16,
                "ram_setup": [(0x300, bytes([0xFF, 0xFF]))],
                "mem_expected": [(0x300, bytes([0xFF, 0xFF]))],
                "regs_initial": {0: 0x00000041},
                "regs_expected": {0: 0x00000042},
                "expected_eip": 6,
            },
            {
                "name": "BT: 0F BA /4 sets CF from the bit and writes nothing",
                "code": bytes([0x0F, 0xBA, 0xE0, 0x05, 0xF4]),   # BT EAX,5
                "regs_initial": {0: 0x00000020},
                "regs_expected": {0: 0x00000020},
                "flags_expected": {"CF": 1},
                "expected_eip": 5,
            },
            {
                "name": "BT: a clear bit clears CF",
                "code": bytes([0x0F, 0xBA, 0xE0, 0x04, 0xF4]),   # BT EAX,4
                "regs_initial": {0: 0x00000020},
                "regs_expected": {0: 0x00000020},
                "flags_expected": {"CF": 0},
                "expected_eip": 5,
            },
            {
                # The imm8 form takes the offset MODULO the operand size, so 37
                # is bit 5. Getting this wrong shifts by 37 and reads zero.
                "name": "BT: the imm8 bit number is taken modulo 32",
                "code": bytes([0x0F, 0xBA, 0xE0, 0x25, 0xF4]),   # BT EAX,37
                "regs_initial": {0: 0x00000020},
                "regs_expected": {0: 0x00000020},
                "flags_expected": {"CF": 1},
                "expected_eip": 5,
            },
            {
                "name": "BTS: 0F BA /5 sets the bit, CF = its old value",
                "code": bytes([0x0F, 0xBA, 0xE8, 0x05, 0xF4]),   # BTS EAX,5
                "regs_initial": {0: 0x00000001},
                "regs_expected": {0: 0x00000021},
                "flags_expected": {"CF": 0},
                "expected_eip": 5,
            },
            {
                "name": "BTR: 0F BA /6 clears the bit, CF = its old value",
                "code": bytes([0x0F, 0xBA, 0xF0, 0x05, 0xF4]),   # BTR EAX,5
                "regs_initial": {0: 0x000000FF},
                "regs_expected": {0: 0x000000DF},
                "flags_expected": {"CF": 1},
                "expected_eip": 5,
            },
            {
                # Twice, which is exactly what a runtime does to EFLAGS.ID: the
                # second toggle must put the bit back.
                "name": "BTC: 0F BA /7 toggles, and toggling twice restores",
                # BTC EAX,5 ; BTC EAX,5 ; HLT
                "code": bytes([0x0F, 0xBA, 0xF8, 0x05,
                               0x0F, 0xBA, 0xF8, 0x05, 0xF4]),
                "regs_initial": {0: 0x00000000},
                "regs_expected": {0: 0x00000000},
                "flags_expected": {"CF": 1},   # the SECOND read saw it set
                "expected_eip": 9,
            },
            {
                "name": "BTS: memory form, disp32",
                # BTS DWORD [0x0300],5 ; HLT
                "code": bytes([0x0F, 0xBA, 0x2D, 0x00, 0x03, 0x00, 0x00,
                               0x05, 0xF4]),
                "ram_setup": [(0x300, bytes([0x01, 0x00, 0x00, 0x00]))],
                "mem_expected": [(0x300, bytes([0x21, 0x00, 0x00, 0x00]))],
                "regs_initial": {},
                "regs_expected": {},
                "flags_expected": {"CF": 0},
                "expected_eip": 9,
            },
            {
                "name": "BT group: /0 is an undefined encoding and stays #UD",
                "code": bytes([0x0F, 0xBA, 0xC0, 0x05, 0xF4]),
                "regs_initial": {0: 0x00000020},
                "regs_expected": {0: 0x00000020},
                "expected_exit_reason": 4,
                "expected_eip": 3,
            },
            {
                "name": "CPUID: leaf 0 returns 1 and GenuineIntel",
                "code": bytes([0x0F, 0xA2, 0xF4]),
                "regs_initial": {0: 0},
                "regs_expected": {0: 1, 3: 0x756E6547,
                                  2: 0x49656E69, 1: 0x6C65746E},
                "expected_eip": 3,
            },
            {
                # Family 4 and NO feature bits -- EDX bit 0 is the on-chip FPU,
                # and it must agree with the x87 cases above.
                "name": "CPUID: leaf 1 is a 486SX signature with no FPU bit",
                "code": bytes([0x0F, 0xA2, 0xF4]),
                "regs_initial": {0: 1, 2: 0xFFFFFFFF},
                "regs_expected": {0: 0x00000400, 2: 0},
                "expected_eip": 3,
            },
        ])

        all_passed = True

        only = getattr(args, "only", None)
        for test_num, tc in enumerate(test_cases, 1):
            if only and test_num not in only:
                continue
            print(f"Running Test {test_num}: {tc['name']}")
            
            # Clear the data scratch region. Without this a value left by an
            # earlier case can sit at the address a later case expects to be
            # written, and the case passes without the CPU doing anything.
            if "ram_setup" in tc or "mem_expected" in tc:
                backend.write_memory(ram_addr + 0x200, bytes(0x2E00))

            # Setup custom RAM if needed
            if "ram_setup" in tc:
                for addr, data in tc["ram_setup"]:
                    backend.write_memory(ram_addr + addr, data)

            # Alias a guest page somewhere the harness's 752 KB of test_ram
            # cannot reach on its own. A far transfer to F000:FFF0 is the BIOS
            # entry point at linear 0xFFF F0, above the identity-mapped range,
            # and without a mapping the case would only ever measure .mem_fault.
            # Entries are reverted after the case so nothing leaks forward.
            page_map = tc.get("page_map", [])
            for guest_page, ram_off in page_map:
                fold = ((ram_addr + ram_off) - (guest_page << 12)) & 0xFFFFFFFF
                backend.write_uint32(page_table_addr + guest_page * 4, fold)

            # Write program to test_ram
            backend.write_memory(ram_addr, tc["code"])
            
            # Reset CPU state (EIP=0, clear all regs, then set initials)
            backend.write_uint32(dtcm_addr + 0, 0)   # eip
            backend.write_uint32(dtcm_addr + 4, tc.get("eflags_initial", 0))  # eflags
            for i in range(8):
                backend.write_uint32(dtcm_addr + 8 + i * 4, tc["regs_initial"].get(i, 0))
            # Reset the whole segment cache, not just the bases. Selectors
            # used to survive from case to case, which was invisible while
            # nothing read them -- now that INT/RETF/MOV Sreg install a
            # selector and derive base = sel<<4 from it, a leftover CS.sel
            # silently relocates a later case's code.
            for idx, seg in enumerate(SEG_ORDER):
                off = SEG_BASE_OFFSETS[seg] - 4      # start of the descriptor
                flags = CS_FLAG_D if (seg == "CS" and tc.get("mode", 32) == 32) else 0
                backend.write_uint32(dtcm_addr + off, flags << 16)   # sel=0, flags
                backend.write_uint32(dtcm_addr + off + 4, 0)         # base
            for seg_name, base in tc.get("segs_initial", {}).items():
                backend.write_uint32(dtcm_addr + SEG_BASE_OFFSETS[seg_name], base)
            # Selectors are a separate axis from the base. `MOV r16,Sreg` reads
            # the SELECTOR, so a case exercising real-mode segment arithmetic
            # has to seed it -- segs_initial alone leaves it zero and the
            # arithmetic then runs on the wrong number.
            for seg_name, sel in tc.get("sels_initial", {}).items():
                off = SEG_BASE_OFFSETS[seg_name] - 4
                flags = CS_FLAG_D if (seg_name == "CS"
                                      and tc.get("mode", 32) == 32) else 0
                backend.write_uint32(dtcm_addr + off, (flags << 16) | sel)

            backend.write_uint32(dtcm_addr + CR0_OFFSET, 0)  # real mode
            backend.write_uint32(dtcm_addr + HLE_ENABLED_OFFSET,
                                 tc.get("hle_enabled", 0))

            # Keyboard queue lives in x86_io, not the CPU struct.
            q = tc.get("kbd_queue", [])
            backend.write_memory(io_addr + IO_KBD_QUEUE, bytes(16))
            if q:
                backend.write_memory(io_addr + IO_KBD_QUEUE, bytes(q))
            # head, tail, last -- tail = how many codes are queued
            backend.write_memory(io_addr + IO_KBD_HEAD,
                                 bytes([0, len(q) & 15, 0]))
            backend.write_uint32(dtcm_addr + 40, 0)   # idtr_base
            # A pending IRQ is delivered at the next boundary, so a case that
            # wants one also pulls the PIT counter forward to reach it.
            irq = tc.get("irq_pending", 0)
            backend.write_uint32(dtcm_addr + HARD_INT_OFFSET, irq)
            backend.write_uint32(dtcm_addr + PIT_COUNTER_OFFSET,
                                 tc.get("pit_counter", 0xFFFFFFFF))
            backend.write_uint32(dtcm_addr + 2196, 0)  # exit_reason
            
            # Trigger run
            backend.write_uint32(run_flag_addr, 1)
            
            # Poll for completion
            timeout = 10
            while timeout > 0:
                if backend.read_uint32(run_flag_addr) == 0:
                    break
                time.sleep(0.05)
                timeout -= 1
                
            for guest_page, _ in page_map:
                backend.write_uint32(page_table_addr + guest_page * 4,
                                     0x80000000)

            if timeout == 0:
                print(f"  -> TIMEOUT: CPU loop did not finish!")
                all_passed = False
                continue

            # Check results
            eip = backend.read_uint32(dtcm_addr + 0)
            passed = eip == tc["expected_eip"]
            
            for reg, expected_val in tc["regs_expected"].items():
                actual_val = backend.read_uint32(dtcm_addr + 8 + reg * 4)
                if actual_val != expected_val:
                    passed = False
                    print(f"  -> FAIL: Reg {reg} was {actual_val}, expected {expected_val}")

            # Only the flags a case names are compared, so each case pins down
            # exactly what it means to and nothing more.
            if "flags_expected" in tc:
                eflags = backend.read_uint32(dtcm_addr + 4)
                for flag_name, expected_bit in tc["flags_expected"].items():
                    bit = FLAG_BITS[flag_name]
                    actual_bit = (eflags >> bit) & 1
                    if actual_bit != expected_bit:
                        passed = False
                        print(f"  -> FAIL: {flag_name} was {actual_bit}, expected "
                              f"{expected_bit} (eflags=0x{eflags:08x})")

            for addr, expected_bytes in tc.get("mem_expected", []):
                actual = bytes(backend.read_memory(ram_addr + addr,
                                                   len(expected_bytes)))
                if actual != expected_bytes:
                    passed = False
                    print(f"  -> FAIL: mem[0x{addr:x}] was {actual.hex()}, "
                          f"expected {expected_bytes.hex()}")

            if "expected_exit_reason" in tc:
                got = backend.read_uint32(dtcm_addr + EXIT_REASON_OFFSET)
                if got != tc["expected_exit_reason"]:
                    passed = False
                    print(f"  -> FAIL: exit_reason was {got}, expected "
                          f"{tc['expected_exit_reason']}")
            if "expected_exit_info" in tc:
                got = backend.read_uint32(dtcm_addr + EXIT_INFO_OFFSET)
                if got != tc["expected_exit_info"]:
                    passed = False
                    print(f"  -> FAIL: exit_info was 0x{got:x}, expected "
                          f"0x{tc['expected_exit_info']:x}")

            if "io_expected" in tc:
                io = read_io_state(backend, io_addr)
                for key, want in tc["io_expected"].items():
                    got = io[key]
                    if got != want:
                        passed = False
                        print(f"  -> FAIL: io.{key} was {got}, expected {want}")

            eflags_now = backend.read_uint32(dtcm_addr + 4)
            exit_reason = backend.read_uint32(dtcm_addr + 2196)
            print(f"  -> {'PASS' if passed else 'FAIL'}: EIP: {eip} "
                  f"(Expected: {tc['expected_eip']}) eflags=0x{eflags_now:08x} "
                  f"exit={exit_reason}")
            
            if not passed:
                all_passed = False
                
        print("Halting...")
        backend.halt()
        
        if all_passed:
            print("All tests passed!")
        return True
        
    finally:
        runner.stop()

if __name__ == "__main__":
    import sys
    import argparse
    from scripts.common.runner import add_runner_args
    parser = argparse.ArgumentParser(description="Run opcodes tests")
    add_runner_args(parser)
    parser.add_argument("--only", type=int, nargs="+", metavar="N",
                        help="run only these test numbers (1-based)")
    args = parser.parse_args()
    success = run_test(args)
    sys.exit(0 if success else 1)
