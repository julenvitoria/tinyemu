# Launch the preselected ROM and then PRESS KEYS AT IT.
#
# Commander Keen 4 finishes its startup by sitting in IN_Ack -- "wait for any
# key" -- polling LastScan at guest linear 0x15CC1. Nothing in
# start-preselected-game.tl ever presses anything, so every Keen measurement
# before this one stopped at that loop by construction and could say nothing
# about what comes after it.
#
# The presses are deliberately spread over several DIFFERENT buttons: a keypress
# only clears IN_Ack if the button is bound to a scancode (GAME opens the OSK
# and is bound to none), and the story/menu screens that follow want more than
# one keystroke to walk through. Capped well under the 60 s timeline limit.
4.0  press a
5.0  press a
12.0 screenshot 01-startup.png
14.0 press b
16.0 press a
18.0 screenshot 02-after-keys.png
20.0 press time
22.0 press select
24.0 screenshot 03-after-more-keys.png
26.0 press a
28.0 press a
30.0 screenshot 04-after-more-a.png
34.0 press down
35.0 press a
38.0 screenshot 05-late.png
44.0 quit
