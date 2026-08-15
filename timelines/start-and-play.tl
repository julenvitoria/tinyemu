# Open the preselected ROM, then keep pressing keys inside the guest.
#
# start-preselected-game.tl only launches the ROM and waits. A DOS game
# typically sits on a title screen until a key arrives, so proving the CPU is
# still alive past that screen needs input DELIVERED TO THE GUEST -- which is
# what these later presses are. A maps to Space and the GAME button to Enter
# (see pad_scancode in src/main.c).
#
# 'press game', NOT 'press start': gwemu's "start" key drives B_START, a
# button the stock Game & Watch does not have, which retro-go reports as
# ODROID_INPUT_X -- a bit the core does not read. The physical GAME button is
# ODROID_INPUT_START. This file said 'start' and so tested nothing.
4.0  press a
5.0  press a
9.0  press game
11.0 press a
14.0 press a
17.0 press game
20.0 press a
23.0 press a
26.0 press a
35.0 quit
