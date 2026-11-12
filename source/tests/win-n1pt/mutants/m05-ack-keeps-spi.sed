# file: hv/win/hv_win_n1pt_logic.h
s/s->ops->spi(s->ops->ctx, s->irq_status != 0);/s->ops->spi(s->ops->ctx, true);/
