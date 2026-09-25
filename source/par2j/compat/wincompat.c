/*
 * wincompat.c -- Win32 API implementation for running par2j natively on Linux.
 * See windows.h for an overview of the path model.
 *
 * Handle model: every HANDLE is a pointer to a w32obj descriptor, so NULL and
 * INVALID_HANDLE_VALUE stay distinguishable exactly as on Windows.  Descriptors
 * are reference counted so that closing a handle while another thread waits on
 * it cannot free it out from under the waiter.
 */
#define _GNU_SOURCE 1

#include "windows.h"

#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <locale.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <sys/select.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <wctype.h>

/* ============================================================ error handling */

static __thread DWORD g_last_error = 0;

DWORD GetLastError(void)                 { return g_last_error; }
void  SetLastError(DWORD e)              { g_last_error = e; }
void  w32_set_error(DWORD e)             { g_last_error = e; }

DWORD W32_ErrnoToWin32(int e)
{
	switch (e) {
	case 0:         return ERROR_SUCCESS;
	case ENOENT:    return ERROR_FILE_NOT_FOUND;
	case ENOTDIR:   return ERROR_PATH_NOT_FOUND;
	case EACCES:    return ERROR_ACCESS_DENIED;
	case EPERM:     return ERROR_ACCESS_DENIED;
	case EEXIST:    return ERROR_ALREADY_EXISTS;
	case ENOSPC:    return ERROR_DISK_FULL;
	case EROFS:     return ERROR_WRITE_PROTECT;
	case EINVAL:    return ERROR_INVALID_PARAMETER;
	case ENOMEM:    return ERROR_NOT_ENOUGH_MEMORY;
	case EISDIR:    return ERROR_ACCESS_DENIED;
	case ENODEV:    return ERROR_NOT_READY;
	case EIO:       return ERROR_GEN_FAILURE;
	case EXDEV:     return ERROR_NOT_SAME_DEVICE;
	case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
	case ESPIPE:    return ERROR_SEEK;
	case EFBIG:     return ERROR_DISK_FULL;
	default:        return ERROR_GEN_FAILURE;
	}
}

/*
 * ENOENT is ambiguous: either the file itself is missing (ERROR_FILE_NOT_FOUND)
 * or a parent directory is missing (ERROR_PATH_NOT_FOUND).  Some callers retry
 * after creating directories, so tell them apart.
 */
static DWORD path_error(const char *path)
{
	if (errno == ENOENT) {
		char parent[PATH_MAX];
		char *slash;
		if (snprintf(parent, sizeof(parent), "%s", path) < (int)sizeof(parent)) {
			slash = strrchr(parent, '/');
			if (slash != NULL) {
				if (slash == parent)
					parent[1] = 0;
				else
					*slash = 0;
				if (parent[0] != 0 && access(parent, F_OK) != 0)
					return ERROR_PATH_NOT_FOUND;
			}
		}
		return ERROR_FILE_NOT_FOUND;
	}
	return W32_ErrnoToWin32(errno);
}

/* ============================================================== handle model */

#define W32_MAGIC 0x57334F48u		/* "W3OH" */

enum {
	W32_KIND_FILE = 1,
	W32_KIND_EVENT,
	W32_KIND_THREAD,
	W32_KIND_FIND,
	W32_KIND_MAPPING
};

typedef unsigned (*w32_thread_fn)(void *);

typedef struct w32obj {
	unsigned magic;
	int kind;
	int refs;
	int closed;

	/* FILE */
	int fd;
	char path[PATH_MAX];

	/* FIND */
	DIR *dir;
	char pattern[MAX_PATH];
	int exact;
	WIN32_FIND_DATA find_data;

	/* EVENT */
	int manual_reset;
	int signaled;

	/* THREAD */
	w32_thread_fn fn;
	void *argdata;
	unsigned code;
	pthread_t tid;
	int joined;

	/* MAPPING */
	size_t map_size;

	struct w32obj *next;
} w32obj;

static w32obj *g_objects = NULL;
static pthread_mutex_t g_list_mu = PTHREAD_MUTEX_INITIALIZER;

/* global event/thread signalling state */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv;
static pthread_condattr_t g_cva;
static int g_cv_ready = 0;
static __thread w32obj *g_this_thread = NULL;

static void ensure_cv(void)
{
	static pthread_mutex_t init_mu = PTHREAD_MUTEX_INITIALIZER;
	pthread_mutex_lock(&init_mu);
	if (!g_cv_ready) {
		pthread_condattr_init(&g_cva);
		pthread_condattr_setclock(&g_cva, CLOCK_MONOTONIC);
		pthread_cond_init(&g_cv, &g_cva);
		g_cv_ready = 1;
	}
	pthread_mutex_unlock(&init_mu);
}

static w32obj *handle_alloc(int kind)
{
	w32obj *o = (w32obj *)calloc(1, sizeof(w32obj));
	if (o == NULL)
		return NULL;
	o->magic = W32_MAGIC;
	o->kind = kind;
	o->refs = 1;
	o->fd = -1;
	o->code = STILL_ACTIVE;
	pthread_mutex_lock(&g_list_mu);
	o->next = g_objects;
	g_objects = o;
	pthread_mutex_unlock(&g_list_mu);
	return o;
}

static void handle_destroy(w32obj *o)
{
	pthread_mutex_lock(&g_list_mu);
	{
		w32obj **pp = &g_objects;
		while (*pp != NULL) {
			if (*pp == o) { *pp = o->next; break; }
			pp = &(*pp)->next;
		}
	}
	pthread_mutex_unlock(&g_list_mu);
	o->magic = 0;
	if (((o->kind == W32_KIND_FILE) || (o->kind == W32_KIND_MAPPING)) && o->fd >= 0)
		close(o->fd);
	free(o);
}

static void handle_unref(w32obj *o)
{
	if (o == NULL)
		return;
	if (__atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL) == 0)
		handle_destroy(o);
}

static void handle_ref(w32obj *o)
{
	if (o != NULL)
		__atomic_add_fetch(&o->refs, 1, __ATOMIC_ACQ_REL);
}

static int handle_ok(HANDLE h)
{
	w32obj *o = (w32obj *)h;
	return (o != NULL && o->magic == W32_MAGIC);
}

/* =========================================================== encoding utils */

static int utf8_decode(const unsigned char *s, const unsigned char *end, uint32_t *cp)
{
	unsigned c0;

	if (s >= end)
		return 0;
	c0 = s[0];
	if (c0 < 0x80) { *cp = c0; return 1; }
	if ((c0 & 0xE0) == 0xC0) {
		if (s + 1 >= end || (s[1] & 0xC0) != 0x80) return 0;
		*cp = ((uint32_t)(c0 & 0x1F) << 6) | (s[1] & 0x3F);
		if (*cp < 0x80) return 0;
		return 2;
	}
	if ((c0 & 0xF0) == 0xE0) {
		if (s + 2 >= end || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;
		*cp = ((uint32_t)(c0 & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
		if (*cp < 0x800 || (*cp >= 0xD800 && *cp <= 0xDFFF)) return 0;
		return 3;
	}
	if ((c0 & 0xF8) == 0xF0) {
		if (s + 3 >= end || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 ||
		    (s[3] & 0xC0) != 0x80) return 0;
		*cp = ((uint32_t)(c0 & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
		      ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
		if (*cp < 0x10000 || *cp > 0x10FFFF) return 0;
		return 4;
	}
	return 0;
}

/* helpers used by the INI code below */
static void wide_to_utf8_len(const wchar_t *in, char *out, size_t outlen, int inlen)
{
	size_t o = 0;
	if (outlen == 0) return;
	while (in != NULL && *in != 0 && o + 4 < outlen) {
		if (inlen == 0) break;		/* character budget exhausted */
		if (inlen > 0) inlen--;
		uint32_t c = (uint32_t)*in++;
		if (c < 0x80) {
			out[o++] = (char)c;
		} else if (c < 0x800) {
			out[o++] = (char)(0xC0 | (c >> 6));
			out[o++] = (char)(0x80 | (c & 0x3F));
		} else if (c < 0x10000) {
			out[o++] = (char)(0xE0 | (c >> 12));
			out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
			out[o++] = (char)(0x80 | (c & 0x3F));
		} else {
			out[o++] = (char)(0xF0 | (c >> 18));
			out[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
			out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
			out[o++] = (char)(0x80 | (c & 0x3F));
		}
	}
	out[o] = 0;
}

static void wide_to_utf8(const wchar_t *in, char *out, size_t outlen)
{
	wide_to_utf8_len(in, out, outlen, -1);
}

static void utf8_to_wide(const char *in, wchar_t *out, size_t outlen)
{
	size_t o = 0;
	const unsigned char *p = (const unsigned char *)in;
	const unsigned char *end = p + strlen(in);
	if (outlen == 0) return;
	while (p < end && o + 1 < outlen) {
		uint32_t c = 0;
		int n = utf8_decode(p, end, &c);
		if (n == 0) { c = 0xFFFD; n = 1; }
		p += n;
		out[o++] = (wchar_t)c;
	}
	out[o] = 0;
}

/*
 * The PAR2 packets store filenames and comments as UTF-16LE: two bytes per
 * code unit, with surrogate pairs for anything above U+FFFF.  On Windows
 * wchar_t *is* UTF-16, so upstream simply memcpy()s the wide string and uses
 * "wcslen(name) * 2" as the byte count.  wchar_t is 4 bytes wide here, so the
 * bytes have to be produced explicitly -- a raw copy would store the wrong
 * encoding (and only half of the name, since the size is in bytes).
 */

/* Encode src as UTF-16LE and return its size in bytes, excluding the
 * terminator.  dst may be NULL to only measure. */
size_t utf16le_from_wcs(const wchar_t *src, unsigned char *dst)
{
	size_t n = 0;

	if (src == NULL)
		return 0;
	for (; *src != 0; src++) {
		uint32_t cp = (uint32_t)*src;
		if (cp > 0xFFFF) {	/* surrogate pair */
			uint32_t v;
			if (cp > 0x10FFFF)
				cp = 0xFFFD;	/* not representable */
			v = cp - 0x10000;
			if (dst != NULL) {
				dst[n    ] = (unsigned char)((0xD800 + (v >> 10)) & 0xFF);
				dst[n + 1] = (unsigned char)((0xD800 + (v >> 10)) >> 8);
				dst[n + 2] = (unsigned char)((0xDC00 + (v & 0x3FF)) & 0xFF);
				dst[n + 3] = (unsigned char)((0xDC00 + (v & 0x3FF)) >> 8);
			}
			n += 4;
		} else {
			if (dst != NULL) {
				dst[n    ] = (unsigned char)(cp & 0xFF);
				dst[n + 1] = (unsigned char)((cp >> 8) & 0xFF);
			}
			n += 2;
		}
	}
	return n;
}

/* Decode srclen bytes of UTF-16LE into a wchar_t string, NUL terminated and
 * never longer than dstlen - 1 code points.  A lone surrogate becomes U+FFFD.
 * Returns the number of code points written. */
int utf16le_to_wcs(const unsigned char *src, size_t srclen, wchar_t *dst, size_t dstlen)
{
	size_t i = 0, n = 0;

	if (dst == NULL || dstlen == 0)
		return 0;
	while (i + 1 < srclen && n + 1 < dstlen) {
		uint32_t u = (uint32_t)src[i] | ((uint32_t)src[i + 1] << 8);
		i += 2;
		if (u == 0)
			break;	/* terminator */
		if (u >= 0xD800 && u < 0xDC00 && i + 1 < srclen) {
			uint32_t lo = (uint32_t)src[i] | ((uint32_t)src[i + 1] << 8);
			if (lo >= 0xDC00 && lo < 0xE000) {
				i += 2;
				dst[n++] = (wchar_t)(0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00));
				continue;
			}
			u = 0xFFFD;	/* lone high surrogate */
		} else if (u >= 0xDC00 && u < 0xE000) {
			u = 0xFFFD;	/* lone low surrogate */
		}
		dst[n++] = (wchar_t)u;
	}
	dst[n] = 0;
	return (int)n;
}

/* True when s is well formed, NUL terminated UTF-8. */
static int utf8_is_valid(const char *s)
{
	const unsigned char *p = (const unsigned char *)s;
	const unsigned char *end = p + strlen(s);
	while (p < end) {
		uint32_t c;
		int n = utf8_decode(p, end, &c);
		if (n == 0)
			return 0;
		p += n;
	}
	return 1;
}

/* ============================================================== path model */

/*
 * Path debugging.  Set PAR2J_TRACE=1 to have every path lookup print what it
 * asked for and what it got; this box has no debugger, so that trace is the
 * main way to see why a lookup failed.
 *
 * w32_trace() is cheap after the first call, and w32_trace_str() writes into a
 * caller supplied buffer so output stays readable when threads interleave.
 */
static int w32_trace(void)
{
	static int on = -1;
	if (on < 0)
		on = (getenv("PAR2J_TRACE") != NULL);
	return on;
}

static void w32_trace_str(const wchar_t *w, char *buf, size_t buflen)
{
	size_t q = 0;

	if (buflen == 0) return;
	if (w == NULL) { snprintf(buf, buflen, "(null)"); return; }
	while (*w != 0 && q + 4 < buflen) {
		wchar_t c = *w++;
		if (c == L'\\') c = '/';
		if (c < 0x80) buf[q++] = (char)c;
		else if (c < 0x800) {
			buf[q++] = (char)(0xC0 | (c >> 6));
			buf[q++] = (char)(0x80 | (c & 0x3F));
		} else {
			buf[q++] = (char)(0xE0 | (c >> 12));
			buf[q++] = (char)(0x80 | ((c >> 6) & 0x3F));
			buf[q++] = (char)(0x80 | (c & 0x3F));
		}
	}
	buf[q] = 0;
}

/*
 * Windows style path -> POSIX path.
 *   "\\?\C:\dir\file" -> "/dir/file"
 *   "C:\dir\file"     -> "/dir/file"
 *   "dir\file"        -> "dir/file"    (relative, left alone)
 * The virtual drive "C:" is the POSIX root directory.
 */
void w32_path_to_posix(LPCWSTR wpath, char *out, size_t outlen)
{
	size_t i = 0, o = 0;
	int rooted = 0;

	if (outlen == 0) return;
	out[0] = 0;
	if (wpath == NULL) return;

	if (wpath[0] == L'\\' && wpath[1] == L'\\' && wpath[2] == L'?' && wpath[3] == L'\\')
		i += 4;

	if (wpath[i] != 0 && wpath[i + 1] == L':' &&
	    ((wpath[i] >= L'A' && wpath[i] <= L'Z') || (wpath[i] >= L'a' && wpath[i] <= L'z'))) {
		i += 2;
		if (wpath[i] == L'\\' || wpath[i] == L'/')
			rooted = 1;
	}
	if (wpath[i] == L'\\' || wpath[i] == L'/')
		rooted = 1;

	if (rooted && o + 1 < outlen)
		out[o++] = '/';

	for (; wpath[i] != 0 && o + 1 < outlen; i++) {
		wchar_t c = wpath[i];
		if (c == L'\\') c = L'/';
		if (c == L'/' && o > 0 && out[o - 1] == L'/')
			continue;
		if (c < 0x80) {
			out[o++] = (char)c;
		} else if (c < 0x800) {
			out[o++] = (char)(0xC0 | (c >> 6));
			if (o + 1 < outlen) out[o++] = (char)(0x80 | (c & 0x3F));
		} else if (c >= 0xD800 && c < 0xE000) {
			out[o++] = '?';	/* lone surrogate */
		} else if (c < 0x10000) {
			out[o++] = (char)(0xE0 | (c >> 12));
			if (o + 2 < outlen) {
				out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
				out[o++] = (char)(0x80 | (c & 0x3F));
			}
		} else if (o + 3 < outlen) {
			out[o++] = (char)(0xF0 | (c >> 18));
			out[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
			out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
			out[o++] = (char)(0x80 | (c & 0x3F));
		}
	}
	out[o] = 0;
}

/* POSIX path -> Windows style path (used for GetModuleFileName). */
void w32_path_to_win(const char *in, wchar_t *out, size_t outlen)
{
	size_t i = 0, o = 0;

	if (outlen == 0) return;
	out[0] = 0;
	if (in == NULL) return;
	if (in[0] == '/') {
		if (outlen < 4) return;
		out[o++] = L'C';
		out[o++] = L':';
	}
	while (in[i] != 0 && o + 1 < outlen) {
		unsigned char c = (unsigned char)in[i];
		if (c == '/') {
			out[o++] = L'\\';
			i++;
		} else if (c < 0x80) {
			out[o++] = (wchar_t)c;
			i++;
		} else {
			uint32_t cp = 0;
			int n = utf8_decode((const unsigned char *)in + i,
			                    (const unsigned char *)in + strlen(in), &cp);
			if (n == 0) { cp = 0xFFFD; n = 1; }
			i += (size_t)n;
			out[o++] = (wchar_t)cp;
		}
	}
	out[o] = 0;
}

/* Lexically normalise a POSIX path: collapse "//", resolve "." and "..". */
static void normalize_posix(char *p)
{
	char work[PATH_MAX];
	char out[PATH_MAX];
	size_t i = 0, w = 0;
	int rooted;
	int trailing;

	if (p[0] == 0) return;
	if (snprintf(work, sizeof(work), "%s", p) >= (int)sizeof(work)) return;
	rooted = (work[0] == '/');
	trailing = (strlen(work) > 1 && work[strlen(work) - 1] == '/');

	out[0] = 0;
	if (rooted) out[w++] = '/';	/* keep the leading slash! */
	while (work[i] != 0) {
		size_t start;
		while (work[i] == '/') i++;
		if (work[i] == 0) break;
		start = i;
		while (work[i] != 0 && work[i] != '/') i++;
		{
			char saved = work[i];
			work[i] = 0;
			if (strcmp(work + start, ".") == 0) {
				/* skip */
			} else if (strcmp(work + start, "..") == 0) {
				while (w > 0 && out[w - 1] != '/') w--;
				if (w > 0) w--;	/* drop the separator */
				if (rooted && w == 0) out[w++] = '/';
			} else {
				if (w > 0 && out[w - 1] != '/') out[w++] = '/';
				if (w + strlen(work + start) + 2 >= sizeof(out)) { work[i] = saved; break; }
				memcpy(out + w, work + start, strlen(work + start));
				w += strlen(work + start);
			}
			work[i] = saved;
		}
	}
	if (rooted && w == 0) out[w++] = '/';
	if (trailing && w > 0 && out[w - 1] != '/' && w + 1 < sizeof(out)) out[w++] = '/';
	out[w] = 0;
	memcpy(p, out, w + 1);
	if (p[0] == 0) { p[0] = '/'; p[1] = 0; }
}

/* Convert an input path (possibly relative, possibly Windows style) into an
 * absolute, normalised POSIX path. */
static void resolve_posix(LPCWSTR name, char *out, size_t outlen)
{
	char rel[PATH_MAX];
	w32_path_to_posix(name, rel, sizeof(rel));
	if (rel[0] != '/') {
		char cwd[PATH_MAX];
		char joined[PATH_MAX * 2];
		if (getcwd(cwd, sizeof(cwd)) == NULL)
			strcpy(cwd, ".");
		if (rel[0] == 0)
			snprintf(joined, sizeof(joined), "%s", cwd);
		else
			snprintf(joined, sizeof(joined), "%s/%s", cwd, rel);
		snprintf(out, outlen, "%s", joined);
	} else {
		snprintf(out, outlen, "%s", rel);
	}
	normalize_posix(out);
}

/* Add the "\\?\" prefix back when the caller supplied one. */
static void restore_prefix(int had_prefix, wchar_t *buf)
{
	wchar_t tmp[PATH_MAX];
	if (!had_prefix) return;
	if (wcslen(buf) + 5 >= PATH_MAX) return;
	wcscpy(tmp, L"\\\\?\\");
	wcscat(tmp, buf);
	wcscpy(buf, tmp);
}

/* ===================================================== GetFullPathName etc. */

DWORD GetFullPathNameW_(LPCWSTR name, DWORD nBufferLength, LPWSTR lpBuffer, LPWSTR *lpFilePart)
{
	char abs[PATH_MAX];
	wchar_t wbuf[PATH_MAX];
	int had_prefix;
	size_t len;
	if (w32_trace()) { char d[1024]; w32_trace_str(name, d, sizeof(d)); fprintf(stderr, "GFPN in=[%s]\n", d); }

	if (name == NULL || lpBuffer == NULL || nBufferLength == 0) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	had_prefix = (wcsncmp(name, L"\\\\?\\", 4) == 0);
	resolve_posix(name, abs, sizeof(abs));
	w32_path_to_win(abs, wbuf, PATH_MAX);
	restore_prefix(had_prefix, wbuf);
	len = wcslen(wbuf);

	if (w32_trace()) { char d[1024]; w32_trace_str(wbuf, d, sizeof(d)); fprintf(stderr, "GFPN out=[%s] abs=[%s]\n", d, abs); }
	if (len + 1 > nBufferLength)
		return (DWORD)(len + 1);
	wcscpy(lpBuffer, wbuf);
	/* lpFilePart must point into the caller's buffer: the program keeps using
	 * it as a write position (copy_path_prefix restores cleaned names). */
	if (lpFilePart != NULL) {
		wchar_t *slash = wcsrchr(lpBuffer, L'\\');
		*lpFilePart = (slash != NULL && *(slash + 1) != 0) ? slash + 1 : NULL;
	}
	return (DWORD)len;
}

DWORD GetLongPathNameW_(LPCWSTR lpszShortPath, LPWSTR lpszLongPath, DWORD cchBuffer)
{
	char abs[PATH_MAX];
	wchar_t wbuf[PATH_MAX];
	int had_prefix;
	size_t len;

	if (lpszShortPath == NULL || lpszLongPath == NULL || cchBuffer == 0) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	had_prefix = (wcsncmp(lpszShortPath, L"\\\\?\\", 4) == 0);
	resolve_posix(lpszShortPath, abs, sizeof(abs));
	w32_path_to_win(abs, wbuf, PATH_MAX);
	restore_prefix(had_prefix, wbuf);
	len = wcslen(wbuf);
	if (len + 1 > cchBuffer) {
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return (DWORD)(len + 1);
	}
	if (w32_trace()) {
		char d1[1024], d2[1024];
		w32_trace_str(lpszShortPath, d1, sizeof(d1));
		w32_trace_str(wbuf, d2, sizeof(d2));
		fprintf(stderr, "GLPN in=[%s] out=[%s]\n", d1, d2);
	}
	wcscpy(lpszLongPath, wbuf);	/* safe: wbuf is a private copy */
	return (DWORD)len;
}

DWORD GetShortPathNameW_(LPCWSTR src, LPWSTR dst, DWORD len)
{
	size_t n = wcslen(src);
	if (dst == NULL || len == 0) return 0;
	if (n + 1 > len) return (DWORD)(n + 1);
	wcscpy(dst, src);
	return (DWORD)n;
}

DWORD GetModuleFileNameW_(HMODULE mod, LPWSTR buf, DWORD len)
{
	char exe[PATH_MAX];
	wchar_t wtmp[PATH_MAX];
	ssize_t n;

	(void)mod;
	n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (n <= 0) {
		if (getcwd(exe, sizeof(exe)) == NULL)
			return 0;
	} else {
		exe[n] = 0;
	}
	w32_path_to_win(exe, wtmp, PATH_MAX);
	if (wcslen(wtmp) + 1 > len) {
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return len;
	}
	wcscpy(buf, wtmp);
	return (DWORD)wcslen(wtmp);
}

/* ====================================================== attributes / stat */

static void unix_time_to_filetime(time_t sec, FILETIME *ft)
{
	ULONGLONG v = ((ULONGLONG)sec + 11644473600ULL) * 10000000ULL;
	ft->dwLowDateTime  = (DWORD)(v & 0xFFFFFFFFULL);
	ft->dwHighDateTime = (DWORD)(v >> 32);
}

static DWORD attrs_from_stat(const struct stat *st, const char *name)
{
	DWORD a;
	if (S_ISDIR(st->st_mode))
		a = FILE_ATTRIBUTE_DIRECTORY;
	else
		a = FILE_ATTRIBUTE_NORMAL;
	if (name != NULL && name[0] == '.')
		a |= FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
	if ((st->st_mode & S_IWUSR) == 0)
		a |= FILE_ATTRIBUTE_READONLY;
	return a;
}

static const char *base_name(const char *p)
{
	const char *s = strrchr(p, '/');
	return (s != NULL) ? s + 1 : p;
}

DWORD GetFileAttributesW_(LPCWSTR path)
{
	char p[PATH_MAX];
	struct stat st;
	DWORD r;

	w32_path_to_posix(path, p, sizeof(p));
	if (p[0] == 0) { SetLastError(ERROR_INVALID_NAME); return INVALID_FILE_ATTRIBUTES; }
	r = (lstat(p, &st) != 0) ? INVALID_FILE_ATTRIBUTES : attrs_from_stat(&st, base_name(p));
	if (w32_trace()) {
		char tb[1024];
		w32_trace_str(path, tb, sizeof(tb));
		fprintf(stderr, "GFA [%s] -> %s (posix=[%s])\n", tb,
		        (r == INVALID_FILE_ATTRIBUTES) ? "INVALID" : "ok", p);
	}
	if (r == INVALID_FILE_ATTRIBUTES) { SetLastError(path_error(p)); return INVALID_FILE_ATTRIBUTES; }
	return r;
}

BOOL GetFileAttributesExW_(LPCWSTR path, int infoLevel, LPVOID lpFileInformation)
{
	char p[PATH_MAX];
	struct stat st;
	WIN32_FILE_ATTRIBUTE_DATA *d = (WIN32_FILE_ATTRIBUTE_DATA *)lpFileInformation;

	(void)infoLevel;
	if (d == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
	w32_path_to_posix(path, p, sizeof(p));
	if (p[0] == 0 || stat(p, &st) != 0) { SetLastError(path_error(p)); return FALSE; }
	memset(d, 0, sizeof(*d));
	d->dwFileAttributes = attrs_from_stat(&st, base_name(p));
	d->nFileSizeHigh = (DWORD)(((uint64_t)st.st_size) >> 32);
	d->nFileSizeLow  = (DWORD)((uint64_t)st.st_size & 0xFFFFFFFFULL);
	unix_time_to_filetime(st.st_ctime, &d->ftCreationTime);
	unix_time_to_filetime(st.st_atime, &d->ftLastAccessTime);
	unix_time_to_filetime(st.st_mtime, &d->ftLastWriteTime);
	return TRUE;
}

BOOL SetFileAttributesW_(LPCWSTR path, DWORD attr)
{
	char p[PATH_MAX];
	struct stat st;

	w32_path_to_posix(path, p, sizeof(p));
	if (p[0] == 0 || stat(p, &st) != 0) { SetLastError(path_error(p)); return FALSE; }
	if (attr & FILE_ATTRIBUTE_READONLY)
		st.st_mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
	else
		st.st_mode |= S_IWUSR;
	if (chmod(p, st.st_mode & 07777) != 0) {
		SetLastError(W32_ErrnoToWin32(errno));
		return FALSE;
	}
	return TRUE;
}

DWORD GetDriveTypeW_(LPCWSTR root) { (void)root; return DRIVE_FIXED; }

BOOL GetVolumeInformationW_(LPCWSTR root, LPWSTR vol, DWORD vollen, LPDWORD serial,
                            LPDWORD maxcomp, LPDWORD flags, LPWSTR fs, DWORD fslen)
{
	(void)root;
	if (vol != NULL && vollen > 0) vol[0] = 0;
	if (serial != NULL) *serial = 0;
	if (maxcomp != NULL) *maxcomp = 255;
	if (flags != NULL)
		*flags = FILE_SUPPORTS_SPARSE_FILES | FILE_SUPPORTS_REPARSE_POINTS;
	if (fs != NULL && fslen > 0) {
		wcsncpy(fs, L"ext4", fslen);
		fs[fslen - 1] = 0;
	}
	return TRUE;
}

/* ================================================================ file IO */

HANDLE CreateFileW_(LPCWSTR name, DWORD acctype, DWORD share,
                    LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE tmpl)
{
	char p[PATH_MAX];
	int oflags = 0;
	int fd;
	int existed = 0;
	w32obj *o;

	(void)sa; (void)share; (void)tmpl; (void)flags;

	w32_path_to_posix(name, p, sizeof(p));
	if (p[0] == 0) { SetLastError(ERROR_INVALID_NAME); return INVALID_HANDLE_VALUE; }

	/* write access needs O_RDWR: several callers use the same handle for
	 * reads, mapping and SetEndOfFile */
	oflags = (acctype & GENERIC_WRITE) ? O_RDWR : O_RDONLY;

	existed = (access(p, F_OK) == 0);

	switch (disposition) {
	case CREATE_NEW:        oflags |= O_CREAT | O_EXCL; break;
	case CREATE_ALWAYS:     oflags |= O_CREAT | O_TRUNC; break;
	case OPEN_EXISTING:     break;
	case OPEN_ALWAYS:       oflags |= O_CREAT; break;
	case TRUNCATE_EXISTING: oflags |= O_TRUNC; break;
	default: break;
	}

	fd = open(p, oflags, 0666);
	if (fd < 0) { SetLastError(path_error(p)); return INVALID_HANDLE_VALUE; }

	if ((disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS) && existed)
		SetLastError(ERROR_ALREADY_EXISTS);

	o = handle_alloc(W32_KIND_FILE);
	if (o == NULL) {
		close(fd);
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return INVALID_HANDLE_VALUE;
	}
	o->fd = fd;
	snprintf(o->path, sizeof(o->path), "%s", p);
	return (HANDLE)o;
}

BOOL ReadFile(HANDLE hh, LPVOID buf, DWORD n, LPDWORD lpRead, LPOVERLAPPED lpov)
{
	w32obj *o = (w32obj *)hh;
	DWORD got = 0;

	if (!handle_ok(hh)) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }

	if (lpov != NULL) {
		/*
		 * Synchronous emulation of overlapped IO.  The program always waits
		 * on ol->hEvent before reusing the OVERLAPPED structure or its
		 * buffer, so completing inline is observationally identical to real
		 * asynchronous IO -- only the pipelining is lost.
		 */
		off_t off = (off_t)(((uint64_t)lpov->OffsetHigh << 32) | (uint64_t)lpov->Offset);
		char *p = (char *)buf;
		DWORD left = n;
		while (left > 0) {
			ssize_t r = pread(o->fd, p, left, off);
			if (r < 0) {
				if (errno == EINTR) continue;
				SetLastError(W32_ErrnoToWin32(errno));
				if (lpRead) *lpRead = 0;
				return FALSE;
			}
			if (r == 0) break;
			p += r; off += r; left -= (DWORD)r; got += (DWORD)r;
		}
		lpov->InternalHigh = got;
		if (lpov->hEvent != NULL)
			SetEvent(lpov->hEvent);
		if (lpRead) *lpRead = got;
		return TRUE;
	}

	while (got < n) {
		ssize_t r = read(o->fd, (char *)buf + got, n - got);
		if (r < 0) {
			if (errno == EINTR) continue;
			SetLastError(W32_ErrnoToWin32(errno));
			if (lpRead) *lpRead = 0;
			return FALSE;
		}
		if (r == 0) break;
		got += (DWORD)r;
	}
	if (lpRead) *lpRead = got;
	return TRUE;
}

BOOL WriteFile(HANDLE hh, LPCVOID buf, DWORD n, LPDWORD lpWritten, LPOVERLAPPED lpov)
{
	w32obj *o = (w32obj *)hh;
	DWORD put = 0;
	off_t base = 0;
	int positioned = 0;

	if (!handle_ok(hh)) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }

	if (lpov != NULL) {
		base = (off_t)(((uint64_t)lpov->OffsetHigh << 32) | (uint64_t)lpov->Offset);
		positioned = 1;
	}
	while (put < n) {
		ssize_t w;
		if (positioned)
			w = pwrite(o->fd, (const char *)buf + put, n - put, base + put);
		else
			w = write(o->fd, (const char *)buf + put, n - put);
		if (w < 0) {
			if (errno == EINTR) continue;
			SetLastError(W32_ErrnoToWin32(errno));
			if (lpWritten) *lpWritten = 0;
			return FALSE;
		}
		put += (DWORD)w;
	}
	if (lpov != NULL) {
		lpov->InternalHigh = put;
		if (lpov->hEvent != NULL)
			SetEvent(lpov->hEvent);
	}
	if (lpWritten) *lpWritten = put;
	return TRUE;
}

BOOL FlushFileBuffers(HANDLE hh)
{
	w32obj *o = (w32obj *)hh;
	if (!handle_ok(hh)) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
	if (o->kind == W32_KIND_FILE && o->fd >= 0 && fsync(o->fd) != 0) {
		SetLastError(W32_ErrnoToWin32(errno));
		return FALSE;
	}
	return TRUE;
}

BOOL SetFilePointerEx(HANDLE hh, LARGE_INTEGER dist, PLARGE_INTEGER lpNew, DWORD method)
{
	w32obj *o = (w32obj *)hh;
	off_t base, res;

	if (!handle_ok(hh) || o->kind != W32_KIND_FILE) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	switch (method) {
	case FILE_BEGIN: base = 0; break;
	case FILE_CURRENT:
		base = lseek(o->fd, 0, SEEK_CUR);
		if (base == (off_t)-1) { SetLastError(W32_ErrnoToWin32(errno)); return FALSE; }
		break;
	case FILE_END: {
		struct stat st;
		if (fstat(o->fd, &st) != 0) { SetLastError(W32_ErrnoToWin32(errno)); return FALSE; }
		base = (off_t)st.st_size;
		break;
	}
	default: SetLastError(ERROR_INVALID_PARAMETER); return FALSE;
	}
	res = base + (off_t)dist.QuadPart;
	if (res < 0) { SetLastError(ERROR_NEGATIVE_SEEK); return FALSE; }
	if (lseek(o->fd, res, SEEK_SET) == (off_t)-1) {
		SetLastError(W32_ErrnoToWin32(errno));
		return FALSE;
	}
	if (lpNew != NULL) lpNew->QuadPart = (LONGLONG)res;
	return TRUE;
}

DWORD SetFilePointer(HANDLE hh, LONG lDistanceToMove, PLONG lpDistanceToMoveHigh, DWORD method)
{
	w32obj *o = (w32obj *)hh;
	LARGE_INTEGER dist, res;

	if (!handle_ok(hh) || o->kind != W32_KIND_FILE) {
		SetLastError(ERROR_INVALID_HANDLE);
		return INVALID_FILE_SIZE;
	}
	dist.QuadPart = lDistanceToMove;
	if (lpDistanceToMoveHigh != NULL)
		dist.QuadPart |= ((LONGLONG)*lpDistanceToMoveHigh) << 32;
	if (!SetFilePointerEx(hh, dist, &res, method))
		return INVALID_FILE_SIZE;
	if (lpDistanceToMoveHigh != NULL)
		*lpDistanceToMoveHigh = (LONG)(res.QuadPart >> 32);
	return (DWORD)(res.QuadPart & 0xFFFFFFFFLL);
}

BOOL SetEndOfFile(HANDLE hh)
{
	w32obj *o = (w32obj *)hh;
	off_t pos;

	if (!handle_ok(hh) || o->kind != W32_KIND_FILE) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	pos = lseek(o->fd, 0, SEEK_CUR);
	if (pos == (off_t)-1) { SetLastError(W32_ErrnoToWin32(errno)); return FALSE; }
	if (ftruncate(o->fd, pos) != 0) { SetLastError(W32_ErrnoToWin32(errno)); return FALSE; }
	return TRUE;
}

BOOL SetFileValidData(HANDLE hh, LONGLONG size)
{
	/* Windows needs SeManageVolumePrivilege for this pre-allocation; it is
	 * purely an optimisation because every write path extends the file. */
	(void)hh; (void)size;
	return TRUE;
}

DWORD GetFileSize(HANDLE hh, LPDWORD lpHigh)
{
	w32obj *o = (w32obj *)hh;
	struct stat st;

	if (!handle_ok(hh) || o->kind != W32_KIND_FILE || fstat(o->fd, &st) != 0) {
		SetLastError(ERROR_INVALID_HANDLE);
		return INVALID_FILE_SIZE;
	}
	if (lpHigh) *lpHigh = (DWORD)(((uint64_t)st.st_size) >> 32);
	return (DWORD)((uint64_t)st.st_size & 0xFFFFFFFFULL);
}

BOOL GetFileSizeEx(HANDLE hh, LARGE_INTEGER *size)
{
	w32obj *o = (w32obj *)hh;
	struct stat st;

	if (!handle_ok(hh) || o->kind != W32_KIND_FILE || size == NULL) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (fstat(o->fd, &st) != 0) { SetLastError(W32_ErrnoToWin32(errno)); return FALSE; }
	size->QuadPart = (LONGLONG)st.st_size;
	return TRUE;
}

BOOL GetFileInformationByHandle(HANDLE hh, LPBY_HANDLE_FILE_INFORMATION info)
{
	w32obj *o = (w32obj *)hh;
	struct stat st;

	if (!handle_ok(hh) || o->kind != W32_KIND_FILE || info == NULL) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (fstat(o->fd, &st) != 0) { SetLastError(W32_ErrnoToWin32(errno)); return FALSE; }
	memset(info, 0, sizeof(*info));
	info->dwFileAttributes = attrs_from_stat(&st, base_name(o->path));
	info->nFileSizeHigh = (DWORD)(((uint64_t)st.st_size) >> 32);
	info->nFileSizeLow  = (DWORD)((uint64_t)st.st_size & 0xFFFFFFFFULL);
	info->nNumberOfLinks = (DWORD)st.st_nlink;
	info->nFileIndexHigh = (DWORD)(((uint64_t)st.st_ino) >> 32);
	info->nFileIndexLow  = (DWORD)((uint64_t)st.st_ino & 0xFFFFFFFFULL);
	info->dwVolumeSerialNumber = (DWORD)((uint64_t)st.st_dev ^
		(((uint64_t)st.st_dev >> 32) ^ 0x9E3779B9u));
	unix_time_to_filetime(st.st_ctime, &info->ftCreationTime);
	unix_time_to_filetime(st.st_atime, &info->ftLastAccessTime);
	unix_time_to_filetime(st.st_mtime, &info->ftLastWriteTime);
	return TRUE;
}

BOOL GetOverlappedResult(HANDLE hh, LPOVERLAPPED lpov, LPDWORD lpTransferred, BOOL bWait)
{
	(void)hh; (void)bWait;
	if (lpov == NULL || lpTransferred == NULL) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*lpTransferred = (DWORD)lpov->InternalHigh;
	return TRUE;
}

BOOL CancelIo(HANDLE hh)    { (void)hh; return TRUE; }
BOOL CancelIoEx(HANDLE hh, LPOVERLAPPED ol) { (void)hh; (void)ol; return TRUE; }

/* ------------------------------------------------- file mapping (PE checksum) */

typedef struct { void *ptr; size_t len; } w32_map;
static w32_map g_maps[64];
static int g_map_count = 0;

HANDLE CreateFileMappingW_(HANDLE hh, LPSECURITY_ATTRIBUTES sa, DWORD protect,
                           DWORD maxhigh, DWORD maxlow, LPCWSTR name)
{
	w32obj *o = (w32obj *)hh;
	w32obj *m;

	(void)sa; (void)protect; (void)maxhigh; (void)maxlow; (void)name;
	if (!handle_ok(hh) || o->kind != W32_KIND_FILE) {
		SetLastError(ERROR_INVALID_HANDLE);
		return NULL;
	}
	m = handle_alloc(W32_KIND_MAPPING);
	if (m == NULL) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
	/* MapViewOfFile needs a live descriptor to mmap() through, and the file
	 * object may well be closed first, so take a private dup for the mapping. */
	m->fd = dup(o->fd);
	if (m->fd < 0) {
		handle_unref(m);
		SetLastError(W32_ErrnoToWin32(errno));
		return NULL;
	}
	{
		struct stat st;
		if (fstat(m->fd, &st) != 0) m->map_size = 0;
		else m->map_size = (size_t)st.st_size;
	}
	return (HANDLE)m;
}

LPVOID MapViewOfFile(HANDLE hm, DWORD access, DWORD offhigh, DWORD offlow, size_t nbytes)
{
	w32obj *m = (w32obj *)hm;
	int prot;
	void *p;
	size_t len;
	uint64_t off;

	if (!handle_ok(hm) || m->kind != W32_KIND_MAPPING) {
		SetLastError(ERROR_INVALID_HANDLE);
		return NULL;
	}
	prot = PROT_READ;
	if (access & FILE_MAP_WRITE) prot = PROT_READ | PROT_WRITE;
	off = ((uint64_t)offhigh << 32) | (uint64_t)offlow;
	len = (nbytes != 0) ? nbytes : (m->map_size > off ? (size_t)(m->map_size - off) : 0);
	if (len == 0) return NULL;

	p = mmap(NULL, len, prot, MAP_PRIVATE, m->fd, (off_t)off);
	if (p == MAP_FAILED) { SetLastError(W32_ErrnoToWin32(errno)); return NULL; }

	pthread_mutex_lock(&g_list_mu);
	if (g_map_count < (int)(sizeof(g_maps) / sizeof(g_maps[0]))) {
		g_maps[g_map_count].ptr = p;
		g_maps[g_map_count].len = len;
		g_map_count++;
	}
	pthread_mutex_unlock(&g_list_mu);
	return p;
}

BOOL UnmapViewOfFile(LPCVOID p)
{
	size_t len = 0;
	int i, found = -1;

	pthread_mutex_lock(&g_list_mu);
	for (i = 0; i < g_map_count; i++) {
		if (g_maps[i].ptr == p) {
			len = g_maps[i].len;
			found = i;
			break;
		}
	}
	if (found >= 0) {
		g_maps[found] = g_maps[g_map_count - 1];
		g_map_count--;
	}
	pthread_mutex_unlock(&g_list_mu);
	if (len != 0)
		return munmap((void *)p, len) == 0 ? TRUE : FALSE;
	return TRUE;
}

BOOL DeviceIoControl(HANDLE hh, DWORD code, LPVOID in, DWORD inlen,
                     LPVOID out, DWORD outlen, LPDWORD lpRet, LPOVERLAPPED ol)
{
	w32obj *o = (w32obj *)hh;

	(void)ol;
	if (!handle_ok(hh)) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
	if (lpRet) *lpRet = 0;

	if (code == FSCTL_SET_SPARSE)
		return TRUE;	/* POSIX files are sparse-capable by default */

	if (code == FSCTL_SET_ZERO_DATA && in != NULL &&
	    inlen >= sizeof(FILE_ZERO_DATA_INFORMATION) && o->kind == W32_KIND_FILE) {
		FILE_ZERO_DATA_INFORMATION *z = (FILE_ZERO_DATA_INFORMATION *)in;
		struct stat st;
		if (fstat(o->fd, &st) == 0) {
			off_t start = (off_t)z->FileOffset.QuadPart;
			off_t end   = (off_t)z->BeyondFinalZero.QuadPart;
			if (start >= 0 && end > start && end <= (off_t)st.st_size) {
#if defined(FALLOC_FL_PUNCH_HOLE)
				if (fallocate(o->fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
				              start, end - start) == 0)
					return TRUE;
#endif
				{
					off_t saved = lseek(o->fd, 0, SEEK_CUR);
					static char zero[65536];
					off_t done = 0;
					if (lseek(o->fd, start, SEEK_SET) != (off_t)-1) {
						while (start + done < end) {
							off_t rem = end - start - done;
							ssize_t chunk = (rem > (off_t)sizeof(zero)) ?
								(ssize_t)sizeof(zero) : (ssize_t)rem;
							ssize_t w = write(o->fd, zero, (size_t)chunk);
							if (w <= 0) break;
							done += w;
						}
						lseek(o->fd, saved, SEEK_SET);
					}
				}
			}
		}
		return TRUE;
	}

	/* IOCTL_STORAGE_QUERY_PROPERTY (SSD detection) and friends: unsupported.
	 * check_seek_penalty() then reports "HDD", which is the safe default. */
	SetLastError(ERROR_INVALID_FUNCTION);
	return FALSE;
}

/* ============================================================ directories */

BOOL CreateDirectoryW_(LPCWSTR path, LPSECURITY_ATTRIBUTES sa)
{
	char p[PATH_MAX];
	(void)sa;
	w32_path_to_posix(path, p, sizeof(p));
	if (p[0] == 0 || mkdir(p, 0777) != 0) {
		SetLastError(W32_ErrnoToWin32(errno));
		return FALSE;
	}
	return TRUE;
}

BOOL RemoveDirectoryW_(LPCWSTR path)
{
	char p[PATH_MAX];
	size_t n;
	w32_path_to_posix(path, p, sizeof(p));
	n = strlen(p);
	while (n > 1 && p[n - 1] == '/') p[--n] = 0;
	if (p[0] == 0 || rmdir(p) != 0) {
		SetLastError(W32_ErrnoToWin32(errno));
		return FALSE;
	}
	return TRUE;
}

BOOL DeleteFileW_(LPCWSTR path)
{
	char p[PATH_MAX];
	struct stat st;
	w32_path_to_posix(path, p, sizeof(p));
	if (p[0] == 0) { SetLastError(ERROR_INVALID_NAME); return FALSE; }
	if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
		if (rmdir(p) != 0) { SetLastError(path_error(p)); return FALSE; }
		return TRUE;
	}
	if (unlink(p) != 0) { SetLastError(path_error(p)); return FALSE; }
	return TRUE;
}

/* Windows refuses to overwrite; move_away_file() relies on ERROR_ALREADY_EXISTS
 * to pick the next free backup number. */
static int posix_rename_no_clobber(const char *a, const char *b)
{
	struct stat st;
	if (stat(b, &st) == 0) {
		SetLastError(ERROR_ALREADY_EXISTS);
		return 0;
	}
	if (rename(a, b) != 0) {
		SetLastError(W32_ErrnoToWin32(errno));
		return 0;
	}
	return 1;
}

BOOL MoveFileW_(LPCWSTR from, LPCWSTR to)
{
	char a[PATH_MAX], b[PATH_MAX];
	w32_path_to_posix(from, a, sizeof(a));
	w32_path_to_posix(to, b, sizeof(b));
	if (a[0] == 0 || b[0] == 0) { SetLastError(ERROR_INVALID_NAME); return FALSE; }
	return posix_rename_no_clobber(a, b);
}

BOOL MoveFileExW_(LPCWSTR from, LPCWSTR to, DWORD flags)
{
	char a[PATH_MAX], b[PATH_MAX];
	w32_path_to_posix(from, a, sizeof(a));
	w32_path_to_posix(to, b, sizeof(b));
	if (a[0] == 0 || b[0] == 0) { SetLastError(ERROR_INVALID_NAME); return FALSE; }
	if (flags & MOVEFILE_REPLACE_EXISTING) {
		if (rename(a, b) != 0) { SetLastError(W32_ErrnoToWin32(errno)); return FALSE; }
		return TRUE;
	}
	return posix_rename_no_clobber(a, b);
}

BOOL ReplaceFileW_(LPCWSTR lpszReplaced, LPCWSTR lpszReplacement, LPCWSTR lpszBackup,
                   DWORD flags, LPVOID a, LPVOID b)
{
	char dst[PATH_MAX], src[PATH_MAX], bak[PATH_MAX];

	(void)flags; (void)a; (void)b;
	w32_path_to_posix(lpszReplaced, dst, sizeof(dst));
	w32_path_to_posix(lpszReplacement, src, sizeof(src));
	if (dst[0] == 0 || src[0] == 0) { SetLastError(ERROR_INVALID_NAME); return FALSE; }

	if (lpszBackup != NULL) {
		w32_path_to_posix(lpszBackup, bak, sizeof(bak));
		if (bak[0] != 0 && rename(dst, bak) != 0 && errno != ENOENT) {
			SetLastError(W32_ErrnoToWin32(errno));
			return FALSE;
		}
	} else if (unlink(dst) != 0 && errno != ENOENT) {
		SetLastError(W32_ErrnoToWin32(errno));
		return FALSE;
	}
	if (rename(src, dst) != 0) {
		SetLastError(W32_ErrnoToWin32(errno));
		return FALSE;
	}
	return TRUE;
}

/* -------------------------------------------------------- directory search */

static void fill_find_data(WIN32_FIND_DATA *d, const char *dir, const char *name)
{
	char full[PATH_MAX];
	struct stat st;

	memset(d, 0, sizeof(*d));
	if (strcmp(dir, ".") != 0)
		snprintf(full, sizeof(full), "%s/%s", dir, name);
	else
		snprintf(full, sizeof(full), "%s", name);

	if (lstat(full, &st) != 0)
		memset(&st, 0, sizeof(st));
	d->dwFileAttributes = attrs_from_stat(&st, name);
	d->nFileSizeHigh = (DWORD)(((uint64_t)st.st_size) >> 32);
	d->nFileSizeLow  = (DWORD)((uint64_t)st.st_size & 0xFFFFFFFFULL);
	unix_time_to_filetime(st.st_ctime, &d->ftCreationTime);
	unix_time_to_filetime(st.st_atime, &d->ftLastAccessTime);
	unix_time_to_filetime(st.st_mtime, &d->ftLastWriteTime);
	utf8_to_wide(name, d->cFileName, MAX_PATH);
}

static int find_advance(w32obj *o)
{
	struct dirent *de;
	if (o->dir == NULL) return 0;
	while ((de = readdir(o->dir)) != NULL) {
		if (fnmatch(o->pattern, de->d_name, 0) == 0) {
			fill_find_data(&o->find_data, o->path, de->d_name);
			return 1;
		}
	}
	return 0;
}

HANDLE FindFirstFileW_(LPCWSTR pattern, WIN32_FIND_DATA *lpFindFileData)
{
	char p[PATH_MAX], dir[PATH_MAX];
	char *slash;
	w32obj *o;

	if (lpFindFileData == NULL) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return INVALID_HANDLE_VALUE;
	}
	w32_path_to_posix(pattern, p, sizeof(p));
	if (w32_trace()) {
		char tb[1024];
		w32_trace_str(pattern, tb, sizeof(tb));
		fprintf(stderr, "FF [%s] -> posix=[%s]\n", tb, p);
	}
	if (p[0] == 0) { SetLastError(ERROR_INVALID_NAME); return INVALID_HANDLE_VALUE; }

	snprintf(dir, sizeof(dir), "%s", p);
	slash = strrchr(dir, '/');
	if (slash != NULL) {
		*slash = 0;
		if (dir[0] == 0) { dir[0] = '/'; dir[1] = 0; }
		snprintf(p, sizeof(p), "%s", slash + 1);	/* pattern part */
	} else {
		snprintf(dir, sizeof(dir), ".");
		/* p already holds the pattern */
	}

	o = handle_alloc(W32_KIND_FIND);
	if (o == NULL) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return INVALID_HANDLE_VALUE; }
	snprintf(o->path, sizeof(o->path), "%s", dir);
	snprintf(o->pattern, sizeof(o->pattern), "%s", p);
	if (w32_trace())
		fprintf(stderr, "FF dir=[%s] pat=[%s]\n", o->path, o->pattern);

	if (strchr(o->pattern, '*') == NULL && strchr(o->pattern, '?') == NULL) {
		char full[PATH_MAX];
		struct stat st;
		if (strcmp(o->path, ".") != 0)
			snprintf(full, sizeof(full), "%s/%s", o->path, o->pattern);
		else
			snprintf(full, sizeof(full), "%s", o->pattern);
		if (lstat(full, &st) != 0) {
			DWORD e = path_error(full);
			handle_unref(o);
			SetLastError(e);
			return INVALID_HANDLE_VALUE;
		}
		o->exact = 1;
		fill_find_data(&o->find_data, o->path, o->pattern);
		*lpFindFileData = o->find_data;
		return (HANDLE)o;
	}

	o->dir = opendir(o->path);
	if (o->dir == NULL) {
		DWORD e = path_error(o->path);
		handle_unref(o);
		SetLastError(e);
		return INVALID_HANDLE_VALUE;
	}
	if (!find_advance(o)) {
		handle_unref(o);
		SetLastError(ERROR_FILE_NOT_FOUND);
		return INVALID_HANDLE_VALUE;
	}
	*lpFindFileData = o->find_data;
	return (HANDLE)o;
}

BOOL FindNextFileW_(HANDLE hh, WIN32_FIND_DATA *lpFindFileData)
{
	w32obj *o = (w32obj *)hh;

	if (!handle_ok(hh) || o->kind != W32_KIND_FIND || lpFindFileData == NULL) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (o->exact || !find_advance(o)) {
		SetLastError(ERROR_NO_MORE_FILES);
		return FALSE;
	}
	*lpFindFileData = o->find_data;
	return TRUE;
}

BOOL FindClose(HANDLE hh)
{
	w32obj *o = (w32obj *)hh;
	if (!handle_ok(hh) || o->kind != W32_KIND_FIND) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (o->dir != NULL) { closedir(o->dir); o->dir = NULL; }
	handle_unref(o);
	return TRUE;
}

/* ======================================================= threads and events */

static void abs_deadline(struct timespec *ts, DWORD ms)
{
	clock_gettime(CLOCK_MONOTONIC, ts);
	ts->tv_sec += (time_t)(ms / 1000);
	ts->tv_nsec += (long)(ms % 1000) * 1000000L;
	while (ts->tv_nsec >= 1000000000L) { ts->tv_nsec -= 1000000000L; ts->tv_sec++; }
}

static int is_signaled(w32obj *o) { return o->signaled; }

/* Consume the signal of an auto-reset event.  Threads are latched: once a
 * thread has terminated its handle stays signaled, as on Windows. */
static void consume_signal(w32obj *o)
{
	if (o->kind == W32_KIND_EVENT && !o->manual_reset)
		o->signaled = 0;
}

static int try_acquire(w32obj *o)
{
	if (!is_signaled(o)) return 0;
	consume_signal(o);
	return 1;
}

HANDLE CreateEventW_(LPSECURITY_ATTRIBUTES sa, BOOL bManualReset, BOOL bInitialState, LPCWSTR name)
{
	w32obj *o;

	(void)sa; (void)name;
	ensure_cv();
	o = handle_alloc(W32_KIND_EVENT);
	if (o == NULL) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
	o->manual_reset = bManualReset ? 1 : 0;
	o->signaled = bInitialState ? 1 : 0;
	return (HANDLE)o;
}

BOOL SetEvent(HANDLE hh)
{
	w32obj *o = (w32obj *)hh;
	if (!handle_ok(hh)) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
	ensure_cv();
	pthread_mutex_lock(&g_mu);
	o->signaled = 1;
	pthread_cond_broadcast(&g_cv);
	pthread_mutex_unlock(&g_mu);
	return TRUE;
}

BOOL ResetEvent(HANDLE hh)
{
	w32obj *o = (w32obj *)hh;
	if (!handle_ok(hh)) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
	ensure_cv();
	pthread_mutex_lock(&g_mu);
	o->signaled = 0;
	pthread_mutex_unlock(&g_mu);
	return TRUE;
}

DWORD WaitForSingleObject(HANDLE hh, DWORD ms)
{
	w32obj *o = (w32obj *)hh;
	struct timespec deadline;
	DWORD ret;

	if (!handle_ok(hh)) { SetLastError(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
	if (o->kind == W32_KIND_FILE) {
		/* waiting on a file handle makes no sense; never block forever */
		SetLastError(ERROR_INVALID_HANDLE);
		return WAIT_FAILED;
	}
	ensure_cv();
	pthread_mutex_lock(&g_mu);
	handle_ref(o);

	if (ms == 0) {
		ret = try_acquire(o) ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
	} else if (ms == INFINITE) {
		while (!try_acquire(o))
			pthread_cond_wait(&g_cv, &g_mu);
		ret = WAIT_OBJECT_0;
	} else {
		abs_deadline(&deadline, ms);
		ret = WAIT_TIMEOUT;
		while (!try_acquire(o)) {
			int rc = pthread_cond_timedwait(&g_cv, &g_mu, &deadline);
			if (rc == ETIMEDOUT) {
				/* a signal may have raced the timeout: check once more */
				if (try_acquire(o)) {
					ret = WAIT_OBJECT_0;
					break;
				}
				ret = WAIT_TIMEOUT;
				break;
			}
			/* rc == 0: woke up, loop re-checks the state */
		}
		if (ret == WAIT_TIMEOUT)
			; /* keep */
		else
			ret = WAIT_OBJECT_0;
	}

	handle_unref(o);
	pthread_mutex_unlock(&g_mu);
	return ret;
}

DWORD WaitForMultipleObjects(DWORD nCount, const HANDLE *lpHandles, BOOL bWaitAll, DWORD ms)
{
	struct timespec deadline;
	DWORD ret = WAIT_TIMEOUT;
	DWORD i;
	int have_deadline = 0;

	if (nCount == 0 || lpHandles == NULL) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return WAIT_FAILED;
	}
	for (i = 0; i < nCount; i++) {
		if (!handle_ok(lpHandles[i])) {
			SetLastError(ERROR_INVALID_HANDLE);
			return WAIT_FAILED;
		}
	}
	ensure_cv();
	pthread_mutex_lock(&g_mu);
	for (i = 0; i < nCount; i++)
		handle_ref((w32obj *)lpHandles[i]);

	if (ms != 0 && ms != INFINITE) {
		abs_deadline(&deadline, ms);
		have_deadline = 1;
	}

	for (;;) {
		if (bWaitAll) {
			int all = 1;
			for (i = 0; i < nCount; i++)
				if (!is_signaled((w32obj *)lpHandles[i])) { all = 0; break; }
			if (all) {
				for (i = 0; i < nCount; i++)
					consume_signal((w32obj *)lpHandles[i]);
				ret = WAIT_OBJECT_0;
				break;
			}
		} else {
			for (i = 0; i < nCount; i++) {
				if (try_acquire((w32obj *)lpHandles[i])) {
					ret = WAIT_OBJECT_0 + i;
					goto done;
				}
			}
		}
		if (ms == 0) { ret = WAIT_TIMEOUT; break; }

		if (ms == INFINITE) {
			pthread_cond_wait(&g_cv, &g_mu);
		} else {
			int rc = pthread_cond_timedwait(&g_cv, &g_mu, &deadline);
			if (rc == ETIMEDOUT) {
				int ready;
				if (bWaitAll) {
					ready = 1;
					for (i = 0; i < nCount; i++)
						if (!is_signaled((w32obj *)lpHandles[i])) { ready = 0; break; }
				} else {
					ready = 0;
					for (i = 0; i < nCount; i++)
						if (is_signaled((w32obj *)lpHandles[i])) { ready = 1; break; }
				}
				if (!ready) { ret = WAIT_TIMEOUT; break; }
			}
		}
		(void)have_deadline;
	}
done:
	for (i = 0; i < nCount; i++)
		handle_unref((w32obj *)lpHandles[i]);	/* one per ref taken above */
	pthread_mutex_unlock(&g_mu);
	return ret;
}

static void *thread_entry(void *arg)
{
	w32obj *o = (w32obj *)arg;
	unsigned code = 0;

	g_this_thread = o;
	if (o->fn != NULL)
		code = o->fn(o->argdata);
	pthread_mutex_lock(&g_mu);
	o->code = code;
	o->signaled = 1;
	pthread_cond_broadcast(&g_cv);
	pthread_mutex_unlock(&g_mu);
	return NULL;
}

uintptr_t w32_beginthreadex(void *security, unsigned stack_size,
                            w32_thread_fn start, void *arg,
                            unsigned initflag, unsigned *thrdaddr)
{
	w32obj *o;
	pthread_t t;

	(void)security; (void)stack_size; (void)initflag;
	ensure_cv();
	o = handle_alloc(W32_KIND_THREAD);
	if (o == NULL) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
	o->fn = start;
	o->argdata = arg;
	o->signaled = 0;
	o->code = STILL_ACTIVE;
	o->joined = 0;
	if (pthread_create(&t, NULL, thread_entry, o) != 0) {
		handle_unref(o);
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return 0;
	}
	o->tid = t;
	if (thrdaddr != NULL)
		*thrdaddr = (unsigned)(uintptr_t)t;
	return (uintptr_t)o;
}

void w32_endthreadex(unsigned retval)
{
	w32obj *o = g_this_thread;
	if (o != NULL) {
		pthread_mutex_lock(&g_mu);
		o->code = retval;
		o->signaled = 1;
		pthread_cond_broadcast(&g_cv);
		pthread_mutex_unlock(&g_mu);
	}
	pthread_exit(NULL);
}

BOOL GetExitCodeThread(HANDLE hh, LPDWORD lpExitCode)
{
	w32obj *o = (w32obj *)hh;
	if (!handle_ok(hh) || o->kind != W32_KIND_THREAD || lpExitCode == NULL) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	*lpExitCode = o->signaled ? o->code : STILL_ACTIVE;
	return TRUE;
}

DWORD GetCurrentThreadId(void) { return (DWORD)(uintptr_t)pthread_self(); }
HANDLE GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }

BOOL GetProcessAffinityMask(HANDLE p, DWORD_PTR *process, DWORD_PTR *system)
{
	cpu_set_t set;
	long n;

	(void)p;
	if (process == NULL || system == NULL) return FALSE;
	n = sysconf(_SC_NPROCESSORS_CONF);
	if (n < 1) n = 1;
	if (n > (long)(sizeof(DWORD_PTR) * 8)) n = (long)(sizeof(DWORD_PTR) * 8);
	*system = (n >= (long)(sizeof(DWORD_PTR) * 8)) ? ~(DWORD_PTR)0
	                                              : (((DWORD_PTR)1 << n) - 1);
	if (sched_getaffinity(0, sizeof(set), &set) != 0) {
		*process = *system;
		return TRUE;
	}
	*process = 0;
	{
		int i;
		for (i = 0; i < (int)(sizeof(DWORD_PTR) * 8); i++)
			if (CPU_ISSET((size_t)i, &set))
				*process |= ((DWORD_PTR)1 << i);
	}
	if (*process == 0) *process = *system;
	return TRUE;
}

BOOL SetProcessAffinityMask(HANDLE p, DWORD_PTR mask)
{
	cpu_set_t set;
	int i;
	(void)p;
	CPU_ZERO(&set);
	for (i = 0; i < (int)(sizeof(DWORD_PTR) * 8); i++)
		if (mask & ((DWORD_PTR)1 << i))
			CPU_SET((size_t)i, &set);
	return sched_setaffinity(0, sizeof(set), &set) == 0 ? TRUE : FALSE;
}

void GetSystemInfo(SYSTEM_INFO *si)
{
	long n;
	memset(si, 0, sizeof(*si));
	si->wProcessorArchitecture = 9;		/* AMD64 */
	si->dwPageSize = (DWORD)sysconf(_SC_PAGESIZE);
	n = sysconf(_SC_NPROCESSORS_CONF);
	si->dwNumberOfProcessors = (n > 0) ? (DWORD)n : 1;
	si->dwActiveProcessorMask = ~(DWORD_PTR)0;
	si->dwAllocationGranularity = 65536;
}

/*
 * A minimal processor-topology table: one core record per logical CPU plus
 * L2/L3 cache records read from sysfs.  check_cpu() only uses these to size
 * its cache-blocking parameters.
 */
BOOL GetLogicalProcessorInformation(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buf, DWORD *len)
{
	static SYSTEM_LOGICAL_PROCESSOR_INFORMATION info[300];
	static DWORD bytes = 0;
	static int built = 0;

	if (!built) {
		long ncpu, l2 = 0, l3 = 0;
		int i, n = 0;
		FILE *f;

		memset(info, 0, sizeof(info));
		ncpu = sysconf(_SC_NPROCESSORS_CONF);
		if (ncpu < 1) ncpu = 1;
		if (ncpu > 200) ncpu = 200;
		for (i = 0; i < (int)ncpu && n + 4 < 300; i++) {
			info[n].Relationship = RelationProcessorCore;
			info[n].ProcessorMask = ((DWORD_PTR)1) << (i % (int)(sizeof(DWORD_PTR) * 8));
			info[n].ProcessorCore.Flags = 0;
			n++;
		}
		f = fopen("/sys/devices/system/cpu/cpu0/cache/index2/size", "r");
		if (f != NULL) { if (fscanf(f, "%ld", &l2) != 1) l2 = 0; fclose(f); }
		f = fopen("/sys/devices/system/cpu/cpu0/cache/index3/size", "r");
		if (f != NULL) { if (fscanf(f, "%ld", &l3) != 1) l3 = 0; fclose(f); }

		if (l2 > 0 && n + 1 < 300) {
			info[n].Relationship = RelationCache;
			info[n].ProcessorMask = ~(DWORD_PTR)0;
			info[n].Cache.Level = 2;
			info[n].Cache.LineSize = 64;
			info[n].Cache.Size = (DWORD)(l2 * 1024);
			info[n].Cache.Associativity = 8;
			info[n].Cache.Type = CacheUnified;
			n++;
		}
		if (l3 > 0 && n + 1 < 300) {
			info[n].Relationship = RelationCache;
			info[n].ProcessorMask = ~(DWORD_PTR)0;
			info[n].Cache.Level = 3;
			info[n].Cache.LineSize = 64;
			info[n].Cache.Size = (DWORD)(l3 * 1024);
			info[n].Cache.Associativity = 16;
			info[n].Cache.Type = CacheUnified;
			n++;
		}
		bytes = (DWORD)(n * sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
		built = 1;
	}

	if (buf == NULL || len == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
	if (*len < bytes) {
		*len = bytes;
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	memcpy(buf, info, bytes);
	*len = bytes;
	return TRUE;
}

BOOL GlobalMemoryStatusEx(LPMEMORYSTATUSEX st)
{
	FILE *f;
	unsigned long long total = 0, avail = 0;
	long pages, pagesize;

	if (st == NULL) return FALSE;
	pages = sysconf(_SC_PHYS_PAGES);
	pagesize = sysconf(_SC_PAGESIZE);
	if (pages > 0 && pagesize > 0)
		total = (unsigned long long)pages * (unsigned long long)pagesize;

	f = fopen("/proc/meminfo", "r");
	if (f != NULL) {
		char line[256];
		while (fgets(line, sizeof(line), f) != NULL) {
			if (strncmp(line, "MemAvailable:", 13) == 0) {
				avail = strtoull(line + 13, NULL, 10) * 1024ULL;
				break;
			}
		}
		fclose(f);
	}
	if (avail == 0) avail = total / 2;

	memset(st, 0, sizeof(*st));
	st->dwLength = sizeof(*st);
	st->dwMemoryLoad = (DWORD)(total ? ((total - avail) * 100ULL / total) : 0);
	st->ullTotalPhys = total;
	st->ullAvailPhys = avail;
	st->ullTotalPageFile = total + total / 2;
	st->ullAvailPageFile = avail + avail / 2;
	st->ullTotalVirtual = 0x00007FFFFFFEFFFFULL;
	st->ullAvailVirtual = 0x00007FFFFFFEFFFFULL;
	st->ullAvailExtendedVirtual = 0;
	return TRUE;
}

BOOL IsWow64Process(HANDLE p, BOOL *lpWow64)
{
	(void)p;
	if (lpWow64) *lpWow64 = FALSE;	/* a 64-bit build is never WOW64 */
	return TRUE;
}

static void seconds_to_systime(time_t sec, SYSTEMTIME *st)
{
	struct tm tmv;
	gmtime_r(&sec, &tmv);
	memset(st, 0, sizeof(*st));
	st->wYear = (WORD)(tmv.tm_year + 1900);
	st->wMonth = (WORD)(tmv.tm_mon + 1);
	st->wDayOfWeek = (WORD)tmv.tm_wday;
	st->wDay = (WORD)tmv.tm_mday;
	st->wHour = (WORD)tmv.tm_hour;
	st->wMinute = (WORD)tmv.tm_min;
	st->wSecond = (WORD)tmv.tm_sec;
}

void GetSystemTimeAsFileTime(FILETIME *ft)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	unix_time_to_filetime(ts.tv_sec, ft);
	{
		ULONGLONG v = ((ULONGLONG)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
		v += (ULONGLONG)(ts.tv_nsec / 100);
		ft->dwLowDateTime = (DWORD)(v & 0xFFFFFFFFULL);
		ft->dwHighDateTime = (DWORD)(v >> 32);
	}
}

void GetSystemTime(SYSTEMTIME *st) { seconds_to_systime(time(NULL), st); }

BOOL FileTimeToSystemTime(const FILETIME *ft, SYSTEMTIME *st)
{
	ULONGLONG v = ((ULONGLONG)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
	time_t t = (time_t)((v / 10000000ULL) - 11644473600ULL);
	seconds_to_systime(t, st);
	return TRUE;
}

BOOL SystemTimeToFileTime(const SYSTEMTIME *st, FILETIME *ft)
{
	struct tm tmv;
	time_t t;
	memset(&tmv, 0, sizeof(tmv));
	tmv.tm_year = st->wYear - 1900;
	tmv.tm_mon = st->wMonth - 1;
	tmv.tm_mday = st->wDay;
	tmv.tm_hour = st->wHour;
	tmv.tm_min = st->wMinute;
	tmv.tm_sec = st->wSecond;
	t = timegm(&tmv);
	unix_time_to_filetime(t, ft);
	return TRUE;
}

/* ============================================================ time / strings */

DWORD GetTickCount(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (DWORD)((DWORD)ts.tv_sec * 1000u + (DWORD)(ts.tv_nsec / 1000000L));
}

void Sleep(DWORD ms)
{
	struct timespec ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
		;
}

static wchar_t wlower(wchar_t c)
{
	if (c >= L'A' && c <= L'Z') return c + 32;
	return c;
}
static unsigned char clower(unsigned char c)
{
	if (c >= 'A' && c <= 'Z') return (unsigned char)(c + 32);
	return c;
}

int _wcsicmp(const wchar_t *a, const wchar_t *b)
{
	for (;;) {
		wchar_t ca = wlower(*a++), cb = wlower(*b++);
		if (ca != cb) return (ca < cb) ? -1 : 1;
		if (ca == 0) return 0;
	}
}

int _wcsnicmp(const wchar_t *a, const wchar_t *b, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++) {
		wchar_t ca = wlower(a[i]), cb = wlower(b[i]);
		if (ca != cb) return (ca < cb) ? -1 : 1;
		if (ca == 0) return 0;
	}
	return 0;
}

int _stricmp(const char *a, const char *b)
{
	for (;;) {
		unsigned char ca = clower((unsigned char)*a++), cb = clower((unsigned char)*b++);
		if (ca != cb) return (ca < cb) ? -1 : 1;
		if (ca == 0) return 0;
	}
}

int _strnicmp(const char *a, const char *b, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++) {
		unsigned char ca = clower((unsigned char)a[i]), cb = clower((unsigned char)b[i]);
		if (ca != cb) return (ca < cb) ? -1 : 1;
		if (ca == 0) return 0;
	}
	return 0;
}

/* _rotl/_rotr come from GCC's <immintrin.h> (x86gprintrin.h), which
 * prefix.h includes; defining them again would clash with the header. */

/* ============================================================== encoding */

/*
 * CP_ACP / CP_OEMCP are UTF-8 on this system, so every code page reduces to a
 * UTF-8 <-> wchar_t conversion.  Flags (MB_ERR_INVALID_CHARS, WC_NO_BEST_FIT_
 * CHARS) are accepted and ignored: a Linux filename is already Unicode.
 *
 * MultiByteToWideChar writes nothing when the destination is too small (which
 * every caller of that function handles).  WideCharToMultiByte fills what fits
 * and NUL-terminates before reporting failure, because utf16_to_utf8() ignores
 * the return value and would otherwise strlen() an uninitialised buffer.
 */
int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR lpMultiByteStr, int cbMultiByte,
                        LPWSTR lpWideCharStr, int cchWideChar)
{
	const unsigned char *s = (const unsigned char *)lpMultiByteStr;
	const unsigned char *end;
	int need = 0, o = 0;
	int until_nul = (cbMultiByte < 0);

	(void)cp; (void)flags;
	if (s == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
	end = until_nul ? NULL : s + cbMultiByte;

	/* pass 1: how many wide characters are required? */
	if (until_nul) {
		const unsigned char *p = s;
		while (*p != 0) {
			uint32_t c = 0;
			int n = utf8_decode(p, p + strlen((const char *)p) + 1, &c);
			if (n == 0) n = 1;
			p += n;
			need++;
		}
		need++;		/* terminating NUL */
	} else {
		const unsigned char *p = s;
		while (p < end) {
			uint32_t c = 0;
			int n = utf8_decode(p, end, &c);
			if (n == 0) n = 1;
			else if (p + n > end) n = 1;
			p += n;
			need++;
		}
	}

	if (lpWideCharStr == NULL || cchWideChar == 0)
		return need;
	if (need > cchWideChar) {
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}

	/* pass 2: convert */
	{
		const unsigned char *p = s;
		while (until_nul ? (*p != 0) : (p < end)) {
			uint32_t c = 0;
			int n = utf8_decode(p, (until_nul ? p + strlen((const char *)p) + 1 : end), &c);
			if (n == 0) { c = 0xFFFD; n = 1; }
			if (!until_nul && p + n > end) { c = 0xFFFD; n = 1; }
			p += n;
			lpWideCharStr[o++] = (wchar_t)c;
		}
		lpWideCharStr[o] = 0;
	}
	return until_nul ? (o + 1) : o;
}

int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR lpWideCharStr, int cchWideChar,
                        LPSTR lpMultiByteStr, int cbMultiByte, LPCSTR lpDefaultChar,
                        BOOL *lpUsedDefaultChar)
{
	int i, o = 0;
	int count;

	(void)cp; (void)flags; (void)lpDefaultChar; (void)lpUsedDefaultChar;
	if (lpWideCharStr == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
	count = (cchWideChar < 0) ? (int)wcslen(lpWideCharStr) + 1 : cchWideChar;

	/* pass 1: byte count */
	{
		int need = 0;
		for (i = 0; i < count; i++) {
			uint32_t c = (uint32_t)lpWideCharStr[i];
			if (c < 0x80) need += 1;
			else if (c < 0x800) need += 2;
			else if (c < 0x10000) need += 3;
			else need += 4;
		}
		if (lpMultiByteStr == NULL || cbMultiByte == 0)
			return need;
	}

	/* pass 2: convert, stopping safely at the end of the destination */
	for (i = 0; i < count; i++) {
		uint32_t c = (uint32_t)lpWideCharStr[i];
		char tmp[4];
		int n = 0;

		if (c >= 0xD800 && c <= 0xDFFF) {
			tmp[n++] = '?';		/* cannot occur with UTF-32 wchar_t */
		} else if (c < 0x80) {
			tmp[n++] = (char)c;
		} else if (c < 0x800) {
			tmp[n++] = (char)(0xC0 | (c >> 6));
			tmp[n++] = (char)(0x80 | (c & 0x3F));
		} else if (c < 0x10000) {
			tmp[n++] = (char)(0xE0 | (c >> 12));
			tmp[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
			tmp[n++] = (char)(0x80 | (c & 0x3F));
		} else {
			tmp[n++] = (char)(0xF0 | (c >> 18));
			tmp[n++] = (char)(0x80 | ((c >> 12) & 0x3F));
			tmp[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
			tmp[n++] = (char)(0x80 | (c & 0x3F));
		}
		if (o + n + 1 > cbMultiByte) {
			lpMultiByteStr[o] = 0;
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return 0;	/* truncated, but NUL-terminated: safe to read */
		}
		memcpy(lpMultiByteStr + o, tmp, (size_t)n);
		o += n;
	}
	lpMultiByteStr[o] = 0;
	return o;
}

UINT GetConsoleOutputCP(void)           { return CP_UTF8; }
UINT SetConsoleOutputCP(UINT cp)        { (void)cp; return CP_UTF8; }
UINT GetOEMCP(void)                     { return CP_UTF8; }
UINT GetACP(void)                       { return CP_UTF8; }

/* ================================================================ INI files */
/*
 * The INI file is plain UTF-8 text ("[section]" / "key=value").  Everything the
 * program appends to the same file as binary data is preserved byte for byte,
 * because lines are copied with explicit lengths.
 */

typedef struct { char *p; size_t len; } sb;	/* string builder */

static void sb_init(sb *b) { b->p = NULL; b->len = 0; }

static int sb_addn(sb *b, const char *s, size_t n)
{
	char *np = (char *)realloc(b->p, b->len + n + 1);
	if (np == NULL) return 0;
	memcpy(np + b->len, s, n);
	b->p = np;
	b->len += n;
	b->p[b->len] = 0;
	return 1;
}

static int sb_add(sb *b, const char *s) { return sb_addn(b, s, strlen(s)); }

static void sb_free(sb *b) { free(b->p); b->p = NULL; b->len = 0; }

static int ini_load(const char *path, char **out, size_t *outlen)
{
	FILE *f = fopen(path, "rb");
	long sz;
	char *data;

	*out = NULL; *outlen = 0;
	if (f == NULL) return 0;
	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
	sz = ftell(f);
	if (sz < 0 || sz > 64L * 1024 * 1024) { fclose(f); return 0; }
	rewind(f);
	data = (char *)malloc((size_t)sz + 1);
	if (data == NULL) { fclose(f); return 0; }
	*outlen = fread(data, 1, (size_t)sz, f);
	data[*outlen] = 0;
	fclose(f);
	*out = data;
	return 1;
}

static int ini_save(const char *path, const char *data, size_t len)
{
	FILE *f = fopen(path, "wb");
	if (f == NULL) return 0;
	if (len > 0 && fwrite(data, 1, len, f) != len) { fclose(f); return 0; }
	fclose(f);
	return 1;
}

static void trim_inplace(char *s)
{
	size_t i = 0, n, j;
	while (s[i] == ' ' || s[i] == '\t') i++;
	if (i > 0) memmove(s, s + i, strlen(s + i) + 1);
	n = strlen(s);
	j = n;
	while (j > 0 && (s[j - 1] == ' ' || s[j - 1] == '\t' || s[j - 1] == '\r')) j--;
	s[j] = 0;
}

/* Return the offset of "[sec]" or (size_t)-1. */
static size_t ini_find_section(const char *data, size_t len, const char *sec)
{
	size_t pos = 0;
	while (pos < len) {
		size_t nl = pos;
		char line[1024];
		size_t ll;
		while (nl < len && data[nl] != '\n') nl++;
		ll = nl - pos;
		if (ll >= sizeof(line)) ll = sizeof(line) - 1;
		memcpy(line, data + pos, ll);
		line[ll] = 0;
		if (line[0] == '[') {
			char *rb = strchr(line, ']');
			if (rb != NULL) {
				*rb = 0;
				if (_stricmp(line + 1, sec) == 0)
					return pos;
			}
		}
		if (nl >= len) break;
		pos = nl + 1;
	}
	return (size_t)-1;
}

DWORD GetPrivateProfileStringW_(LPCWSTR lpAppName, LPCWSTR lpKeyName, LPCWSTR lpDefault,
                                LPWSTR lpReturnedString, DWORD nSize, LPCWSTR lpFileName)
{
	char path[PATH_MAX], sec[512], key[512];
	char *data = NULL;
	size_t len = 0, spos, pos;
	char value[8192];

	if (lpReturnedString == NULL || nSize == 0) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	lpReturnedString[0] = 0;
	if (lpDefault != NULL) {
		size_t i;
		for (i = 0; i + 1 < nSize && lpDefault[i] != 0; i++)
			lpReturnedString[i] = lpDefault[i];
		lpReturnedString[i] = 0;
	}

	wide_to_utf8(lpAppName, sec, sizeof(sec));
	wide_to_utf8(lpKeyName, key, sizeof(key));
	w32_path_to_posix(lpFileName, path, sizeof(path));
	if (path[0] == 0 || !ini_load(path, &data, &len))
		return (DWORD)wcslen(lpReturnedString);

	spos = ini_find_section(data, len, sec);
	if (spos == (size_t)-1) {
		free(data);
		return (DWORD)wcslen(lpReturnedString);
	}
	pos = spos;
	while (pos < len && data[pos] != '\n') pos++;
	if (pos < len) pos++;

	value[0] = 0;
	while (pos < len) {
		size_t nl = pos;
		char line[4096];
		size_t ll;
		char *eq;
		while (nl < len && data[nl] != '\n') nl++;
		ll = nl - pos;
		if (ll >= sizeof(line)) ll = sizeof(line) - 1;
		memcpy(line, data + pos, ll);
		line[ll] = 0;
		if (line[0] == '[')
			break;
		eq = strchr(line, '=');
		if (eq != NULL) {
			char k[512];
			size_t klen = (size_t)(eq - line);
			if (klen >= sizeof(k)) klen = sizeof(k) - 1;
			memcpy(k, line, klen);
			k[klen] = 0;
			trim_inplace(k);
			if (_stricmp(k, key) == 0) {
				char *v = eq + 1;
				trim_inplace(v);
				if (v[0] == '"') {
					size_t vl = strlen(v);
					if (vl >= 2 && v[vl - 1] == '"') { v[vl - 1] = 0; v++; }
				}
				snprintf(value, sizeof(value), "%s", v);
				break;
			}
		}
		if (nl >= len) break;
		pos = nl + 1;
	}
	free(data);

	if (value[0] != 0) {
		wchar_t wv[4096];
		size_t i;
		utf8_to_wide(value, wv, 4096);
		for (i = 0; i + 1 < nSize && wv[i] != 0; i++)
			lpReturnedString[i] = wv[i];
		lpReturnedString[i] = 0;
	}
	return (DWORD)wcslen(lpReturnedString);
}

UINT GetPrivateProfileIntW_(LPCWSTR lpAppName, LPCWSTR lpKeyName, INT nDefault, LPCWSTR lpFileName)
{
	wchar_t buf[128];
	GetPrivateProfileStringW_(lpAppName, lpKeyName, L"", buf, 128, lpFileName);
	if (buf[0] == 0) return (UINT)nDefault;
	return (UINT)wcstol(buf, NULL, 10);
}

BOOL WritePrivateProfileStringW_(LPCWSTR lpAppName, LPCWSTR lpKeyName, LPCWSTR lpString,
                                 LPCWSTR lpFileName)
{
	char path[PATH_MAX], sec[512], key[512], val[8192];
	char *data = NULL;
	size_t len = 0, spos = (size_t)-1, sec_end;
	int had = 0;
	sb out;

	if (lpAppName == NULL || lpKeyName == NULL) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	wide_to_utf8(lpAppName, sec, sizeof(sec));
	wide_to_utf8(lpKeyName, key, sizeof(key));
	val[0] = 0;
	if (lpString != NULL)
		wide_to_utf8(lpString, val, sizeof(val));
	w32_path_to_posix(lpFileName, path, sizeof(path));
	if (path[0] == 0) { SetLastError(ERROR_INVALID_NAME); return FALSE; }

	ini_load(path, &data, &len);	/* a missing file simply starts empty */
	sb_init(&out);

	if (data != NULL) {
		spos = ini_find_section(data, len, sec);
		if (spos != (size_t)-1) {
			/* section end = start of the next '[', or end of text part */
			sec_end = spos;
			while (sec_end < len) {
				if (data[sec_end] == '\n') {
					size_t next = sec_end + 1;
					if (next < len && data[next] == '[') { sec_end = next; break; }
				}
				sec_end++;
			}
			if (sec_end > len) sec_end = len;
		} else {
			sec_end = len;
		}
	} else {
		spos = (size_t)-1;
		sec_end = 0;
	}

	/* deleting: drop the key line only */
	if (lpString == NULL) {
		if (data != NULL && spos != (size_t)-1) {
			size_t p = spos;
			while (p < len && data[p] != '\n') p++;
			if (p < len) p++;
			while (p < sec_end) {
				size_t nl = p;
				char line[4096];
				size_t ll;
				char *eq;
				while (nl < len && data[nl] != '\n') nl++;
				ll = nl - p;
				if (ll >= sizeof(line)) ll = sizeof(line) - 1;
				memcpy(line, data + p, ll);
				line[ll] = 0;
				if (line[0] == '[') { sec_end = p; break; }
				eq = strchr(line, '=');
				if (eq != NULL) {
					*eq = 0;
					trim_inplace(line);
					if (_stricmp(line, key) == 0) {
						size_t skip = nl - p;
						if (nl < len) skip++;	/* also drop '\n' */
						/* keep everything before and after the removed line */
						sb_free(&out);
						sb_init(&out);
						if (!sb_addn(&out, data, p)) goto fail;
						if (!sb_addn(&out, data + p + skip, len - (p + skip))) goto fail;
						had = 2;
						break;
					}
				}
				if (nl >= len) break;
				p = nl + 1;
			}
		}
		if (had != 2) {
			/* key not present: nothing to delete */
			sb_free(&out);
			free(data);
			return TRUE;
		}
		ini_save(path, out.p ? out.p : "", out.len);
		sb_free(&out);
		free(data);
		return TRUE;
	}

	/* writing: copy everything up to the section (or end of file), then the
	 * section with the key replaced or appended, then the remainder */
	if (data == NULL || spos == (size_t)-1) {
		if (data != NULL) {
			if (!sb_addn(&out, data, len)) goto fail;
			if (out.len > 0 && out.p[out.len - 1] != '\n' && !sb_add(&out, "\n")) goto fail;
		}
		if (!sb_add(&out, "[")) goto fail;
		if (!sb_add(&out, sec)) goto fail;
		if (!sb_add(&out, "]\n")) goto fail;
		if (!sb_add(&out, key)) goto fail;
		if (!sb_add(&out, "=")) goto fail;
		if (!sb_add(&out, val)) goto fail;
		if (!sb_add(&out, "\n")) goto fail;
		if (data != NULL && sec_end < len) {
			if (!sb_addn(&out, data + sec_end, len - sec_end)) goto fail;
		}
	} else {
		size_t p;
		/* header + section up to its first key line */
		if (!sb_addn(&out, data, spos)) goto fail;
		p = spos;
		while (p < len && data[p] != '\n') p++;
		if (p < len) p++;
		if (!sb_addn(&out, data + spos, p - spos)) goto fail;

		while (p < sec_end) {
			size_t nl = p;
			char line[4096];
			size_t ll;
			char *eq;
			while (nl < len && data[nl] != '\n') nl++;
			ll = nl - p;
			if (ll >= sizeof(line)) ll = sizeof(line) - 1;
			memcpy(line, data + p, ll);
			line[ll] = 0;
			if (line[0] == '[') break;
			eq = strchr(line, '=');
			if (eq != NULL) {
				*eq = 0;
				trim_inplace(line);
				if (_stricmp(line, key) == 0) {
					/* replace this line */
					if (!sb_add(&out, key)) goto fail;
					if (!sb_add(&out, "=")) goto fail;
					if (!sb_add(&out, val)) goto fail;
					if (nl < len && !sb_add(&out, "\n")) goto fail;
					had = 1;
					p = (nl < len) ? nl + 1 : nl;
					/* copy the rest of the section untouched */
					if (p <= sec_end && !sb_addn(&out, data + p, sec_end - p)) goto fail;
					p = sec_end;
					break;
				}
			}
			/* keep this line as-is */
			{
				size_t stop = (nl < len) ? nl + 1 : nl;
				if (stop > p && !sb_addn(&out, data + p, stop - p)) goto fail;
				p = stop;
			}
		}
		if (!had) {
			if (p > 0 && out.len > 0 && out.p[out.len - 1] != '\n' && !sb_add(&out, "\n"))
				goto fail;
			if (!sb_add(&out, key)) goto fail;
			if (!sb_add(&out, "=")) goto fail;
			if (!sb_add(&out, val)) goto fail;
			if (!sb_add(&out, "\n")) goto fail;
		}
		if (sec_end < len && !sb_addn(&out, data + sec_end, len - sec_end)) goto fail;
	}

	ini_save(path, out.p ? out.p : "", out.len);
	sb_free(&out);
	free(data);
	return TRUE;

fail:
	sb_free(&out);
	free(data);
	SetLastError(ERROR_NOT_ENOUGH_MEMORY);
	return FALSE;
}

/* ================================================= dynamic libs / resources */

HMODULE LoadLibraryA(LPCSTR name)
{
	void *h;
	char alt[512];
	const char *base;

	if (name == NULL) return NULL;
	h = dlopen(name, RTLD_NOW | RTLD_LOCAL);
	if (h != NULL) return (HMODULE)h;

	/* "OpenCL.DLL" -> "libOpenCL.so.1" */
	base = strrchr(name, '/');
	base = (base != NULL) ? base + 1 : name;
	snprintf(alt, sizeof(alt), "lib%s", base);
	{
		char *dot = strstr(alt, ".DLL");
		if (dot == NULL) dot = strstr(alt, ".dll");
		if (dot != NULL) snprintf(dot, sizeof(alt) - (size_t)(dot - alt), ".so.1");
	}
	h = dlopen(alt, RTLD_NOW | RTLD_LOCAL);
	if (h == NULL)
		h = dlopen("libOpenCL.so.1", RTLD_NOW | RTLD_LOCAL);
	return (HMODULE)h;	/* NULL when no OpenCL: the CPU path is used */
}

HMODULE LoadLibraryW_(LPCWSTR name)
{
	char buf[512];
	wide_to_utf8(name, buf, sizeof(buf));
	return LoadLibraryA(buf);
}

BOOL FreeLibrary(HMODULE m)
{
	if (m == NULL) return FALSE;
	return dlclose(m) == 0 ? TRUE : FALSE;
}

void *GetProcAddress(HMODULE m, LPCSTR name)
{
	if (m == NULL || name == NULL) return NULL;
	return dlsym(m, name);
}

/*
 * The OpenCL kernel normally ships as a Windows resource (RT_STRING id 1).
 * The Makefile embeds source.cl so the GPU path stays reachable when
 * libOpenCL happens to be installed.
 */
extern const unsigned char w32_source_cl[];
extern const unsigned char w32_source_cl_end[];
#define w32_source_cl_len \
	((unsigned int)((const unsigned char *)w32_source_cl_end - w32_source_cl))

static char *g_res = NULL;
static size_t g_res_len = 0;

HRSRC FindResourceA(HMODULE m, LPCSTR name, LPCSTR type)
{
	(void)m;
	if (name == NULL || type == NULL) return NULL;
	if (strcmp(type, "RT_STRING") == 0 &&
	    (strcmp(name, "#1") == 0 || strcmp(name, "1") == 0)) {
		g_res = (char *)w32_source_cl;
		g_res_len = w32_source_cl_len;
		return (g_res != NULL && g_res_len > 0) ? (HRSRC)(uintptr_t)1 : NULL;
	}
	return NULL;
}

HGLOBAL LoadResource(HMODULE m, HRSRC r) { (void)m; return (HGLOBAL)r; }
LPVOID  LockResource(HGLOBAL r)          { (void)r; return (LPVOID)g_res; }
DWORD   SizeofResource(HMODULE m, HRSRC r) { (void)m; return (r != NULL) ? (DWORD)g_res_len : 0; }
BOOL    FreeResource(HGLOBAL r)          { (void)r; return TRUE; }

/* ================================================================ messages */

static const struct { DWORD code; const char *text; } g_msg_table[] = {
	{ ERROR_SUCCESS,             "The operation completed successfully." },
	{ ERROR_FILE_NOT_FOUND,      "The system cannot find the file specified." },
	{ ERROR_PATH_NOT_FOUND,      "The system cannot find the path specified." },
	{ ERROR_ACCESS_DENIED,       "Access is denied." },
	{ ERROR_NOT_ENOUGH_MEMORY,   "Not enough memory resources are available." },
	{ ERROR_SHARING_VIOLATION,   "The process cannot access the file because it is being used by another process." },
	{ ERROR_LOCK_VIOLATION,      "The process cannot access the file because another process has locked a portion of the file." },
	{ ERROR_HANDLE_EOF,          "Reached the end of the file." },
	{ ERROR_NOT_READY,           "The device is not ready." },
	{ ERROR_CRC,                 "Data error (cyclic redundancy check)." },
	{ ERROR_DISK_FULL,           "There is not enough space on the disk." },
	{ ERROR_INVALID_PARAMETER,   "The parameter is incorrect." },
	{ ERROR_INSUFFICIENT_BUFFER, "The data area passed to a system call is too small." },
	{ ERROR_ALREADY_EXISTS,      "Cannot create a file when that file already exists." },
	{ ERROR_NO_MORE_FILES,       "There are no more files." },
	{ ERROR_INVALID_FLAGS,       "Invalid flags." },
	{ ERROR_NO_UNICODE_TRANSLATION, "No mapping for the Unicode character exists in the target multi-byte code page." },
	{ ERROR_NOT_SAME_DEVICE,     "The system cannot move the file to a different disk drive." },
	{ ERROR_DIR_NOT_EMPTY,       "The directory is not empty." },
	{ ERROR_GEN_FAILURE,         "A device attached to the system is not functioning." },
	{ ERROR_INVALID_FUNCTION,    "The request is not supported." },
	{ ERROR_NEGATIVE_SEEK,       "An attempt was made to move the file pointer before the beginning of the file." },
};

static void error_text(DWORD code, char *out, size_t outlen)
{
	size_t i;
	for (i = 0; i < sizeof(g_msg_table) / sizeof(g_msg_table[0]); i++) {
		if (g_msg_table[i].code == code) {
			snprintf(out, outlen, "%s\r\n", g_msg_table[i].text);
			return;
		}
	}
	snprintf(out, outlen, "Unknown error (0x%08X)\r\n", code);
}

DWORD FormatMessageA(DWORD flags, LPCVOID lpSource, DWORD dwMessageId, DWORD dwLanguageId,
                     LPSTR lpBuffer, DWORD nSize, void *Arguments)
{
	char tmp[1024];
	size_t need, len;

	(void)lpSource; (void)dwLanguageId; (void)Arguments;
	error_text(dwMessageId, tmp, sizeof(tmp));
	need = strlen(tmp) + 1;

	if ((flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) != 0) {
		char *p = (char *)malloc(need);
		if (p == NULL) return 0;
		memcpy(p, tmp, need);
		*((char **)lpBuffer) = p;
		return (DWORD)(need - 1);
	}
	if (lpBuffer == NULL || nSize == 0) return 0;
	len = strlen(tmp);
	if (len + 1 > nSize) {
		memcpy(lpBuffer, tmp, nSize - 1);
		lpBuffer[nSize - 1] = 0;
		return nSize - 1;
	}
	memcpy(lpBuffer, tmp, len + 1);
	return (DWORD)len;
}

DWORD FormatMessageW_(DWORD flags, LPCVOID lpSource, DWORD dwMessageId, DWORD dwLanguageId,
                      LPWSTR lpBuffer, DWORD nSize, void *Arguments)
{
	char tmp[1024];
	wchar_t wtmp[1024];
	size_t i = 0, len;

	(void)lpSource; (void)dwLanguageId; (void)Arguments;
	error_text(dwMessageId, tmp, sizeof(tmp));
	while (tmp[i] != 0 && i + 1 < sizeof(wtmp) / sizeof(wtmp[0])) {
		wtmp[i] = (wchar_t)(unsigned char)tmp[i];
		i++;
	}
	wtmp[i] = 0;
	len = i;

	if ((flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) != 0) {
		wchar_t *p = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
		if (p == NULL) return 0;
		memcpy(p, wtmp, (len + 1) * sizeof(wchar_t));
		*((wchar_t **)lpBuffer) = p;
		return (DWORD)len;
	}
	if (lpBuffer == NULL || nSize == 0) return 0;
	if (len + 1 > nSize) {
		memcpy(lpBuffer, wtmp, (nSize - 1) * sizeof(wchar_t));
		lpBuffer[nSize - 1] = 0;
		return nSize - 1;
	}
	memcpy(lpBuffer, wtmp, (len + 1) * sizeof(wchar_t));
	return (DWORD)len;
}

HLOCAL  LocalFree(HLOCAL p) { free(p); return NULL; }
LPVOID  LocalAlloc(UINT flags, size_t n) { (void)flags; return calloc(1, n ? n : 1); }

/* ================================================================== shell */

int SHFileOperationW_(SHFILEOPSTRUCT *op)
{
	/* "move to the recycle bin" is not available: delete instead. */
	if (op != NULL && op->pFrom != NULL) {
		const wchar_t *p = op->pFrom;
		while (*p != 0) {
			const wchar_t *end = p;
			while (*end != 0) end++;
			DeleteFileW_(p);
			if (end[1] == 0) break;	/* double-NUL terminated list */
			p = end + 1;
		}
		op->fAnyOperationsAborted = FALSE;
	}
	return 0;
}

void SHGetSetSettings(SHELLSTATE *ss, DWORD mask, BOOL set)
{
	(void)set;
	if (ss == NULL) return;
	memset(ss, 0, sizeof(*ss));
	if (mask & SSF_SHOWALLOBJECTS)  ss->fShowAllObjects = TRUE;
	if (mask & SSF_SHOWSUPERHIDDEN) ss->fShowSuperHidden = TRUE;
}

/* com.cpp's DeleteItem(); here a plain unlink (no trash can on Linux). */
HRESULT DeleteItem(PCWSTR path)
{
	if (path == NULL) return E_FAIL;
	return DeleteFileW_(path) ? S_OK : E_FAIL;
}

BOOL OpenProcessToken(HANDLE p, DWORD access, HANDLE *token)
{
	(void)p; (void)access;
	/* SeManageVolumePrivilege does not exist here; reporting failure makes
	 * the caller skip SetFileValidData(), which is stubbed out anyway. */
	if (token != NULL) *token = NULL;
	return FALSE;
}

BOOL LookupPrivilegeValueA(LPCSTR sys, LPCSTR name, LUID *luid)
{
	(void)sys; (void)name;
	if (luid != NULL) { luid->LowPart = 0; luid->HighPart = 0; }
	return FALSE;
}

BOOL AdjustTokenPrivileges(HANDLE token, BOOL disable, PTOKEN_PRIVILEGES tp,
                           DWORD len, PTOKEN_PRIVILEGES prev, PDWORD needed)
{
	(void)token; (void)disable; (void)tp; (void)len; (void)prev;
	if (needed != NULL) *needed = 0;
	SetLastError(ERROR_NOT_ALL_ASSIGNED);
	return FALSE;
}

/* ================================================= wide printf compatibility */
/*
 * MSVC's swprintf() reads %s as a wide string, glibc's reads it as a narrow
 * one.  Translate the format at run time so the program's L"%s.vol%d"-style
 * calls keep producing correct paths.  %I64 is folded to ll as well.
 */
static void translate_format(const wchar_t *in, wchar_t *out, size_t outmax)
{
	size_t o = 0;

	while (*in != 0 && o + 6 < outmax) {
		int had_l = 0;
		wchar_t conv;

		if (*in != L'%') { out[o++] = *in++; continue; }
		out[o++] = *in++;			/* '%' */
		if (*in == L'%') { out[o++] = *in++; continue; }

		/* MSVC: %s = wide string, %S = narrow string.  glibc's wide printf
		 * functions read both the other way round. */
		if (*in == L's') { out[o++] = L'l'; out[o++] = L's'; in++; continue; }
		if (*in == L'S') { out[o++] = L's'; in++; continue; }

		/* flags / width / precision (MSVC writes "%13I64d" too) */
		while (*in != 0 && wcschr(L"-+ #0'*.0123456789", *in) != NULL && o + 6 < outmax)
			out[o++] = *in++;

		/* length modifiers; MSVC's I64 becomes ll */
		if (in[0] == L'I' && in[1] == L'6' && in[2] == L'4') {
			in += 3;
			out[o++] = L'l';
			out[o++] = L'l';
			had_l = 1;
		}
		while (*in != 0 && wcschr(L"hjlLtzq", *in) != NULL && o + 6 < outmax) {
			if (*in == L'l') had_l = 1;
			out[o++] = *in++;
		}

		conv = *in;
		if (conv == L's' && !had_l) {
			out[o++] = L'l';		/* wide conversion */
			out[o++] = L's';
		} else if (conv != 0) {
			out[o++] = conv;
		}
		if (*in != 0) in++;
	}
	out[o] = 0;
}

int w32_swprintf(wchar_t *buf, size_t n, const wchar_t *fmt, ...)
{
	wchar_t tfmt[2048];
	va_list ap;
	int r;

	if (buf == NULL || n == 0) return -1;
	translate_format(fmt, tfmt, sizeof(tfmt) / sizeof(tfmt[0]));
	va_start(ap, fmt);
	r = vswprintf(buf, n, tfmt, ap);
	va_end(ap);
	if (r < 0) buf[n - 1] = 0;
	return r;
}

int w32_wsprintf(wchar_t *buf, const wchar_t *fmt, ...)
{
	va_list ap;
	int r;
	wchar_t tfmt[2048];

	translate_format(fmt, tfmt, sizeof(tfmt) / sizeof(tfmt[0]));
	va_start(ap, fmt);
	r = vswprintf(buf, 4096, tfmt, ap);
	va_end(ap);
	if (r < 0) buf[0] = 0;
	return r;
}

/* close / handle management ------------------------------------------------ */

BOOL CloseHandle(HANDLE hh)
{
	w32obj *o = (w32obj *)hh;

	if (!handle_ok(hh)) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
	if (hh == (HANDLE)(intptr_t)-1) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }

	if (o->kind == W32_KIND_THREAD && !o->joined) {
		int already_done;
		pthread_mutex_lock(&g_mu);
		already_done = o->signaled;
		pthread_mutex_unlock(&g_mu);
		if (already_done)
			pthread_join(o->tid, NULL);
		else
			pthread_detach(o->tid);
		o->joined = 1;
	}
	if (o->kind == W32_KIND_FIND && o->dir != NULL) {
		closedir(o->dir);
		o->dir = NULL;
	}
	o->closed = 1;
	handle_unref(o);
	return TRUE;
}

/* ================================================================== entry */

extern int wmain(int argc, wchar_t **argv);

/*
 * An absolute POSIX path starts with '/', which the option parser also uses as
 * an option marker (it compares against argv[2][0]).  Rewrite such arguments
 * into the virtual-drive form so they are treated as file names.
 *
 * "/d..." and "/vd..." carry an inline path and are left alone: their value is
 * normalised later by GetFullPathName().  That collides with every real absolute
 * path whose first directory begins with d -- /data, /dev and /disk1 all read as
 * "-d ata/..." at first glance, so the file system has to break the tie: an
 * argument that exists (or whose parent directory exists, which is how a new
 * .par2 file is named) is a path, anything else keeps the option reading.
 */
static int narrow_is_dir(const char *p)
{
	struct stat st;

	if (p[0] == 0) return 0;
	if (stat(p, &st) != 0) return 0;
	return S_ISDIR(st.st_mode) ? 1 : 0;
}

static int wide_path_exists(const wchar_t *wpath, int want_dir)
{
	char p[PATH_MAX];

	w32_path_to_posix(wpath, p, sizeof(p));
	if (!want_dir)
		return (p[0] != 0 && access(p, F_OK) == 0) ? 1 : 0;
	return narrow_is_dir(p);
}

static int wide_parent_is_dir(const wchar_t *wpath)
{
	char p[PATH_MAX];
	size_t n;

	w32_path_to_posix(wpath, p, sizeof(p));
	n = strlen(p);
	while (n > 1 && p[n - 1] == '/') n--;	/* ignore a trailing separator */
	while (n > 1 && p[n - 1] != '/') n--;	/* step back to the parent */
	if (n <= 1) return 0;			/* only the root would be left */
	p[n - 1] = 0;
	return narrow_is_dir(p);
}

static int is_inline_path_option(const wchar_t *a)
{
	const wchar_t *value;

	if (wcsncmp(a, L"/vd", 3) == 0)
		value = a + 3;
	else if (wcsncmp(a, L"/d", 2) == 0)
		value = a + 2;
	else
		return 0;

	if (wcschr(value, L'/') == NULL) return 0;	/* no separator: plain option */
	if (wide_path_exists(a, 0) || wide_parent_is_dir(a)) return 0;
	return 1;
}

static void arg_to_windows(wchar_t *dst, const wchar_t *src, size_t dstmax)
{
	size_t i = 0, o = 0;
	if (dstmax < 3) { dst[0] = 0; return; }
	dst[o++] = L'C';
	dst[o++] = L':';
	for (; src[i] != 0 && o + 1 < dstmax; i++)
		dst[o++] = (src[i] == L'/') ? L'\\' : src[i];
	dst[o] = 0;
}

/* port: elapsed-time refresh interval, owned by common2.c.  A front end that
 * wants a smoother progress bar sets PAR2J_PROGRESS_INTERVAL=<ms>; the value
 * only changes how often the progress display refreshes. */
extern int progress_tick_ms;

static void setup_progress_interval(void)
{
	const char *v = getenv("PAR2J_PROGRESS_INTERVAL");
	long ms;

	if (v == NULL || *v == 0) return;
	ms = strtol(v, NULL, 10);
	if (ms < 20 || ms > 10000) return;
	progress_tick_ms = (int)ms;
}

int w32_main_entry(int argc, char **argv)
{
	wchar_t **wargv;
	int i, r;

	setlocale(LC_ALL, "");
	ensure_cv();
	setup_progress_interval();

	wargv = (wchar_t **)calloc((size_t)argc + 1, sizeof(wchar_t *));
	if (wargv == NULL) return 1;

	for (i = 0; i < argc; i++) {
		char narrow[PATH_MAX];
		wchar_t wtmp[PATH_MAX];

		/* Decode the argument as UTF-8, which is what the rest of the compat
		 * layer assumes for paths.  Converting through the locale instead
		 * (mbsrtowcs) breaks every non-ASCII name whenever the locale is not
		 * UTF-8 -- LC_ALL=C, cron, containers -- because the bytes then turn
		 * into a byte-per-wchar Latin-1 string that no longer names the file. */
		if (utf8_is_valid(argv[i])) {
			utf8_to_wide(argv[i], wtmp, PATH_MAX);
		} else {
			/* not UTF-8 at all: keep the raw bytes addressable */
			size_t k;
			for (k = 0; k + 1 < PATH_MAX && argv[i][k]; k++)
				wtmp[k] = (wchar_t)(unsigned char)argv[i][k];
			wtmp[k] = 0;
		}

		if (i == 0) {
			/* argv[0] is our own path: always convert to virtual-drive form,
			 * otherwise the option parser would read its first '/x' as options. */
			wchar_t conv[PATH_MAX];
			arg_to_windows(conv, wtmp, PATH_MAX);
			wargv[i] = wcsdup(conv);
			continue;
		}
		if (wtmp[0] == L'/' && !is_inline_path_option(wtmp)) {
			int is_path = (wcschr(wtmp + 1, L'/') != NULL);
			if (!is_path) {
				w32_path_to_posix(wtmp, narrow, sizeof(narrow));
				if (access(narrow, F_OK) == 0)
					is_path = 1;
			}
			if (is_path) {
				wchar_t conv[PATH_MAX];
				arg_to_windows(conv, wtmp, PATH_MAX);
				wargv[i] = wcsdup(conv);
				continue;
			}
		}
		wargv[i] = wcsdup(wtmp);
	}
	wargv[argc] = NULL;

	if (getenv("PAR2J_TRACE")) {
		for (i = 0; i < argc; i++) {
			char d[1024]; size_t q=0;
			for (const wchar_t *z=wargv[i]; *z && q<1000; z++){ if (*z<0x80) d[q++]=(char)*z; else d[q++]='?'; }
			d[q]=0; fprintf(stderr, "ARGV[%d]=[%s]\n", i, d);
		}
	}
	r = wmain(argc, wargv);

	for (i = 0; i < argc; i++)
		free(wargv[i]);
	free(wargv);
	return r;
}

int main(int argc, char **argv)
{
	return w32_main_entry(argc, argv);
}

/* ================================================= MSVC CRT helpers (Linux) */

/* Fold MSVC's %I64 length modifier to ll; glibc would read the "I" as a flag
 * and "64" as a field width, silently truncating the value to int. */
static void translate_nfmt(const char *in, char *out, size_t outmax)
{
	size_t o = 0;

	while (*in != 0 && o + 8 < outmax) {
		char c = *in++;
		if (c != '%') { out[o++] = c; continue; }
		out[o++] = '%';
		if (*in == '%') { out[o++] = *in++; continue; }
		while (*in != 0 && strchr("-+ #0'*.0123456789", *in) != NULL && o + 8 < outmax)
			out[o++] = *in++;
		if (in[0] == 'I' && in[1] == '6' && in[2] == '4') {
			in += 3;
			out[o++] = 'l';
			out[o++] = 'l';
		}
		while (*in != 0 && strchr("hlLzjt", *in) != NULL && o + 8 < outmax)
			out[o++] = *in++;
		if (*in != 0)
			out[o++] = *in++;
	}
	out[o] = 0;
}

int w32_printf(const char *fmt, ...)
{
	char tfmt[4096];
	va_list ap;
	int r;

	translate_nfmt(fmt, tfmt, sizeof(tfmt));
	va_start(ap, fmt);
	r = vprintf(tfmt, ap);
	va_end(ap);
	return r;
}

int w32_wsprintfA(char *buf, const char *fmt, ...)
{
	char tfmt[1024];
	va_list ap;
	int r;

	translate_nfmt(fmt, tfmt, sizeof(tfmt));
	va_start(ap, fmt);
	r = vsnprintf(buf, 1024, tfmt, ap);
	va_end(ap);
	return r;
}

int w32_fwprintf(FILE *stream, const wchar_t *fmt, ...)
{
	wchar_t tfmt[2048];
	va_list ap;
	int r;

	translate_format(fmt, tfmt, 2048);
	va_start(ap, fmt);
	r = vfwprintf(stream, tfmt, ap);
	va_end(ap);
	return r;
}

/* MSVC's _wfopen accepts mode suffixes glibc does not know ("wt, ccs=UTF-8");
 * keep only r/w/a, '+' and 'b'. */
FILE *w32_wfopen(const wchar_t *name, const wchar_t *mode)
{
	char path[PATH_MAX];
	char m[32];
	size_t o = 0;

	w32_path_to_posix(name, path, sizeof(path));

	if (*mode != 0)
		m[o++] = (char)*mode++;
	while (*mode != 0 && o + 1 < sizeof(m)) {
		wchar_t c = *mode++;
		if (c == L',') break;		/* ", ccs=..." tail */
		if (c == L'+' || c == L'b') m[o++] = (char)c;
	}
	m[o] = 0;
	if (o == 0) { m[0] = 'r'; m[1] = 0; }

	return fopen(path, m);
}

/* Console input: only the plain c/p/r keys of print_progress() are used, and
 * only when stdin is a terminal.  Canonical mode is kept, so a key has to be
 * confirmed with Enter -- no terminal state is modified. */
int w32_kbhit(void)
{
	struct timeval tv;
	fd_set fds;

	if (!isatty(STDIN_FILENO))
		return 0;
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	FD_ZERO(&fds);
	FD_SET(STDIN_FILENO, &fds);
	return (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0);
}

int w32_getch(void)
{
	unsigned char c = 0;

	if (read(STDIN_FILENO, &c, 1) != 1)
		return 0;
	return (int)c;
}

int w32_getche(void)
{
	int c = w32_getch();

	if (c != 0) {
		fputc(c, stdout);
		fflush(stdout);
	}
	return c;
}

wchar_t *w32_wcslwr(wchar_t *s)
{
	wchar_t *p = s;

	if (s == NULL)
		return s;
	while (*p != 0) {
		*p = towlower(*p);
		p++;
	}
	return s;
}

void *w32_aligned_malloc(size_t size, size_t align)
{
	void *p = NULL;

	if (align < sizeof(void *))
		align = sizeof(void *);
	while ((align & (align - 1)) != 0)	/* posix_memalign wants a power of 2 */
		align <<= 1;
	if (posix_memalign(&p, align, size != 0 ? size : 1) != 0)
		return NULL;
	return p;
}

void w32_aligned_free(void *p)
{
	free(p);
}

/* There is no PE checksum in an ELF file: report the two sums as equal so
 * par2_checksum() proceeds to its own CRC test (which reports the expected
 * mismatch for a rebuilt binary). */
PIMAGE_NT_HEADERS CheckSumMappedFile(void *base, unsigned int length,
                                     unsigned int *header_sum,
                                     unsigned int *file_sum)
{
	(void)length;
	if (base == NULL)
		return NULL;
	if (header_sum != NULL) *header_sum = 0;
	if (file_sum != NULL)   *file_sum = 0;
	return (PIMAGE_NT_HEADERS)base;
}

BOOL CopyFileW_(LPCWSTR from, LPCWSTR to, BOOL failIfExists)
{
	char src[PATH_MAX], dst[PATH_MAX];
	int fd_in = -1, fd_out = -1;
	char buf[65536];
	ssize_t r;
	BOOL ok = TRUE;

	w32_path_to_posix(from, src, sizeof(src));
	w32_path_to_posix(to, dst, sizeof(dst));

	if (failIfExists && access(dst, F_OK) == 0) {
		SetLastError(ERROR_ALREADY_EXISTS);
		return FALSE;
	}
	fd_in = open(src, O_RDONLY);
	if (fd_in < 0) { path_error(src); return FALSE; }
	fd_out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd_out < 0) { path_error(dst); close(fd_in); return FALSE; }
	while ((r = read(fd_in, buf, sizeof(buf))) != 0) {
		if (r < 0 || write(fd_out, buf, (size_t)r) != r) {
			ok = FALSE;
			break;
		}
	}
	close(fd_in);
	close(fd_out);
	if (!ok) path_error(dst);
	return ok;
}

/* Executable memory for gf16.c's SSE2 JIT (gf_jit.h). */
LPVOID VirtualAlloc(LPVOID addr, size_t size, DWORD type, DWORD protect)
{
	void *p;
	int flags = PROT_READ | PROT_WRITE;
	size_t len = (size + 4095) & ~4095UL;

	(void)type;
	if (protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_READ)
		flags |= PROT_EXEC;
	p = mmap(addr, len, flags, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	return p;
}

BOOL VirtualFree(LPVOID addr, size_t size, DWORD type)
{
	(void)size; (void)type;
	/* The allocation size is not passed on MEM_RELEASE; /proc/self/maps is
	 * not worth parsing for the JIT's single region, so this leaks at exit. */
	if (addr == NULL)
		return FALSE;
	return TRUE;
}

/* Locale aware comparison used by sort_list() (common2.c): returns
 * CSTR_LESS(1) / CSTR_EQUAL(2) / CSTR_GREATER(3).  The flag actually used
 * there is SORT_DIGITSASNUMBERS (numeric order, "file2" < "file10"), which
 * glibc provides as strverscmp() on the UTF-8 form of both strings. */
int CompareStringEx(LPCWSTR locale, DWORD flags, LPCWSTR str1, int len1,
                    LPCWSTR str2, int len2, LPVOID reserved1, LPVOID reserved2,
                    LPARAM lParam)
{
	char a[PATH_MAX * 4];
	char b[PATH_MAX * 4];
	int r;
	int l1, l2;

	(void)locale; (void)reserved1; (void)reserved2; (void)lParam;
	if (str1 == NULL || str2 == NULL)
		return 0;

	/* A negative length means "NUL-terminated" on Windows, but the caller's
	 * buffer may end exactly at the string's own NUL (sort_list packs entries
	 * back to back with no slack).  Cap the copy at len1/len2 characters so
	 * wide_to_utf8 never scans past the region the caller owns. */
	l1 = (len1 < 0) ? -1 : len1;
	l2 = (len2 < 0) ? -1 : len2;
	wide_to_utf8_len(str1, a, sizeof(a), l1);
	wide_to_utf8_len(str2, b, sizeof(b), l2);

	if (flags & SORT_DIGITSASNUMBERS)
		r = strverscmp(a, b);
	else if (flags & NORM_IGNORECASE)
		r = strcasecmp(a, b);
	else
		r = strcmp(a, b);
	return (r < 0) ? 1 : ((r == 0) ? 2 : 3);
}
