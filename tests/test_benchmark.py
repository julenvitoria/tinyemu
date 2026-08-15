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
        
        print("Starting C-side benchmark (1,000,000 instructions)...")
        backend.write_uint32(run_benchmark_flag_addr, 1)
        
        # Wait for completion
        while backend.read_uint32(run_benchmark_flag_addr) != 0:
            time.sleep(0.01)
            
        print("Benchmark complete! Reading results...")
        
        ebx = backend.read_uint32(dtcm_addr + 8 + 3*4)
        cycles = backend.read_uint32(benchmark_cycles_addr)
        
        # We initialized EAX to 500,000. Each loop is 2 instructions (DEC EAX, JNZ).
        # Plus the HLT instruction = 1,000,001 instructions.
        icount = 1000000
        
        # CPU runs at 280MHz on hardware. 
        duration = cycles / 280000000.0
        
        mips = (icount / duration) / 1000000
        interrupts_per_sec = ebx / duration
        
        print("-" * 40)
        print("BENCHMARK RESULTS")
        print("-" * 40)
        print(f"Target:       {'Hardware' if args.target == 'hardware' else 'gwemu'}")
        print(f"ARM Cycles:   {cycles}")
        print(f"Time (280MHz):{duration:.4f} seconds")
        print(f"Instructions: {icount}")
        print(f"Interrupts:   {ebx}")
        
        eip = backend.read_uint32(dtcm_addr + 0)
        eax = backend.read_uint32(dtcm_addr + 8)
        pit_counter = backend.read_uint32(dtcm_addr + 2184)
        print(f"End EIP:      {eip}")
        print(f"End EAX:      {eax}")
        
        benchmark_filesize_addr = syms.get("benchmark_filesize")
        benchmark_filesize = backend.read_uint32(benchmark_filesize_addr)
        print(f"File Size:    {benchmark_filesize} bytes")
        
        print(f"End PIT:      {pit_counter}")
        
        print(f"Speed:        {mips:.2f} MIPS")
        print(f"Intr/Sec:     {interrupts_per_sec:.2f} Hz")
        print("-" * 40)
        
    except KeyboardInterrupt:
        print("Benchmark aborted.")
    finally:
        runner.stop()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Run CPU Benchmark")
    add_runner_args(parser)
    args = parser.parse_args()
    run_benchmark(args)
