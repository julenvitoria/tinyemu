# Cortex-M7 Pipeline & CPU Optimization Strategy

## 1. The Cortex-M7 Microarchitecture
The STM32H7B0VB utilizes an ARM Cortex-M7 core, which features a **6-stage, superscalar, dual-issue pipeline**. 
This means the CPU can execute two instructions simultaneously per clock cycle, provided there are no data dependencies or resource conflicts.

Writing raw Thumb v2 assembly is not enough; we must actively structure the code to prevent pipeline stalls and branch mispredictions.

## 2. ITCM and DTCM Pinning
- **ITCM (64KB)**: The main `cpu_exec` loop (which fetches, decodes, and executes x86 instructions) will be entirely pinned to the ITCM. This ensures instructions are fed into the pipeline with 0 wait states, completely bypassing the L1 instruction cache overhead.
- **DTCM (128KB)**: The primary CPU state structure (x86 registers, EIP, EFLAGS, segment descriptors) will reside at the very beginning of the DTCM. The 8KB Memory Page Table (see `01_memory_architecture.md`) will also reside here, guaranteeing 0-wait-state data access for memory operations.

## 3. Dual-Issue Optimization
To maximize IPC (Instructions Per Clock), we must avoid Read-After-Write (RAW) dependencies between consecutive instructions.

**Bad (Stalls pipeline):**
```assembly
LDR R0, [R1]
ADD R0, R0, #1  @ Stalls: depends on the load completing
```

**Good (Dual-issue capable):**
```assembly
LDR R0, [R1]
LDR R2, [R3]    @ Independent: executes alongside or hides load latency
ADD R0, R0, #1
```
The x86 emulator state machine will be hand-interleaved to keep both issue ports busy, particularly during the instruction decoding phase.

## 4. Branch Prediction
The M7 has sophisticated static and dynamic branch prediction. 
- Avoid using `IT` (If-Then) blocks for anything other than extremely short 1-2 instruction conditionals. On the M7, `IT` blocks can sometimes perform worse than standard branches because they serialize execution.
- Order conditional branches such that the most common path (e.g., no page fault, no interrupt pending) falls through without branching.

## 5. Register Mapping
Given ARM's limited register file compared to the heavy state of an x86 CPU, we will permanently map the most accessed x86 state variables to high ARM registers (`R8`-`R11`) within the main execution loop:
- `R8`: Pointer to the x86 CPU context in DTCM.
- `R9`: Pointer to the DTCM Page Table.
- `R10`: Current x86 Instruction Pointer (EIP).
- `R11`: Current x86 EFLAGS cache.
This reduces memory loads/stores dramatically during tight loops.

## 6. CPU State Structure (DTCM)
To enforce this architecture, the entire CPU context struct (which holds EAX, EBX, EIP, EFLAGS, segment descriptors, etc.) must be explicitly mapped to the `.dtcm_bss` section in C:
```c
typedef struct {
    uint32_t regs[8]; // EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI
    uint32_t eip;
    uint32_t eflags;
    // ... Segment descriptors, CR registers, etc.
} X86CPUState;

// Force the entire x86 register state into 0-wait-state DTCM memory
X86CPUState cpu_state __attribute__((section(".dtcm_bss")));
```

## 7. Assembly Core Loop (ITCM)
The outer `cpu_exec` dispatch loop is written in pure ARM Thumb v2 assembly and pinned to the `.itcram_hot` section. The entry prologue establishes the high-register mapping to our DTCM structures.
```assembly
    .syntax unified
    .cpu cortex-m7
    .thumb

    .section .itcram_hot, "ax", %progbits
    .global cpu_exec
    .type cpu_exec, %function

cpu_exec:
    // PROLOGUE: Save ARM host state
    PUSH {R4-R11, LR}

    // Load static pointers into high registers
    LDR R8, =cpu_state          // R8 = CPU Context in DTCM
    LDR R9, =guest_page_table   // R9 = Page Table in DTCM
    LDR R10, [R8, #EIP_OFFSET]  // R10 = Cached EIP
    LDR R11, [R8, #EFLAGS_OFFSET] // R11 = Cached EFLAGS

.fetch_decode_loop:
    // ... Thumb v2 optimized fetch and decode logic ...
    
    // EPILOGUE: Restore state
    STR R10, [R8, #EIP_OFFSET]
    STR R11, [R8, #EFLAGS_OFFSET]
    POP {R4-R11, PC}
```
