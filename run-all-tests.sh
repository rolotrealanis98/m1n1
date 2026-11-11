#!/bin/bash
# Every host test for resident r4.4 (r4.3 suites + r3.5 items + r4.4 write-path items: tag 2, flush not FUA, IOQ register clear); (r2.2 suites + native ro/rw end-to-end + reviewer r3 suite + static gate + S1WC decoder). Host only; no hardware.
set -uo pipefail; cd "$(dirname "$0")"; rc=0
export TMPDIR="$PWD/scratch" PYTHONDONTWRITEBYTECODE=1
mkdir -p "$TMPDIR"
python3 -I tests/sync-model.py || rc=1
python3 -I check-controller.py | python3 -c "import json,sys;d=json.load(sys.stdin);print('controller:',d['status'],d['output'].splitlines()[-1])" || rc=1
python3 -I check-memory.py | python3 -c "import json,sys;d=json.load(sys.stdin);print('memory:',d['output'].splitlines()[-1],'|',d['identity_output'].splitlines()[-1])" || rc=1
python3 -I tests/storage/s0/run.py | python3 -c "import json,sys;d=json.load(sys.stdin);assert d['status']=='pass';print('S0:',d['s0']['checks'],'checks')" || rc=1
python3 -I tests/storage/s1/run.py | python3 -c "import json,sys;d=json.load(sys.stdin);assert d['status']=='pass',d;print('S1:',d['s1']['checks'],'checks, mutants per receipt')" || rc=1
python3 -I tests/handoff/run.py > handoff.out 2>&1 && python3 -I -c "import json;d=json.load(open('handoff.out'));print('handoff:',d['results'][0]['last_stdout'],'mutants caught',sum(1 for r in d['results'][1:] if r['returncode']))" || rc=1
python3 -I tests/r2/run.py > tests/r2/last.json && python3 -I -c "import json;d=json.load(open('tests/r2/last.json'));print('r2:',d['suite'],'mutants caught',len(d['mutants_caught']))" || rc=1
python3 -I -c "import json;d=json.load(open('tests/r2/last.json'));print('s1dg decoder:',d['decoder_tests'])" || rc=1
python3 -I tests/s1dg/test_live.py "$(python3 -I -c "import json;print(json.load(open('tests/r2/last.json'))['output_directory'])")/s1dg.dump" | tail -1 || rc=1
(cd review-tests && OUT=$(mktemp -d) ./run.sh 200000 | tail -1 | sed 's/^/review: /') || rc=1
python3 -I review-tests/oracle_mutants.py | tail -1 || rc=1
(cd confirm-tests && OUT=$(mktemp -d) ./run.sh 20000 | tail -1 | sed 's/^/confirm: /') || rc=1
python3 -I confirm-tests/b3_mutants.py | tail -1 || rc=1
python3 -I review-tests/gap_mutants.py > review-tests/gap_mutants.out 2>&1 && grep -c "=> KILLED" review-tests/gap_mutants.out | sed 's/^/gap mutants killed: /' || rc=1
(cd tests/uefi-e2e && for f in 1 0; do bash build.sh out-flag$f $f >/dev/null 2>&1 && ./out-flag$f/e2e | grep -E "^PASS|win_takeover:"; done) || rc=1
if [ "${SKIP_NATIVE:-0}" != 1 ]; then
python3 -I tests/native/run.py > tests/native/last.json 2>&1 && python3 -I -c "import json;d=json.load(open('tests/native/last.json'));print('r4.4 native ro+rw e2e:',d['status'],d['scenarios'],'scenarios',d['checks'],'checks, mutants',d['mutants_caught'],'/',d['mutants_total'])" || rc=1
fi
(cd review-tests-r3 && python3 -I run.py 60000 > run.out 2>&1 && tail -1 run.out | sed "s/^/review-r3: /") || rc=1
if [ -f artifacts/m1n1-raw.elf ]; then python3 -I tests/static_r31.py || rc=1; else echo "static ELF gate: unverified (binaries not built)"; fi
python3 -I tests/s1wc_test.py || rc=1
echo "overall rc=$rc"; exit $rc
