#!/bin/bash
# Quick mutant check: each mutant must make the host tests fail. Works on a scratch copy.
set -u
cd "$(dirname "$0")/.." >/dev/null
OUT=${OUT:-$(mktemp -d "${TMPDIR:-/tmp}/dcpw-mut.XXXXXX")}
CC=${CC:-clang}
killed=0; total=0
mut() { # file, perl substitution, label
  total=$((total+1)); rm -rf "$OUT/m"; mkdir -p "$OUT/m"; cp -R service tests abi "$OUT/m/"
  perl -0pi -e "$2" "$OUT/m/$1" || { echo "MUTANT $3: substitution failed"; return; }
  if cmp -s "$1" "$OUT/m/$1"; then echo "MUTANT $3: no change (bad mutant)"; return; fi
  if (cd "$OUT/m" && $CC -std=c11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all tests/test_dcpw.c tests/fake_dcp.c service/*.c -o t 2>/dev/null && ./t >/dev/null 2>&1 \
      && $CC -std=c11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all tests/test_live.c tests/fake_live.c tests/fake_dcp.c service/*.c -o tl 2>/dev/null && ./tl >/dev/null 2>&1 \
      && $CC -std=c11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all tests/test_dart4.c service/dart4.c -o td 2>/dev/null && ./td >/dev/null 2>&1); then
    echo "MUTANT $3: SURVIVED"; else echo "MUTANT $3: killed"; killed=$((killed+1)); fi
}
mut service/iomfb_codec.c 's/\(4ull << 4\)/(5ull << 4)/' shmem-flag
mut service/iomfb_codec.c 's/p\[0\] = tag\[3\]; p\[1\] = tag\[2\];/p[0] = tag[0]; p[1] = tag[1];/' tag-order
mut service/iomfb_codec.h 's/IOMFB_SW_SWAP_ID        0x0d8/IOMFB_SW_SWAP_ID        0x098/' swap-id-27.2
mut service/iomfb_codec.h 's/IOMFB_SW_BL_UPDATE      0x3b4/IOMFB_SW_BL_UPDATE      0x354/' bl-27.2
mut service/iomfb_codec.c 's/put32\(out \+ 0x24, 3\)/put32(out + 0x24, 1)/' param-count
mut service/iomfb_codec.c 's/if \(\(t >> 29\) & 3\)/if (0)/' parser-padding
mut service/iomfb_codec.c 's/if \(depth > 32\)/if (depth > 100000)/' parser-depth
mut service/iomfb_session.c 's/if \(m->flags & DCPW_MODE_VRR\) \{/if (0) {/' no-adaptive-sync
mut service/iomfb_session.c 's/int ok = nul && s->prop_active && s->prop_contig/int ok = nul \&\& s->prop_active/' prop-contig
mut service/iomfb_session.c 's/if \(id && id == s->swap_inflight\)/if (id)/' stale-complete
mut service/iomfb_session.c 's/\t\tfail\(s, p.tag\); \/\* not acked/\t\ts->tp.send(s->tp.ctx, IOMFB_EP, iomfb_msg_ack(m->ctx)); \/\* acked/' ack-unknown
mut service/dcpw_core.c 's/if \(p.gpa & 0x3fff\)/if (p.gpa \& 0xfff)/' gpa-align
mut service/dcpw_core.c 's/if \(\!seq \|\| seq \!= q.seq\)/if (!seq)/' seq-check
mut service/dcpw_core.c 's/d->vbl_count \+= missed \+ 1;/d->vbl_count += 1;/' missed-vbl
mut service/dcpw_core.c 's/d->irq_status &= ~\(val & DCPW_IRQ_ALL\); update_irq\(d\);/d->irq_status \&= ~(val \& DCPW_IRQ_ALL);/' w1c-irq
mut service/dcpw_core.c 's/p.dw > p.width - p.dx \|\| //' dirty-x
mut service/dcpw_core.c 's/if \(\!\(d->caps & DCPW_CAP_VRR\)\)\n\t\t\td->modes\[n\].flags &= ~DCPW_MODE_VRR;//' strip-vrr
# live session and DART (D3)
mut service/iomfb_live.c 's/cpy\(out, in \+ 0x40, 0x1000\);/;/' d113-zero-reply
mut service/iomfb_live.c 's/issue\(l, 1, "A535"/issue(l, 0, "A535"/' a535-not-oob
mut service/iomfb_live.c 's/case B_A000: p32\(z, 1\);/case B_A000: p32(z, 0);/' a000-arg
mut service/iomfb_live.c 's/l->bw_scratch = 0x286720000ull;/l->bw_scratch = 0x286700000ull;/' d003-scratch
mut service/iomfb_live.c 's/static void d586_done\(struct live \*l, struct live_cmd \*c, const uint8_t \*o\) \{ \(void\)c; \(void\)o; cb_finish\(l, 1\); \}/static void d586_done(struct live *l, struct live_cmd *c, const uint8_t *o) { (void)c; (void)o; cb_finish(l, 0); }/' d586-false
mut service/iomfb_live.c 's/if \(l->stage < LIVE_FLIP \|\| l->state != LS_READY \|\| l->swap_inflight/if (l->state != LS_READY \&\& l->state != LS_STARTED || l->swap_inflight/' flip-gate
mut service/iomfb_live.c 's/for \(uint32_t i = 0; i < p.out_len; i\+\+\)\n\t\tp.out\[i\] = 0;/;/' no-zero-out
mut service/dart4.c 's/if \(!io->rd64\(io->ctx, d->root \+ 8ull \* \(uint64_t\)s\)\)/if (1)/' dart-claims-fw-slot
mut service/dart4.c 's/\| DART4_SP_END \| DART4_PTE_VALID; \}/| DART4_PTE_VALID; }/' dart-no-sp-end
mut service/dart4.c 's/v \|= DART4_TLB_V2;/;/' dart-no-v2
mut service/dart4.c 's/ \| DART4_TLB_RANGE \| DART4_TLB_STT;/;/' dart-whole-stream-flush
mut service/dart4.c 's/d->next_dva = dva \+ len \+ DART4_PAGE;/d->next_dva = dva + len;/' dart-no-guard
mut service/dart4.c 's/\|\| d->root != adt_root //' dart-root-unchecked
mut service/dart4.h 's/#define DART4_ROOT_SHIFT  36/#define DART4_ROOT_SHIFT  47/' dart-wrong-levels
mut service/iomfb_live.c 's/off = l->chn\[k\] \? l->chend\[k\]\[l->chn\[k\] - 1\] : 0;/off = l->txn[s] ? l->tx[s][l->txn[s] - 1].end + 0x40 : 0x40;/' nested-placement
mut service/iomfb_live.c 's/ctx = l->cbs\[l->ncb - 1\].ctx == IOMFB_CTX_OOBCB \|\| oob \? IOMFB_CTX_OOBCB : IOMFB_CTX_CB;/ctx = IOMFB_CTX_CMD;/' nested-ctx
mut service/iomfb_live.c 's/else if \(teq\(t, "D207"\)\) r = issue\(l, 0, "A137"/else if (teq(t, "D207")) r = issue(l, 0, "A136"/' d207-tag
echo "mutants: $killed/$total killed"
