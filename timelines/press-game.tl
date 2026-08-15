# Launch the preselected ROM, then exercise the GAME button INSIDE the guest.
#
# GAME is the physical button retro-go reports as ODROID_INPUT_START, which
# pad_scancode[] maps to Return. It is NOT the same thing as gwemu's "start"
# key: that one drives B_START, a button the stock Game & Watch does not have,
# which the firmware reports as ODROID_INPUT_X. A timeline that says
# "press start" therefore tests a button nothing in the core reads, which is
# how "START sends Return" came to be believed working while the device said
# otherwise. Press GAME to test GAME.
#
# TIME first: Alley Cat opens on "use a joystick (Y/N)?" and TIME sends N,
# which is what puts the game on the keyboard we emulate. Pressing Return at
# that prompt is not observable, so the answer has to come first.
4.0  press a
5.0  press a
10.0 press time
12.0 screenshot 01-after-time.png
14.0 press game
16.0 screenshot 02-after-game.png
18.0 press game
20.0 screenshot 03-after-game2.png
22.0 press a
24.0 screenshot 04-after-a.png
34.0 quit
