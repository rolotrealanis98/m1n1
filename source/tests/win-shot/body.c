
/* ---- nightly-12 win-shot host test body ---- */
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static u8 *fb_mem; static size_t fb_size;
static u8 *sh_mem; static size_t sh_size;

/* a panel of w x h with the given stride and depth; the allocation is exactly stride * h, so ASan catches any read
 * outside the surface */
static void panel(u32 w, u32 h, u32 stride, u32 depth, u32 (*px)(u32 x, u32 y))
{
    free(fb_mem);
    fb_size = (size_t)stride * h;
    fb_mem = malloc(fb_size);
    memset(fb_mem, 0xa5, fb_size);
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++)
            ((u32 *)(fb_mem + (size_t)y * stride))[x] = px(x, y);
    cur_boot_args.video = (struct boot_video){(u64)fb_mem, 0, stride, w, h, depth};
    mapped_lo = (u64)fb_mem; mapped_hi = (u64)fb_mem + fb_size;
}

static void reset_events(void)
{
    for (int i = 0; i < nev; i++) free(evs[i].data);
    nev = 0; partial_writes = 0;
}

static bool request(int src, int scale, u32 x, u32 y, u32 w, u32 h)
{
    char s[32];
    snprintf(s, sizeof(s), "%x%x%04x%04x%04x%04x", src, scale, x, y, w, h);
    bool all = hv_win_shot_rx(HV_WIN_SHOT_BYTE);
    for (int i = 0; s[i]; i++) all = hv_win_shot_rx((u8)s[i]) && all;
    return all;
}

/* run ticks until idle (the host drains the ring between ticks); returns ticks used */
static int run(int max_ticks)
{
    int t = 0;
    while (t < max_ticks) {
        hv_win_shot_tick();
        t++;
        ring_free = 1 << 20;
        if (!hv_win_shot_stat(7)) break;
    }
    return t;
}

static int find_kind(int kind) { for (int i = 0; i < nev; i++) if (evs[i].kind == kind) return i; return -1; }

/* checks the event sequence BEGIN, ROW 0..oh-1, END and returns the image (w*h*3) */
static u8 *collect(u32 *ow, u32 *oh)
{
    CHECK(nev >= 2 && evs[0].kind == 0, "first event BEGIN (nev %d kind %d)", nev, nev ? evs[0].kind : -1);
    if (nev < 2 || evs[0].kind != 0) return NULL;
    *ow = evs[0].w; *oh = evs[0].h;
    CHECK(nev == (int)*oh + 2, "events %d for %u rows", nev, *oh);
    CHECK(evs[nev - 1].kind == 2 && evs[nev - 1].row == *oh, "END last with row count");
    u8 *img = malloc((size_t)*ow * *oh * 3 + 1);
    for (u32 r = 0; r < *oh && (int)r + 1 < nev; r++) {
        struct ev *e = &evs[r + 1];
        CHECK(e->type == EVT_M5W_SHOT && e->kind == 1 && e->row == r && e->len == 24 + *ow * 3,
              "row %u: type %u kind %u row %u len %u", r, e->type, e->kind, e->row, e->len);
        if (e->len == 24 + *ow * 3) memcpy(img + (size_t)r * *ow * 3, e->data, (size_t)*ow * 3);
    }
    return img;
}

static u32 px10(u32 x, u32 y) { u32 r = (x * 37 + y * 11) & 0x3ff, g = (x * 5 + y * 71) & 0x3ff, b = (x ^ y) * 13 & 0x3ff;
                                return 0xc0000000u | r << 20 | g << 10 | b; }
static u32 px8(u32 x, u32 y) { u32 r = (x * 37 + y * 11) & 0xff, g = (x * 5 + y * 71) & 0xff, b = (x ^ y) * 13 & 0xff;
                               return 0xff000000u | r << 16 | g << 8 | b; }
static u32 white10(u32 x, u32 y) { (void)x; (void)y; return 0x3fffffffu; }

/* independent reference: mean over the block in floating point, then scale to 8 bits */
static int ref(u32 (*px)(u32, u32), bool ten, u32 x0, u32 y0, u32 s, int c)
{
    double sum = 0;
    for (u32 dy = 0; dy < s; dy++)
        for (u32 dx = 0; dx < s; dx++) {
            u32 v = px(x0 + dx, y0 + dy);
            sum += ten ? (v >> (20 - 10 * c)) & 0x3ff : (v >> (16 - 8 * c)) & 0xff;
        }
    double m = sum / (s * s);
    return (int)(ten ? m * 255.0 / 1023.0 + 0.5 : m + 0.5);
}

static void test_capture(const char *name, u32 (*px)(u32, u32), bool ten, u32 W, u32 H, u32 stride, int scale,
                         u32 x, u32 y, u32 w, u32 h, int tol)
{
    panel(W, H, stride, ten ? 30 : 32, px);
    reset_events();
    CHECK(request(0, scale, x, y, w, h), "%s: request consumed", name);
    run(100000);
    u32 ow = 0, oh = 0;
    u8 *img = collect(&ow, &oh);
    if (!w || !h) { x = y = 0; w = W; h = H; }
    if (w > W - x) w = W - x;
    if (h > H - y) h = H - y;
    CHECK(ow == w / scale && oh == h / scale, "%s: out %ux%u want %ux%u", name, ow, oh, w / scale, h / scale);
    CHECK(evs[0].fmt == (ten ? 0 : 1), "%s: fmt", name);
    int worst = 0;
    if (img)
        for (u32 r = 0; r < oh; r++)
            for (u32 c = 0; c < ow; c++)
                for (int k = 0; k < 3; k++) {
                    int d = abs(img[((size_t)r * ow + c) * 3 + k] - ref(px, ten, x + c * scale, y + r * scale, scale, k));
                    if (d > worst) worst = d;
                }
    CHECK(worst <= tol, "%s: max channel error %d > %d", name, worst, tol);
    free(img);
}

static void test_fail(const char *name, int src, int scale, u32 x, u32 y, u32 w, u32 h, u32 reason)
{
    reset_events();
    request(src, scale, x, y, w, h);
    run(100);
    CHECK(nev == 1 && evs[0].kind == 3 && evs[0].aux0 == reason, "%s: FAIL reason %u (nev %d kind %d aux0 %u)",
          name, reason, nev, nev ? evs[0].kind : -1, nev ? evs[0].aux0 : 0);
    CHECK(!hv_win_shot_stat(7), "%s: idle after FAIL", name);
}

int main(void)
{
    /* 1. formats and box downscale */
    test_capture("10-bit 4x full", px10, true, 64, 40, 64 * 4 + 32, 4, 0, 0, 0, 0, 1);
    test_capture("10-bit 1x full", px10, true, 48, 20, 48 * 4, 1, 0, 0, 0, 0, 0);
    test_capture("8-bit 2x crop", px8, false, 64, 40, 64 * 4, 2, 10, 6, 20, 12, 0);
    test_capture("8-bit 8x", px8, false, 70, 50, 80 * 4, 8, 0, 0, 0, 0, 0);
    test_capture("white 10-bit", white10, true, 32, 32, 32 * 4, 4, 0, 0, 0, 0, 0);
    reset_events();
    request(0, 4, 0, 0, 0, 0); run(1000);
    CHECK(nev > 1 && evs[1].data[0] == 255 && evs[1].data[1] == 255 && evs[1].data[2] == 255, "white -> 255");
    /* 2. bounds: region at and past the bottom-right corner (ASan: the allocation is exactly the surface) */
    test_capture("corner 1x", px10, true, 64, 40, 64 * 4, 1, 60, 37, 100, 100, 0);
    test_capture("corner 4x", px10, true, 66, 41, 66 * 4, 4, 1, 1, 0xffff, 0xffff, 1);
    /* 3. failures */
    panel(64, 40, 64 * 4, 30, px10);
    test_fail("scale 3", 0, 3, 0, 0, 0, 0, 4);
    test_fail("scale 0", 0, 0, 0, 0, 0, 0, 4);
    test_fail("x outside", 0, 1, 64, 0, 4, 4, 5);
    test_fail("y outside", 0, 1, 0, 40, 4, 4, 5);
    test_fail("region smaller than scale", 0, 8, 60, 0, 4, 40, 5);
    test_fail("src 2", 2, 1, 0, 0, 0, 0, 2);
    test_fail("shadow off", 1, 1, 0, 0, 0, 0, 2);
    cur_boot_args.video.depth = 16;
    test_fail("depth 16", 0, 1, 0, 0, 0, 0, 3);
    cur_boot_args.video.depth = 30;
    cur_boot_args.video.width = 5000; cur_boot_args.video.stride = 64 * 4;
    test_fail("stride < width*4", 0, 1, 0, 0, 0, 0, 2);
    panel(64, 40, 64 * 4, 30, px10);
    mapped_hi = mapped_lo + 64 * 4 * 20;          /* lower half not mapped at EL2 */
    test_fail("unmapped", 0, 1, 0, 0, 0, 0, 6);
    panel(64, 40, 64 * 4, 30, px10);
    /* 4. shadow source: BGRA8 at its own size */
    sh_size = 40 * 32 * 4; sh_mem = malloc(sh_size);
    for (u32 y = 0; y < 32; y++) for (u32 x = 0; x < 40; x++) ((u32 *)sh_mem)[y * 40 + x] = px8(x, y);
    shadow_on = true; shadow_base = (u64)sh_mem; shadow_stride = 40 * 4; shadow_w = 40; shadow_h = 32;
    mapped_lo = (u64)sh_mem; mapped_hi = (u64)sh_mem + sh_size;
    reset_events(); request(1, 2, 0, 0, 0, 0); run(1000);
    { u32 ow, oh; u8 *img = collect(&ow, &oh);
      CHECK(ow == 20 && oh == 16 && (evs[0].scale_src >> 8) == 1 && evs[0].fmt == 1, "shadow 20x16 src 1 fmt 8-bit");
      CHECK(img && img[0] == ref(px8, false, 0, 0, 2, 0) && img[(16 * 20 - 1) * 3 + 2] == ref(px8, false, 38, 30, 2, 2),
            "shadow pixels");
      free(img); }
    shadow_on = false;
    panel(64, 40, 64 * 4, 30, px10);
    /* 5. request receiver: '!' and the heartbeat byte are never swallowed by a partial request */
    reset_events();
    CHECK(hv_win_shot_rx(HV_WIN_SHOT_BYTE) && hv_win_shot_rx('0') && hv_win_shot_rx('4'), "partial request consumed");
    CHECK(!hv_win_shot_rx('!'), "'!' handed back mid-request");
    CHECK(!hv_win_shot_rx('a'), "after an abort, hex digits are not consumed");
    CHECK(!hv_win_shot_rx(0x16), "heartbeat byte not consumed when idle");
    hv_win_shot_rx(HV_WIN_SHOT_BYTE); hv_win_shot_rx('0');
    CHECK(!hv_win_shot_rx(0x16), "heartbeat byte handed back mid-request");
    run(10); CHECK(nev == 0, "aborted request started nothing");
    hv_win_shot_rx(HV_WIN_SHOT_BYTE); hv_win_shot_rx('0'); hv_win_shot_rx('4');
    u64 dropped = hv_win_shot_stat(6);
    CHECK(request(0, 4, 0, 0, 0, 0), "a new request byte restarts");
    CHECK(hv_win_shot_stat(6) == dropped + 1, "restart counted as a dropped partial");
    run(1000); CHECK(find_kind(2) >= 0 && nev == 12, "restarted request completes (nev %d)", nev);
    /* partial request timeout (500 ms) */
    reset_events();
    hv_win_shot_rx(HV_WIN_SHOT_BYTE); hv_win_shot_rx('0');
    now_ticks += freq;                           /* 1 s later */
    hv_win_shot_tick();
    CHECK(!hv_win_shot_rx('4'), "timed-out partial request dropped");
    /* 6. the ring: an event is queued whole or not at all; the tick never waits */
    reset_events();
    request(0, 1, 0, 0, 0, 0);                   /* 48-pixel... 64 x 40 at 1x: rows of 192 bytes */
    ring_free = 100;                             /* less than one row */
    hv_win_shot_tick();
    CHECK(nev == 1 && evs[0].kind == 0, "BEGIN only (nev %d)", nev);
    for (int i = 0; i < 5; i++) { ring_free = 100; hv_win_shot_tick(); }
    CHECK(nev == 1 && partial_writes == 0, "no row without room, nothing torn");
    ring_free = 8 + 24 + 64 * 3 + 4;             /* exactly one row */
    hv_win_shot_tick();
    CHECK(nev == 2 && evs[1].row == 0 && partial_writes == 0, "exactly one row when one fits");
    run(100000);
    CHECK(find_kind(2) == nev - 1 && nev == 42 && partial_writes == 0, "completes after the host reads (nev %d)", nev);
    CHECK(lock_depth == 0, "iodev lock balanced");
    /* host stops reading: abort after 30 s with FAIL(timeout), never blocks */
    reset_events();
    request(0, 1, 0, 0, 0, 0);
    ring_free = 36 + 5 * (36 + 64 * 3); hv_win_shot_tick();     /* BEGIN + 5 rows */
    int got = nev;
    for (int i = 0; i < 31; i++) { ring_free = 0; now_ticks += freq; hv_win_shot_tick(); }
    CHECK(nev == got && hv_win_shot_stat(7), "nothing queued with a full ring, still active");
    ring_free = 1 << 20; now_ticks += freq; hv_win_shot_tick();
    CHECK(nev == got + 1 && evs[nev - 1].kind == 3 && evs[nev - 1].aux0 == 7, "FAIL timeout after 30 s");
    CHECK(!hv_win_shot_stat(7), "idle after timeout");
    /* a FAIL event that never finds room: the capture still ends (5 s grace), silently */
    reset_events();
    request(0, 1, 0, 0, 0, 0);
    for (int i = 0; i < 40; i++) { ring_free = 0; now_ticks += freq; hv_win_shot_tick(); }
    CHECK(nev == 0 && !hv_win_shot_stat(7), "gave up without room");
    /* 7. one at a time */
    reset_events();
    request(0, 1, 0, 0, 0, 0);
    ring_free = 8 + 24 + 4 + 8 + 24 + 64 * 3 + 4; hv_win_shot_tick();
    u64 failed = hv_win_shot_stat(2);
    request(0, 4, 0, 0, 0, 0);
    CHECK(hv_win_shot_stat(2) == failed + 1, "second request while busy refused");
    run(100000);
    CHECK(nev == 42 && evs[0].w == 64, "the running capture finished unchanged (nev %d)", nev);
    /* 8. time budget (150 us): with 200 us per counter read, one row per tick; never zero rows per tick */
    reset_events();
    request(0, 1, 0, 0, 0, 0);
    tick_step = freq / 5000;
    hv_win_shot_tick();
    CHECK(nev == 2, "one row per tick when every row exceeds the budget (nev %d)", nev);
    tick_step = 0;
    run(100000);
    /* 9. not a USB proxy link: no events at all, timeout, no crash */
    reset_events();
    uartproxy_iodev = IODEV_UART;
    request(0, 1, 0, 0, 0, 0);
    for (int i = 0; i < 40; i++) { now_ticks += freq; hv_win_shot_tick(); }
    CHECK(nev == 0 && !hv_win_shot_stat(7), "UART link: nothing sent, gave up");
    uartproxy_iodev = IODEV_USB0;
    /* 10. the host never supplies an address: random requests over two surfaces stay inside them (ASan) */
    srand(12345);
    shadow_on = true;
    int completed = 0, failedr = 0;
    for (int i = 0; i < 3000; i++) {
        u32 W = 1 + rand() % 90, H = 1 + rand() % 70, pad = (rand() % 4) * 4;
        panel(W, H, W * 4 + pad, (rand() & 1) ? 30 : 32, (rand() & 1) ? px10 : px8);
        int src = rand() % 3 == 0;
        if (src) { mapped_lo = (u64)sh_mem; mapped_hi = (u64)sh_mem + sh_size; }
        static const int sc[] = {1, 2, 4, 8, 3};
        reset_events();
        request(src, sc[rand() % 5], rand() % 120, rand() % 100, rand() % 3 ? rand() % 140 : 0, rand() % 3 ? rand() % 110 : 0);
        run(100000);
        if (find_kind(2) >= 0) completed++;
        else if (find_kind(3) >= 0) failedr++;
        CHECK(partial_writes == 0, "random %d torn", i);
    }
    CHECK(completed > 1000 && failedr > 100 && completed + failedr == 3000, "random: %d done, %d refused", completed, failedr);
    reset_events();
    free(fb_mem); free(sh_mem);
    printf("x");
    fprintf(stdout, "win-shot: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
