# file: n1pt_plat.c
s/read32(N1PT_FN_CFG(p, fn) + CFG_CMD) \& 0xffff \& ~CMD_MASTER);/read32(N1PT_FN_CFG(p, fn) + CFG_CMD) \& 0xffff);/
