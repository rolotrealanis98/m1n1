/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "kboot.h"
#include "adt.h"
#include "assert.h"
#include "firmware.h"
#include "malloc.h"
#include "math.h"
#include "pmgr.h"
#include "soc.h"
#include "string.h"
#include "utils.h"
#include "xnuboot.h"

#include "libfdt/libfdt.h"

#define bail(...)                                                                                  \
    do {                                                                                           \
        printf(__VA_ARGS__);                                                                       \
        return -1;                                                                                 \
    } while (0)

#define MAX_PSTATES  16
#define MAX_CLUSTERS 8
#define MAX_DIES     2

struct perf_state {
    u32 freq;
    u32 volt;
};

struct aux_perf_state {
    u64 volt;
    u64 freq;
};

struct aux_perf_states {
    u64 dies;
    u64 count;
    struct aux_perf_state states[];
};

int32_t rust_gpu_initdata_size(uint32_t compat_maj, uint32_t compat_min, size_t *data_a,
                               size_t *data_b, size_t *globals);

struct initdata_inputs {
    size_t perf_state_table_count;
    size_t perf_state_count;
    const struct perf_state *c_perf_states;
    uint32_t *max_pwr;
    float *core_leak;
    float *sram_leak;
    float *cs_leak;
    float *afr_leak;
    size_t n_perf_states_cs;
    const struct aux_perf_state *pstates_cs;
    size_t n_perf_states_afr;
    const struct aux_perf_state *pstates_afr;
    uint32_t compat_maj;
    uint32_t compat_min;
};

int32_t rust_fill_gpu_initdata(struct initdata_inputs *ins, void *data_a, void *data_b,
                               void *globals);

static int get_core_counts(u32 *count, u32 nclusters, u32 ncores)
{
    u64 base;
    pmgr_adt_power_enable("/arm-io/sgx");

    int adt_sgx_path[8];
    if (adt_path_offset_trace(adt, "/arm-io/sgx", adt_sgx_path) < 0)
        bail("ADT: GPU: Failed to get sgx\n");

    if (adt_get_reg(adt, adt_sgx_path, "reg", 0, &base, NULL) < 0)
        bail("ADT: GPU: Failed to get sgx reg 0\n");

    u32 cores[3] = {0, 0, 0};

    switch (chip_id) {
        case T6002:
            cores[1] = read32(base + 0xd01514);
            /* fallthrough */
        case T8103:
        case T8112:
        case T6000:
        case T6001:
            cores[0] = read32(base + 0xd01500);
            break;
        case T6020:
        case T6021:
        case T6022:
            cores[0] = read32(base + 0xe01500);
            cores[1] = read32(base + 0xe01504);
            cores[2] = read32(base + 0xe01508);
            break;
    }

    for (u32 i = 0; i < nclusters; i++) {
        count[i] = __builtin_popcount(cores[0] & MASK(ncores));

        for (u32 j = 0; j < ARRAY_SIZE(cores); j++) {
            cores[j] >>= ncores;
            if (j < (ARRAY_SIZE(cores) - 1))
                cores[j] |= cores[j + 1] << (32 - ncores);
        }
    }

    return 0;
}

static void adjust_leakage(float *val, u32 clusters, u32 *cores, u32 max, float uncore_fraction)
{
    for (u32 i = 0; i < clusters; i++) {
        float uncore = val[i] * uncore_fraction;
        float core = val[i] - uncore;

        val[i] = uncore + (cores[i] / (float)max) * core;
    }
}

static void load_fuses(float *out, u32 count, u64 base, u32 start, u32 width, float scale,
                       float offset, bool flip)
{
    for (u32 i = 0; i < count; i++) {
        base += (start / 32) * 4;
        start &= 31;

        u32 low = read32(base);
        u32 high = read32(base + 4);
        u32 val = (((((u64)high) << 32) | low) >> start) & MASK(width);

        float fval = (float)val * scale + offset;

        if (flip)
            out[count - i - 1] = fval;
        else
            out[i] = fval;

        start += width;
    }
}

static u32 t8103_pwr_scale[] = {0, 63, 80, 108, 150, 198, 210};

static int calc_power_t8103(u32 count, u32 table_count, const struct perf_state *core,
                            const struct perf_state *sram, const struct aux_perf_states *cs,
                            u32 *max_pwr, float *core_leak, float *sram_leak, float *cs_leak,
                            float *afr_leak)
{
    UNUSED(sram);
    UNUSED(cs);
    UNUSED(core_leak);
    UNUSED(sram_leak);
    UNUSED(cs_leak);
    UNUSED(afr_leak);
    u32 *pwr_scale;
    u32 pwr_scale_count;
    u32 core_count;
    u32 max_cores;

    switch (chip_id) {
        case T8103:
            pwr_scale = t8103_pwr_scale;
            pwr_scale_count = ARRAY_SIZE(t8103_pwr_scale);
            max_cores = 8;
            break;
        default:
            bail("ADT: GPU: Unsupported chip\n");
    }

    if (get_core_counts(&core_count, 1, max_cores))
        return -1;

    if (table_count != 1)
        bail("ADT: GPU: expected 1 perf state table but got %d\n", table_count);

    if (count != pwr_scale_count)
        bail("ADT: GPU: expected %d perf states but got %d\n", pwr_scale_count, count);

    for (u32 i = 0; i < pwr_scale_count; i++)
        max_pwr[i] = (u32)core[i].volt * (u32)pwr_scale[i] * 100;

    core_leak[0] = 1000.0;
    sram_leak[0] = 45.0;

    adjust_leakage(core_leak, 1, &core_count, max_cores, 0.12);
    adjust_leakage(sram_leak, 1, &core_count, max_cores, 0.2);

    return 0;
}

static int calc_power_t600x(u32 count, u32 table_count, const struct perf_state *core,
                            const struct perf_state *sram, const struct aux_perf_states *cs,
                            u32 *max_pwr, float *core_leak, float *sram_leak, float *cs_leak,
                            float *afr_leak)
{
    float s_sram, k_sram, s_core, k_core, s_cs, k_cs;
    float dk_core, dk_sram = 0, dk_cs = 0;
    float imax = 1000;

    u32 ndies = 1;
    u32 nclusters = 0;
    u32 ncores = 0;
    u32 core_count[MAX_CLUSTERS];

    bool simple_exps = false;
    bool adjust_leakages = true;
    bool has_cs = false;

    switch (chip_id) {
        case T6002:
            ndies = 2;
            nclusters += 4;
            load_fuses(core_leak + 4, 4, 0x22922bc1b8, 25, 13, 2, 2, true);
            load_fuses(sram_leak + 4, 4, 0x22922bc1cc, 4, 9, 1, 1, true);
            // fallthrough
        case T6001:
            nclusters += 2;
        case T6000:
            nclusters += 2;
            load_fuses(core_leak + 0, min(4, nclusters), 0x2922bc1b8, 25, 13, 2, 2, false);
            load_fuses(sram_leak + 0, min(4, nclusters), 0x2922bc1cc, 4, 9, 1, 1, false);

            s_sram = 4.3547606;
            k_sram = 0.024927923;
            // macOS difference: macOS uses a misbehaved piecewise function here
            // Since it's obviously wrong, let's just use only the first component
            s_core = 1.48461742;
            k_core = 0.39013552;
            dk_core = 1.06975;
            dk_sram = 0.00625;

            ncores = 8;
            adjust_leakages = true;
            imax = 26.0;
            break;
        case T8112:
            nclusters = 1;
            load_fuses(core_leak, 1, 0x23d2c84dc, 30, 13, 2, 2, false);
            load_fuses(sram_leak, 1, 0x23d2c84b0, 15, 9, 1, 1, false);

            s_sram = 3.61619841;
            k_sram = 0.0529281;
            // macOS difference: macOS uses a misbehaved piecewise function here
            // Since it's obviously wrong, let's just use only the first component
            s_core = 1.21356187;
            k_core = 0.43328839;
            dk_core = 0.983196;
            dk_sram = 0.007828;

            simple_exps = true;
            ncores = 10;
            adjust_leakages = false; // pre-adjusted?
            imax = 24.0;
            break;
        case T6022:
            ndies = 2;
            nclusters += 4;
            load_fuses(core_leak + 4, min(4, nclusters), 0x229e2cc1f8, 4, 13, 2, 2, true);
            load_fuses(sram_leak + 4, min(4, nclusters), 0x229e2cc208, 19, 9, 1, 1, true);
            load_fuses(cs_leak + 1, 1, 0x229e2cc204, 8, 12, 1, 1, false);
            load_fuses(afr_leak + 1, 1, 0x229e2cc210, 0, 12, 1, 1, false);

            // For some reason, this one is different on T6022...
            dk_cs = 6.7;
            // fallthrough
        case T6021:
            if (!dk_cs)
                dk_cs = 4.492;

            nclusters += 4;
            s_sram = 5.808;
            k_sram = 0.00707;
            // macOS difference: macOS uses a misbehaved piecewise function here
            // Since it's obviously wrong, let's just use only the first component
            s_core = 1.24554153;
            k_core = 0.56203084;

            s_cs = 1.87;
            k_cs = 0.162;

            goto t602x;

        case T6020:
            nclusters = 2;
            s_sram = 5.02191218;
            k_sram = 0.0145621013;
            // macOS difference: macOS uses a misbehaved piecewise function here
            // Since it's obviously wrong, let's just use only the first component
            s_core = 1.21006932;
            k_core = 0.52776378;

            s_cs = 1.8;
            k_cs = 0.162;
            dk_cs = 1.889;

        t602x:
            dk_core = 1.00075;
            dk_sram = 0.00785;
            load_fuses(core_leak + 0, min(4, nclusters), 0x29e2cc1f8, 4, 13, 2, 2, false);
            load_fuses(sram_leak + 0, min(4, nclusters), 0x29e2cc208, 19, 9, 1, 1, false);
            load_fuses(cs_leak + 0, 1, 0x29e2cc204, 8, 12, 1, 1, false);
            load_fuses(afr_leak + 0, 1, 0x29e2cc210, 0, 12, 1, 1, false);

            simple_exps = true;
            ncores = 10;
            adjust_leakages = false; // pre-adjusted?
            imax = 33.0;
            has_cs = true;
            break;

        default:
            bail("ADT: GPU: Unsupported chip\n");
    }

    if (get_core_counts(core_count, nclusters, ncores))
        return -1;

    printf("FDT: GPU: Core counts: ");
    for (u32 i = 0; i < nclusters; i++) {
        printf("%d ", core_count[i]);
    }
    printf("\n");

    if (adjust_leakages) {
        adjust_leakage(core_leak, nclusters, core_count, ncores, 0.0825);
        adjust_leakage(sram_leak, nclusters, core_count, ncores, 0.2247);
    }

    if (table_count != nclusters)
        bail("ADT: GPU: expected %d perf state tables but got %d\n", nclusters, table_count);

    if (has_cs && (!cs || !cs_leak)) {
        bail("ADT: GPU: expected CS perf table, but not found\n");
    }

    max_pwr[0] = 0;

    for (u32 i = 1; i < count; i++) {
        u32 total_mw = 0;

        for (u32 j = 0; j < nclusters; j++) {
            // macOS difference: macOS truncates Hz to integer MHz before doing this math.
            // That's probably wrong, so let's not do that.

            float mw = 0;
            size_t idx = j * count + i;

            mw += sram[idx].volt / 1000.f * sram_leak[j] * k_sram *
                  expf(sram[idx].volt / 1000.f * s_sram);
            mw += core[idx].volt / 1000.f * core_leak[j] * k_core *
                  expf(core[idx].volt / 1000.f * s_core);

            float sbase = sram[idx].volt / 750.f;
            float sram_v_p;
            if (simple_exps)
                sram_v_p = sbase * sbase; // v ^ 2
            else
                sram_v_p = sbase * sbase * sbase; // v ^ 3
            mw += dk_sram * core_count[j] * (sram[idx].freq / 1000000.f) * sram_v_p;

            float cbase = core[idx].volt / 750.f;
            float core_v_p;
            if (simple_exps || core[idx].volt < 750)
                core_v_p = cbase * cbase; // v ^ 2
            else
                core_v_p = cbase * cbase * cbase; // v ^ 3
            mw += dk_core * core_count[j] * (core[idx].freq / 1000000.f) * core_v_p;

            if (mw > imax * core[idx].volt)
                mw = imax * core[idx].volt;

            total_mw += mw;
        }

        // CS gets added after the imax limit

        if (has_cs) {
            for (u32 j = 0; j < ndies; j++) {
                float mw = 0;

                int csi = j * cs->count + min(i, cs->count - 1);
                u32 cs_mv = cs->states[csi].volt / 1000;
                u32 cs_hz = cs->states[csi].freq;

                mw += cs_mv / 1000.f * cs_leak[j] * k_cs * expf(cs_mv / 1000.f * s_cs);
                float csbase = cs_mv / 750.f;
                float cs_v_p = powf(csbase, 1.8);
                mw += dk_cs * (cs_hz / 1000000.f) * cs_v_p;

                total_mw += mw;
            }
        }

        max_pwr[i] = total_mw * 1000;
    }

    return 0;
}

static int dt_set_resvmem(void *dt, const char *path, u64 base, u64 size)
{
    int node = fdt_path_offset(dt, path);
    if (node < 0)
        bail("FDT: GPU: failed to find %s node\n", path);

    fdt64_t reg[2];

    fdt64_st(&reg[0], base);
    fdt64_st(&reg[1], size);

    if (fdt_setprop_string(dt, node, "status", "okay"))
        bail("FDT: GPU: failed to un-disable memory region");

    if (fdt_setprop(dt, node, "reg", reg, sizeof(reg)))
        bail("FDT: GPU: failed to set reg prop for %s\n", path);

    return 0;
}

static int dt_set_region(void *dt, int sgx, const char *name, const char *path)
{
    u64 base, size;
    char prop[64];

    snprintf(prop, sizeof(prop), "%s-base", name);
    if (ADT_GETPROP(adt, sgx, prop, &base) < 0 || !base)
        bail("ADT: GPU: failed to find %s property\n", prop);

    snprintf(prop, sizeof(prop), "%s-size", name);
    if (ADT_GETPROP(adt, sgx, prop, &size) < 0 || !base)
        bail("ADT: GPU: failed to find %s property\n", prop);

    return dt_set_resvmem(dt, path, base, size);
}

int fdt_set_float_array(void *dt, int node, const char *name, float *val, int count)
{
    fdt32_t data[MAX_CLUSTERS];

    if (count > MAX_CLUSTERS)
        bail("FDT: GPU: fdt_set_float_array() with too many values\n");

    memcpy(data, val, sizeof(float) * count);
    for (int i = 0; i < count; i++) {
        data[i] = cpu_to_fdt32(data[i]);
    }

    if (fdt_setprop_inplace(dt, node, name, data, sizeof(u32) * count))
        bail("FDT: GPU: Failed to set %s\n", name);

    return 0;
}

static int fdt_set_aux_opp(void *dt, int gpu, const char *prop, const struct aux_perf_states *ps,
                           u32 dies)
{
    int len;
    const fdt32_t *opps_ph = fdt_getprop(dt, gpu, prop, &len);
    if (!opps_ph || len != 4)
        bail("FDT: GPU: %s not found\n", prop);

    int opps = fdt_node_offset_by_phandle(dt, fdt32_ld(opps_ph));
    if (opps < 0)
        bail("FDT: GPU: node for phandle %u not found\n", fdt32_ld(opps_ph));

    u32 count = ps->count;

    u32 i = 0;
    int opp;
    fdt_for_each_subnode(opp, dt, opps)
    {
        fdt32_t volts[MAX_DIES];

        for (u32 j = 0; j < dies; j++) {
            volts[j] = cpu_to_fdt32(ps->states[i + j * ps->count].volt);
        }

        if (i >= count)
            bail("FDT: GPU: Expected %d operating points, but found more\n", count);

        if (fdt_setprop_inplace(dt, opp, "opp-microvolt", &volts, sizeof(u32) * dies))
            bail("FDT: GPU: Failed to set opp-microvolt for aux PS %d\n", i);

        if (fdt_setprop_inplace_u64(dt, opp, "opp-hz", ps->states[i].freq))
            bail("FDT: GPU: Failed to set opp-hz for PS %d\n", i);

        i++;
    }

    return 0;
}

/*
 * T6050 (M5 Pro / M5 Max) GPU hand-off for aurora-silicon/linux's G17 M5 driver (compatible
 * apple,agx-g17s), opt-in: it runs only with the payload variable chosen.m5pro,gpu=1. Without
 * the variable every GPU node of the DT stays as the DT has it (disabled), as before.
 *
 * iBoot loads both GPU coprocessor images (/arm-io/gfx-asc, /arm-io/gfx1-asc: pre-loaded, not
 * started) and publishes the UAT regions in /arm-io/sgx. From the ADT of this boot this fills in:
 *  - the UAT regions the driver adopts: sgx gpu-region, gfx-shared-region, gfx-handoff and
 *    gfx-shared-l2-region as the GPU's memory-region "ttbs", "pagetables", "handoff" and
 *    "l2-pagetables";
 *  - "fw-text" from the start of gfx-asc __TEXT to the end of gfx1-asc __TEXT, and "fw-data" from
 *    the start of gfx-asc __DATA to the end of gfx1-asc __DATA_SHARED_RO, both from
 *    segment-ranges. Each span must be contiguous and the DATA bases must agree with the sgx
 *    gfx{,1}-data{,-shared-ro}-base properties. The images move between iBoot builds (by 0x8000
 *    from mBoot 41.x to 60.65), so nothing here is hardcoded;
 *  - the reg of each mailbox the GPU node's mboxes name: the ADT wrapper (reg[0]) and iop-vbar
 *    (reg[1], cut to the 8 bytes the mailbox driver maps) of the coprocessor whose ADT role
 *    matches the mailbox's apple,firmware-role (GFX, GFX1).
 * It checks the GPU node's sgx window, the mailboxes' interrupts and the PMGR power-state
 * registers of the GPU's power domain node against the ADT. Only if every check passes are the
 * six regions, the mailboxes, the GPU node and the PMGR node set "okay" (in that order). Any
 * mismatch leaves all of them disabled and the boot goes on: this never fails the boot.
 * It writes no register. It reads the PMGR power-state words of the GPU domains once, for the log
 * (pmgr_init() reads every PMGR power-state word on every boot already).
 */

#define T6050_GPU_PFX "FDT: GPU: T6050: "
/* SGX_SIZE in drivers/gpu/drm/asahi/regs.rs: the part of sgx the driver maps */
#define T6050_SGX_MAP_SIZE 0x1000000

enum t6050_gpu_region_id {
    T6050_TTBS,
    T6050_PAGETABLES,
    T6050_HANDOFF,
    T6050_L2_PAGETABLES,
    T6050_FW_TEXT,
    T6050_FW_DATA,
    T6050_NUM_REGIONS,
};

/* memory-region-names of the GPU node, and where each region comes from in the ADT */
static const char *const t6050_region_names[T6050_NUM_REGIONS] = {
    "ttbs", "pagetables", "handoff", "l2-pagetables", "fw-text", "fw-data",
};

static const char *const t6050_sgx_props[T6050_FW_TEXT] = {
    "gpu-region",
    "gfx-shared-region",
    "gfx-handoff",
    "gfx-shared-l2-region",
};

/* The two GPU coprocessors, by their ADT role (= the mailboxes' apple,firmware-role) */
static const char *const t6050_asc_paths[2] = {"/arm-io/gfx-asc", "/arm-io/gfx1-asc"};

/* The PMGR power domains of the GPU, by DT label = ADT pmgr device name in lower case */
static const char *const t6050_ps_names[] = {"gpx", "afr", "cs", "gfx"};

struct t6050_gpu_mbox {
    u32 phandle;
    u64 wrapper, wrapper_size;
    u64 vbar;
};

struct t6050_gpu_handoff {
    u64 base[T6050_NUM_REGIONS];
    u64 size[T6050_NUM_REGIONS];
    u32 region_phandle[T6050_NUM_REGIONS];
    struct t6050_gpu_mbox mbox[2];
    u32 ps_phandle; /* the GPU's power domain; its parent node is the PMGR node */
};

static u64 t6050_cells(const fdt32_t *p, int n)
{
    u64 v = 0;

    for (int i = 0; i < n; i++)
        v = (v << 32) | fdt32_ld(&p[i]);
    return v;
}

/* reg entry <idx> of a DT node, in its parent's cells. The buses involved map 1:1. */
static int t6050_dt_reg(void *dt, int node, int idx, u64 *base, u64 *size)
{
    int parent = fdt_parent_offset(dt, node);
    if (parent < 0)
        return -1;

    int ac = fdt_address_cells(dt, parent);
    int sc = fdt_size_cells(dt, parent);
    if (ac < 1 || ac > 2 || sc < 1 || sc > 2)
        return -1;

    int len;
    const fdt32_t *reg = fdt_getprop(dt, node, "reg", &len);
    if (!reg || len < (idx + 1) * (ac + sc) * 4)
        return -1;

    reg += idx * (ac + sc);
    *base = t6050_cells(reg, ac);
    *size = t6050_cells(reg + ac, sc);
    return 0;
}

/* The GPU node: the "gpu" alias, else the first apple,agx-g17s node */
static int t6050_gpu_node(void *dt)
{
    int gpu = fdt_path_offset(dt, "gpu");

    if (gpu < 0)
        gpu = fdt_node_offset_by_compatible(dt, -1, "apple,agx-g17s");
    if (gpu < 0 || fdt_node_check_compatible(dt, gpu, "apple,agx-g17s"))
        return -1;
    return gpu;
}

/* phys and size of segment <name> of the ADT coprocessor node <path> */
static int t6050_asc_segment(const char *path, const char *name, u64 *phys, u64 *size)
{
    int node = adt_path_offset(adt, path);
    if (node < 0) {
        printf(T6050_GPU_PFX "no %s in the ADT\n", path);
        return -1;
    }

    u32 names_len, segs_len;
    const char *names = adt_getprop(adt, node, "segment-names", &names_len);
    const struct adt_segment_ranges *seg = adt_getprop(adt, node, "segment-ranges", &segs_len);
    if (!names || !names_len || !seg || !segs_len || segs_len % sizeof(*seg)) {
        printf(T6050_GPU_PFX "%s has no usable segment-names/segment-ranges\n", path);
        return -1;
    }

    /* segment-names: one ';'-separated string, in segment-ranges order */
    u32 count = segs_len / sizeof(*seg);
    size_t want = strlen(name);
    const char *p = names, *end = names + names_len;
    for (u32 idx = 0; p < end && *p; idx++) {
        const char *q = p;
        while (q < end && *q && *q != ';')
            q++;
        if ((size_t)(q - p) == want && !memcmp(p, name, want)) {
            if (idx >= count)
                break;
            *phys = seg[idx].phys;
            *size = seg[idx].size;
            if (!*phys || !*size)
                break;
            return 0;
        }
        if (q >= end || *q != ';')
            break;
        p = q + 1;
    }

    printf(T6050_GPU_PFX "%s has no usable %s segment\n", path, name);
    return -1;
}

static int t6050_sgx_u64(int sgx, const char *prop, u64 *val)
{
    if (ADT_GETPROP(adt, sgx, prop, val) < 0 || !*val) {
        printf(T6050_GPU_PFX "no /arm-io/sgx %s\n", prop);
        return -1;
    }
    return 0;
}

/* The six regions from the ADT */
static int t6050_gpu_regions(struct t6050_gpu_handoff *h)
{
    char prop[64];
    int sgx = adt_path_offset(adt, "/arm-io/sgx");
    if (sgx < 0) {
        printf(T6050_GPU_PFX "no /arm-io/sgx in the ADT\n");
        return -1;
    }

    for (int i = 0; i < T6050_FW_TEXT; i++) {
        snprintf(prop, sizeof(prop), "%s-base", t6050_sgx_props[i]);
        if (t6050_sgx_u64(sgx, prop, &h->base[i]))
            return -1;
        snprintf(prop, sizeof(prop), "%s-size", t6050_sgx_props[i]);
        if (t6050_sgx_u64(sgx, prop, &h->size[i]))
            return -1;
    }

    /* fw-text: gfx-asc __TEXT, then gfx1-asc __TEXT right after it */
    u64 t0, ts0, t1, ts1;
    if (t6050_asc_segment(t6050_asc_paths[0], "__TEXT", &t0, &ts0) ||
        t6050_asc_segment(t6050_asc_paths[1], "__TEXT", &t1, &ts1))
        return -1;
    if (t0 + ts0 != t1) {
        printf(T6050_GPU_PFX "fw-text not contiguous: gfx-asc __TEXT end 0x%lx != 0x%lx\n",
               t0 + ts0, t1);
        return -1;
    }
    h->base[T6050_FW_TEXT] = t0;
    h->size[T6050_FW_TEXT] = t1 + ts1 - t0;

    /* fw-data: gfx-asc __DATA, __DATA_SHARED_RO, gfx1-asc __DATA, __DATA_SHARED_RO, back to back */
    static const char *const data_segs[2] = {"__DATA", "__DATA_SHARED_RO"};
    static const char *const data_props[2][2] = {
        {"gfx-data-base", "gfx-data-shared-ro-base"},
        {"gfx1-data-base", "gfx1-data-shared-ro-base"},
    };
    u64 next = 0, first = 0;
    for (int asc = 0; asc < 2; asc++) {
        for (int s = 0; s < 2; s++) {
            u64 phys, size, sgx_base;
            if (t6050_asc_segment(t6050_asc_paths[asc], data_segs[s], &phys, &size) ||
                t6050_sgx_u64(sgx, data_props[asc][s], &sgx_base))
                return -1;
            if (phys != sgx_base) {
                printf(T6050_GPU_PFX "%s %s at 0x%lx, but sgx %s is 0x%lx\n", t6050_asc_paths[asc],
                       data_segs[s], phys, data_props[asc][s], sgx_base);
                return -1;
            }
            if (!first) {
                first = phys;
            } else if (phys != next) {
                printf(T6050_GPU_PFX "fw-data not contiguous: %s %s at 0x%lx, expected 0x%lx\n",
                       t6050_asc_paths[asc], data_segs[s], phys, next);
                return -1;
            }
            next = phys + size;
        }
    }
    h->base[T6050_FW_DATA] = first;
    h->size[T6050_FW_DATA] = next - first;

    u64 ram_lo = cur_boot_args.phys_base;
    u64 ram_hi = cur_boot_args.phys_base + cur_boot_args.mem_size;
    for (int i = 0; i < T6050_NUM_REGIONS; i++) {
        u64 b = h->base[i], e = h->base[i] + h->size[i];
        if ((b | h->size[i]) & (SZ_16K - 1)) {
            printf(T6050_GPU_PFX "%s 0x%lx+0x%lx is not 16 KiB aligned\n", t6050_region_names[i], b,
                   h->size[i]);
            return -1;
        }
        printf(T6050_GPU_PFX "%-13s 0x%lx..0x%lx (0x%lx), %s\n", t6050_region_names[i], b, e,
               h->size[i], (e <= ram_lo || b >= ram_hi) ? "outside Linux RAM" : "inside Linux RAM");
    }
    return 0;
}

/* The GPU node's sgx window must be the ADT's (at least the part the driver maps) */
static int t6050_gpu_check_sgx(void *dt, int gpu)
{
    int path[8];
    u64 adt_base, adt_size, base, size;

    if (adt_path_offset_trace(adt, "/arm-io/sgx", path) < 0 ||
        adt_get_reg(adt, path, "reg", 0, &adt_base, &adt_size) < 0) {
        printf(T6050_GPU_PFX "no /arm-io/sgx reg\n");
        return -1;
    }
    if (t6050_dt_reg(dt, gpu, 0, &base, &size) < 0) {
        printf(T6050_GPU_PFX "GPU node has no usable reg\n");
        return -1;
    }
    if (base != adt_base || size < T6050_SGX_MAP_SIZE || size > adt_size) {
        printf(T6050_GPU_PFX "GPU node sgx 0x%lx+0x%lx, ADT 0x%lx+0x%lx\n", base, size, adt_base,
               adt_size);
        return -1;
    }
    printf(T6050_GPU_PFX "sgx 0x%lx+0x%lx\n", base, size);
    return 0;
}

/* memory-region phandle of the GPU node for <name> */
static int t6050_gpu_regions_dt(void *dt, int gpu, struct t6050_gpu_handoff *h)
{
    int plen, nlen;
    const fdt32_t *ph = fdt_getprop(dt, gpu, "memory-region", &plen);
    const char *names = fdt_getprop(dt, gpu, "memory-region-names", &nlen);
    if (!ph || !names || plen <= 0 || plen % 4) {
        printf(T6050_GPU_PFX "GPU node has no memory-region / memory-region-names\n");
        return -1;
    }

    for (int i = 0; i < T6050_NUM_REGIONS; i++) {
        int idx = fdt_stringlist_search(dt, gpu, "memory-region-names", t6050_region_names[i]);
        if (idx < 0 || idx >= plen / 4) {
            printf(T6050_GPU_PFX "GPU node names no %s memory-region\n", t6050_region_names[i]);
            return -1;
        }
        h->region_phandle[i] = fdt32_ld(&ph[idx]);
        int node = fdt_node_offset_by_phandle(dt, h->region_phandle[i]);
        int parent = node < 0 ? node : fdt_parent_offset(dt, node);
        if (parent < 0 || fdt_address_cells(dt, parent) != 2 || fdt_size_cells(dt, parent) != 2) {
            printf(T6050_GPU_PFX "%s memory-region node unusable\n", t6050_region_names[i]);
            return -1;
        }
    }
    return 0;
}

static bool t6050_adt_has_irq(int anode, u32 irq)
{
    u32 len;
    const u32 *irqs = adt_getprop(adt, anode, "interrupts", &len);

    for (u32 i = 0; irqs && i < len / 4; i++)
        if (irqs[i] == irq)
            return true;
    return false;
}

/* The mailboxes the GPU node names: role -> ADT coprocessor -> wrapper and iop-vbar */
static int t6050_gpu_mboxes(void *dt, int gpu, struct t6050_gpu_handoff *h)
{
    int len;
    const fdt32_t *mboxes = fdt_getprop(dt, gpu, "mboxes", &len);
    if (!mboxes || len != 2 * 4) {
        printf(T6050_GPU_PFX "GPU node needs exactly two mboxes\n");
        return -1;
    }

    bool seen[2] = {false, false};
    for (int i = 0; i < 2; i++) {
        struct t6050_gpu_mbox *mb = &h->mbox[i];
        mb->phandle = fdt32_ld(&mboxes[i]);
        int node = fdt_node_offset_by_phandle(dt, mb->phandle);
        if (node < 0) {
            printf(T6050_GPU_PFX "mbox phandle %u not found\n", mb->phandle);
            return -1;
        }
        int parent = fdt_parent_offset(dt, node);
        if (parent < 0 || fdt_address_cells(dt, parent) != 2 || fdt_size_cells(dt, parent) != 2) {
            printf(T6050_GPU_PFX "mailbox %s: parent cells are not 2/2\n",
                   fdt_get_name(dt, node, NULL));
            return -1;
        }

        const char *role = fdt_getprop(dt, node, "apple,firmware-role", NULL);
        int asc = -1, anode = -1;
        for (int j = 0; role && j < 2; j++) {
            int n = adt_path_offset(adt, t6050_asc_paths[j]);
            const char *arole = n < 0 ? NULL : adt_getprop(adt, n, "role", NULL);
            if (arole && !strcmp(arole, role)) {
                asc = j;
                anode = n;
            }
        }
        if (asc < 0 || seen[asc]) {
            printf(T6050_GPU_PFX "mailbox %s: role %s matches no unused ADT GPU coprocessor\n",
                   fdt_get_name(dt, node, NULL), role ? role : "(none)");
            return -1;
        }
        seen[asc] = true;

        int path[8];
        u64 vbar_size;
        if (adt_path_offset_trace(adt, t6050_asc_paths[asc], path) < 0 ||
            adt_get_reg(adt, path, "reg", 0, &mb->wrapper, &mb->wrapper_size) < 0 ||
            adt_get_reg(adt, path, "reg", 1, &mb->vbar, &vbar_size) < 0) {
            printf(T6050_GPU_PFX "%s: no wrapper/iop-vbar reg\n", t6050_asc_paths[asc]);
            return -1;
        }
        /* drivers/soc/apple/mailbox.c requires exactly these sizes for ASCWrap v6 */
        if (mb->wrapper_size != 0x88000 || vbar_size < 8) {
            printf(T6050_GPU_PFX "%s: wrapper size 0x%lx, iop-vbar size 0x%lx\n",
                   t6050_asc_paths[asc], mb->wrapper_size, vbar_size);
            return -1;
        }

        /* the DT's interrupts (AIC: type, die, irq, flags) must be the coprocessor's */
        int ilen;
        const fdt32_t *irqs = fdt_getprop(dt, node, "interrupts", &ilen);
        if (!irqs || ilen <= 0 || ilen % 16) {
            printf(T6050_GPU_PFX "mailbox %s: no AIC interrupts\n", fdt_get_name(dt, node, NULL));
            return -1;
        }
        for (int k = 0; k < ilen / 16; k++) {
            u32 irq = fdt32_ld(&irqs[4 * k + 2]);
            if (!t6050_adt_has_irq(anode, irq)) {
                printf(T6050_GPU_PFX "mailbox %s: interrupt %u is not one of %s's\n",
                       fdt_get_name(dt, node, NULL), irq, t6050_asc_paths[asc]);
                return -1;
            }
        }
        printf(T6050_GPU_PFX "mailbox %s: wrapper 0x%lx, iop-vbar 0x%lx (%s)\n", role, mb->wrapper,
               mb->vbar, t6050_asc_paths[asc]);
    }
    return 0;
}

/* The GPU's power domain and its PMGR node: every power-state register must be the ADT's */
static int t6050_gpu_pmgr(void *dt, int gpu, struct t6050_gpu_handoff *h)
{
    int len;
    const fdt32_t *pd = fdt_getprop(dt, gpu, "power-domains", &len);
    if (!pd || len != 4) {
        printf(T6050_GPU_PFX "GPU node needs exactly one power domain\n");
        return -1;
    }
    h->ps_phandle = fdt32_ld(pd);
    int ps = fdt_node_offset_by_phandle(dt, h->ps_phandle);
    int pmgr = ps < 0 ? ps : fdt_parent_offset(dt, ps);
    u64 pmgr_base, pmgr_size;
    if (pmgr < 0 || t6050_dt_reg(dt, pmgr, 0, &pmgr_base, &pmgr_size) < 0) {
        printf(T6050_GPU_PFX "GPU power domain has no usable PMGR node\n");
        return -1;
    }

    const char *gfx_label = fdt_getprop(dt, ps, "label", NULL);
    if (!gfx_label || strcmp(gfx_label, "gfx")) {
        printf(T6050_GPU_PFX "GPU power domain is not labelled gfx\n");
        return -1;
    }

    int checked = 0, child;
    fdt_for_each_subnode(child, dt, pmgr)
    {
        const char *label = fdt_getprop(dt, child, "label", NULL);
        u64 off, size;
        if (!label || t6050_dt_reg(dt, child, 0, &off, &size) < 0) {
            printf(T6050_GPU_PFX "PMGR child %s has no label or reg\n",
                   fdt_get_name(dt, child, NULL));
            return -1;
        }

        char name[16];
        size_t n = 0;
        for (; label[n] && n < sizeof(name) - 1; n++)
            name[n] = (label[n] >= 'a' && label[n] <= 'z') ? label[n] - 32 : label[n];
        name[n] = 0;

        uintptr_t want = pmgr_psreg_by_name(0, name);
        if (!want || want != pmgr_base + off || off + size > pmgr_size) {
            printf(T6050_GPU_PFX "PMGR %s: DT 0x%lx, ADT 0x%lx\n", label, pmgr_base + off, want);
            return -1;
        }

        bool known = false;
        for (size_t i = 0; i < ARRAY_SIZE(t6050_ps_names); i++)
            known |= !strcmp(label, t6050_ps_names[i]);
        if (!known) {
            printf(T6050_GPU_PFX "PMGR node holds %s, not only GPU domains\n", label);
            return -1;
        }

        printf(T6050_GPU_PFX "PS %-3s 0x%lx = 0x%08x\n", label, want, read32(want));
        checked++;
    }
    if (checked != ARRAY_SIZE(t6050_ps_names)) {
        printf(T6050_GPU_PFX "PMGR node has %d GPU domains, expected %d\n", checked,
               (int)ARRAY_SIZE(t6050_ps_names));
        return -1;
    }
    return 0;
}

static int t6050_set_okay(void *dt, int node)
{
    return fdt_setprop_string(dt, node, "status", "okay");
}

/* Every check passed: fill in and enable, the GPU and its PMGR node last */
static int t6050_gpu_apply(void *dt, const struct t6050_gpu_handoff *h)
{
    for (int i = 0; i < T6050_NUM_REGIONS; i++) {
        int node = fdt_node_offset_by_phandle(dt, h->region_phandle[i]);
        fdt64_t reg[2];
        fdt64_st(&reg[0], h->base[i]);
        fdt64_st(&reg[1], h->size[i]);
        if (node < 0 || fdt_setprop(dt, node, "reg", reg, sizeof(reg)))
            return -1;
        node = fdt_node_offset_by_phandle(dt, h->region_phandle[i]);
        if (node < 0 || fdt_setprop_empty(dt, node, "no-map"))
            return -1;
        node = fdt_node_offset_by_phandle(dt, h->region_phandle[i]);
        if (node < 0 || t6050_set_okay(dt, node))
            return -1;
    }

    for (int i = 0; i < 2; i++) {
        const struct t6050_gpu_mbox *mb = &h->mbox[i];
        fdt64_t reg[4];
        fdt64_st(&reg[0], mb->wrapper);
        fdt64_st(&reg[1], mb->wrapper_size);
        fdt64_st(&reg[2], mb->vbar);
        fdt64_st(&reg[3], 8);
        int node = fdt_node_offset_by_phandle(dt, mb->phandle);
        if (node < 0 || fdt_setprop(dt, node, "reg", reg, sizeof(reg)))
            return -1;
        node = fdt_node_offset_by_phandle(dt, mb->phandle);
        if (node < 0 || t6050_set_okay(dt, node))
            return -1;
    }

    int gpu = t6050_gpu_node(dt);
    if (gpu < 0 || t6050_set_okay(dt, gpu))
        return -1;

    int ps = fdt_node_offset_by_phandle(dt, h->ps_phandle);
    int pmgr = ps < 0 ? ps : fdt_parent_offset(dt, ps);
    if (pmgr < 0 || t6050_set_okay(dt, pmgr))
        return -1;

    return 0;
}

static int dt_set_gpu_t6050(void *dt)
{
    const char *opt = kboot_get_chosen("m5pro,gpu");

    if (!opt || strcmp(opt, "1")) {
        printf(T6050_GPU_PFX "hand-off off (chosen.m5pro,gpu=%s), GPU nodes as the DT has them\n",
               opt ? opt : "unset");
        return 0;
    }
    printf(T6050_GPU_PFX "chosen.m5pro,gpu=1: GPU hand-off\n");

    int gpu = t6050_gpu_node(dt);
    if (gpu < 0) {
        printf(T6050_GPU_PFX "no apple,agx-g17s node in the DT: nothing to hand off\n");
        return 0;
    }

    struct t6050_gpu_handoff h;
    memset(&h, 0, sizeof(h));
    if (t6050_gpu_regions(&h) || t6050_gpu_check_sgx(dt, gpu) ||
        t6050_gpu_regions_dt(dt, gpu, &h) || t6050_gpu_mboxes(dt, gpu, &h) ||
        t6050_gpu_pmgr(dt, gpu, &h)) {
        printf(T6050_GPU_PFX "STOP: check failed, GPU nodes stay disabled\n");
        return 0;
    }

    if (t6050_gpu_apply(dt, &h)) {
        /* A region or mailbox may already be okay; without the GPU node nothing uses them. */
        gpu = t6050_gpu_node(dt);
        if (gpu >= 0)
            fdt_setprop_string(dt, gpu, "status", "disabled");
        printf(T6050_GPU_PFX "STOP: could not update the DT, GPU node disabled\n");
        return 0;
    }

    printf(T6050_GPU_PFX "regions, mailboxes, GPU and PMGR nodes okay\n");
    return 0;
}

int dt_set_gpu(void *dt)
{
    bool has_cs_afr = false;
    int (*calc_power)(u32 count, u32 table_count, const struct perf_state *core,
                      const struct perf_state *sram, const struct aux_perf_states *cs, u32 *max_pwr,
                      float *core_leak, float *sram_leak, float *cs_leak, float *afr_leak);

    u32 dies = 1;

    printf("FDT: GPU: Initializing GPU info\n");

    switch (chip_id) {
        case T8103:
            calc_power = calc_power_t8103;
            break;
        case T6022:
            dies = 2;
            // fallthrough
        case T6021:
        case T6020:
            has_cs_afr = true;
            calc_power = calc_power_t600x;
            break;
        case T6002:
            dies = 2;
            // fallthrough
        case T6001:
        case T6000:
        case T8112:
            calc_power = calc_power_t600x;
            break;
        case T6050:
            return dt_set_gpu_t6050(dt);
        default:
            printf("ADT: GPU: unsupported chip!\n");
            return 0;
    }

    int gpu = fdt_path_offset(dt, "gpu");
    if (gpu < 0) {
        printf("FDT: GPU: gpu alias not found in device tree\n");
        return 0;
    }

    /* check for old-style downstream compatibles */
    bool downstream_dtb = false;
    downstream_dtb |= fdt_node_check_compatible(dt, gpu, "apple,agx-t8103") == 0;
    downstream_dtb |= fdt_node_check_compatible(dt, gpu, "apple,agx-t8112") == 0;
    downstream_dtb |= fdt_node_check_compatible(dt, gpu, "apple,agx-t6000") == 0;
    downstream_dtb |= fdt_node_check_compatible(dt, gpu, "apple,agx-t6001") == 0;
    downstream_dtb |= fdt_node_check_compatible(dt, gpu, "apple,agx-t6002") == 0;
    downstream_dtb |= fdt_node_check_compatible(dt, gpu, "apple,agx-t6020") == 0;
    downstream_dtb |= fdt_node_check_compatible(dt, gpu, "apple,agx-t6021") == 0;
    downstream_dtb |= fdt_node_check_compatible(dt, gpu, "apple,agx-t6022") == 0;

    int sgx = adt_path_offset(adt, "/arm-io/sgx");
    if (sgx < 0)
        bail("ADT: GPU: /arm-io/sgx node not found\n");

    u32 perf_state_count;
    if (ADT_GETPROP(adt, sgx, "perf-state-count", &perf_state_count) < 0 || !perf_state_count)
        bail("ADT: GPU: missing perf-state-count\n");

    u32 perf_state_table_count;
    if (ADT_GETPROP(adt, sgx, "perf-state-table-count", &perf_state_table_count) < 0 ||
        !perf_state_table_count)
        bail("ADT: GPU: missing perf-state-table-count\n");

    if (perf_state_count > MAX_PSTATES)
        bail("ADT: GPU: perf-state-count too large\n");

    if (perf_state_table_count > MAX_CLUSTERS)
        bail("ADT: GPU: perf-state-table-count too large\n");

    u32 perf_states_len;
    const struct perf_state *perf_states, *perf_states_sram;
    const struct aux_perf_states *perf_states_afr, *perf_states_cs;

    perf_states = adt_getprop(adt, sgx, "perf-states", &perf_states_len);
    if (!perf_states ||
        perf_states_len != sizeof(*perf_states) * perf_state_count * perf_state_table_count)
        bail("ADT: GPU: invalid perf-states length\n");

    perf_states_sram = adt_getprop(adt, sgx, "perf-states-sram", &perf_states_len);
    if (perf_states_sram &&
        perf_states_len != sizeof(*perf_states) * perf_state_count * perf_state_table_count)
        bail("ADT: GPU: invalid perf-states-sram length\n");

    perf_states_afr = adt_getprop(adt, sgx, "afr-perf-states", NULL);
    perf_states_cs = adt_getprop(adt, sgx, "cs-perf-states", NULL);

    if (has_cs_afr && !perf_states_cs)
        bail("ADT: GPU: cs-perf-states not found\n");

    if (has_cs_afr && !perf_states_afr)
        bail("ADT: GPU: afr-perf-states not found\n");

    u32 max_pwr[MAX_PSTATES];
    float core_leak[MAX_CLUSTERS];
    float sram_leak[MAX_CLUSTERS];
    float cs_leak[MAX_DIES];
    float afr_leak[MAX_DIES];

    if (calc_power(perf_state_count, perf_state_table_count, perf_states, perf_states_sram,
                   perf_states_cs, max_pwr, core_leak, sram_leak, cs_leak, afr_leak))
        return -1;

    printf("FDT: GPU: Max power table: ");
    for (u32 i = 0; i < perf_state_count; i++) {
        printf("%d ", max_pwr[i]);
    }
    printf("\nFDT: GPU: Core leakage table: ");
    for (u32 i = 0; i < perf_state_table_count; i++) {
        printf("%d.%03d ", (int)core_leak[i], ((int)(core_leak[i] * 1000) % 1000));
    }
    printf("\nFDT: GPU: SRAM leakage table: ");
    for (u32 i = 0; i < perf_state_table_count; i++) {
        printf("%d.%03d ", (int)sram_leak[i], ((int)(sram_leak[i] * 1000) % 1000));
    }
    printf("\n");

    if (has_cs_afr) {
        printf("FDT: GPU: CS leakage table: ");
        for (u32 i = 0; i < dies; i++) {
            printf("%d.%03d ", (int)cs_leak[i], ((int)(cs_leak[i] * 1000) % 1000));
        }
        printf("\n");

        printf("FDT: GPU: AFR leakage table: ");
        for (u32 i = 0; i < dies; i++) {
            printf("%d.%03d ", (int)afr_leak[i], ((int)(afr_leak[i] * 1000) % 1000));
        }
        printf("\n");
    }

    if (downstream_dtb) {
        if (fdt_set_float_array(dt, gpu, "apple,core-leak-coef", core_leak, perf_state_table_count))
            return -1;

        if (fdt_set_float_array(dt, gpu, "apple,sram-leak-coef", sram_leak, perf_state_table_count))
            return -1;

        int len;
        const fdt32_t *opps_ph = fdt_getprop(dt, gpu, "operating-points-v2", &len);
        if (!opps_ph || len != 4)
            bail("FDT: GPU: operating-points-v2 not found\n");

        int opps = fdt_node_offset_by_phandle(dt, fdt32_ld(opps_ph));
        if (opps < 0)
            bail("FDT: GPU: node for phandle %u not found\n", fdt32_ld(opps_ph));

        u32 i = 0;
        int opp;
        fdt_for_each_subnode(opp, dt, opps)
        {
            fdt32_t volts[MAX_CLUSTERS];

            for (u32 j = 0; j < perf_state_table_count; j++) {
                volts[j] = cpu_to_fdt32(perf_states[i + j * perf_state_count].volt * 1000);
            }

            if (i >= perf_state_count)
                bail("FDT: GPU: Expected %d operating points, but found more\n", perf_state_count);

            if (fdt_setprop_inplace(dt, opp, "opp-microvolt", &volts,
                                    sizeof(u32) * perf_state_table_count))
                bail("FDT: GPU: Failed to set opp-microvolt for PS %d\n", i);

            if (fdt_setprop_inplace_u64(dt, opp, "opp-hz", perf_states[i].freq))
                bail("FDT: GPU: Failed to set opp-hz for PS %d\n", i);

            if (fdt_setprop_inplace_u32(dt, opp, "opp-microwatt", max_pwr[i]))
                bail("FDT: GPU: Failed to set opp-microwatt for PS %d\n", i);

            i++;
        }

        if (i != perf_state_count)
            bail("FDT: GPU: Expected %d operating points, but found %d\n", perf_state_count, i);

        if (has_cs_afr) {
            int ret = fdt_set_aux_opp(dt, gpu, "apple,cs-opp", perf_states_cs, dies);
            if (ret)
                return ret;

            if (fdt_set_float_array(dt, gpu, "apple,cs-leak-coef", cs_leak, dies))
                return -1;
        }

        if (has_cs_afr) {
            int ret = fdt_set_aux_opp(dt, gpu, "apple,afr-opp", perf_states_afr, dies);
            if (ret)
                return ret;
            if (fdt_set_float_array(dt, gpu, "apple,afr-leak-coef", afr_leak, dies))
                return -1;
        }
    }

    if (dt_set_region(dt, sgx, "gfx-handoff", "/reserved-memory/uat-handoff"))
        return -1;
    if (dt_set_region(dt, sgx, "gfx-shared-region", "/reserved-memory/uat-pagetables"))
        return -1;
    if (dt_set_region(dt, sgx, "gpu-region", "/reserved-memory/uat-ttbs"))
        return -1;

    // refresh gpu dt node offset after modifying the dt in dt_set_region()
    gpu = fdt_path_offset(dt, "gpu");
    if (gpu < 0) {
        printf("FDT: GPU: gpu alias not found in device tree\n");
        return 0;
    }

    const struct fw_version_info *compat;

    switch (os_firmware.version) {
        case V12_3_1:
            compat = &fw_versions[V12_3];
            break;
        case V13_5B4:
        case V13_6_1:
            compat = &fw_versions[V13_5];
            break;
        default:
            compat = &os_firmware;
            break;
    }

    if (downstream_dtb) {
        if (firmware_set_fdt(dt, gpu, "apple,firmware-version", &os_firmware))
            return -1;
        if (firmware_set_fdt(dt, gpu, "apple,firmware-compat", compat))
            return -1;
    }
    // Ignoring errors for old dts compat
    firmware_set_fdt(dt, gpu, "apple,firmware-abi", compat);

    size_t data_a_size, data_b_size, globals_size;
    if (rust_gpu_initdata_size(compat->num[0], compat->num[1], &data_a_size, &data_b_size,
                               &globals_size) == -1)
        return -1;

    void *data_a = (void *)top_of_memory_alloc(ALIGN_UP(data_a_size, SZ_16K));
    void *data_b = (void *)top_of_memory_alloc(ALIGN_UP(data_b_size, SZ_16K));
    void *globals = (void *)top_of_memory_alloc(ALIGN_UP(globals_size, SZ_16K));
    memset(data_a, 0, data_a_size);
    memset(data_b, 0, data_b_size);
    memset(globals, 0, globals_size);

    size_t n_perf_states_cs = 0, n_perf_states_afr = 0;
    const struct aux_perf_state *pstate_cs_raw = NULL, *pstate_afr_raw = NULL;

    if (has_cs_afr) {
        n_perf_states_cs = perf_states_cs->count;
        n_perf_states_afr = perf_states_afr->count;
        pstate_cs_raw = perf_states_cs->states;
        pstate_afr_raw = perf_states_afr->states;
    }
    struct initdata_inputs ins = {
        .perf_state_table_count = perf_state_table_count,
        .perf_state_count = perf_state_count,
        .c_perf_states = perf_states,
        .max_pwr = max_pwr,
        .core_leak = core_leak,
        .sram_leak = sram_leak,
        .cs_leak = cs_leak,
        .afr_leak = afr_leak,
        .n_perf_states_cs = n_perf_states_cs,
        .pstates_cs = pstate_cs_raw,
        .n_perf_states_afr = n_perf_states_afr,
        .pstates_afr = pstate_afr_raw,
        .compat_maj = compat->num[0],
        .compat_min = compat->num[1],
    };
    if (rust_fill_gpu_initdata(&ins, data_a, data_b, globals) == -1)
        return -1;

    if (dt_set_resvmem(dt, "/reserved-memory/hw-cal-a", (u64)data_a, ALIGN_UP(data_a_size, SZ_16K)))
        return 0; // Old dts.
    if (dt_set_resvmem(dt, "/reserved-memory/hw-cal-b", (u64)data_b, ALIGN_UP(data_b_size, SZ_16K)))
        return -1;
    if (dt_set_resvmem(dt, "/reserved-memory/globals", (u64)globals,
                       ALIGN_UP(globals_size, SZ_16K)))
        return -1;

    /* Save init data sizes for debugging */
    // refresh gpu dt node offset after modifying the dt in dt_set_region()
    gpu = fdt_path_offset(dt, "gpu");
    if (gpu < 0) {
        printf("FDT: GPU: gpu alias not found in device tree\n");
        return 0;
    }
    fdt_setprop_u32(dt, gpu, "debug,hw-cal-a-size", data_a_size);
    fdt_setprop_u32(dt, gpu, "debug,hw-cal-b-size", data_b_size);
    fdt_setprop_u32(dt, gpu, "debug,globals-size", globals_size);

    return 0;
}
