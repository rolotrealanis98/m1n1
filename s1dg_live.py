"""Paused live S1DG read for the EXISTING primary owner's hv shell (resident r4; S1DG layout identical to r2.1..r3.1; S1WC follows it).
Not a launcher or package. Run only after a user interrupt (guest paused) or at a
guest-exit shell:

    import builtins
    builtins.exec(open('/Users/rolo/Work/agents/win-el2/storage/s1-resident-r2.1-20261109/s1dg_live.py').read(), globals())
    read_s1dg_live(p, u, '/ABSOLUTE/NEW/RUN', cont)

One bounded read-only u.iface.readmem() of exactly S1DG_SIZE bytes at
u.base + S1DG_OFFSET (offset of struct s1_diag inside the resident's `controller`,
taken from the r4 m1n1-raw.elf whose SHA256 is pinned below). Before anything is
read it checks that the running resident is r2.1 (hv_win_storage_stat(0) magic);
after the read it checks the S1DG header. No target writes, no PREP, no reset.
Exclusive 0600 outputs (O_EXCL): s1dg-live[-TAG].bin (raw) and .json (decoded,
if decoding succeeds). `cont()` in finally resumes the guest even if a read,
check or receipt fails. Never rerun against the same run/tag.
"""
import importlib.util, json, os, struct, time
from pathlib import Path

R21 = Path('/Users/rolo/Work/agents/win-el2/storage/s1-resident-r4.4-native-rw-20261110')
RAW_ELF_SHA256 = 'cf68f9e7a6c1277809463d65383787ec526e09eb820237e36376d6d649920c5d'
S1DG_OFFSET = 0x447088          # controller (0x426000) + S1DG_MODEL_OFFSET (0x21088)
S1DG_SIZE = 32232
S1DG_MAGIC, S1DG_VERSION = 0x47443153, 1


def read_s1dg_live(p, u, run_dir, cont, tag=''):
    stem = 's1dg-live' if not tag else 's1dg-live-%s' % tag
    run = Path(run_dir).resolve()
    result = {'utc': time.time(), 'address': None, 'bytes': 0, 'decoded': False, 'target_writes': False,
              'raw_elf_sha256': RAW_ELF_SHA256, 'offset': S1DG_OFFSET, 'size': S1DG_SIZE}
    try:
        fd = os.open(run / (stem + '.json'), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        try:
            ident = p.hv_win_storage_stat(0)
            result['storage_id'] = ident
            if ident >> 32 != 0x314e3553:
                raise RuntimeError('resident does not report S1 storage; not reading')
            addr = u.base + S1DG_OFFSET
            result['address'] = addr
            blob = bytes(u.iface.readmem(addr, S1DG_SIZE))       # single bounded read
            result['bytes'] = len(blob)
            bfd = os.open(run / (stem + '.bin'), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(bfd, 'wb') as out:
                out.write(blob)
            magic, version = struct.unpack_from('<II', blob, 0)
            if magic != S1DG_MAGIC or version != S1DG_VERSION:
                raise RuntimeError('S1DG header mismatch (%#x v%d): not an r2.1/r2.2/r3 resident' % (magic, version))
            spec = importlib.util.spec_from_file_location(
                's1dg_decode', R21 / 'source/proxyclient/tools/s1dg_decode.py')
            dec = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(dec)
            result['s1dg'] = dec.decode(blob)
            result['decoded'] = True
        except BaseException as exc:
            result.update(error_type=type(exc).__name__, error=str(exc))
            raise
        finally:
            with os.fdopen(fd, 'w') as out:
                json.dump(result, out)
                out.write('\n')
        summary = {k: result['s1dg'][k] for k in ('write_seq', 'admin_first_count', 'mmio_first_count',
                                                  'nonzero_status', 'writes', 'fatal_count',
                                                  'first_fatal_reason_name', 'refusals', 'invalid_db')}
        print(summary, flush=True)
        return result
    finally:
        cont()
