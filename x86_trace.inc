/* Instruction-trace parameters for cpu_exec.S.
 *
 * Rewritten by scripts/trace_diff.py; checked in with tracing OFF so a normal
 * build carries none of it. Defining X86_TRACE_EIP samples the linear EIP into
 * x86_eip_log[64], one slot every 1<<X86_TRACE_SHIFT instructions, starting
 * after X86_TRACE_SKIP of them and stopping once the 64 slots are full. A
 * saturating window rather than a ring: a ring spans only its own length and
 * silently shows the end of the run wherever you point it.
 */
/* #define X86_TRACE_EIP 1 */
#define X86_TRACE_SHIFT 14
#define X86_TRACE_SKIP  0
