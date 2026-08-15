# Launch the preselected ROM and MASH THROUGH WOLF3D'S MENUS INTO GAMEPLAY.
#
# Why this exists: gwemu cannot reach WOLF3D gameplay inside the 60 s cap (~3.0 M
# insn/s), so every Mode Y presentation fix so far has been graded against a
# MODELLED gameplay case in tests/main.c rather than against the raycaster. The
# in-game "split viewport / house of mirrors" defect the owner reports on silicon
# therefore has no witness at tier 1 or tier 2 -- this timeline is the tier 3 one.
#
# Wolf3D's menus are simply nested deeply: difficulty, episode, then the level
# intro. That is roughly 30-45 s of pressing A on hardware (~5.5 MIPS), which is
# not a symptom of anything -- it is just how many screens there are. So A is
# pressed steadily rather than cleverly.
#
# After gameplay is reached the d-pad turns the player, because the artefact is
# reported as WORST WHILE ROTATING: rotation is what makes consecutive frames
# differ across the whole viewport, so a paint taken mid-redraw composites two
# visibly different views. A static frame can hide the same bug completely.
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
34.0 screenshot 01-after-menus.png
36.0 press a
38.0 press a
40.0 screenshot 02-maybe-ingame.png
# Turn. Held long enough to span many guest frames, not a tap.
42.0 press left
44.0 screenshot 03-turning-left.png
46.0 press right
48.0 screenshot 04-turning-right.png
50.0 press right
52.0 screenshot 05-turning-more.png
56.0 quit
