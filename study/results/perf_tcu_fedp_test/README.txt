FEDP differential test (perf-tcu). fedp_ref.h = sw/common/fedp.h of base 540b3dad2 with the class renamed TfrFedpRef.
Build against a tree's header:  g++ -std=c++17 -O2 -I. -I<tree>/sw/common -o fedp_diff fedp_diff.cpp
Run: ./fedp_diff <cases> <seed> [mode]   (mode 0 compare; 1/2 run ref/new only, for diffing -DFEDP_RTL_WATCH -DFEDP_TRACE stderr)
run4_*.txt: 4 x 10M cases against perf/tcu 3a7e7efee, 0 mismatches.
fedp_minbench.cpp: min-of-trials ns/dot; needs fedp_v1.h / fedp_cur.h copies with renamed classes.
