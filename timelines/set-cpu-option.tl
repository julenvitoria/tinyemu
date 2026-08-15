# Launch the preselected ROM, then walk retro-go's in-game menu to the core's
# own "CPU Speed" entry and change it.
#
# The path is: PAUSE opens the game menu (Save & Continue / Save & Quit / -- /
# Reload / Options / ...), A on "Options" opens the settings menu, and our
# entry is APPENDED after the firmware's own (Scaling / Filtering / Speed) --
# odroid_overlay_game_settings_menu() memcpy's extra_options onto the end.
#
# So: three downs to "Options", A, then ONE UP. The dialog's selection wraps
# (odroid_overlay_dialog_find_next_item), and ours is the LAST entry, so up
# from the first item lands on it regardless of how many entries the firmware
# put above it -- which is what makes this timeline survive a firmware that
# grows an option.
#
# RIGHT is what cycles the value; the core's callback writes guest_kips and the
# persisted setting inside that event, so `probe.py --watch guest_kips,
# cpu_profile_persisted` sees the change at exactly this timestamp.
4.0  press a
4.5  press a
12.0 press pause
13.5 press down
14.5 press down
15.5 press down
16.5 press a
18.0 press up
19.5 press right
21.0 press b
22.5 press b
# ... and now prove it PERSISTS. PAUSE, one down to "Save & Quit", A: that
# leaves the core and returns to the launcher. Relaunching re-enters app_main,
# so cpu_profile_init() reads the setting back from the store with no menu
# involved -- guest_kips must come up 330, not the KIPS_MAX default.
30.0 press pause
31.5 press down
33.0 press a
44.0 press a
48.0 press a
52.0 press a
58.0 quit
