#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d /tmp/health-voice-verify.XXXXXX)"
CACHE="$(mktemp -d /tmp/health-voice-pycache.XXXXXX)"
cleanup(){ rm -rf -- "$WORK" "$CACHE"; }
trap cleanup EXIT

if git -C "$ROOT" rev-parse --verify HEAD >/dev/null 2>&1; then
  git -C "$ROOT" archive HEAD | tar -x -C "$WORK"
else
  tar -C "$ROOT" --exclude=.git --exclude=build --exclude=managed_components --exclude=sdkconfig --exclude=sdkconfig.old --exclude=__pycache__ -cf - . | tar -x -C "$WORK"
fi

export PYTHONPYCACHEPREFIX="$CACHE"
python3 "$WORK/scripts/secret_scan.py" --root "$WORK"
python3 "$WORK/scripts/check_repo.py" --root "$WORK"
python3 -m py_compile "$WORK"/scripts/*.py "$WORK"/tests/*.py "$WORK"/tools/*.py
(cd "$WORK" && python3 -m unittest discover -s tests -v)
(
  cd "$WORK"
  env -u DEEPSEEK_API_KEY -u DEEPSEEK_API_BASE -u DEEPSEEK_MODEL \
    python3 tools/eval_offline.py --scenarios tools/eval_scenarios.json
)

IDF_EXPORT="${IDF_EXPORT:-$HOME/.espressif/v5.5/esp-idf/export.sh}"
if [[ ! -f "$IDF_EXPORT" ]]; then
  echo "ESP-IDF export script not found: $IDF_EXPORT" >&2
  exit 1
fi
(
  cd "$WORK"
  # shellcheck disable=SC1090
  source "$IDF_EXPORT" >/dev/null
  idf.py set-target esp32s3
  idf.py build
)

echo "Verification: PASS"
