# Reach WOLF3D gameplay and stand STILL.
#
# The rotating variant (wolf3d-ingame.tl) was built to make a mid-redraw paint
# visible, but it also makes every frame it captures un-gradeable against a
# DOSBox oracle: the player has turned by an unknown angle, so a wrong-looking
# scene cannot be told from a differently-aimed one. This timeline presses only
# A, so the frame it screenshots is the LEVEL 1 SPAWN VIEW -- the exact state
# scripts/ref_dos.py reproduces.
4.0  press a
6.0  press a
8.0  press a
10.0 press a
12.0 press a
14.0 press a
16.0 press a
18.0 press a
20.0 press a
22.0 press a
24.0 press a
26.0 press a
28.0 press a
30.0 press a
32.0 press a
34.0 press a
36.0 press a
38.0 screenshot 01-spawn.png
44.0 screenshot 02-spawn.png
50.0 screenshot 03-spawn.png
54.0 quit
