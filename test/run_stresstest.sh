set -euo pipefail

SRC="stresstest.cpp"
OUTDIR="${OUTDIR:-./dodo_builds}"
LOG="${LOG:-./dodo_all_results.txt}"

mkdir -p "$OUTDIR"
: > "$LOG"

ts() { date +"%Y-%m-%d %H:%M:%S"; }

append() {
  # $1 = label
  # $2 = command string
  {
    echo
    echo "================================================================================"
    echo "[$(ts)] $1"
    echo "CMD: $2"
    echo "--------------------------------------------------------------------------------"
  } >> "$LOG"
}

run_one() {
  local label="$1"
  local compile_cmd="$2"
  local exe="$3"
  local run_cmd="$4"

  append "$label (BUILD)" "$compile_cmd"
  if eval "$compile_cmd" >>"$LOG" 2>&1; then
    echo "[OK] build: $label" >>"$LOG"
  else
    echo "[FAIL] build: $label" >>"$LOG"
    return 1
  fi

  append "$label (RUN)" "$run_cmd"
  # run (do not stop whole script on runtime failure; record status)
  set +e
  eval "$run_cmd" >>"$LOG" 2>&1
  local rc=$?
  set -e
  echo "[EXIT CODE] $rc" >>"$LOG"

  # cleanup exe
  rm -f "$exe"
  # cleanup possible LTO temporaries (best-effort)
  rm -f "${exe}.lto" "${exe}.o" 2>/dev/null || true
}

COMMON_WARN="-Wall -Wextra -Wpedantic -Werror -Wconversion -Wsign-conversion -Wshadow -Wundef -Wdouble-promotion -Wcast-align -Wcast-qual -Wformat=2 -Wnull-dereference"
COMMON_BASE="-std=c++20 -fno-exceptions -fno-rtti -pthread"

echo "Dodo build+run sweep started at $(ts)" >>"$LOG"
echo "Source: $SRC" >>"$LOG"
echo "Output dir: $OUTDIR" >>"$LOG"

# 0) O3 strict
EXE0="$OUTDIR/dodo_test_O3"
CMD0="g++ $COMMON_BASE -O3 -DNDEBUG -march=native -mtune=native $COMMON_WARN \"$SRC\" -o \"$EXE0\""
RUN0="\"$EXE0\""
run_one "O3 strict" "$CMD0" "$EXE0" "$RUN0"

# 1) O3 + LTO
EXE1="$OUTDIR/dodo_test_O3_lto"
CMD1="g++ $COMMON_BASE -O3 -DNDEBUG -march=native -mtune=native -flto -fuse-linker-plugin $COMMON_WARN \"$SRC\" -o \"$EXE1\""
RUN1="\"$EXE1\""
run_one "O3 + LTO" "$CMD1" "$EXE1" "$RUN1"

# 2) Debug
EXE2="$OUTDIR/dodo_test_dbg"
CMD2="g++ $COMMON_BASE -O0 -g3 $COMMON_WARN \"$SRC\" -o \"$EXE2\""
RUN2="\"$EXE2\""
run_one "Debug O0" "$CMD2" "$EXE2" "$RUN2"

# 3) FAST_MODE
EXE3="$OUTDIR/dodo_test_fast_O3"
CMD3="g++ $COMMON_BASE -O3 -DNDEBUG -DDODO_FAST_MODE -march=native -mtune=native $COMMON_WARN \"$SRC\" -o \"$EXE3\""
RUN3="\"$EXE3\""
run_one "FAST_MODE O3" "$CMD3" "$EXE3" "$RUN3"

# 4) ASan + UBSan
EXE4="$OUTDIR/dodo_test_asan_ubsan"
CMD4="g++ $COMMON_BASE -O1 -g3 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer $COMMON_WARN \"$SRC\" -o \"$EXE4\""
RUN4="ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \"$EXE4\""
run_one "ASan+UBSan O1" "$CMD4" "$EXE4" "$RUN4"

# 5) UBSan only
EXE5="$OUTDIR/dodo_test_ubsan"
CMD5="g++ $COMMON_BASE -O1 -g3 -fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer $COMMON_WARN \"$SRC\" -o \"$EXE5\""
RUN5="UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \"$EXE5\""
run_one "UBSan O1" "$CMD5" "$EXE5" "$RUN5"

# 6) TSan
EXE6="$OUTDIR/dodo_test_tsan"
CMD6="g++ $COMMON_BASE -O1 -g3 -fsanitize=thread -fno-omit-frame-pointer $COMMON_WARN \"$SRC\" -o \"$EXE6\""
RUN6="TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 \"$EXE6\""
run_one "TSan O1" "$CMD6" "$EXE6" "$RUN6"

# 7) g++ -Os
EXE7="$OUTDIR/dodo_test_Os"
CMD7="g++ $COMMON_BASE -Os -DNDEBUG $COMMON_WARN \"$SRC\" -o \"$EXE7\""
RUN7="\"$EXE7\""
run_one "Os size" "$CMD7" "$EXE7" "$RUN7"

echo >>"$LOG"
echo "Dodo build+run sweep finished at $(ts)" >>"$LOG"
echo "Log saved to: $LOG" >>"$LOG"


#opt
echo "Done. Results in: $LOG"

