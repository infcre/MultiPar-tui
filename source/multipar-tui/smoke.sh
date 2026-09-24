#!/bin/sh
# End to end smoke test for the MultiPar TUI.
#
# Builds the front end, drives par2j through --plain mode in a scratch
# directory (create / verify / damage / verify / repair / verify), then runs the
# pty based TUI test when python3 is available.
#
#   ./smoke.sh                 # uses ../par2j/par2j
#   PAR2J_BIN=/path/par2j ./smoke.sh
set -u

cd "$(dirname "$0")" || exit 1
SRC=$(pwd)
fails=0

check() { # description expected actual
	if [ "$2" = "$3" ]; then
		echo "  ok   $1"
	else
		echo "  FAIL $1 (expected $2, got $3)"
		fails=$((fails + 1))
	fi
}

if ! command -v go >/dev/null 2>&1; then
	for candidate in "$HOME/.local/go/bin/go" /usr/local/go/bin/go; do
		[ -x "$candidate" ] && PATH="$(dirname "$candidate"):$PATH" && export PATH
	done
fi
if ! command -v go >/dev/null 2>&1; then
	echo "go toolchain not found"
	exit 1
fi

PAR2J="${PAR2J_BIN:-../par2j/par2j}"
PAR2J=$(cd "$(dirname "$PAR2J")" 2>/dev/null && pwd)/$(basename "$PAR2J")
if [ ! -x "$PAR2J" ]; then
	echo "no par2j at $PAR2J (build it with: make -C ../par2j)"
	exit 1
fi

echo "== build"
go build -o multipar-tui . || exit 1
check "binary built" ok ok

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work" || exit 1
for name in a.bin b.bin; do
	head -c 1400000 /dev/urandom > "$name"
done
md5sum a.bin b.bin > before.md5
TUI="$SRC/multipar-tui"

plain() { # op [extra args...]
	op=$1
	shift
	PAR2J_BIN="$PAR2J" "$TUI" --plain --dir "$work" --op "$op" "$@" 2>&1
}

echo "== create through the driver"
out=$(plain c --par set.par2 --inputs a.bin,b.bin --ss 716800 --rr 100)
code=$(echo "$out" | sed -n 's/^exit \([0-9]*\).*/\1/p')
check "create exits 0" 0 "$code"
check "recovery file created" yes "$([ -f set.par2 ] && echo yes || echo no)"

echo "== verify a complete set"
code=$(plain v --par set.par2 | sed -n 's/^exit \([0-9]*\).*/\1/p')
check "complete set exits 0" 0 "$code"

echo "== verify with a lost file"
rm -f a.bin
out=$(plain v --par set.par2)
code=$(echo "$out" | sed -n 's/^exit \([0-9]*\).*/\1/p')
check "damaged set exits 132 (4|128)" 132 "$code"

echo "== repair through the driver"
out=$(plain r --par set.par2)
code=$(echo "$out" | sed -n 's/^exit \([0-9]*\).*/\1/p')
check "repair exits 16" 16 "$code"

echo "== restored bit for bit"
code=$(plain v --par set.par2 | sed -n 's/^exit \([0-9]*\).*/\1/p')
check "verify after repair exits 0" 0 "$code"
if md5sum -c before.md5 >/dev/null 2>&1; then
	check "md5sum -c before.md5" ok ok
else
	check "md5sum -c before.md5" ok failed
fi

if command -v python3 >/dev/null 2>&1; then
	echo "== TUI in a pty"
	if PAR2J_BIN="$PAR2J" TUI_BIN="$TUI" "$SRC/test_tui.py"; then
		check "pty TUI test" ok ok
	else
		check "pty TUI test" ok failed
	fi
else
	echo "== TUI in a pty: skipped (no python3)"
fi

echo
if [ "$fails" -eq 0 ]; then
	echo "ALL SMOKE TESTS PASSED"
else
	echo "$fails SMOKE TEST(S) FAILED"
fi
exit "$fails"
