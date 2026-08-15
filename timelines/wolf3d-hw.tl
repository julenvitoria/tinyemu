# WOLF3D gameplay on REAL HARDWARE. Timing-ROBUST by construction.
#
# Two retimings of the owner's gwemu recording (unshifted, and +5 s) both
# ended with x86_modey_presents stuck at 2 and IDENTICAL final store counts
# (906,129), i.e. the inputs changed nothing and the guest ran its attract
# loop twice. Injection was NOT the problem -- x86_kbd_port60_reads climbed
# in step with delivery and the player reported 12/124 events at 0.1 ms
# lateness -- so the recording's KEYS/PHASE were wrong for this tier, not its
# clock.
#
# So do not retime a recording: press BOTH menu keys, repeatedly, for long
# enough that no single missed screen can strand the run. Extra presses in
# game just fire the weapon, which is real work and welcome in a profile.
#
# GATE: x86_modey_presents must CLIMB. Constant means menus, and every
# number from the run is then about menus.

3.0 press a
4.5 press start
6.0 press a
7.5 press start
9.0 press a
10.5 press start
12.0 press a
13.5 press start
15.0 press a
16.5 press start
18.0 press a
19.5 press start
21.0 press a
22.5 press start
24.0 press a
25.5 press start
27.0 press a
28.5 press start
30.0 screenshot 01-entered.png

# ---- sustained gameplay: walk and turn, every frame a full redraw -------
31.0 hold up 3.0
34.5 hold right 2.5
37.5 hold up 3.0
41.0 hold left 2.5
44.0 hold up 2.5
47.0 hold right 2.5
50.0 hold up 3.0
53.5 hold left 2.0
56.0 screenshot 02-playing.png
58.0 quit
