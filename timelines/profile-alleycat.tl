# Get Alley Cat RUNNING and then leave it alone, for scripts/profile_all.py.
#
# Existing timelines could not serve this. start-preselected-game.tl launches
# and waits, which leaves the game on its opening prompt; press-game.tl answers
# with the wrong letter; osk-over-game.tl answers correctly but then opens the
# on-screen keyboard over the game, which puts the OSK in every profile.
#
# THE ANSWER IS N, AND IT IS ON gwemu's "select". Alley Cat opens with
#   "Do you want to use a joystick (Y/N)?"
# and reads port 0x60. pad_scancode[9] = ODROID_INPUT_Y = the physical SELECT
# button = gwemu `press select` = 0x31 = N, which is what puts the game on the
# keyboard we emulate. `press time` sends Y (pad_scancode[7], 0x15), and the
# game then reports "Game Control Adapter is not present" and waits -- MEASURED
# with screenshot.py, after a profile run of that state came back as 56%
# `CMP Gv,Ev` + 44% `JZ Jb`. A two-instruction spin loop is what profiling a
# guest that never started looks like, and it is easy to mistake for a result.
#
#
# THEN THE SKILL MENU, WHICH ONLY THE ON-SCREEN KEYBOARD CAN ANSWER. Past the
# joystick prompt the game asks for (K)itten / (H)ouse Cat / (T)omcat /
# (A)lley Cat and reads a LETTER. No button sends one -- that is the open
# defect in STATUS.md -- so the OSK is the door: GAME opens it on layer 0
# cell 0, which is `a`, and pressing A there types it. Two more GAME presses
# walk the cycle keyboard -> mouse -> OFF so the keyboard is not in the
# picture the profile measures.
4.0  press a
5.0  press a
10.0 press select
14.0 press game
16.0 press a
18.0 press game
20.0 press game
# "Press any key to start" -- and by here the OSK is closed again, so A is the
# plain Space the pad table sends.
24.0 press a
# Nothing after this: a profile window must contain the steady state and
# nothing else, so no screenshots and no further presses. The last press is at
# 24 s, which is why the preset settles 28 -- room for a 10 s window and the
# 3 s margin timeline_quit_time() wants, inside the 60 s cap in CLAUDE.md.
50.0 quit
