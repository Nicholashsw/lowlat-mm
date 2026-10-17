#!/usr/bin/env bash
# Replay the same synthetic feeds through the C++ engine and through lobsim with
# the touch rule, for every shipped config, and require byte-identical fill
# blotters and identical summaries. Run from the repository root after building
# both:  cmake --build build && (cd ocaml && dune build) && ocaml/crosscheck.sh
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cpp="$root/build/run_backtest"
ml="$root/ocaml/_build/default/bin/replay.exe"
gen="$root/ocaml/_build/default/bin/gen_feed.exe"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

fail=0
for cfg in "$root"/data/*.ini; do
  name="$(basename "$cfg" .ini)"
  [ "$name" = debug ] && continue   # trace logging; same rules as default.ini
  for seed in 3 11 29; do
    "$gen" --events 60000 --seed "$seed" --quotes feed.csv --trades prints.csv
    grep -v -E '^(pnl_csv|trades_csv|log_level)' "$cfg" > run.ini
    printf 'log_level = warn\ntrades_csv = cpp_trades.csv\n' >> run.ini
    "$cpp" run.ini feed.csv > cpp.txt
    "$ml" run.ini feed.csv --trades-out ml_trades.csv > ml.txt
    if cmp -s cpp_trades.csv ml_trades.csv && \
       diff -q <(grep -v -E 'fill model|trade prints|replay time' ml.txt) cpp.txt > /dev/null; then
      printf 'same  %-14s seed %-3s %6d fills\n' "$name" "$seed" "$(($(wc -l < ml_trades.csv) - 1))"
    else
      printf 'DIFF  %-14s seed %s\n' "$name" "$seed"
      fail=1
    fi
  done
done
exit $fail
