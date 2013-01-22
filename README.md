carrefour-runtime
=================

This is the Carrefour runtime.
Every second, it decides whether Carrefour needs to be run or not, based on hardware counters feedback (local access ratio, memory load imbalance ...). 

Thresholds can be configured at the top of 'carrefour.c'. Default options are those we used for the ASPLOS paper.

To gather hardware counter values, we use the perf API so your kernel must have been built with perf support.
We also require libnuma and libgsl. On Ubuntu: sudo apt-get install libnuma-dev libgsl0-dev

Finally, the Makefile automatically builds tags. It requires cscope/ctags. You can also remove the 'tags' rule.


NOTES
=====

Hardware counters are only valid for AMD Family 10h. If you are running on another architecture, you will have to update 

There are two branches: master and carrefour-per-pid. The first branch supports only one CPU-intensive application, whereas the second one supports multiple applications.
