# Launch the preselected ROM, open the ON-SCREEN KEYBOARD with GAME and type.
#
# GAME is the OSK's door on every unit (docs/input/02-button-mapping.md), and it
# is bound to no guest key for exactly that reason -- so a run that presses GAME
# and sees nothing reach the guest is CORRECT, and the thing to watch is
# x86_osk_mode instead.
#
# The presses after it are the grid: RIGHT moves the cursor a cell, A presses
# the selected key, TIME cycles the layer. Screenshots bracket each so the
# keyboard's own picture is evidence, not just the counters -- the bars are
# rows 0-19 and 220-239 and nothing else in the core draws there.
4.0  press a
5.0  press a
12.0 screenshot 01-guest.png
14.0 press game
16.0 screenshot 02-osk-open.png
18.0 press right
19.0 press right
20.0 screenshot 03-osk-moved.png
22.0 press a
24.0 screenshot 04-after-key.png
26.0 press time
28.0 screenshot 05-layer-2.png
30.0 press game
32.0 screenshot 06-mouse-mode.png
34.0 press down
35.0 press right
36.0 screenshot 07-pointer-moved.png
38.0 press game
40.0 screenshot 08-closed.png
44.0 quit
