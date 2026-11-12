# file: n1pt_plat.c
s/    if ((pmcsr \& 3) != 3) {/    if (0 \&\& (pmcsr \& 3) != 3) {/
