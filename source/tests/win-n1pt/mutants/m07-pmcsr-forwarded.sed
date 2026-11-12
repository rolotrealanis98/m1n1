# file: hv/win/hv_win_n1pt_logic.h
s|case 0x44: if (be \& 0xffff) s->ep.pmcsr = (uint16_t)(v \& 0x0103u); break; /\* swallowed \*/|case 0x44: if (be \& 0xffff) { s->ep.pmcsr = (uint16_t)(v \& 0x0103u); s->ops->cfg_write(s->ops->ctx, 0, 0x44, v); } break;|
