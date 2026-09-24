# Native Linux build of par2j -- MultiPar's command line PAR2 client.
#
#   make          build ./par2j
#   make WARN=    build quietly (errors only), useful for the first bring-up
#   make clean
#
# The Win32 surface the program needs is provided by compat/windows.h +
# compat/wincompat.c; compat/prefix.h (force-included via -include) supplies
# MSVC keywords and renames the few CRT functions whose semantics differ
# between MSVC and glibc (printf's %I64d, swprintf's %s-as-wide, ...).
#
# Paths on the command line may be given POSIX style (/data/x); the entry
# wrapper in wincompat.c rewrites them to the program's virtual "C:\" drive
# form, which is mapped back to "/" at every system call.

CC      := gcc
# -fcommon: gf16.h contains tentative definitions of function pointers that
# every TU includes; MSVC merges them at link time, GCC >= 10 needs -fcommon.
# -Wno-incompatible-pointer-types is needed unconditionally: the OpenCL loader
# passes char** where the headers want const char** (GCC 14 makes it an error).
BASE    := -std=gnu11 -O2 -g -fno-strict-aliasing -fcommon \
           -Wno-incompatible-pointer-types -Wno-implicit-int \
           -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 \
           -Icompat -IOpenCL -include compat/prefix.h
WARN    := -Wall \
           -Wno-format -Wno-unused-variable -Wno-unused-but-set-variable \
           -Wno-unused-function -Wno-unused-parameter -Wno-sign-compare \
           -Wno-pointer-sign -Wno-unknown-pragmas -Wno-char-subscripts \
           -Wno-missing-field-initializers -Wno-parentheses \
           -Wno-misleading-indentation -Wno-old-style-definition \
           -Wno-redundant-decls -Wno-implicit-fallthrough -Wno-switch \
           -Wno-type-limits -Wno-empty-body -Wno-unused-value \
           -Wno-dangling-pointer -Wno-array-compare -Wno-calloc-transposed-args
CFLAGS  ?= $(BASE) $(WARN)
LDLIBS  := -pthread -ldl -lm

TARGET  := par2j

# SIMD: crc.c uses PCLMUL intrinsics (guarded by cpu_flag & 8 at runtime).
# gf16.c only gets -mssse3: giving the whole file -mavx2 makes GCC emit VEX
# encodings even for plain SSE2 code, which SIGILLs on CPUs without AVX; the
# AVX2 functions are tagged with #pragma GCC target("avx2") in the source
# instead. All SIMD paths are chosen at runtime through cpu_flag, and -O2 does
# not auto-vectorize, so the target flags never leak into non-guarded code.
crc.o:    CFLAGS += -mpclmul -msse4.1
gf16.o:   CFLAGS += -mssse3
phmd5a.o: CFLAGS += -msse3
phmd5s.o: CFLAGS += -msse3

SRCS    := common2.c crc.c create.c gf16.c ini.c json.c lib_opencl.c list.c \
           md5_crc.c par2.c par2_cmd.c phmd5.c phmd5a.c phmd5s.c \
           reedsolomon.c repair.c rs_decode.c rs_encode.c search.c verify.c \
           compat/wincompat.c
OBJS    := $(SRCS:.c=.o)

# Embed the OpenCL kernel source as a symbol pair (RT_STRING resource #1).
# Note: objcopy's --redefine-sym does not match "source.cl" because dots become
# underscores inside generated symbol names, hence the literal spellings.
KERNEL_SRC := source.cl
CLRES_O    := source_cl.res.o

all: $(TARGET)

$(TARGET): $(OBJS) $(CLRES_O)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(CLRES_O) $(LDLIBS)

$(CLRES_O): $(KERNEL_SRC)
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		--redefine-sym _binary_source_cl_start=w32_source_cl \
		--redefine-sym _binary_source_cl_end=w32_source_cl_end \
		$(KERNEL_SRC) $@

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(CLRES_O) $(TARGET)

# Diagnostic build with AddressSanitizer: make asan
#
# CFLAGS has to be overridden as a whole, which switches off the per-object
# "CFLAGS +=" rules above, so the SIMD flags are repeated here.  The build
# leaves ASan-instrumented objects behind: run "make clean" before the next
# normal build.  This CPU has SSSE3/SSE4.1/PCLMUL but no AVX, so these flags
# cannot hide an illegal instruction here (gf16.c's AVX2 code is only reached
# through the pragma and the cpu_flag guard).
ASAN_FLAGS := $(BASE) -O1 -fsanitize=address -fno-omit-frame-pointer \
              -mpclmul -msse4.1 -mssse3 -msse3
asan:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(ASAN_FLAGS)" \
	        LDLIBS="$(LDLIBS) -fsanitize=address"
	mv -f $(TARGET) par2j_asan

.PHONY: all asan clean
