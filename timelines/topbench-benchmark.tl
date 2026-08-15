# Open TOPBENCH and walk its menu to "Benchmarking", which is where the five
# Zen-Timer sub-timings (MemTest / MemEA / Opcodes / VidMem / 3DGames) and their
# total are printed. The main menu's first entry is "Introduction", so one DOWN
# then GAME (Return) selects the second.
#
# This exists because those six numbers are the ONLY tier-2 evidence that the
# PIT counters are readable and counting: they all read 65535 while `IN 0x40`
# floated open-bus and port 0x20 answered 0xFF, because the Precision Zen Timer
# then took its overflow path (ZTIMER11/PZTIMER.ASM:488-493).
#
# Capped at 58 s, inside the 60 s limit in CLAUDE.md.
4.0  press a
5.0  press a
14.0 press down
16.0 press start
18.0 press start


58.0 quit
