#!/bin/bash
# End-to-end test for the native Linux port of par2j (Parchive 2.0 client).
#
#   ./test_par2j.sh [path-to-par2j]
#
# Exercises create / list / verify / repair on a set of files, including the
# failure cases (not enough recovery slices) and a couple of path shapes that
# used to break the Win32 -> POSIX compat layer.
#
# Exits 0 when every check passes, 1 otherwise.
#
# Exit codes of par2j itself are a bitmask, and the test pins the ones that
# matter: 0 = nothing to do, 4 = repair needed, 8 = not enough recovery slices,
# 16 = repaired, 128 = repairable.  A set that is damaged but repairable
# therefore exits with 132 (4|128), and an unrepairable one with 12 (4|8).

BIN=${1:-"$(cd "$(dirname "$0")" && pwd)/par2j"}
SLICE=716800
FAILED=0

if [ ! -x "$BIN" ]; then
	echo "par2j not found or not executable: $BIN" >&2
	echo "run 'make' first, or pass the binary path as \$1" >&2
	exit 1
fi
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")

ok()   { printf '  ok   %s\n' "$1"; }
bad()  { printf '  FAIL %s\n' "$1"; FAILED=1; }
check(){ if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (want '$3', got '$2')"; fi; }

WORK=$(mktemp -d)
# KEEP=1 ./test_par2j.sh leaves the scratch directory behind for inspection.
if [ -n "${KEEP:-}" ]; then
	echo "work dir: $WORK"
else
	trap 'rm -rf "$WORK"' EXIT
fi

# --------------------------------------------------------------- source data
# f1 -> 2 slices, f2 -> 1 slice, f3 -> 3 slices   (6 source slices in total)
# -rr50 asks for 3 recovery slices.
make_source() {
	local dir=$1
	mkdir -p "$dir"
	head -c 1000000 /dev/urandom > "$dir/f1.bin"
	head -c  500000 /dev/urandom > "$dir/f2.bin"
	head -c 2000000 /dev/urandom > "$dir/f3.bin"
	( cd "$dir" && md5sum f1.bin f2.bin f3.bin > before.md5 )
}

create_set() {
	local dir=$1
	( cd "$dir" && "$BIN" c -ss$SLICE -rr50 rery.par2 f1.bin f2.bin f3.bin )
}

# --------------------------------------------------------------- basic flow
# A deep, long directory name: this is what exposed the "\\?\" prefix bug in
# copy_path_prefix() (only half of the path was shifted on a 4-byte wchar_t).
BASE=$WORK/deep/nested/path/that/is/long/enough/to/stress/path/handling

make_source "$BASE"
echo '== create'
create_set "$BASE" > "$WORK/create.log" 2>&1
check "create exit status" "$?" 0
grep -q 'Created successfully' "$WORK/create.log" && ok 'created successfully' \
	|| { bad 'created successfully'; sed 's/^/      /' "$WORK/create.log"; }
[ -f "$BASE/rery.par2" ] && ok 'rery.par2 written' || bad 'rery.par2 written'

echo '== list'
LC_ALL=C "$BIN" l "$BASE/rery.par2" > "$WORK/list.log" 2>&1
check "list exit status" "$?" 0
for f in f1.bin f2.bin f3.bin; do
	grep -q "\"$f\"" "$WORK/list.log" && ok "list shows $f" || bad "list shows $f"
done

echo '== verify (intact)'
LC_ALL=C "$BIN" v "$BASE/rery.par2" > "$WORK/verify.log" 2>&1
check "verify exit status" "$?" 0
grep -q 'All Files Complete' "$WORK/verify.log" && ok 'reports All Files Complete' \
	|| { bad 'reports All Files Complete'; sed 's/^/      /' "$WORK/verify.log"; }

echo '== verify (incomplete set)'
S0=$WORK/scenario0
cp -a "$BASE" "$S0"
rm -f "$S0/f2.bin"
LC_ALL=C "$BIN" v "$S0/rery.par2" > "$WORK/verify_bad.log" 2>&1
check "incomplete verify exit status (132 = 4|128)" "$?" 132
grep -q 'Ready to repair' "$WORK/verify_bad.log" && ok 'reports the set as repairable' \
	|| { bad 'reports the set as repairable'; sed 's/^/      /' "$WORK/verify_bad.log"; }

echo '== verify from another directory (relative + absolute names)'
( cd / && LC_ALL=C "$BIN" v "${BASE#/}/rery.par2" ) > "$WORK/verify2.log" 2>&1
check "relative verify exit status" "$?" 0
LC_ALL=C "$BIN" v "$BASE/rery.par2" > "$WORK/verify3.log" 2>&1
check "absolute verify exit status" "$?" 0

# ------------------------------------------------------------------- repair
# 1) one whole file lost + one slice of another file overwritten -> 2 losses,
#    3 recovery slices available -> repair must succeed.
echo '== repair (deleted file + overwritten slice)'
S1=$WORK/scenario1
cp -a "$BASE" "$S1"
rm -f "$S1/f2.bin"
dd if=/dev/urandom of="$S1/f1.bin" bs=4096 count=1 seek=3 conv=notrunc status=none
LC_ALL=C "$BIN" r "$S1/rery.par2" > "$WORK/repair1.log" 2>&1
check "repair exit status (16 = repaired)" "$?" 16
grep -q 'Repaired successfully' "$WORK/repair1.log" && ok 'reports Repaired successfully' \
	|| { bad 'reports Repaired successfully'; sed 's/^/      /' "$WORK/repair1.log"; }
( cd "$S1" && md5sum -c before.md5 > /dev/null 2>&1 ) && ok 'all files restored bit-exactly' \
	|| bad 'all files restored bit-exactly'
LC_ALL=C "$BIN" v "$S1/rery.par2" > "$WORK/repair1v.log" 2>&1
grep -q 'All Files Complete' "$WORK/repair1v.log" && ok 'verifies after repair' \
	|| bad 'verifies after repair'

# 2) delete a file and repair it by name (the file is rebuilt from parity).
echo '== repair (single deleted file)'
S2=$WORK/scenario2
cp -a "$BASE" "$S2"
rm -f "$S2/f3.bin"
LC_ALL=C "$BIN" r "$S2/rery.par2" > "$WORK/repair2.log" 2>&1
check "repair exit status (16 = repaired)" "$?" 16
grep -q 'Repaired successfully' "$WORK/repair2.log" && ok 'reports Repaired successfully' \
	|| { bad 'reports Repaired successfully'; sed 's/^/      /' "$WORK/repair2.log"; }
( cd "$S2" && md5sum -c before.md5 > /dev/null 2>&1 ) && ok 'deleted file restored bit-exactly' \
	|| bad 'deleted file restored bit-exactly'

# 3) not enough recovery slices: 5 slices lost (f1 + f3 keep only their file
#    descriptions), only 3 recovery slices exist -> must refuse.
echo '== repair (not enough recovery slices)'
S3=$WORK/scenario3
cp -a "$BASE" "$S3"
rm -f "$S3/f1.bin" "$S3/f3.bin"
LC_ALL=C "$BIN" r "$S3/rery.par2" > "$WORK/repair3.log" 2>&1
check "repair exit status (12 = 4|8, not repairable)" "$?" 12
grep -q 'Need [0-9]* more slice(s)' "$WORK/repair3.log" && ok 'reports missing slices' \
	|| { bad 'reports missing slices'; sed 's/^/      /' "$WORK/repair3.log"; }
[ ! -f "$S3/f1.bin" ] && [ ! -f "$S3/f3.bin" ] && ok 'no bogus files produced' \
	|| bad 'no bogus files produced'

# 4) a recovery volume on its own must be usable for repair (no index file).
echo '== repair (using only the volume file)'
S4=$WORK/scenario4
cp -a "$BASE" "$S4"
rm -f "$S4/rery.par2" "$S4/f2.bin"
VOL=$(echo "$S4"/rery.vol*.par2)
LC_ALL=C "$BIN" r "$VOL" > "$WORK/repair4.log" 2>&1
check "volume-only repair exit status" "$?" 16
grep -q 'Repaired successfully' "$WORK/repair4.log" && ok 'volume-only repair' \
	|| { bad 'volume-only repair'; sed 's/^/      /' "$WORK/repair4.log"; }
( cd "$S4" && md5sum -c before.md5 > /dev/null 2>&1 ) && ok 'volume-only repair bit-exact' \
	|| bad 'volume-only repair bit-exact'

# ------------------------------------------------ special (Unicode) filenames
# Names travel through four representations: the command line (UTF-8), the File
# Description packet (UTF-8), the Unicode Filename packet (UTF-16LE) and the
# host wchar_t, which is 4 bytes wide here.  This set exercises all of them in
# one go, including a surrogate pair (U+1F600) and a non-ASCII directory name.
#
# Everything here runs under LC_ALL=C on purpose: a locale based decode of the
# command line used to turn these names into mojibake, which is exactly what
# scripts, cron jobs and containers hit.
echo '== special filenames (CJK, surrogate pair, combining marks, spaces)'
SU="$WORK/unicode/中文 目录"
mkdir -p "$SU"
U1='中文文件.bin'; U2='emoji😀.bin'; U3='café-ünïcodé.dat'; U4='with space.bin'
head -c 1500 /dev/urandom > "$SU/$U1"
head -c 1200 /dev/urandom > "$SU/$U2"
head -c  800 /dev/urandom > "$SU/$U3"
head -c  900 /dev/urandom > "$SU/$U4"
( cd "$SU" && md5sum "$U1" "$U2" "$U3" "$U4" > before.md5 )

LC_ALL=C "$BIN" c -ss2000 -rr100 -up -c"中文注释😀" "$SU/rery.par2" \
	"$U1" "$U2" "$U3" "$U4" > "$WORK/uni_create.log" 2>&1
check 'create exit status' "$?" 0

LC_ALL=C "$BIN" l "$SU/rery.par2" > "$WORK/uni_list.log" 2>&1
for n in "$U1" "$U2" "$U3" "$U4"; do
	grep -qF "\"$n\"" "$WORK/uni_list.log" && ok "list shows $n" \
		|| bad "list shows $n"
done

# The Unicode packets must hold UTF-16LE code units: 中文 is 2d 4e 87 65 and the
# comment 中文注释 is 2d 4e 87 65 e8 6c ca 91.  Storing the host's 4-byte
# wchar_t instead would give 2d 4e 00 00 ..., so check both directions.
# BusyBox builds do not always install od(1), so hexdump is the stand-in; when
# neither tool exists the packet bytes cannot be inspected and that is reported
# as a skip, not a failure (the visible behaviour is covered by the checks above).
packet_hex() {
	if command -v od >/dev/null 2>&1; then
		cat "$@" | od -An -tx1 -v | tr -d ' \n'
	elif command -v hexdump >/dev/null 2>&1; then
		hexdump -ve '1/1 "%02x"' "$@"
	else
		return 1
	fi
}
HEX=$(packet_hex "$SU"/rery*.par2)
if [ -z "$HEX" ]; then
	printf '  skip  neither od nor hexdump can dump the packets here\n'
else
	case "$HEX" in
	*2d4e8765*) ok 'Unicode Filename packet holds UTF-16LE' ;;
	*) bad 'Unicode Filename packet holds UTF-16LE' ;;
	esac
	case "$HEX" in
	*2d4e0000*) bad "Unicode packet has 4-byte wchar_t units" ;;
	*) ok 'no 4-byte wchar_t units in the packets' ;;
	esac
	case "$HEX" in
	*2d4e8765e86cca91*) ok 'Unicode Comment packet holds UTF-16LE' ;;
	*) bad 'Unicode Comment packet holds UTF-16LE' ;;
	esac
fi

LC_ALL=C "$BIN" v "$SU/rery.par2" > "$WORK/uni_verify.log" 2>&1
check 'verify exit status' "$?" 0
grep -qF 'All Files Complete' "$WORK/uni_verify.log" && ok 'set verifies' \
	|| { bad 'set verifies'; sed 's/^/      /' "$WORK/uni_verify.log"; }
grep -qF 'Comment : 中文注释😀' "$WORK/uni_verify.log" \
	&& ok 'Unicode comment round-trips' \
	|| { bad 'Unicode comment round-trips'; grep -a 'Comment' "$WORK/uni_verify.log"; }

rm -f "$SU/$U1" "$SU/$U2"
LC_ALL=C "$BIN" r "$SU/rery.par2" > "$WORK/uni_repair.log" 2>&1
check 'repair exit status (16 = repaired)' "$?" 16
( cd "$SU" && md5sum -c before.md5 > /dev/null 2>&1 ) && ok 'Unicode files restored bit-exactly' \
	|| bad 'Unicode files restored bit-exactly'
[ -f "$SU/$U1" ] && [ -f "$SU/$U2" ] && ok 'restored names match byte for byte' \
	|| bad 'restored names match byte for byte'

# Windows-reserved characters: upstream refuses them, and refusing is fine --
# silently mangling the name would not be.
echo '== names Windows cannot represent'
SB="$WORK/badname"
mkdir -p "$SB"
head -c 1000 /dev/urandom > "$SB/a<b.bin"
( cd "$SB" && LC_ALL=C "$BIN" c -ss1000 -rr50 bad.par2 'a<b.bin' ) > "$WORK/badname.log" 2>&1
grep -q 'is invalid' "$WORK/badname.log" && ok 'reports the invalid name' \
	|| { bad 'reports the invalid name'; sed 's/^/      /' "$WORK/badname.log"; }
[ ! -f "$SB/bad.par2" ] && ok 'no half-written recovery file' \
	|| bad 'no half-written recovery file'

# Trailing dot / space: upstream warns (Windows would strip them) but the set
# stays usable, which is what matters on a filesystem that allows them.
echo '== trailing dot and trailing space'
SD="$WORK/dotspace"
mkdir -p "$SD"
head -c 800 /dev/urandom > "$SD/trail."
head -c 800 /dev/urandom > "$SD/trail "
( cd "$SD" && LC_ALL=C "$BIN" c -ss1000 -rr50 s.par2 'trail.' 'trail ' ) > "$WORK/dot.log" 2>&1
LC_ALL=C "$BIN" v "$SD/s.par2" > "$WORK/dotv.log" 2>&1
check 'verify exit status' "$?" 0
grep -qF 'All Files Complete' "$WORK/dotv.log" && ok 'trailing dot/space names still resolve' \
	|| { bad 'trailing dot/space names still resolve'; sed 's/^/      /' "$WORK/dotv.log"; }

# An absolute path whose first directory starts with "d" (/data, /dev, /disk1)
# looks exactly like the "/d<dir>" option until the file system is consulted.
# It has to be treated as a path, while -vd<existing dir> still has to be the
# option, so both halves are pinned here.
echo '== absolute paths starting with /d'
DCACHE="$WORK/cache"
mkdir -p "$DCACHE"
LC_ALL=C "$BIN" v -vd"$DCACHE/" -vs1 "$BASE/rery.par2" > "$WORK/dopt.log" 2>&1
check '-vd<dir> still parsed as an option' "$?" 0
ls "$DCACHE"/2_*.ini >/dev/null 2>&1 && ok 'the option wrote its cache file where asked' \
	|| bad 'the option wrote its cache file where asked'

DROOT=/dev/shm/par2j-$$
if mkdir -p "$DROOT/tree/子目录" 2>/dev/null; then
	head -c $SLICE /dev/urandom > "$DROOT/tree/d1.bin"
	head -c 1234 /dev/urandom > "$DROOT/tree/子目录/d2.bin"
	( cd "$WORK" && LC_ALL=C "$BIN" c -ss$SLICE -rr100 "$DROOT/d.par2" "$DROOT/tree" ) > "$WORK/droot.log" 2>&1
	check 'create exit status under /dev/shm' "$?" 0
	rm -rf "$DROOT/tree"
	LC_ALL=C "$BIN" r "$DROOT/d.par2" > "$WORK/drootr.log" 2>&1
	check 'repair exit status under /dev/shm (16 = repaired)' "$?" 16
	[ -f "$DROOT/tree/d1.bin" ] && [ -f "$DROOT/tree/子目录/d2.bin" ] \
		&& ok 'directory tree rebuilt from a /d path' \
		|| bad 'directory tree rebuilt from a /d path'
	rm -rf "$DROOT"
else
	printf '  skip  %s is not writable\n' "$DROOT"
fi

# An input that ends with a separator means "record this empty folder, do not
# look inside it" (the first branch of search_files() in par2_cmd.c), so the set
# comes out 292 bytes with no recovery volume and still says "Created
# successfully".  This is upstream behaviour on Windows as well, so it is pinned
# here rather than changed; the front end strips the separator instead.
echo '== trailing separator records the folder, not its contents'
TS="$WORK/tailslash"
mkdir -p "$TS/src"
head -c 300000 /dev/urandom > "$TS/src/x.bin"
( cd "$TS" && LC_ALL=C "$BIN" c -ss$SLICE -rr50 plain.par2 src ) > /dev/null 2>&1
( cd "$TS" && LC_ALL=C "$BIN" c -ss$SLICE -rr50 slash.par2 src/ ) > /dev/null 2>&1
( cd "$TS" && LC_ALL=C "$BIN" l plain.par2 ) > "$WORK/ts_plain.log" 2>&1
( cd "$TS" && LC_ALL=C "$BIN" l slash.par2 ) > "$WORK/ts_slash.log" 2>&1
grep -qF '"src/x.bin"' "$WORK/ts_plain.log" && ok 'a folder name without a separator searches inside' \
	|| bad 'a folder name without a separator searches inside'
grep -qF '"src/x.bin"' "$WORK/ts_slash.log" && bad 'a trailing separator skips the contents' \
	|| ok 'a trailing separator skips the contents'
grep -qE 'Input File Slice count[[:space:]]*:[[:space:]]*0' "$WORK/ts_slash.log" \
	&& ok 'the folder-only set carries no slices' || bad 'the folder-only set carries no slices'

echo
if [ "$FAILED" = 0 ]; then
	echo 'ALL TESTS PASSED'
else
	echo 'SOME TESTS FAILED'
fi
exit $FAILED
