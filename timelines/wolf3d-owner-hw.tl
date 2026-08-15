# The owner's recorded Wolf3D session, SHIFTED +5.0 s for REAL HARDWARE.
#
# Recorded under gwemu, which reached Wolf3D's menus FASTER than the device
# does. Replayed unshifted on silicon the whole session landed in menus:
# x86_modey_presents stayed at 2 and flip_gap at 19 for all 54 s, i.e. the
# display never flipped and no gameplay was ever measured.
#
# A recorded timeline is wall-clock, so it is only valid on the tier that
# recorded it unless it is retimed. Regenerate a different offset with:
#   python3 -c "import re,sys; S=float(sys.argv[2]); print('\n'.join(
#     re.sub(r'^(\d+\.?\d*)', lambda m: '%.3f'%(float(m.group(1))+S), l)
#     for l in open(sys.argv[1]).read().splitlines()))" IN.tl 5.0
#
# VERIFY BEFORE TRUSTING ANY NUMBER FROM THIS: x86_modey_presents must
# CLIMB. A constant value means it never left the menus.

9.240 down a
9.360 release a
10.568 down start
10.669 release start
12.268 down start
12.388 release start
14.913 down start
15.052 release start
16.426 down start
16.547 release start
17.575 down start
17.692 release start
25.361 down up
25.837 release up
25.955 down a
26.075 release a
26.933 down up
27.261 down right
27.519 release up
27.676 down up
27.798 release up
27.976 down up
28.156 release right
28.252 down left
28.471 release up
28.987 release left
29.109 down start
29.246 release start
29.351 down start
29.488 release start
29.654 down left
30.170 down up
30.664 release up
30.765 down up
30.846 release left
30.906 down right
31.103 release up
31.856 down a
31.914 release right
31.995 release a
32.050 down up
32.346 release up
32.579 down left
32.778 down up
32.797 release left
33.372 release up
33.510 down down
33.987 down left
34.323 release down
34.481 down up
34.938 release left
35.472 down right
35.652 release right
35.912 down a
35.989 release up
36.050 release a
36.279 down up
36.736 down right
36.893 release right
37.191 down left
37.469 release left
38.103 down right
38.481 release right
38.659 down left
38.878 release left
39.094 down left
39.254 down a
39.276 release left
39.374 release a
39.630 down right
39.770 release right
40.106 release up
40.147 down right
40.325 release right
40.454 down right
40.632 down up
40.671 release right
41.603 down left
42.991 release left
43.030 down right
43.864 release up
44.140 down up
44.458 release right
44.499 down right
44.580 release right
45.292 down left
45.552 release left
45.668 down left
45.709 down a
45.866 release a
45.926 release up
46.206 down up
46.303 release left
46.978 down right
47.138 release right
47.851 down right
48.068 release right
48.248 down left
48.803 release left
48.822 down right
49.240 release right
49.279 down right
49.439 release right
49.677 down left
49.875 release left
49.894 down right
50.291 release right
50.331 down left
50.768 release left
51.025 down left
51.383 release left
51.442 down a
51.480 down right
51.600 release a
52.038 release right
52.097 down a
52.215 down left
52.276 release a
52.554 release left
52.772 down left
52.811 down a
52.929 release a
53.087 release left
53.107 release up
59.231 quit
