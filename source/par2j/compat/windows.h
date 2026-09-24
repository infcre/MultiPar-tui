/*
 * windows.h -- Win32 compatibility layer for building par2j on Linux.
 *
 * This header (and wincompat.c) let the unmodified MultiPar PAR2 client compile
 * with gcc on POSIX systems.  It is placed on the include path so that the
 * sources' "#include <windows.h>" resolves here instead of the real SDK.
 *
 * Path model: internally the program keeps Windows style paths
 * ("\\?\C:\dir\file", backslashes, virtual drive letter).  Every syscall
 * wrapper in wincompat.c translates that back to a POSIX path before touching
 * the kernel, so the path handling code in common2.c never has to change.
 * The virtual drive "C:" is mapped to the POSIX root directory "/".
 */
#ifndef WINCOMPAT_WINDOWS_H
#define WINCOMPAT_WINDOWS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- MSVC-isms */

#define __int64   long long
#define __int32    int
#define __int16    short
#define __int8     char
#ifndef _WIN64
#define _WIN64    1          /* we are building a 64-bit binary: skips the
                                32-bit inline assembly paths in gf16.c */
#endif

/* __declspec( align(64) ) -> __attribute__((aligned(64)))
 * The inner token "align" is never used as an identifier in this code base,
 * so it is safe to alias it to GCC's "aligned". */
#define align              aligned
#define __declspec(x)      __attribute__((x))

#define __in
#define __out
#define __inout
#define __in_opt
#define __out_opt
#define _countof(a)        (sizeof(a) / sizeof((a)[0]))

#ifndef WINAPI
#define WINAPI
#endif
#ifndef CALLBACK
#define CALLBACK
#endif
#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef __stdcall
#define __stdcall
#endif
#ifndef __cdecl
#define __cdecl
#endif
#ifndef CONST
#define CONST const
#endif
#ifndef VOID
#define VOID void
#endif
#ifndef FAR
#define FAR
#endif
#ifndef NEAR
#define NEAR
#endif

/* --------------------------------------------------------------- basic types */

typedef int             BOOL;
typedef unsigned char   BYTE;
typedef unsigned short  WORD;
typedef unsigned short  USHORT;
typedef short           SHORT;
typedef int             LONG;            /* 32-bit, as on Windows */
typedef unsigned int    ULONG;           /* 32-bit, as on Windows */
typedef unsigned int    DWORD;           /* 32-bit, as on Windows */
typedef int             INT;
typedef unsigned int    UINT;
typedef unsigned int    UINT32;
typedef int             INT32;
typedef long long       LONGLONG;
typedef unsigned long long ULONGLONG;
typedef int64_t         INT64;
typedef uint64_t        UINT64;
typedef intptr_t        LONG_PTR;
typedef intptr_t        INT_PTR;
typedef uintptr_t       ULONG_PTR;
typedef uintptr_t       DWORD_PTR;
typedef uintptr_t       UINT_PTR;
typedef uintptr_t       size_type_unused_; /* placeholder, unused */
typedef char            CHAR;
typedef unsigned char   UCHAR;
typedef unsigned char   *PUCHAR;
typedef wchar_t         WCHAR;
typedef WCHAR           *LPWSTR;
typedef const WCHAR     *LPCWSTR;
typedef WCHAR           *PWSTR;
typedef const WCHAR     *PCWSTR;
typedef CHAR            *LPSTR;
typedef const CHAR      *LPCSTR;
typedef CHAR            *PSTR;
typedef void            *LPVOID;
typedef const void      *LPCVOID;
typedef void            *PVOID;
typedef void            *HANDLE;
typedef HANDLE          *PHANDLE;
typedef HANDLE          HMODULE;
typedef HANDLE          HINSTANCE;
typedef HANDLE          HWND;
typedef HANDLE          HKEY;
typedef HANDLE          HLOCAL;
typedef HANDLE          HGLOBAL;
typedef HANDLE          HRSRC;
typedef void            *HACCEL;
typedef int             HRESULT;
typedef DWORD           *LPDWORD;
typedef DWORD           *PDWORD;
typedef ULONG_PTR       *PULONG_PTR;
typedef BYTE            *LPBYTE;
typedef ULONG           *PULONG;
typedef LONG            *PLONG;
typedef LONG_PTR        LRESULT;
typedef UINT_PTR        WPARAM;
typedef LONG_PTR        LPARAM;

typedef union _LARGE_INTEGER {
	struct {
		DWORD LowPart;
		LONG  HighPart;
	} u;
	struct {
		DWORD LowPart;
		LONG  HighPart;
	} DUMMYSTRUCTNAME;
	LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef struct _ULARGE_INTEGER {
	ULONGLONG QuadPart;
} ULARGE_INTEGER;

typedef struct _FILETIME {
	DWORD dwLowDateTime;
	DWORD dwHighDateTime;
} FILETIME, *PFILETIME;

typedef struct _SYSTEMTIME {
	WORD wYear, wMonth, wDayOfWeek, wDay;
	WORD wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME, *PSYSTEMTIME;

/* ----------------------------------------------------------- calling / SAL */

#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef NULL
#define NULL ((void *)0)
#endif
#ifndef IN
#define IN
#endif
#ifndef OUT
#define OUT
#endif
#ifndef OPTIONAL
#define OPTIONAL
#endif

#define WINBASEAPI
#define WINADVAPI

/* ------------------------------------------------------------------ constants */

#define INVALID_HANDLE_VALUE  ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define INVALID_FILE_SIZE       ((DWORD)-1)
#define INVALID_SET_FILE_POINTER ((DWORD)-1)
#define NO_ERROR                 0
#define MAX_PATH                260
#define MAX_PATH_WIDE           1024
#define INFINITE                0xFFFFFFFF
#define WAIT_OBJECT_0           0
#define WAIT_ABANDONED          0x80
#define WAIT_TIMEOUT            258
#define WAIT_FAILED             ((DWORD)0xFFFFFFFF)
#define STILL_ACTIVE            259
#define INFINITE_MS             INFINITE

#define GENERIC_READ            0x80000000
#define GENERIC_WRITE           0x40000000
#define GENERIC_EXECUTE         0x20000000
#define GENERIC_ALL             0x10000000

#define FILE_SHARE_READ         0x00000001
#define FILE_SHARE_WRITE        0x00000002
#define FILE_SHARE_DELETE       0x00000004

#define CREATE_NEW              1
#define CREATE_ALWAYS           2
#define OPEN_EXISTING           3
#define OPEN_ALWAYS             4
#define TRUNCATE_EXISTING       5

#define FILE_BEGIN              0
#define FILE_CURRENT            1
#define FILE_END                2

#define FILE_ATTRIBUTE_READONLY    0x00000001
#define FILE_ATTRIBUTE_HIDDEN      0x00000002
#define FILE_ATTRIBUTE_SYSTEM      0x00000004
#define FILE_ATTRIBUTE_DIRECTORY   0x00000010
#define FILE_ATTRIBUTE_ARCHIVE     0x00000020
#define FILE_ATTRIBUTE_DEVICE      0x00000040
#define FILE_ATTRIBUTE_NORMAL      0x00000080
#define FILE_ATTRIBUTE_TEMPORARY   0x00000100
#define FILE_ATTRIBUTE_SPARSE_FILE 0x00000200
#define FILE_ATTRIBUTE_REPARSE_POINT 0x00000400
#define FILE_ATTRIBUTE_COMPRESSED  0x00000800
#define FILE_ATTRIBUTE_OFFLINE     0x00001000
#define FILE_ATTRIBUTE_NOT_CONTENT_INDEXED 0x00002000
#define FILE_ATTRIBUTE_ENCRYPTED   0x00004000

#define FILE_FLAG_WRITE_THROUGH   0x80000000
#define FILE_FLAG_OVERLAPPED      0x40000000
#define FILE_FLAG_NO_BUFFERING    0x20000000
#define FILE_FLAG_RANDOM_ACCESS   0x10000000
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000
#define FILE_FLAG_POSIX_SEMANTICS 0x01000000

#define PAGE_NOACCESS            0x01
#define PAGE_READONLY            0x02
#define PAGE_READWRITE           0x04
#define PAGE_WRITECOPY           0x08
#define PAGE_EXECUTE             0x10
#define PAGE_EXECUTE_READ        0x20
#define PAGE_EXECUTE_READWRITE   0x40

#define MEM_COMMIT               0x00001000
#define MEM_RESERVE              0x00002000
#define MEM_RELEASE              0x00008000
#define MEM_MAPPED               0x00040000
#define MEM_PRIVATE              0x00020000
#define FILE_MAP_COPY            0x00000001
#define FILE_MAP_WRITE           0x00000002
#define FILE_MAP_READ            0x00000004

/* GetLastError() values used by this program */
#define ERROR_SUCCESS              0
#define ERROR_INVALID_FUNCTION     1
#define ERROR_FILE_NOT_FOUND       2
#define ERROR_PATH_NOT_FOUND       3
#define ERROR_ACCESS_DENIED        5
#define ERROR_NOT_ENOUGH_MEMORY    8
#define ERROR_BAD_FORMAT           11
#define ERROR_INVALID_DATA         13
#define ERROR_OUTOFMEMORY          14
#define ERROR_WRITE_PROTECT        19
#define ERROR_SHARING_VIOLATION    32
#define ERROR_LOCK_VIOLATION       33
#define ERROR_HANDLE_EOF           38
#define ERROR_NOT_READY            21
#define ERROR_CRC                  23
#define ERROR_SEEK                 25
#define ERROR_WRITE_FAULT          29
#define ERROR_READ_FAULT           30
#define ERROR_GEN_FAILURE          31
#define ERROR_INVALID_HANDLE       6
#define ERROR_INVALID_PARAMETER    87
#define ERROR_NEGATIVE_SEEK        131
#define ERROR_BROKEN_PIPE          109
#define ERROR_DISK_FULL            112
#define ERROR_CALL_NOT_IMPLEMENTED 120
#define ERROR_INSUFFICIENT_BUFFER  122
#define ERROR_INVALID_NAME         123
#define ERROR_DIR_NOT_EMPTY        145
#define ERROR_ALREADY_EXISTS       183
#define ERROR_NO_MORE_FILES        18
#define ERROR_INVALID_FLAGS        1004
#define ERROR_NO_UNICODE_TRANSLATION 1113
#define ERROR_IO_PENDING           997
#define ERROR_NOT_SAME_DEVICE      17
#define ERROR_UNABLE_TO_MOVE_REPLACEMENT    1176
#define ERROR_UNABLE_TO_MOVE_REPLACEMENT_2  1177
#define ERROR_UNABLE_TO_REMOVE_REPLACED     1175
#define ERROR_UNABLE_TO_MOVE_REPLACEMENT_1  1175

/* code pages */
#define CP_ACP                     0
#define CP_OEMCP                   1
#define CP_THREAD_ACP              3
#define CP_UTF8                    65001
#define MB_ERR_INVALID_CHARS       0x00000008
#define WC_NO_BEST_FIT_CHARS       0x00000400
#define MB_PRECOMPOSED             0x00000001

#define FORMAT_MESSAGE_ALLOCATE_BUFFER 0x00000100
#define FORMAT_MESSAGE_IGNORE_INSERTS  0x00000200
#define FORMAT_MESSAGE_FROM_STRING     0x00000400
#define FORMAT_MESSAGE_FROM_SYSTEM     0x00001000
#define FORMAT_MESSAGE_MAX_WIDTH_MASK  0x000000FF

#define DRIVE_UNKNOWN     0
#define DRIVE_NO_ROOT_DIR 1
#define DRIVE_REMOVABLE   2
#define DRIVE_FIXED       3
#define DRIVE_REMOTE      4
#define DRIVE_CDROM       5
#define DRIVE_RAMDISK     6

#define FILE_TYPE_DISK   0x0001
#define FILE_SUPPORTS_SPARSE_FILES  0x00000040
#define FILE_SUPPORTS_REPARSE_POINTS 0x00000080

#define GetFileExInfoStandard 0

/* GetLogicalProcessorInformation relationships */
typedef enum _LOGICAL_PROCESSOR_RELATIONSHIP {
	RelationProcessorCore = 0,
	RelationNumaNode = 1,
	RelationCache = 2,
	RelationProcessorPackage = 3,
	RelationGroup = 4,
	RelationAll = 0xffff
} LOGICAL_PROCESSOR_RELATIONSHIP;

typedef enum _PROCESSOR_CACHE_TYPE {
	CacheUnified = 0,
	CacheInstruction = 1,
	CacheData = 2,
	CacheTrace = 3
} PROCESSOR_CACHE_TYPE;

/* --------------------------------------------------------------- structures */

typedef struct _OVERLAPPED {
	ULONG_PTR Internal;
	ULONG_PTR InternalHigh;
	union {
		struct {
			DWORD Offset;
			DWORD OffsetHigh;
		};
		void *Pointer;
	};
	HANDLE hEvent;
} OVERLAPPED, *LPOVERLAPPED;

typedef struct _SECURITY_ATTRIBUTES {
	DWORD nLength;
	LPVOID lpSecurityDescriptor;
	BOOL bInheritHandle;
} SECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;

typedef struct _WIN32_FIND_DATAW {
	DWORD dwFileAttributes;
	FILETIME ftCreationTime;
	FILETIME ftLastAccessTime;
	FILETIME ftLastWriteTime;
	DWORD nFileSizeHigh;
	DWORD nFileSizeLow;
	DWORD dwReserved0;
	DWORD dwReserved1;
	WCHAR cFileName[MAX_PATH];
	WCHAR cAlternateFileName[14];
} WIN32_FIND_DATAW, *PWIN32_FIND_DATAW, *LPWIN32_FIND_DATAW;
typedef WIN32_FIND_DATAW WIN32_FIND_DATA;

typedef struct _WIN32_FILE_ATTRIBUTE_DATA {
	DWORD dwFileAttributes;
	FILETIME ftCreationTime;
	FILETIME ftLastAccessTime;
	FILETIME ftLastWriteTime;
	DWORD nFileSizeHigh;
	DWORD nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA, *LPWIN32_FILE_ATTRIBUTE_DATA;

typedef struct _BY_HANDLE_FILE_INFORMATION {
	DWORD dwFileAttributes;
	FILETIME ftCreationTime;
	FILETIME ftLastAccessTime;
	FILETIME ftLastWriteTime;
	DWORD dwVolumeSerialNumber;
	DWORD nFileSizeHigh;
	DWORD nFileSizeLow;
	DWORD nNumberOfLinks;
	DWORD nFileIndexHigh;
	DWORD nFileIndexLow;
} BY_HANDLE_FILE_INFORMATION, *LPBY_HANDLE_FILE_INFORMATION;

typedef struct _MEMORYSTATUSEX {
	DWORD dwLength;
	DWORD dwMemoryLoad;
	ULONGLONG ullTotalPhys;
	ULONGLONG ullAvailPhys;
	ULONGLONG ullTotalPageFile;
	ULONGLONG ullAvailPageFile;
	ULONGLONG ullTotalVirtual;
	ULONGLONG ullAvailVirtual;
	ULONGLONG ullAvailExtendedVirtual;
} MEMORYSTATUSEX, *LPMEMORYSTATUSEX;

typedef struct _SYSTEM_INFO {
	union {
		DWORD dwOsmId;
		struct {
			WORD wProcessorArchitecture;
			WORD wReserved;
		};
	};
	DWORD dwPageSize;
	LPVOID lpMinimumApplicationAddress;
	LPVOID lpMaximumApplicationAddress;
	DWORD_PTR dwActiveProcessorMask;
	DWORD dwNumberOfProcessors;
	DWORD dwProcessorType;
	DWORD dwAllocationGranularity;
	WORD wProcessorLevel;
	WORD wProcessorRevision;
} SYSTEM_INFO, *LPSYSTEM_INFO;

typedef struct _CACHE_DESCRIPTOR {
	DWORD Level;
	DWORD LineSize;
	DWORD Size;
	WORD Associativity;
	WORD Type;
} CACHE_DESCRIPTOR, *PCACHE_DESCRIPTOR;

typedef struct _SYSTEM_LOGICAL_PROCESSOR_INFORMATION {
	DWORD_PTR ProcessorMask;
	LOGICAL_PROCESSOR_RELATIONSHIP Relationship;
	union {	/* anonymous: sources write ptr->ProcessorCore etc. */
		struct {
			BYTE Flags;
		} ProcessorCore;
		struct {
			DWORD NodeNumber;
		} NumaNode;
		struct {
			DWORD Reserved[2];
			BYTE Group;
		} Reserved_;
		CACHE_DESCRIPTOR Cache;
	};
} SYSTEM_LOGICAL_PROCESSOR_INFORMATION, *PSYSTEM_LOGICAL_PROCESSOR_INFORMATION;

/* aliases so that the sources can write ptr->ProcessorCore / ptr->Cache */

/* storage query (used to guess SSD vs HDD) */
typedef struct _STORAGE_PROPERTY_QUERY {
	int PropertyId;
	int QueryType;
	BYTE AdditionalParameters[1];
} STORAGE_PROPERTY_QUERY;

typedef struct _DEVICE_SEEK_PENALTY_DESCRIPTOR {
	DWORD Version;
	DWORD Size;
	BOOL IncursSeekPenalty;
} DEVICE_SEEK_PENALTY_DESCRIPTOR;

typedef struct _DEVICE_TRIM_DESCRIPTOR {
	DWORD Version;
	DWORD Size;
	BOOL TrimEnabled;
} DEVICE_TRIM_DESCRIPTOR;

typedef struct _STORAGE_ADAPTER_DESCRIPTOR {
	DWORD Version;
	DWORD Size;
 DWORD MaximumTransferLength;
	DWORD AlignmentMask;
	WORD AdapterUsesPio;
	WORD AdapterScansDown;
	WORD CommandQueueDepth;
	WORD BusType;
	WORD BusMajorVersion;
	WORD BusMinorVersion;
	WORD reserved;
} STORAGE_ADAPTER_DESCRIPTOR;

#define IOCTL_STORAGE_QUERY_PROPERTY 0x002D1400
#define FSCTL_SET_SPARSE             0x000900C4
#define FSCTL_SET_ZERO_DATA          0x000900C8

typedef struct _FILE_ZERO_DATA_INFORMATION {
	LARGE_INTEGER FileOffset;
	LARGE_INTEGER BeyondFinalZero;
} FILE_ZERO_DATA_INFORMATION;

typedef struct _FILE_SET_SPARSE_BUFFER {
	BOOL SetSparse;
} FILE_SET_SPARSE_BUFFER;

/* --- extra Win32 API (implemented in wincompat.c) --- */
BOOL   CopyFileW_(LPCWSTR from, LPCWSTR to, BOOL failIfExists);
#define CopyFile CopyFileW_
LPVOID VirtualAlloc(LPVOID addr, size_t size, DWORD type, DWORD protect);
BOOL   VirtualFree(LPVOID addr, size_t size, DWORD type);

/* shell */
#define FO_DELETE    0x0003
#define FOF_ALLOWUNDO        0x0040
#define FOF_NOCONFIRMATION   0x0010
#define FOF_SILENT           0x0004
#define FOF_NOERRORUI        0x0400
#define FOF_NOCONFIRMMKDIR   0x0200

typedef struct _SHFILEOPSTRUCTW {
	HWND hwnd;
	UINT wFunc;
	LPCWSTR pFrom;
	LPCWSTR pTo;
	WORD fFlags;
	BOOL fAnyOperationsAborted;
	LPVOID hNameMappings;
	LPCWSTR lpszProgressTitle;
} SHFILEOPSTRUCTW, *LPSHFILEOPSTRUCTW;
typedef SHFILEOPSTRUCTW SHFILEOPSTRUCT;

typedef struct tagSHELLSTATE {
	BOOL fShowExtensions;
	BOOL fShowCheckBoxes;
	BOOL fAlwaysShowExt;
	BOOL fMapNetDrvBtn;
	BOOL fShowFullPath;
	BOOL fShowSortColumns;
	BOOL fShowInfoTip;
	BOOL fDoubleClickInWebView;
	BOOL fAllowWebViewX;
	BOOL fClassicShellMenu;
	BOOL fDesktopHTML;
	BOOL fWin95Classic;
	BOOL fDontPrettyPath;
	BOOL fShowAttribCol;
	BOOL fShowSyncProviderNotifications;
	BOOL fUseDoubleClickTimer;
	BOOL fShowSuperHidden;   /* only meaningful when queried */
	BOOL fShowAllObjects;
} SHELLSTATE;

#define SSF_SHOWALLOBJECTS     0x00000001
#define SSF_SHOWSUPERHIDDEN    0x00040000

/* tokens / privileges (stubs) */
#define TOKEN_ASSIGN_PRIMARY 0x0001
#define TOKEN_DUPLICATE      0x0002
#define TOKEN_IMPERSONATE    0x0004
#define TOKEN_QUERY          0x0008
#define TOKEN_ADJUST_PRIVILEGES 0x0020
#define SE_PRIVILEGE_ENABLED_BY_DEFAULT 0x00000001
#define SE_PRIVILEGE_ENABLED 0x00000002
#define ERROR_NOT_ALL_ASSIGNED 1300
#define SE_MANAGE_VOLUME_NAME "SeManageVolumePrivilege"

typedef struct _LUID {
	DWORD LowPart;
	LONG HighPart;
} LUID, *PLUID;

typedef struct _LUID_AND_ATTRIBUTES {
	LUID Luid;
	DWORD Attributes;
} LUID_AND_ATTRIBUTES;

typedef struct _TOKEN_PRIVILEGES {
	DWORD PrivilegeCount;
	LUID_AND_ATTRIBUTES Privileges[1];
} TOKEN_PRIVILEGES, *PTOKEN_PRIVILEGES;

/* ReplaceFile() flags */
#define REPLACEFILE_WRITE_THROUGH          0x1
#define REPLACEFILE_IGNORE_MERGE_ERRORS    0x2
#define REPLACEFILE_IGNORE_ACL_ERRORS      0x4

/* imagehlp PE checksum */
typedef struct _IMAGE_NT_HEADERS { int dummy; } IMAGE_NT_HEADERS, *PIMAGE_NT_HEADERS;

/* HRESULT helpers */
#define S_OK    ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_FAIL  ((HRESULT)0x80004005)
#define E_OUTOFMEMORY ((HRESULT)0x8000000E)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr)    (((HRESULT)(hr)) < 0)

/* ------------------------------------------------- Win32 API (implemented) */

/* --- path / string helpers --- */
DWORD  GetFullPathNameW_(LPCWSTR name, DWORD len, LPWSTR buf, LPWSTR *part);
#define GetFullPathName  GetFullPathNameW_
DWORD  GetLongPathNameW_(LPCWSTR src, LPWSTR dst, DWORD len);
#define GetLongPathName  GetLongPathNameW_
DWORD  GetShortPathNameW_(LPCWSTR src, LPWSTR dst, DWORD len);
#define GetShortPathName GetShortPathNameW_
DWORD  GetModuleFileNameW_(HMODULE mod, LPWSTR buf, DWORD len);
#define GetModuleFileName GetModuleFileNameW_

DWORD  GetFileAttributesW_(LPCWSTR path);
#define GetFileAttributes  GetFileAttributesW_
BOOL   GetFileAttributesExW_(LPCWSTR path, int infoLevel, LPVOID info);
#define GetFileAttributesEx GetFileAttributesExW_
#define GetFileAttributesExEx GetFileAttributesExW_
BOOL   SetFileAttributesW_(LPCWSTR path, DWORD attr);
#define SetFileAttributes SetFileAttributesW_
DWORD  GetDriveTypeW_(LPCWSTR root);
#define GetDriveType GetDriveTypeW_
BOOL   GetVolumeInformationW_(LPCWSTR root, LPWSTR vol, DWORD vollen, LPDWORD serial,
                              LPDWORD maxcomp, LPDWORD flags, LPWSTR fs, DWORD fslen);
#define GetVolumeInformation GetVolumeInformationW_

/* --- file IO --- */
HANDLE CreateFileW_(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                    DWORD disp, DWORD flags, HANDLE tmpl);
#define CreateFile CreateFileW_
BOOL   ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD read, LPOVERLAPPED ol);
BOOL   WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD written, LPOVERLAPPED ol);
BOOL   FlushFileBuffers(HANDLE h);
BOOL   CloseHandle(HANDLE h);
DWORD  SetFilePointer(HANDLE h, LONG dist, LONG *high, DWORD method);
BOOL   SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, PLARGE_INTEGER neu, DWORD method);
BOOL   SetEndOfFile(HANDLE h);
BOOL   SetFileValidData(HANDLE h, LONGLONG size);
DWORD  GetFileSize(HANDLE h, DWORD *high);
BOOL   GetFileSizeEx(HANDLE h, LARGE_INTEGER *size);
BOOL   GetFileInformationByHandle(HANDLE h, LPBY_HANDLE_FILE_INFORMATION info);
BOOL   GetOverlappedResult(HANDLE h, LPOVERLAPPED ol, LPDWORD transferred, BOOL wait);
BOOL   CancelIo(HANDLE h);
BOOL   CancelIoEx(HANDLE h, LPOVERLAPPED ol);
HANDLE CreateFileMappingW_(HANDLE h, LPSECURITY_ATTRIBUTES sa, DWORD protect,
                           DWORD maxhigh, DWORD maxlow, LPCWSTR name);
#define CreateFileMapping CreateFileMappingW_
LPVOID MapViewOfFile(HANDLE m, DWORD access, DWORD offhigh, DWORD offlow, size_t n);
BOOL   UnmapViewOfFile(LPCVOID p);
BOOL   DeviceIoControl(HANDLE h, DWORD code, LPVOID in, DWORD inlen,
                       LPVOID out, DWORD outlen, LPDWORD ret, LPOVERLAPPED ol);

/* --- directory / find --- */
BOOL   CreateDirectoryW_(LPCWSTR path, LPSECURITY_ATTRIBUTES sa);
#define CreateDirectory CreateDirectoryW_
BOOL   RemoveDirectoryW_(LPCWSTR path);
#define RemoveDirectory RemoveDirectoryW_
BOOL   DeleteFileW_(LPCWSTR path);
#define DeleteFile DeleteFileW_
BOOL   MoveFileW_(LPCWSTR from, LPCWSTR to);
#define MoveFile MoveFileW_
BOOL   MoveFileExW_(LPCWSTR from, LPCWSTR to, DWORD flags);
#define MoveFileEx MoveFileExW_
#define MOVEFILE_REPLACE_EXISTING 0x1
BOOL   ReplaceFileW_(LPCWSTR replaced, LPCWSTR replacement, LPCWSTR backup,
                     DWORD flags, LPVOID reserved1, LPVOID reserved2);
#define ReplaceFile ReplaceFileW_
HANDLE FindFirstFileW_(LPCWSTR pattern, WIN32_FIND_DATA *data);
#define FindFirstFile FindFirstFileW_
BOOL   FindNextFileW_(HANDLE h, WIN32_FIND_DATA *data);
#define FindNextFile FindNextFileW_
BOOL   FindClose(HANDLE h);

/* --- errors --- */
DWORD  GetLastError(void);
void   SetLastError(DWORD e);
DWORD  W32_ErrnoToWin32(int e);

/* --- time / threads / sync --- */
DWORD  GetTickCount(void);
void   Sleep(DWORD ms);
DWORD  WaitForSingleObject(HANDLE h, DWORD ms);
DWORD  WaitForMultipleObjects(DWORD count, const HANDLE *h, BOOL waitAll, DWORD ms);
BOOL   SetEvent(HANDLE h);
BOOL   ResetEvent(HANDLE h);
HANDLE CreateEventW_(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCWSTR name);
#define CreateEvent CreateEventW_
BOOL   GetExitCodeThread(HANDLE h, LPDWORD code);
DWORD  GetCurrentThreadId(void);
HANDLE GetCurrentProcess(void);
BOOL   GetProcessAffinityMask(HANDLE p, DWORD_PTR *process, DWORD_PTR *system);
BOOL   SetProcessAffinityMask(HANDLE p, DWORD_PTR mask);
void   GetSystemInfo(SYSTEM_INFO *si);
BOOL   GetLogicalProcessorInformation(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buf, DWORD *len);
BOOL   GlobalMemoryStatusEx(LPMEMORYSTATUSEX st);
BOOL   IsWow64Process(HANDLE p, BOOL *wow64);
void   GetSystemTimeAsFileTime(FILETIME *ft);
void   GetSystemTime(SYSTEMTIME *st);
BOOL   FileTimeToSystemTime(const FILETIME *ft, SYSTEMTIME *st);
BOOL   SystemTimeToFileTime(const SYSTEMTIME *st, FILETIME *ft);

/* Interlocked* -- macros so any integer/pointer width works (the sources pass
 * "volatile int *" while Windows would take "LONG volatile *"). */
#define InterlockedExchange(p, v) \
	__atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedIncrement(p) \
	__atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedDecrement(p) \
	__atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedAdd(p, v) \
	__atomic_add_fetch((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedCompareExchange(p, desired, expected) \
	__sync_val_compare_and_swap((p), (expected), (desired))
#define InterlockedOr(p, v)  __sync_or_and_fetch((p), (v))
#define InterlockedXor(p, v) __sync_xor_and_fetch((p), (v))
#define InterlockedAnd(p, v) __sync_and_and_fetch((p), (v))

/* --- encoding --- */
#define LOCALE_NAME_USER_DEFAULT NULL
#define NORM_IGNORECASE          0x00000001
#define SORT_DIGITSASNUMBERS     0x00000008
int  CompareStringEx(LPCWSTR locale, DWORD flags, LPCWSTR str1, int len1,
                     LPCWSTR str2, int len2, LPVOID reserved1, LPVOID reserved2,
                     LPARAM lParam);
int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR src, int srclen,
                        LPWSTR dst, int dstlen);
int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR src, int srclen,
                        LPSTR dst, int dstlen, LPCSTR defchar, BOOL *used);
UINT GetConsoleOutputCP(void);
UINT SetConsoleOutputCP(UINT cp);
UINT GetOEMCP(void);
UINT GetACP(void);

/* --- INI file --- */
DWORD GetPrivateProfileStringW_(LPCWSTR sec, LPCWSTR key, LPCWSTR def,
                                 LPWSTR out, DWORD n, LPCWSTR file);
#define GetPrivateProfileString GetPrivateProfileStringW_
UINT  GetPrivateProfileIntW_(LPCWSTR sec, LPCWSTR key, INT def, LPCWSTR file);
#define GetPrivateProfileInt GetPrivateProfileIntW_
BOOL  WritePrivateProfileStringW_(LPCWSTR sec, LPCWSTR key, LPCWSTR val, LPCWSTR file);
#define WritePrivateProfileString WritePrivateProfileStringW_

/* --- dynamic libraries / resources --- */
HMODULE LoadLibraryA(LPCSTR name);
HMODULE LoadLibraryW_(LPCWSTR name);
#define LoadLibrary LoadLibraryW_
BOOL    FreeLibrary(HMODULE m);
void   *GetProcAddress(HMODULE m, LPCSTR name);
HRSRC   FindResourceA(HMODULE m, LPCSTR name, LPCSTR type);
HGLOBAL LoadResource(HMODULE m, HRSRC r);
LPVOID  LockResource(HGLOBAL r);
DWORD   SizeofResource(HMODULE m, HRSRC r);
BOOL    FreeResource(HGLOBAL r);
#define MAKEINTRESOURCEA(i) ((LPCSTR)(uintptr_t)(uint16_t)(i))
#define MAKEINTRESOURCEW(i) ((LPCWSTR)(uintptr_t)(uint16_t)(i))

/* --- messages --- */
DWORD FormatMessageW_(DWORD flags, LPCVOID src, DWORD msgid, DWORD lang,
                      LPWSTR buf, DWORD size, void *args);
#define FormatMessage FormatMessageW_
DWORD FormatMessageA(DWORD flags, LPCVOID src, DWORD msgid, DWORD lang,
                     LPSTR buf, DWORD size, void *args);
HLOCAL LocalFree(HLOCAL p);

/* --- shell (stubs) --- */
int  SHFileOperationW_(SHFILEOPSTRUCT *op);
#define SHFileOperation SHFileOperationW_
void SHGetSetSettings(SHELLSTATE *ss, DWORD mask, BOOL set);
HRESULT DeleteItem(PCWSTR path);

/* --- privileges (stubs) --- */
BOOL OpenProcessToken(HANDLE p, DWORD access, HANDLE *token);
BOOL LookupPrivilegeValueA(LPCSTR sys, LPCSTR name, LUID *luid);
#define LookupPrivilegeValue LookupPrivilegeValueA
BOOL AdjustTokenPrivileges(HANDLE token, BOOL disable, PTOKEN_PRIVILEGES tp,
                           DWORD len, PTOKEN_PRIVILEGES prev, PDWORD needed);
typedef DWORD *PDWORD;

/* --- misc --- */
LPVOID LocalAlloc(UINT flags, size_t n);
int    _wcsicmp(const wchar_t *a, const wchar_t *b);
int    _wcsnicmp(const wchar_t *a, const wchar_t *b, size_t n);
int    _stricmp(const char *a, const char *b);
int    _strnicmp(const char *a, const char *b, size_t n);
/* _rotl/_rotr: provided by GCC's <immintrin.h>, included by prefix.h */
void   w32_set_error(DWORD e);

/* --- MSVC CRT helpers implemented in wincompat.c (renamed in prefix.h) --- */
FILE   *w32_wfopen(const wchar_t *name, const wchar_t *mode);
int     w32_kbhit(void);
int     w32_getch(void);
int     w32_getche(void);
wchar_t *w32_wcslwr(wchar_t *s);
void   *w32_aligned_malloc(size_t size, size_t align);
void    w32_aligned_free(void *p);
int     w32_printf(const char *fmt, ...);
int     w32_fwprintf(FILE *stream, const wchar_t *fmt, ...);
int     w32_swprintf(wchar_t *buf, size_t n, const wchar_t *fmt, ...);
int     w32_wsprintf(wchar_t *buf, const wchar_t *fmt, ...);
int     w32_wsprintfA(char *buf, const char *fmt, ...);
uintptr_t w32_beginthreadex(void *security, unsigned stack_size,
                            unsigned int (*start)(void *), void *arg,
                            unsigned initflag, unsigned *thrdaddr);
void      w32_endthreadex(unsigned retval);

/* internal entry points used by wincompat.c */
int    w32_main_entry(int argc, char **argv);
void   w32_path_to_posix(LPCWSTR wpath, char *out, size_t outlen);
void   w32_path_to_win(const char *in, wchar_t *out, size_t outlen);
/* PAR2 packets carry UTF-16LE; the host wchar_t is 4 bytes wide here. */
size_t utf16le_from_wcs(const wchar_t *src, unsigned char *dst);
int    utf16le_to_wcs(const unsigned char *src, size_t srclen, wchar_t *dst, size_t dstlen);

#ifdef __cplusplus
}
#endif

#endif /* WINCOMPAT_WINDOWS_H */
