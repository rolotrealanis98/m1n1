# file: hv/win/hv_win_n1pt_logic.h
s/    if (fn <= 2 \&\& off == 0x04 \&\& (lanes \& N1PT_CMD_FWD) \&\& !s->dead/    if (0 \&\& fn <= 2 \&\& off == 0x04 \&\& (lanes \& N1PT_CMD_FWD) \&\& !s->dead/
