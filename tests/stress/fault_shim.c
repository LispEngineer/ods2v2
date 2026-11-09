/* MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
 *
 * fault_shim.c - write interruption for a build of ods2 that has no fault hook of its own (upstream's code).
 * Linked with -Wl,--wrap=fwrite: every fwrite to a stream other than stdout/stderr is one "write" (in ods2v2 that is one
 * ods2_write_block() or one chunk of file content, exactly what ods5-wr2's ODS2_FAULT_AFTER_WRITES counts).
 *   ODS2_FAULT_AFTER_WRITES=k  after the k-th write has been done and flushed the process ends at once with _Exit(99),
 *                              as a power cut would (nothing later is written, nothing is undone);
 *   ODS2_FAULT_COUNT=1         at exit "ODS2_FAULT: n writes" on stderr.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

size_t __real_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);

static long fault_count, fault_limit = -1;
static int fault_init, fault_report;

static void fault_atexit(void)
{
    if (fault_report) fprintf(stderr, "ODS2_FAULT: %ld writes\n", fault_count);
}

size_t __wrap_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    size_t r = __real_fwrite(ptr, size, nmemb, stream);
    if (stream == stdout || stream == stderr) return r;
    if (!fault_init) {
        const char *a = getenv("ODS2_FAULT_AFTER_WRITES");
        fault_init = 1;
        if (a != NULL) fault_limit = atol(a);
        if (getenv("ODS2_FAULT_COUNT") != NULL) {
            fault_report = 1;
            atexit(fault_atexit);
        }
    }
    fault_count++;
    if (fault_limit >= 0 && fault_count == fault_limit) {
        fflush(stream);
        fprintf(stderr, "ODS2_FAULT: ended after %ld writes\n", fault_count);
        _Exit(99);
    }
    return r;
}
