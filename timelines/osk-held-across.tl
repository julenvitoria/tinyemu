# Does a button HELD across an on-screen-keyboard open/close wedge the input
# path? The device report is that Alley Cat goes permanently deaf to buttons
# after the OSK has been used, while frames and guest execution continue.
#
# Same two gates as profile-alleycat.tl (SELECT = N answers the joystick
# prompt, then the OSK types `a` at the skill menu), and then the experiment:
# LEFT is HELD from 26 s to 38 s while GAME cycles the OSK all the way round
# off -> keyboard -> mouse -> off underneath it. After LEFT is released, two
# ordinary taps. If `input_scancodes` stops moving at those taps, poll_input()
# has stopped emitting; if it moves, the break is downstream.
4.0  press a
5.0  press a
10.0 press select
14.0 press game
16.0 press a
18.0 press game
20.0 press game
22.0 press a
26.0 hold left 12.0
28.0 press game
30.0 press game
32.0 press game
40.0 press left
42.0 press right
44.0 press left
55.0 quit
