# Does a cell pressed WHILE the on-screen keyboard is up reach the guest THEN,
# or only once the keyboard is closed? The device report is the latter: "I had
# to turn off the keyboard before it would send the keys."
#
# Alley Cat's joystick prompt is answered with SELECT (= N) first, so the guest
# is at the skill menu and genuinely reading the keyboard. Then GAME opens the
# OSK on layer 0 cell 0 ('a') and A presses it three times, several seconds
# apart, with the OSK left up throughout. Two GAME presses at the end walk it
# back to OFF.
#
# What to watch: x86_osk_codes (codes the OSK injected) against
# x86_kbd_port60_reads (codes the GUEST took off the controller). If the reads
# stall while the OSK is up and then catch up at 34-38 s, the keys are being
# queued, not delivered.
4.0  press a
5.0  press a
10.0 press select
14.0 press game
18.0 press a
22.0 press a
26.0 press a
34.0 press game
36.0 press game
50.0 quit
