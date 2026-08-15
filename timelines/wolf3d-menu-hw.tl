# WOLF3D MENUS on REAL HARDWARE -- the CONTROL for wolf3d-hw.tl.
#
# The gameplay profile is only interpretable against a run of the SAME guest,
# the SAME core and the SAME clock that is NOT raycasting. Without it, "95
# cycles per instruction" has nothing to be 95 against, and a changed opcode
# MIX and a genuine per-opcode slowdown look identical in the average.
#
# So: press NOTHING. Wolf3D's intro screens advance on a keypress and this
# file never sends one, so the guest sits on the title/ordering screens for
# the whole window -- 16-colour text and static art, no raycaster, no Mode Y
# page flip.
#
# GATE, and it is the mirror of the gameplay one:
#   x86_modey_presents=const   -- it must NOT flip. A climb means the attract
#                                 demo started and this is not a menu run.
#   guest_insns_total          -- must climb, or the guest is not running at
#                                 all and "no Mode Y" is trivially true.
#
# Nothing here is a wait for something to happen (CLAUDE.md): the window is
# short and the state it measures is present from the first second.

40.0 quit
