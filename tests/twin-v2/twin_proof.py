#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""twin_proof.py [RESIDENT_DIR] -- falsifiable write-confinement proof for the offline twin (review H3 / C5).

For the resident (default r4.4) and for deliberately broken variants built in scratch from the same
source, this compiles the resident's real storage stack (hv_win_storage*.c, s1/ incl. hv_nvme_queue.c,
native_window.c, ans_ro.c, m1n1 asc/rtkit/sart) against the v2 fake ANS (patch_fake_v2.py: accepts and
stores every write, logs every command, durable media = a sparse 1 TB backing image) and runs the
adversarial workload (scenario_twin_proof.c). Then three INDEPENDENT detectors judge the run:

  D1 image:  the whole backing image outside the window, before vs after, with the C2 verifier
             (m5w_outside_verify.py --sparse: every outside chunk, content AND allocation; holes = zeros);
  D2 log:    every write-like command the fake received, recomputed from its JSON log (LBA/length);
             any I/O opcode other than READ/WRITE/FLUSH or any non-queue admin opcode also fails;
  D3 fake:   the fake's own writes_outside_window counter (now captured in the receipt, not stderr).
Positive controls: every legitimate in-window write succeeded and read back, the image changed inside
the window, and the last window block in the image holds the guest's data.

PASS = D1 identical AND D2 = 0 AND D3 = 0 AND positive controls. A broken variant must FAIL.
Nothing here touches hardware. Scratch lives in $TMPDIR and is deleted (image clones share blocks).
"""
import hashlib, json, os, shutil, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOP = HERE.parent
VER = Path("/Users/rolo/Work/agents/win-el2/tooling/write-safety-20261110/verifier/m5w_outside_verify.py")
RES = Path(sys.argv[1] if len(sys.argv) > 1 else
           "/Users/rolo/Work/agents/win-el2/storage/s1-resident-r4.5-native-rw-20261110").resolve()
V1_AUDIT = Path("/Users/rolo/Work/agents/win-el2/tooling/offline-twin-20261109/native/disk-audit.py")
TN = RES / "tests/native"
SRC = RES / "source/src"; WIN = SRC / "hv/win"; S1 = WIN / "s1"
M1N1 = ["asc.c", "asc.h", "rtkit.c", "rtkit.h", "sart.c", "sart.h"]
W_START, W_BLOCKS, NSZE, BS = 174213120, 24424454, 244276265, 4096
W_END = W_START + W_BLOCKS

# Original twin-v2 fault models, retargeted to all r4.5 redundant bounds.
VARIANTS = {'M1 window check disabled': [('s1/hv_nvme_queue.c',
                               '    if (lba >= ctrl->namespace_blocks || blocks > ctrl->namespace_blocks - '
                               'lba) {\n'
                               '        if (ctrl->ops->abort) ctrl->ops->abort(ctrl->opaque);\n'
                               '        return VNVME_SC_INVALID_FIELD;\n'
                               '    }\n'
                               '    size_t total = (size_t)blocks * VNVME_LBA_SIZE, capacity;\n'
                               '    u8 *buffer = data_buffer(ctrl, &capacity);\n'
                               '    static struct prp_plan plan;\n'
                               '    if (!plan_prps_dir(ctrl, cmd, total, &plan, capacity, false))',
                               '    if (0) {\n'
                               '        if (ctrl->ops->abort) ctrl->ops->abort(ctrl->opaque);\n'
                               '        return VNVME_SC_INVALID_FIELD;\n'
                               '    }\n'
                               '    size_t total = (size_t)blocks * VNVME_LBA_SIZE, capacity;\n'
                               '    u8 *buffer = data_buffer(ctrl, &capacity);\n'
                               '    static struct prp_plan plan;\n'
                               '    if (!plan_prps_dir(ctrl, cmd, total, &plan, capacity, false))'),
                              ('s1/native_window.c',
                               '    return verified && blocks && blocks <= 256 && lba < S1N_PIN_BLOCKS && '
                               'blocks <= S1N_PIN_BLOCKS - lba;',
                               '    return verified && blocks && blocks <= 256;'),
                              ('s1/native_window.c',
                               '        if (phys < S1N_PIN_START || phys - S1N_PIN_START >= S1N_PIN_BLOCKS '
                               '||\n'
                               '            n > S1N_PIN_BLOCKS - (phys - S1N_PIN_START)) {\n'
                               '            ans_ro_abort(ANS_RO_E_ABORT);\n'
                               '            return false;\n'
                               '        }\n',
                               '        if (0) { ans_ro_abort(ANS_RO_E_ABORT); return false; }\n'),
                              ('s1/ans_ro.c',
                               '    return window_set && n && lba >= S1N_PIN_START && lba - S1N_PIN_START < '
                               'S1N_PIN_BLOCKS &&\n'
                               '           n <= S1N_PIN_BLOCKS - (lba - S1N_PIN_START);',
                               '    return window_set && n;'),
                              ('s1/ans_ro.c',
                               '    return in_window(lba, n) && n <= S1N_WRITE_MAX_BLOCKS &&\n'
                               '           lba >= S1N_SUB_START && lba - S1N_SUB_START < S1N_SUB_BLOCKS &&\n'
                               '           n <= S1N_SUB_BLOCKS - (lba - S1N_SUB_START)\n'
                               '#if S1N_STAGE_CANARY\n'
                               '           && canary_running\n'
                               '#endif\n'
                               '           ;',
                               '    return in_window(lba,n) && n <= S1N_WRITE_MAX_BLOCKS;')],
 'M2 off-by-one at the end': [('s1/hv_nvme_queue.c',
                               '    if (lba >= ctrl->namespace_blocks || blocks > ctrl->namespace_blocks - '
                               'lba) {\n'
                               '        if (ctrl->ops->abort) ctrl->ops->abort(ctrl->opaque);\n'
                               '        return VNVME_SC_INVALID_FIELD;\n'
                               '    }\n'
                               '    size_t total = (size_t)blocks * VNVME_LBA_SIZE, capacity;\n'
                               '    u8 *buffer = data_buffer(ctrl, &capacity);\n'
                               '    static struct prp_plan plan;\n'
                               '    if (!plan_prps_dir(ctrl, cmd, total, &plan, capacity, false))',
                               '    if (lba > ctrl->namespace_blocks || blocks > ctrl->namespace_blocks - '
                               'lba + 1) {\n'
                               '        if (ctrl->ops->abort) ctrl->ops->abort(ctrl->opaque);\n'
                               '        return VNVME_SC_INVALID_FIELD;\n'
                               '    }\n'
                               '    size_t total = (size_t)blocks * VNVME_LBA_SIZE, capacity;\n'
                               '    u8 *buffer = data_buffer(ctrl, &capacity);\n'
                               '    static struct prp_plan plan;\n'
                               '    if (!plan_prps_dir(ctrl, cmd, total, &plan, capacity, false))'),
                              ('s1/native_window.c',
                               '    return verified && blocks && blocks <= 256 && lba < S1N_PIN_BLOCKS && '
                               'blocks <= S1N_PIN_BLOCKS - lba;',
                               '    return verified && blocks && blocks <= 256 && lba <= S1N_PIN_BLOCKS && '
                               'blocks <= S1N_PIN_BLOCKS - lba + 1;'),
                              ('s1/native_window.c',
                               '        if (phys < S1N_PIN_START || phys - S1N_PIN_START >= S1N_PIN_BLOCKS '
                               '||\n'
                               '            n > S1N_PIN_BLOCKS - (phys - S1N_PIN_START)) {\n'
                               '            ans_ro_abort(ANS_RO_E_ABORT);\n'
                               '            return false;\n'
                               '        }\n',
                               '        if (phys < S1N_PIN_START || phys - S1N_PIN_START > S1N_PIN_BLOCKS || '
                               'n > S1N_PIN_BLOCKS - (phys - S1N_PIN_START) + 1) { '
                               'ans_ro_abort(ANS_RO_E_ABORT); return false; }\n'),
                              ('s1/ans_ro.c',
                               '    return window_set && n && lba >= S1N_PIN_START && lba - S1N_PIN_START < '
                               'S1N_PIN_BLOCKS &&\n'
                               '           n <= S1N_PIN_BLOCKS - (lba - S1N_PIN_START);',
                               '    return window_set && n && lba >= S1N_PIN_START && lba - S1N_PIN_START <= '
                               'S1N_PIN_BLOCKS && n <= S1N_PIN_BLOCKS - (lba - S1N_PIN_START) + 1;'),
                              ('s1/ans_ro.c',
                               '    return in_window(lba, n) && n <= S1N_WRITE_MAX_BLOCKS &&\n'
                               '           lba >= S1N_SUB_START && lba - S1N_SUB_START < S1N_SUB_BLOCKS &&\n'
                               '           n <= S1N_SUB_BLOCKS - (lba - S1N_SUB_START)\n'
                               '#if S1N_STAGE_CANARY\n'
                               '           && canary_running\n'
                               '#endif\n'
                               '           ;',
                               '    return in_window(lba,n) && n <= S1N_WRITE_MAX_BLOCKS;')],
 'M3 off-by-one at the start': [('s1/native_window.c',
                                 '        u64 phys = S1N_PIN_START + lba + done;',
                                 '        u64 phys = S1N_PIN_START + lba + done - 1;'),
                                ('s1/native_window.c',
                                 '        if (phys < S1N_PIN_START || phys - S1N_PIN_START >= S1N_PIN_BLOCKS '
                                 '||\n'
                                 '            n > S1N_PIN_BLOCKS - (phys - S1N_PIN_START)) {\n'
                                 '            ans_ro_abort(ANS_RO_E_ABORT);\n'
                                 '            return false;\n'
                                 '        }\n',
                                 '        if (phys + 1 < S1N_PIN_START || phys + 1 - S1N_PIN_START >= '
                                 'S1N_PIN_BLOCKS || n > S1N_PIN_BLOCKS - (phys + 1 - S1N_PIN_START)) { '
                                 'ans_ro_abort(ANS_RO_E_ABORT); return false; }\n'),
                                ('s1/ans_ro.c',
                                 '    return window_set && n && lba >= S1N_PIN_START && lba - S1N_PIN_START '
                                 '< S1N_PIN_BLOCKS &&\n'
                                 '           n <= S1N_PIN_BLOCKS - (lba - S1N_PIN_START);',
                                 '    return window_set && n && lba + 1 >= S1N_PIN_START && lba + 1 - '
                                 'S1N_PIN_START < S1N_PIN_BLOCKS && n <= S1N_PIN_BLOCKS - (lba + 1 - '
                                 'S1N_PIN_START);'),
                                ('s1/ans_ro.c',
                                 '    return in_window(lba, n) && n <= S1N_WRITE_MAX_BLOCKS &&\n'
                                 '           lba >= S1N_SUB_START && lba - S1N_SUB_START < S1N_SUB_BLOCKS '
                                 '&&\n'
                                 '           n <= S1N_SUB_BLOCKS - (lba - S1N_SUB_START)\n'
                                 '#if S1N_STAGE_CANARY\n'
                                 '           && canary_running\n'
                                 '#endif\n'
                                 '           ;',
                                 '    return in_window(lba,n) && n <= S1N_WRITE_MAX_BLOCKS;')],
 'M4 wrapped LBA': [('s1/hv_nvme_queue.c',
                     '    if (lba >= ctrl->namespace_blocks || blocks > ctrl->namespace_blocks - lba) {\n'
                     '        if (ctrl->ops->abort) ctrl->ops->abort(ctrl->opaque);\n'
                     '        return VNVME_SC_INVALID_FIELD;\n'
                     '    }\n'
                     '    size_t total = (size_t)blocks * VNVME_LBA_SIZE, capacity;\n'
                     '    u8 *buffer = data_buffer(ctrl, &capacity);\n'
                     '    static struct prp_plan plan;\n'
                     '    if (!plan_prps_dir(ctrl, cmd, total, &plan, capacity, false))',
                     '    if (lba + blocks > ctrl->namespace_blocks) {\n'
                     '        if (ctrl->ops->abort) ctrl->ops->abort(ctrl->opaque);\n'
                     '        return VNVME_SC_INVALID_FIELD;\n'
                     '    }\n'
                     '    size_t total = (size_t)blocks * VNVME_LBA_SIZE, capacity;\n'
                     '    u8 *buffer = data_buffer(ctrl, &capacity);\n'
                     '    static struct prp_plan plan;\n'
                     '    if (!plan_prps_dir(ctrl, cmd, total, &plan, capacity, false))'),
                    ('s1/native_window.c',
                     '    return verified && blocks && blocks <= 256 && lba < S1N_PIN_BLOCKS && blocks <= '
                     'S1N_PIN_BLOCKS - lba;',
                     '    return verified && blocks && blocks <= 256 && lba + blocks <= S1N_PIN_BLOCKS;'),
                    ('s1/native_window.c',
                     '        if (phys < S1N_PIN_START || phys - S1N_PIN_START >= S1N_PIN_BLOCKS ||\n'
                     '            n > S1N_PIN_BLOCKS - (phys - S1N_PIN_START)) {\n'
                     '            ans_ro_abort(ANS_RO_E_ABORT);\n'
                     '            return false;\n'
                     '        }\n',
                     '        if (phys + n > S1N_PIN_START + S1N_PIN_BLOCKS) { ans_ro_abort(ANS_RO_E_ABORT); '
                     'return false; }\n'),
                    ('s1/ans_ro.c',
                     '    return window_set && n && lba >= S1N_PIN_START && lba - S1N_PIN_START < '
                     'S1N_PIN_BLOCKS &&\n'
                     '           n <= S1N_PIN_BLOCKS - (lba - S1N_PIN_START);',
                     '    return window_set && n && lba + n <= S1N_PIN_START + S1N_PIN_BLOCKS;'),
                    ('s1/ans_ro.c',
                     '    return in_window(lba, n) && n <= S1N_WRITE_MAX_BLOCKS &&\n'
                     '           lba >= S1N_SUB_START && lba - S1N_SUB_START < S1N_SUB_BLOCKS &&\n'
                     '           n <= S1N_SUB_BLOCKS - (lba - S1N_SUB_START)\n'
                     '#if S1N_STAGE_CANARY\n'
                     '           && canary_running\n'
                     '#endif\n'
                     '           ;',
                     '    return in_window(lba,n) && n <= S1N_WRITE_MAX_BLOCKS;')],
 'r4.5 (unmodified)': []}

def strip(text, drop=()):
    return "\n".join(l for l in text.splitlines() if not l.startswith("#include") and l not in drop)


def build(root, name, muts):
    out = root / name.split()[0].replace(".", "_")
    (out / "src").mkdir(parents=True); (out / "s1").mkdir()
    (out / "config.h").write_text("/* host */\n")
    shutil.copyfile(SRC / "proxy.h", out / "src/proxy.h")
    for n in M1N1:
        shutil.copyfile(SRC / n, out / "src" / n)
    for p in S1.iterdir():
        if p.suffix in (".c", ".h"):
            shutil.copyfile(p, out / "s1" / p.name)
    for n in ["hv_win_storage.c", "hv_win_storage.h", "hv_win_storage_mem.h", "hv_win_storage_native.c"]:
        shutil.copyfile(WIN / n, out / n)
    for f, old, new in muts:
        p = out / f; s = p.read_text()
        assert s.count(old) == 1, (name, f, old[:60], s.count(old))
        p.write_text(s.replace(old, new))
    shutil.copytree(TN / "shim", out / "shim"); shutil.copyfile(TN / "fake_ans.h", out / "fake_ans.h")
    subprocess.run([sys.executable, "-I", str(HERE / "patch_fake_v2.py"), str(TN / "fake_ans.c"), str(out / "fake_ans.c")], check=True)
    body = (TN / "body.c").read_text()
    anchor = "int main(int argc, char **argv)\n{"
    assert body.count(anchor) == 1
    body = body.replace(anchor, (HERE / "scenario_twin_proof.c").read_text() + "\n" + anchor)
    disp = '    else { fprintf(stderr, "FAIL: unknown scenario %s\\n", s); return 2; }'
    assert body.count(disp) == 1
    body = body.replace(disp, '    else if (!strcmp(s, "twin_proof")) s_twin_proof();\n'
                              '    else if (!strcmp(s, "twin_mkimg")) s_twin_mkimg();\n' + disp)
    test = ((TN / "prelude.h").read_text()
            + strip((out / "hv_win_storage.c").read_text(), ("extern char _base[], _end[];",))
            + "\n" + strip((out / "hv_win_storage_native.c").read_text()) + "\n" + body)
    (out / "test.c").write_text(test)
    s1c = [str(p) for p in sorted((out / "s1").glob("*.c"))]
    # same source set as tests/native/run.py: only these s1 files are compiled
    s1c = [str(out / "s1" / n) for n in ["native_link.c", "ans_ro.c", "native_window.c", "hv_nvme.c", "hv_pci.c", "hv_nvme_queue.c", "ram_disk.c"]]
    cmd = ["clang", "-std=gnu11", "-DVNVME_HOST_TEST", "-DANS_RO_HOST_TEST", "-DS1N_STAGE=" + os.environ.get("TWIN_STAGE", "3"), "-O1", "-g", "-w",
           "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
           f"-I{TN}", f"-I{out / 'shim'}", f"-I{out}", f"-I{out / 'src'}", f"-I{out / 's1'}",
           str(out / "test.c"), str(out / "fake_ans.c"), str(HERE / "fake_disk_file.c"),
           *[str(out / "src" / n) for n in M1N1 if n.endswith(".c")], *s1c, "-o", str(out / "test")]
    b = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    if b.returncode:
        raise RuntimeError(f"{name}: build failed\n{b.stderr[-3000:]}")
    (out / "source-hashes.json").write_text(json.dumps({str(p.relative_to(out)): hashlib.sha256(p.read_bytes()).hexdigest() for p in out.rglob("*") if p.is_file() and p.suffix in (".c", ".h")}, indent=2))
    return out / "test"


def write_backup_gpt(img):
    import zlib
    with open(img, "r+b") as f:
        f.seek(BS); h = bytearray(f.read(BS)); ents = f.read(4 * BS)
        b = bytearray(h[:92])
        b[24:32] = (NSZE - 1).to_bytes(8, "little"); b[32:40] = (1).to_bytes(8, "little")
        b[72:80] = (NSZE - 5).to_bytes(8, "little"); b[16:20] = b"\0\0\0\0"
        b[16:20] = (zlib.crc32(bytes(b)) & 0xFFFFFFFF).to_bytes(4, "little")
        f.seek((NSZE - 5) * BS); f.write(ents); f.write(bytes(b) + bytes(BS - 92))


def make_pins(img, path):
    sys.path.insert(0, str(VER.parent))
    import m5w_outside_verify as V
    dev = V.Dev(str(img), True, BS); fd = dev.open()
    hp, hb, ents = V.read_gpt(dev, fd); os.close(fd)
    W_GUID = bytes([0x59, 0x72, 0xac, 0x89, 0xa4, 0xfa, 0x10, 0x41, 0xb6, 0x21, 0xe9, 0xa2, 0x93, 0x4b, 0x45, 0xc1])
    pins = {"comment": "twin image (fake ANS GPT); window pinned independently from the fake's W_START/W_BLOCKS/W_GUID",
            "disk_guid": hp["disk_guid"], "block_size": BS, "nsze": NSZE,
            "window": {"entry": 7, "type_guid": V.BASIC_DATA, "unique_guid": V.guid(W_GUID), "start": W_START, "blocks": W_BLOCKS},
            "expected_entries": {str(k): {f: v[f] for f in ("start", "blocks", "type_guid", "unique_guid")} for k, v in ents.items()}}
    json.dump(pins, open(path, "w"), indent=1)


def verifier(*a, ok=(0, 1)):
    r = subprocess.run([sys.executable, "-I", str(VER), *a], capture_output=True, text=True, timeout=1200)
    if r.returncode not in ok:
        raise RuntimeError(f"verifier {a[0]} rc {r.returncode}: {r.stdout[-800:]} {r.stderr[-800:]}")
    return r.returncode, r.stdout


def outside(s, n):
    return s < W_START or s >= W_END or n > W_END - s


def analyse_log(path):
    ok_io, ok_admin = {0x00, 0x01, 0x02}, {0x00, 0x01, 0x04, 0x05}
    d = {"commands": 0, "write_like": 0, "write_like_outside": 0, "outside_blocks": 0, "inside_writes": 0,
         "forbidden_io": 0, "forbidden_admin": 0, "outside_examples": []}
    for line in open(path):
        e = json.loads(line); d["commands"] += 1
        if e["q"] == "admin":
            d["forbidden_admin"] += e["op"] not in ok_admin
            continue
        if e["op"] not in ok_io:
            d["forbidden_io"] += 1
        if e["op"] in (0x01, 0x04, 0x08, 0x09) or e["what"].startswith(("write", "dsm")):
            d["write_like"] += 1
            if outside(e["slba"], e["nlb"]):
                d["write_like_outside"] += 1; d["outside_blocks"] += e["nlb"]
                if len(d["outside_examples"]) < 6:
                    d["outside_examples"].append({k: e[k] for k in ("op", "slba", "nlb", "status", "what")})
            elif e["status"] == 0:
                d["inside_writes"] += 1
    return d


def prove_case(binary, tmpl, before_man, pins, wd, tag, maxxfer, legacy=False, case=-1):
    img, wlog = wd / f"{tag}.img", wd / f"{tag}.wlog"
    for p in (img, wlog):
        if p.exists():
            p.unlink()
    subprocess.run(["cp", "-c", str(tmpl), str(img)] if sys.platform == "darwin" else ["cp", "--reflink=auto", str(tmpl), str(img)], check=True)
    env = dict(os.environ, S1TWIN_DISK=str(img), S1TWIN_WLOG=str(wlog), S1N_MAXXFER=str(maxxfer),
               S1TWIN_CASE=str(case), ASAN_OPTIONS="abort_on_error=1:detect_leaks=0", UBSAN_OPTIONS="halt_on_error=1")
    if legacy:
        env["S1TWIN_LEGACY_REFUSE"] = "1"
    r = subprocess.run([str(binary), "twin_proof"], capture_output=True, text=True, timeout=900, env=env)
    lines = [l for l in r.stdout.splitlines() if l.startswith('{"proof"')]
    if r.returncode or not lines:
        return {"variant": tag, "verdict": "ERROR", "rc": r.returncode, "stderr": r.stderr[-1500:]}
    rec = json.loads(lines[0])
    after = wd / f"{tag}.after.json"
    rc, so = verifier("hash", str(img), "--image", "--sparse", "--pins", str(pins), "-o", str(after), "--jobs", "8", ok=(0, 2))
    refused = None
    if rc == 2:  # the GPT itself was damaged: that is a D1 failure; re-hash on the pinned layout to name the LBAs
        refused = json.loads(so)["why"]
        verifier("hash", str(img), "--image", "--sparse", "--pins", str(pins), "-o", str(after), "--jobs", "8", "--after-damage")
    rc, so = verifier("compare", str(before_man), str(after), "--max-list", "8")
    cmp = json.loads(so)
    if refused:
        cmp["verdict"] = "FAIL"
    log = analyse_log(wlog)
    with open(img, "rb") as f:   # positive control: last window block holds the guest's data (gen_block: LBA, gen)
        f.seek((W_END - 1) * BS); last = f.read(12)
    pos = {"legit_failed": rec["legit_failed"], "readback_bad": rec["readback_bad"], "inside_writes_logged": log["inside_writes"],
           "last_window_block_is_guest_data": last == (W_BLOCKS - 1).to_bytes(8, "little") + (1).to_bytes(4, "little")}
    pos_ok = not pos["legit_failed"] and not pos["readback_bad"] and pos["inside_writes_logged"] > 0 and pos["last_window_block_is_guest_data"]
    if case != -1: pos_ok = not rec["file_errors"]
    d1 = cmp["verdict"] == "PASS"
    d2 = log["write_like_outside"] == 0 and log["forbidden_io"] == 0 and log["forbidden_admin"] == 0
    d3 = rec["fake_writes_outside_window"] == 0
    res = {"variant": tag, "maxxfer": maxxfer, "legacy_v1_fake": legacy,
           "D1_image_outside_identical": d1, "D1_gpt_refusal": refused,
           "D1_changed": [{"region": x["region"], "blocks": x["changed_blocks"], "first": x["diffs"][:3]} for x in cmp["regions"]],
           "D2_log": log, "D3_fake_writes_outside_window": rec["fake_writes_outside_window"],
           "positive_controls": pos, "receipt": rec,
           "verdict": "PASS" if (d1 and d2 and d3 and pos_ok) else "FAIL"}
    if legacy:  # what the v1 audit (offline-twin-20261109/native/disk-audit.py) concludes on the same image
        a, b = wd / "v1a.json", wd / "v1b.json"
        subprocess.run([sys.executable, "-I", str(V1_AUDIT), "snapshot", str(tmpl), str(a)], capture_output=True, check=True)
        subprocess.run([sys.executable, "-I", str(V1_AUDIT), "snapshot", str(img), str(b)], capture_output=True, check=True)
        v = subprocess.run([sys.executable, "-I", str(V1_AUDIT), "compare", str(a), str(b)], capture_output=True, text=True)
        res["v1_disk_audit_says"] = json.loads(v.stdout)["pass"]
        for p in (a, b):
            p.unlink()
    (wd / (tag + ".case" + str(case) + ".json")).write_text(json.dumps(res, indent=2))
    img.unlink(); wlog.unlink(); after.unlink()
    return res


def prove(binary, tmpl, before_man, pins, wd, tag, maxxfer, legacy=False):
    cases = []
    for case in range(-1, 23):
        r = prove_case(binary, tmpl, before_man, pins, wd, tag, maxxfer, legacy, case)
        r["case"] = case
        cases.append(r)
        print(f"{tag} maxxfer={maxxfer} case={case}: {r['verdict']}", file=sys.stderr, flush=True)
    good = all(r["verdict"] == "PASS" for r in cases)
    return {"variant": tag, "maxxfer": maxxfer, "legacy_v1_fake": legacy,
            "verdict": "PASS" if good else ("ERROR" if any(r["verdict"] == "ERROR" for r in cases) else "FAIL"),
            "D1_image_outside_identical": all(r.get("D1_image_outside_identical", False) for r in cases),
            "D2_log": {"write_like_outside": sum(r.get("D2_log", {}).get("write_like_outside", 0) for r in cases)},
            "D3_fake_writes_outside_window": sum(r.get("D3_fake_writes_outside_window", 0) for r in cases),
            "cases": cases}

def main():
    wd = Path(tempfile.mkdtemp(prefix="m5w-twinv2-", dir=os.environ.get("TMPDIR")))
    results = []
    try:
        bins = {name: build(wd, name, muts) for name, muts in VARIANTS.items()}
        tmpl = wd / "template.img"
        env = dict(os.environ, S1TWIN_DISK=str(tmpl), ASAN_OPTIONS="detect_leaks=0")
        r = subprocess.run([str(bins["r4.5 (unmodified)"]), "twin_mkimg"], capture_output=True, text=True, env=env, timeout=300)
        assert r.returncode == 0, r.stderr[-2000:]
        write_backup_gpt(tmpl)
        pins = wd / "twin-pins.json"; make_pins(tmpl, pins)
        before = wd / "before.json"
        rc, so = verifier("hash", str(tmpl), "--image", "--sparse", "--pins", str(pins), "-o", str(before), "--jobs", "8")
        cov = json.load(open(before))["coverage"]
        for mx in (256, 1):
            results.append(prove(bins["r4.5 (unmodified)"], tmpl, before, pins, wd, "r4.5 (unmodified)", mx))
        for name in [n for n in VARIANTS if n.startswith("M")]:
            results.append(prove(bins[name], tmpl, before, pins, wd, name, 256))
        # H3 demonstration: the v1 fake (refuses out-of-window writes) + v1 audit on the disabled-check variant
        # The historical v1 blind-spot demonstration is retained in the parent receipt; not rerun here.
    finally:
        # Keep receipts for independent review; remove only our sparse template/manifests.
        for p in wd.glob("*.img"): p.unlink()
        for p in wd.glob("*.after.json"): p.unlink()
        if (wd / "before.json").exists(): (wd / "before.json").unlink()
    base_ok = all(r["verdict"] == "PASS" for r in results if r["variant"].startswith("r4.5"))
    negatives = [r for r in results if r["variant"].startswith("M")]
    neg_ok = len(negatives) == 4 and all(r["verdict"] == "FAIL" for r in negatives)
    summary = {"resident": str(RES), "evidence_directory": str(wd), "coverage": cov, "r4.5_passes": base_ok, "all_broken_variants_fail": neg_ok,
               "status": "pass" if base_ok and neg_ok else "fail", "hardware_access": False,
               "table": [{"variant": r["variant"], "maxxfer": r.get("maxxfer"), "legacy_v1": r.get("legacy_v1_fake"), "verdict": r["verdict"],
                          "D1_image": r.get("D1_image_outside_identical"), "D2_log_outside_writes": r.get("D2_log", {}).get("write_like_outside"),
                          "D3_fake_counter": r.get("D3_fake_writes_outside_window"), "v1_audit_pass": r.get("v1_disk_audit_says")} for r in results],
               "results": results}
    print(json.dumps(summary, indent=1))
    return 0 if summary["status"] == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
