#!/usr/bin/bash
# cmp.sh <rtl file> <simx file>
paste -d' ' <(awk '{print $1,$2,$4}' $1) <(awk '{print $4, $7,$8}' $2) | sed 's/cyc_per_iter_warp=//g' | awk '$3+0>0{d=($4-$3)/$3*100; printf "%-16s %-4s rtl=%-8s simx=%-8s %+.2f%% %s %s\n",$1,$2,$3,$4,d,$5,$6} $3+0==0{print}'
