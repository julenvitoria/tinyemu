# The OSK over a GRAPHICS-mode guest.
#
# Alley Cat opens on "use a joystick (Y/N)?" in text mode and only switches to
# CGA mode 4 once that is answered. The answer is N, which pad_scancode[9] puts
# on the physical SELECT button (ODROID_INPUT_Y, zelda) -- gwemu's "select" key.
# Without it the capture is of the prompt, in text mode, which is the OTHER
# case and is covered by osk-open.tl.
#
# Then GAME opens the keyboard over the running game, which is the picture that
# matters: the bars belong to the OSK and rows 20-219 must still be the guest's.
4.0  press a
5.0  press a
10.0 press select
14.0 screenshot 01-game-running.png
18.0 press game
20.0 screenshot 02-osk-over-game.png
22.0 press right
24.0 press a
26.0 screenshot 03-key-sent.png
30.0 press game
32.0 screenshot 04-mouse-over-game.png
34.0 press left
35.0 press up
36.0 screenshot 05-pointer.png
40.0 quit
