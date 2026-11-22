#!/usr/bin/bash
# usage: tests-bwstream_wait.sh <result-name>...  ; returns when every meta.txt has rc=
R=/storage/project/r-hkim358-0/echung67/vortex-exp/results
done_all() { for n in "$@"; do grep -q '^rc=' "$R/$n/meta.txt" 2>/dev/null || return 1; done; }
until done_all "$@"; do sleep 60; done
echo all done
