# The OSK must COEXIST with retro-go's own PAUSE overlay, not fight it.
#
# The overlay draws into the active buffer and calls our repaint callback for
# the picture underneath -- unconditionally, with no NULL test, which is the
# hardfault docs/04 records. core_repaint() therefore renders the guest AND the
# keyboard, so this run is what proves the keyboard survives the menu opening
# on top of it and is still there when the menu closes.
4.0  press a
5.0  press a
12.0 press game
14.0 screenshot 01-osk-open.png
16.0 press pause
18.0 screenshot 02-menu-over-osk.png
20.0 press b
22.0 screenshot 03-osk-still-there.png
26.0 quit
