#!/usr/bin/bash
# Run inside the holder after A (same process): B sweep, A under MPS, C step 6.
cd /storage/project/r-hkim358-0/echung67/vortex-exp/gpu
until grep -q "^done" ../results_gpu/h200_gng_a_same.txt && ! pgrep -f "gng_corun --map" > /dev/null; do sleep 2; done
echo "B start $(date)"; ./gng_run_b.sh > /dev/null 2>&1; echo "B done $(date)"
echo "A-mps start $(date)"; GNG_CUTLASS_CARVEOUT=100 SETUP_S=45 ./gng_run_a.sh mps; echo "A-mps done $(date)"
echo "stall start $(date)"; ./gng_stall.sh; echo "stall done $(date)"
echo "C6 start $(date)"; NTPCS="13 26 52 66" DUR=5 TMO=200 ./gng_c6.sh; echo "C6 done $(date)"
