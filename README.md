# MultiPar — native Linux build of `par2j`, plus a terminal front end

Fork of [Yutaka-Sawada/MultiPar](https://github.com/Yutaka-Sawada/MultiPar).
Upstream publishes only the command-line cores as C source and ships Windows
binaries only. This fork builds `par2j` (the PAR 2.0 client) natively on Linux
through a Win32 compatibility layer, and adds a Go / Bubble Tea front end.

**What this is not.** PAR 2.0 is an open format and Linux already has an
implementation: Debian's `par2` package (par2cmdline), which SABnzbd and nzbget
drive. This fork does not bring PAR 2 to Linux — it makes *MultiPar's*
implementation buildable here, with the options and machine-readable output a
front end needs. PAR 2 is also not a backup tool: no increments, no versioning,
no encryption, and any edit means re-creating the set. For backups use restic
or borg. PAR 2's real niche is *in-place* redundancy for data that stops
changing — cold archives, NAS volumes, disc images — where a damaged file can
be repaired on the spot with no restore workflow.

## Build

```sh
cd source/par2j && make -j4          # -> ./par2j        (gcc, make, pthread)
cd source/multipar-tui && CGO_ENABLED=0 go build -o multipar-tui .
cd source/par2j && bash test_par2j.sh   # 12 scenarios / 44 assertions
```

**Copying the binary to another machine? Build it statically:**

```sh
cd source/par2j && make -j4 static   # -> ./par2j with no ELF interpreter at all
```

A plain `make` records the build host's glibc in the symbol version table - gcc 14
routes `strtol`/`fscanf` to the `__isoc23_*` aliases that only glibc 2.38 has - so
the result fails on older distributions with `GLIBC_2.38 not found`, and on musl
systems (**OpenWrt, Alpine**) there is no `/lib64/ld-linux-x86-64.so.2` to load in
the first place, which looks like "the file won't open". `make static` carries its
own libc, so only the CPU architecture and the kernel matter. Usethe same reason. Both flags are what the release assets are built with. Nothing
above the x86-64 baseline is required to start: SSSE3, SSE4.1, PCLMUL and AVX2 are
all behind per-function `#pragma GCC target` and a runtime `cpu_flag` check, so the
binary also runs on CPUs without AVX (disassembly-verified, see manual §2.2). Static
linking costs about 1 MB and leaves the GPU path inert, since `dlopen()` cannot
reach libOpenCL from a static binary - it falls back to the CPU code cleanly.

## Use

```sh
./par2j c -ss65536 -rr20 backup.par2 /data/dir   # create
./par2j v backup.par2                            # verify
./par2j r -vd"$HOME/.cache" -vs1 backup.par2     # repair
```

Two things that bite: block size is `-ss`, not `-s`; and a successful repair
exits **16**, not 0 (the exit code is a bitmask — see the manual). Options
table, exit codes, recipes and troubleshooting:
[`source/par2j/MANUAL.zh.md`](source/par2j/MANUAL.zh.md) (中文),
[`source/multipar-tui/README.md`](source/multipar-tui/README.md).

## Where `par2j` goes beyond `par2cmdline`

| | |
|---|---|
| `-vs` verification cache | avoids a full re-scan; measured 1056 ms → 7 ms on a 3 × 100 MB set |
| `-w` JSON report, `PAR2J_PROGRESS` events | machine-readable output, which is what the TUI rides on |
| exit-code bitmask | separates repairable / rename-only / needs-more-blocks |
| `-rp -rd -rf -ri -lr -lp -ls -sn -sr -sm -fa -fe` | considerably wider redundancy and volume-layout control |

## Out of scope

No GUI (upstream has no GUI source), no Explorer shell extension, no
`.sfv` / `.md5` manifests — `source/sfv_md5` needs Windows CNG, so use
`md5sum` / `sha256sum` here. `source/par1j` (PAR 1.0) does compile against this
compatibility layer and can create and repair `.par` sets, but its filename
handling is broken and `-i` segfaults; see manual §1.1 and §10 before using it.

**Interop caveat, worth reading before you archive anything.** MultiPar records
multi-level directories by putting slash-separated relative paths
(`src/a/b/x.bin`) into the PAR 2 `FileName` field, and writes zero-byte entries
for empty folders. Those are extensions beyond the spec, so another PAR 2
implementation may not rebuild your tree the way `par2j` does. Flat,
ASCII-named sets carry the least risk. Cross-implementation behaviour has not
been tested yet.

## Extra environment variables from this port

`PAR2J_TRACE=1` path debugging · `PAR2J_PROGRESS=1` one JSON event per line on
stderr · `PAR2J_PROGRESS_INTERVAL=<ms>` throttle (20–10000, default 1024).
With none set, output is byte-identical to the Windows build.

---

## Upstream release notes, unmodified

Kept for reference as of v1.3.3.6 (original title was `# MultiPar`; see
`README.md` at commit `4d0fbe3`).

### v1.3.3.6 is public

&nbsp; This is a minor update version. 
If there is no serious problem or large change, 
next version will be the last of v1.3.3 tree.

&nbsp; Because recent Nvidia GPU doesn't support 32-bit application, 
I changed MultiPar's behavior not to check GPU property. 
Thanks [betagitman for reporting this issue](https://github.com/Yutaka-Sawada/MultiPar/issues/155). 
On 64-bit Windows OS, users need to use par2j64.exe instead of par2j.exe for GPU acceleration.

&nbsp; MultiPar supports FLAC fingerprint (ffp). 
as [it was requested](https://github.com/Yutaka-Sawada/MultiPar/discussions/157). 
If you want to use FLAC Fingerprint on MultiPar, you need to put `flac.exe` and `libFLAC.dll` 
in MultiPar folder or somewhere PATH variable environment (such like Windows/System32). 
You can download them from [FLAC official download page](https://xiph.org/flac/download.html).

&nbsp; MultiPar shows misnamed or moved files at verification with .SFV and .MD5 checksum. 
To search misnamed files, you need to change "Verification level" option to "Additional verification" 
at "Verification and Repair options" on MultiPar Option window. 
If recursive search is slow, you may check "Don't search subfolders" item.

&nbsp; My web-pages on `vector.co.jp` disappered at 2024 December 20. 
Thanks Vector to support MultiPar for long time. 
I use [this GitHub page](https://github.com/Yutaka-Sawada/MultiPar) for MultiPar announcement.


[ Changes from 1.3.3.5 to 1.3.3.6 ]  

Installer update
- Inno Setup was updated from v6.5.4 to v6.7.1.

GUI update
- Change
  - GPU option is enabled always, even when GPU device doesn't exist.

SFV/MD5 client update
- New
  - FLAC Fingerprint files are supported (by flac.exe and libFLAC.dll).
  - It will search misnamed files in base directory by "vl2" option.


[ Hash value ]  

&nbsp; MultiPar1336.zip  
MD5: D84778B6BE68AD2A488FB6492C3F8992  
SHA1: 9CFE9EA7AB3559E6AE2C98A443F4EF1721D70614  

&nbsp; MultiPar1336_setup.exe  
MD5: 9A8D238DD657B9F93817BCCB90DC3C92  
SHA1: 82F22F5815F185FA50B2A6BD69967B0C6BFC2A92  

&nbsp; To install under "Program Files" or "Program Files (x86)" directory, 
you must select "Install for all users" at the first dialog.

&nbsp; Old versions and source code packages are available at 
[GitHub](https://github.com/Yutaka-Sawada/MultiPar/releases) or 
[OneDrive](https://1drv.ms/f/c/8eb5bd32c534a1d1/QtGhNMUyvbUggI5pAAAAAAAAKjWf9HxrAn-GDQ).
