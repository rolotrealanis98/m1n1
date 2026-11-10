# r4.6 execution report

Implemented M-A: atomic submission ownership (`source/src/hv/win/s1/ans_ro.c:409`), pre-buffer re-entry rejection (`:838`), exact-slot checks under ownership (`:475`), and protected first-write GPT revalidation (`:495`). Boot-CPU/proxy/CANARY gates are in `native_link.c:15,34,66`; full exception-session tracking is in `source/src/uartproxy.c:133`.

Guest past-end READs now return a counted normal error without freezing (`hv_nvme_queue.c:691`; `storage_stat(12)`). WRITEs still freeze on confinement violations. Stage-3 WRITE requests split into at most 32-block backend calls; exact submitted ranges remain pinned and overflow-safe. WRITE ZEROES, DSM/trim and other optional write-class commands are refused. FLUSH is canonical NSID 1 with no address/DMA fields. The audit is in REVIEW-GUIDE.md.

WRITE/FLUSH timeouts are 30 seconds; canary saves are double-read/compared. The three smp2h files match exactly.

Baseline r4.5+overlay: stages 0/1/2/3 pass 48/54/54/196 scenarios, 86/86 native mutants. r4.6 inherited scenarios pass 47/48, 51/54, 51/54, 186/196. **Acceptance remains blocked:** incompatible C9/split expectations were preserved. Added checks pass 140/140 scenarios and 23/23 mutants; 84/86 inherited native mutants caught (two stale anchors), 44/44 r4.5 mutants with added cases, 7/7 overlay mutants. Broad host and both falsifiable twin suites pass. Legacy inspection/location and reviewer missing-header failures are recorded.

Image self-hash remains open: trusted digest/layout design is not locally established. Firmware binaries: **not built** as instructed. Hardware/Windows behavior: **unverified**. No network, hardware or upstream actions.
