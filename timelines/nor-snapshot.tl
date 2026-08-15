# Launch the preselected ROM, let the guest settle, then walk retro-go's
# in-game menu to the core's "Cold RAM->NOR" entry and press A on it.
#
# Same path as set-cpu-option.tl: PAUSE, three downs to "Options", A. Our
# entries are APPENDED after the firmware's own, and the dialog's selection
# WRAPS, so one UP from the first item lands on the LAST entry -- which is
# "Cold RAM->NOR", because it is declared after "CPU Speed".
#
# A on it is ODROID_DIALOG_ENTER, which is what runs the snapshot: placeholder
# file, relocate-callback fill, erase+program, readback verify, remap. The
# firmware draws its own "Caching game" progress bar over it.
#
# The settle before PAUSE is deliberate and is the measurement: pages the guest
# has already written stay in AXI, and everything the snapshot leaves in NOR is
# memory the guest read but never wrote. Snapshotting at t=0 would measure
# nothing.
4.0  press a
4.5  press a
20.0 press pause
21.5 press down
22.5 press down
23.5 press down
24.5 press a
26.0 press up
27.5 press a
# Back out and let the guest run ON from NOR, so the COW pull-back counters
# have something to count.
34.0 press b
35.5 press b
55.0 quit
