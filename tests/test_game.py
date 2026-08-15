#!/usr/bin/env python3
import time
import sys
import subprocess
import os
import argparse

# Add the project root to sys.path to allow importing scripts.common
sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))
from scripts.common.runner import build_and_create_runner, add_runner_args
from scripts.common.elf import get_symbols
from scripts.common.faults import check_fault

def run_benchmark(args):
    runner = build_and_create_runner(args)
    runner.start()
    
    syms = get_symbols("test_core.elf")
    dtcm_addr = syms.get("cpu_state_dtcm")
    ram_addr = syms.get("test_ram")
    run_flag_addr = syms.get("run_flag")
    
    backend = runner.backend
    
    try:
        # Trigger C-side benchmark
        run_benchmark_flag_addr = syms.get("run_benchmark_flag")
        benchmark_cycles_addr = syms.get("benchmark_cycles")
        
        benchmark_filesize_addr = syms.get("benchmark_filesize")
        
        print("Waiting for boot...")
        time.sleep(1)

        # The guest image lives in memory-mapped external flash, which needs
        # OCTOSPI setup the bare-metal firmware does not do. The load is
        # therefore host-triggered rather than done at reset -- doing it at
        # reset bus-faults on silicon while passing under gwemu, which models
        # extflash as plain RAM. Only ask for it where extflash is known good.
        if args.target != "qemu":
            print("NOTE: extflash requires OCTOSPI configuration that this "
                  "firmware does not perform; the guest load will fault on "
                  "hardware. See docs/04.")
        backend.write_uint32(syms["load_guest_flag"], 1)

        deadline = 50
        while deadline > 0 and backend.read_uint32(syms["load_guest_flag"]) != 0:
            time.sleep(0.1)
            deadline -= 1
        if check_fault(backend, syms):
            return False

        load_status = backend.read_uint32(benchmark_filesize_addr)
        if load_status == 0x1337:
            print("MZ Executable loaded successfully!")
        else:
            print(f"Failed to load MZ. Status: 0x{load_status:08X}")
            return False


        print("Starting game loop...")
        run_benchmark_flag_addr = syms.get("run_benchmark_flag")
        backend.write_uint32(run_benchmark_flag_addr, 2)
        
        print("Game running... monitoring...")
        dtcm_addr = syms.get("cpu_state_dtcm")
        debug_log_idx_addr = syms.get("debug_log_idx")
        debug_log_addr = syms.get("debug_log")
        
        last_idx = 0
        
        for _ in range(10): # Monitor for 10 seconds
            time.sleep(1)
            eip = backend.read_uint32(dtcm_addr)
            idx = backend.read_uint32(debug_log_idx_addr)
            exit_reason = backend.read_uint32(dtcm_addr + 2196)
            
            print(f"EIP: 0x{eip:08X} | Logged INTs: {idx} | Exit Reason: {exit_reason}")
            
            # Dump memory around EIP
            if eip > 0:
                cs_base = backend.read_uint32(dtcm_addr + 48 + 1*12 + 4) # segs[1].base
                test_ram_addr = syms.get("test_ram")
                if test_ram_addr:
                    linear_addr = cs_base + eip
                    print(f"CS.base = 0x{cs_base:08X}, Linear EIP = 0x{linear_addr:08X}")
                    try:
                        bytes_at_eip = backend.read_memory(test_ram_addr + linear_addr - 16, 32)
                        print("Bytes around EIP:")
                        print(" ".join(f"{b:02X}" for b in bytes_at_eip))
                        
                        bytes_at_cs0 = backend.read_memory(test_ram_addr + cs_base, 16)
                        print("Bytes at CS:0000:")
                        print(" ".join(f"{b:02X}" for b in bytes_at_cs0))
                    except Exception as e:
                        print("Could not read memory:", e)
            
            if idx > last_idx:
                for i in range(last_idx, min(idx, 256)):
                    val = backend.read_uint32(debug_log_addr + i * 4)
                    int_num = (val >> 16) & 0xFF
                    ah = (val >> 8) & 0xFF
                    al = val & 0xFF
                    print(f"  INT {int_num:02X}h, AH={ah:02X}h, AL={al:02X}h")
                last_idx = idx
        
        backend.halt() 
    except KeyboardInterrupt:
        print("Benchmark aborted.")
    finally:
        runner.stop()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Run CPU Benchmark")
    add_runner_args(parser)
    args = parser.parse_args()
    run_benchmark(args)
