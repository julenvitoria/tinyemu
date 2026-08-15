# Reach WOLF3D gameplay and MOVE, so a profiling run measures the raycaster
# under load instead of a static frame.
#
# Why this exists next to the other two:
#   wolf3d-still.tl  stands still at the spawn -- gradeable against an oracle,
#                    but the raycaster redraws almost nothing, so it is the
#                    WRONG workload for a performance measurement.
#   wolf3d-ingame.tl turns, but was written to expose a mid-redraw paint and
#                    its frames cannot be graded.
#   this file        sustained walking and turning: every frame is a full
#                    redraw, which is the case the owner is judging as "not
#                    smooth" and the case a profiler needs to attribute.
#
# The A presses that walk the menus keep firing once in game. That is
# deliberate -- a fired weapon is real work (sprite blit, sound, state) and a
# profile of a player who never shoots is not a profile of playing.
#
# Capped at 58 s against the 60 s limit in CLAUDE.md. Movement starts at 28 s;
# if a slower tier has not reached gameplay by then the presses are harmless
# and the run simply profiles menus -- check x86_vga_modey_armed before
# trusting any number this produces.
2.0  press a
4.0  press a
6.0  press a
8.0  press a
10.0 press a
12.0 press a
14.0 press a
16.0 press a
18.0 press a
20.0 press a
22.0 press a
24.0 press a
26.0 press a
28.0 screenshot 01-entered.png

# ---- sustained gameplay: walk, turn, walk, strafe-ish, turn back ----------
29.0 hold up 3.0
32.5 hold right 2.0
35.0 hold up 3.0
38.5 hold left 2.5
41.5 hold up 2.0
44.0 hold right 3.0
47.5 hold up 3.0
51.0 hold left 2.0
53.5 hold up 2.5
56.0 screenshot 02-after-play.png
58.0 quit
