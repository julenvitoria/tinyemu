# Open the preselected ROM and let the guest RUN, up to the 60 s cap.
#
# start-preselected-game.tl quits at 35 s, which is right for "did it boot" but
# not for a guest that is doing real work: TOPBENCH benchmarks the CPU and only
# prints its score at the end. This is the one legitimate reason to sit through
# a longer window -- the wait IS the result, not a symptom to chase with
# probe.py. Still capped: 58 s, inside the 60 s limit in CLAUDE.md.
#
# No screenshots: gwemu's headless screendump is black anyway, and
# scripts/screenshot.py recovers the real frame from the LTDC registers.
4.0  press a
5.0  press a
58.0 quit
