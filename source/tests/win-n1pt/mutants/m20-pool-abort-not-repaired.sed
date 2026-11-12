# file: hv/win/hv_win_n1pt.c
s/    if (!ecam \&\& !win \&\& !inpool)/    if (!ecam \&\& !win \&\& !(inpool \&\& 0))/
