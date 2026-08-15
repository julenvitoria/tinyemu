# Launch the preselected ROM, then tap PAUSE/SET to open retro-go's in-game
# overlay menu. The menu is opened on RELEASE of PAUSE with no macro button
# held, so a short tap is exactly the gesture that reaches open_pause_menu().
# Used by tests/probe runs that exercise the overlay path from a headless run.
4.0  press a
4.5  press a
12.0 press pause
12.5 screenshot 01-pause-tapped.png
14.0 screenshot 02-menu.png
16.0 screenshot 03-menu.png
# B backs out of the menu, so the run also covers "the emulator resumes".
18.0 press b
20.0 screenshot 04-resumed.png
24.0 quit
