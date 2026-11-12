# file: n1pt_plat.c
s/on ? (0xfffffff0u \& ~(u32)P_INT_LINK_DOWN) : 0xfffffff0u/on ? ~(u32)P_INT_LINK_DOWN : 0xffffffffu/
