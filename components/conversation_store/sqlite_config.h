#pragma once
/* Official SQLite 3.50.4 (2025-07-30), unmodified amalgamation.
 * https://sqlite.org/2025/sqlite-amalgamation-3500400.zip
 * Archive SHA256: 1d3049dd0f830a025a53105fc79fd2ab9431aea99e137809d064d8ee8356b032
 * sqlite3.c SHA3-256 verified against https://sqlite.org/releaselog/3_50_4.html:
 * 9145255e83da6529e70121ee4d7a4c88fe83ca4511da0c9ed13d10842df36782
 * Source ID: 2025-07-30 19:33:53 4d8adfb30e03f9cf27f800a2c1ba3c48fb4ca1b08b0f5ed59a4d5ecbf45e20a3
 * SQLite's public-domain dedication/notices are preserved verbatim in vendor/sqlite3.{c,h}.
 * This is a dedicated SQLite instance: never call its API outside the owning task.
 */
/* SQLite release builds omit debug-only variables along with assertions.
 * ESP's pragma-once assert.h otherwise evaluates NDEBUG expressions and can
 * capture assert before SQLite sets NDEBUG. Align only this translation unit
 * with SQLite's documented release semantics, not the rest of the firmware. */
#if defined(ARVEL_SQLITE_AMALGAMATION) && !defined(SQLITE_DEBUG)
#define NDEBUG 1
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#undef CONFIG_COMPILER_ASSERT_NDEBUG_EVALUATE
#define CONFIG_COMPILER_ASSERT_NDEBUG_EVALUATE 0
#endif
#endif
#define SQLITE_OS_OTHER 1
#define SQLITE_THREADSAFE 0
#define SQLITE_ENABLE_MEMSYS5 1
#define SQLITE_DEFAULT_MEMSTATUS 1
#define SQLITE_DEFAULT_CACHE_SIZE -24
#define SQLITE_DEFAULT_PAGE_SIZE 4096
#define SQLITE_DEFAULT_SYNCHRONOUS 2
#define SQLITE_DEFAULT_LOCKING_MODE 1
#define SQLITE_TEMP_STORE 3
#define SQLITE_MAX_MMAP_SIZE 0
#define SQLITE_MAX_ATTACHED 0
#define SQLITE_DQS 0
#define SQLITE_OMIT_LOAD_EXTENSION 1
#define SQLITE_OMIT_SHARED_CACHE 1
#define SQLITE_OMIT_UTF16 1
#define SQLITE_OMIT_DEPRECATED 1
#define SQLITE_OMIT_COMPILEOPTION_DIAGS 1
#define SQLITE_OMIT_JSON 1
/* WINDOWFUNC/CTE/VIRTUALTABLE omission needs a regenerated parser; the
 * official pre-generated amalgamation retains these features. */
#define SQLITE_OMIT_AUTOVACUUM 1
/* Deliberately retain sync, disk rollback journals, real truncate, integrity
 * checks, schema/versioning, constraints, transactions and query interruption. */
/* The pre-generated WAL implementation is retained; the checked VFS exposes
 * io_methods v1 without shared-memory hooks and the service requires PERSIST.
 * WAL cannot be enabled through this service or its FAT VFS. */
