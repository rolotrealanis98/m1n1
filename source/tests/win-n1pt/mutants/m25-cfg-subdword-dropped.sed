# file: hv/win/hv_win_n1pt_logic.h
s/    if ((r \& ~3u) == N1SV_CFG_DATA) {/    if (width != 2 \&\& (r \& ~3u) == N1SV_CFG_DATA) { return; } if (0) {/
