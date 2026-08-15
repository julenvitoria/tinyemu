# Launch the preselected ROM and drive its MENU with the d-pad and GAME.
#
# The keyboard chain is only proven when the guest's PICTURE changes, so this
# presses DOWN (scancode 0x50, an extended key with no ASCII) and then GAME
# (Return) with a screenshot between each -- a menu that ignores the key looks
# identical, a menu that took it does not.
4.0  press a
5.0  press a
12.0 screenshot 01-menu.png
14.0 press down
16.0 screenshot 02-after-down.png
18.0 press down
20.0 screenshot 03-after-down2.png
22.0 press game
24.0 screenshot 04-after-game.png
40.0 quit
