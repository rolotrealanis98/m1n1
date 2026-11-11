/* SPDX-License-Identifier: MIT */
#include "hid_filter.h"

/* One pass over the short items: find each depth-0 Application collection, the span
 * from the end of the previous depth-0 main item to its End Collection, and the usage
 * page in effect at its opening. Vendor spans are removed (memmove), the rest kept.
 * Globals set inside a removed span would leak into a later TLC in the original;
 * removing the span removes them for the later TLC too, so a vendor TLC is removed
 * only when it is the last TLC or the next TLC sets its own usage page before its
 * collection (the HID convention); otherwise it is kept and the caller sees removed=0. */
#define MAX_TLC 16
int hid_strip_vendor_tlcs(hm_u8 *d, hm_u32 n, struct hid_filter_result *r)
{
    hm_u32 p = 0, depth = 0, page = 0, span_start = 0, ntlc = 0, i;
    hm_u32 s[MAX_TLC], e[MAX_TLC], vend[MAX_TLC], cur_id = 0;
    hm_u8 ids_tmp[32];
    hm_zero(r, sizeof(*r));
    if (!d || !n) return -1;
    while (p < n) {
        hm_u32 b = d[p], sz = b & 3, v = 0, k, tag = b >> 4, type = (b >> 2) & 3;
        if (b == 0xfe) { if (n - p < 3 || d[p + 1] > n - p - 3) return -1; p += 3u + d[p + 1]; continue; }
        if (sz == 3) sz = 4;
        if (sz > n - p - 1) return -1;
        for (k = 0; k < sz; k++) v |= (hm_u32)d[p + 1 + k] << (8 * k);
        if (type == 1 && tag == 0) page = v;                    /* Usage Page */
        if (type == 0 && tag == 10) {                           /* Collection */
            if (depth == 0) {
                if (ntlc == MAX_TLC) return -1;
                s[ntlc] = span_start;
                vend[ntlc] = page >= 0xff00;
            }
            depth++;
        } else if (type == 0 && tag == 12) {                    /* End Collection */
            if (!depth) return -1;
            if (--depth == 0) { e[ntlc++] = p + 1; span_start = p + 1; }
        } else if (type == 0 && depth == 0) {
            span_start = p + 1 + sz;
        }
        p += 1 + sz;
    }
    if (depth) return -1;
    /* A vendor span is removable if it is last, or the following span starts with a
     * Usage Page item (so the next TLC does not inherit the vendor page). */
    for (i = 0; i < ntlc; i++)
        if (vend[i] && i + 1 < ntlc && (d[e[i]] & 0xfc) != 0x04) vend[i] = 0;
    /* Compact. Trailing bytes after the last TLC are kept. */
    {
        hm_u32 out = 0, from = 0, k;
        for (i = 0; i < ntlc; i++) {
            if (!vend[i]) continue;
            /* copy [from, s[i]) */
            for (k = from; k < s[i]; k++) d[out++] = d[k];
            from = e[i];
            r->removed_tlcs++;
        }
        for (k = from; k < n; k++) d[out++] = d[k];
        r->len = out;
    }
    /* Input report IDs that remain (for dropping reports Windows has no layout for). */
    hm_zero(ids_tmp, sizeof(ids_tmp));
    p = 0; cur_id = 0;
    while (p < r->len) {
        hm_u32 b = d[p], sz = b & 3, v = 0, k2, tag = b >> 4, type = (b >> 2) & 3;
        if (b == 0xfe) { p += 3u + d[p + 1]; continue; }
        if (sz == 3) sz = 4;
        for (k2 = 0; k2 < sz; k2++) v |= (hm_u32)d[p + 1 + k2] << (8 * k2);
        if (type == 1 && tag == 8) cur_id = v & 0xff;
        if (type == 0 && tag == 8) ids_tmp[cur_id >> 3] |= (hm_u8)(1u << (cur_id & 7));
        p += 1 + sz;
    }
    hm_copy(r->input_ids, ids_tmp, sizeof(ids_tmp));
    return 0;
}
