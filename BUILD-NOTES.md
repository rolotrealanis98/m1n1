# unified-u1: one resident with every confirmed line (2026-11-11)

= r4.14 disk boot (storage files byte-identical, tests/u1 pins them) + smpN-sched (2/6/18 vCPUs, uniform CPU classes)
+ disp3 display (shadow, dirty tracking, 2x upscale; 1 vCPU only) + kbd-hw-hidm (--builtin-keyboard, default off)
+ the TZ-carveout SError fix (already on both the smp and storage sides).
Storage with more than one vCPU is refused (hv_win_storage_smp_gate, proxy.c).

Full notes, review guide, SMP safety list and hardware commands:
/Users/rolo/Work/agents/win-el2/storage/s1-resident-u1-unified-20261111/ (BUILD-NOTES.md, REVIEW-GUIDE.md, SMP-SAFETY.md, LINES.md).
Per-line notes of the merged lines: line-history/.
