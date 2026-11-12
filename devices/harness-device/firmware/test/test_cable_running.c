// The "All agents" page model (dial-reach T6), on a laptop: no LVGL, no cJSON, no ESP-IDF.
//
//   devices/harness-device/firmware/test/run.sh
//
// The dial holds ONE page of at most four rows and a serial for its latest request. The daemon answers a `running.get` with a
// `running.page` that echoes the serial; a reply for an older serial must not touch the model, because two quick drags send two
// requests and the older answer can arrive last (plan Review Focus 5).
#include "../main/cable_running.h"

#include <stdio.h>
#include <string.h>

static int failures, checks;
static void check(int cond, const char *what)
{
    checks++;
    if (!cond) { printf("  FAIL %s\n", what); failures++; }
}

static void rows_of(cable_running_row_t *r, int n, const char *tag)
{
    memset(r, 0, n * sizeof *r);
    for (int i = 0; i < n; i++) {
        snprintf(r[i].id, sizeof r[i].id, "%s-id%d", tag, i);
        snprintf(r[i].name, sizeof r[i].name, "%s-name%d", tag, i);
        snprintf(r[i].engine, sizeof r[i].engine, "claude");
        r[i].status = (running_status_t)(i % 3);
    }
}

int main(void)
{
    printf("cable_running\n");
    cable_running_row_t rows[CABLE_RUNNING_ROWS];

    // The page size is the daemon's RUNNING_PAGE_SIZE (test_running_empty.py reads the TypeScript and compares).
    check(CABLE_RUNNING_ROWS == 4, "four rows a page");

    // ── the request serial ────────────────────────────────────────────────────────────────────────────
    {
        cable_running_t m;
        cable_running_init(&m);
        check(m.request == 0 && !m.loaded && m.count == 0 && m.total == 0 && m.offset == 0, "init clears");
        uint32_t a = cable_running_request(&m, 0);
        uint32_t b = cable_running_request(&m, 0);
        check(a == 1 && b == 2 && m.request == 2, "serials count up from 1");
        m.request = 0x7ffffffeu;
        check(cable_running_request(&m, 0) == 0x7fffffffu, "the largest serial is 0x7fffffff");
        check(cable_running_request(&m, 0) == 1, "0x7fffffff wraps to 1, never 0 and never above the wire's limit");
        m.request = 0xffffffffu;   // a corrupted counter must still give a legal serial
        uint32_t c = cable_running_request(&m, 0);
        check(c >= 1 && c <= 0x7fffffffu, "a corrupted counter still gives a legal serial");
        for (int i = 0; i < 100; i++) check(cable_running_request(&m, 0) != 0, "never 0");
    }

    // ── apply: only the latest serial lands ───────────────────────────────────────────────────────────
    {
        cable_running_t m;
        cable_running_init(&m);
        uint32_t r1 = cable_running_request(&m, 0);
        uint32_t r2 = cable_running_request(&m, 4);
        rows_of(rows, 4, "p1");
        check(!cable_running_apply(&m, r1, 0, 10, rows, 4), "a reply for an older serial is dropped");
        check(!m.loaded && m.count == 0 && m.total == 0, "...and the model is untouched");
        check(cable_running_apply(&m, r2, 4, 10, rows, 4), "a reply for the latest serial is applied");
        check(m.loaded && m.count == 4 && m.offset == 4 && m.total == 10, "count, offset, total and loaded are set");
        check(!strcmp(m.rows[2].id, "p1-id2") && !strcmp(m.rows[3].name, "p1-name3") && !strcmp(m.rows[0].engine, "claude"), "rows are copied");
        check(m.rows[1].status == RUNNING_WORKING && m.rows[2].status == RUNNING_NEEDS_YOU, "status is copied");
        // an older reply arriving after the newer one changes nothing
        rows_of(rows, 4, "old");
        check(!cable_running_apply(&m, r1, 0, 99, rows, 4) && m.total == 10 && !strcmp(m.rows[0].id, "p1-id0"), "a late old reply keeps the shown page");
        // the same serial answered twice is idempotent
        rows_of(rows, 2, "p2");
        check(cable_running_apply(&m, r2, 8, 10, rows, 2) && m.count == 2 && m.offset == 8, "the reply's own offset is applied (the daemon may clamp)");
        // a short last page and an empty list
        check(cable_running_apply(&m, r2, 0, 0, rows, 0) && m.count == 0 && m.total == 0 && m.loaded, "total 0 after a page is a loaded, empty list");
    }

    // ── apply: invalid frames leave the model alone ───────────────────────────────────────────────────
    {
        cable_running_t m;
        cable_running_init(&m);
        uint32_t r = cable_running_request(&m, 0);
        rows_of(rows, 4, "ok");
        check(cable_running_apply(&m, r, 0, 4, rows, 4), "four rows, total 4, offset 0");
        cable_running_t before = m;
        cable_running_row_t big[CABLE_RUNNING_ROWS + 1];
        rows_of(big, CABLE_RUNNING_ROWS + 1, "big");
        check(!cable_running_apply(&m, r, 0, 10, big, 5), "five rows are rejected");
        check(!cable_running_apply(&m, r, 0, 10, rows, -1), "n below 0 is rejected");
        check(!cable_running_apply(&m, r, 0, -1, rows, 0), "a negative total is rejected");
        check(!cable_running_apply(&m, r, -1, 10, rows, 4), "a negative offset is rejected");
        check(!cable_running_apply(&m, r, 8, 10, rows, 4), "offset + n above total is rejected");
        check(!cable_running_apply(&m, r, 7, 10, rows, 4), "offset 7 + 4 rows is 11, above 10: rejected");
        check(memcmp(&m, &before, sizeof m) == 0, "none of those touched the model");
        check(cable_running_apply(&m, r, 6, 10, rows, 4), "offset 6 + 4 rows is exactly total 10: accepted");
        check(cable_running_apply(&m, r, 40, 3, rows, 0), "no rows with an offset past the total is accepted (the daemon clamps, an empty page shows)");
    }

    // ── a request keeps the shown page when the offset is the same ────────────────────────────────────
    {
        cable_running_t m;
        cable_running_init(&m);
        uint32_t r = cable_running_request(&m, 0);
        rows_of(rows, 4, "k");
        cable_running_apply(&m, r, 0, 10, rows, 4);
        cable_running_request(&m, 0);
        check(m.loaded && m.count == 4, "a refresh of the same offset keeps the page on screen");
        cable_running_request(&m, 4);
        check(!m.loaded, "a request for another offset is not loaded until it answers");
        check(m.count == 4 && m.total == 10, "(the rows stay for the screen to draw until the reply)");
    }

    // ── step ──────────────────────────────────────────────────────────────────────────────────────────
    {
        cable_running_t m;
        cable_running_init(&m);
        check(cable_running_step(&m, 1) == 0 && cable_running_step(&m, -1) == 0 && cable_running_step(&m, 100) == 0, "total 0 steps to 0");
        uint32_t r = cable_running_request(&m, 0);
        rows_of(rows, 4, "s");
        cable_running_apply(&m, r, 0, 10, rows, 4);
        check(cable_running_step(&m, 1) == 1, "one row down");
        check(cable_running_step(&m, 4) == 4 && cable_running_step(&m, 100) == 6, "clamped at total - 4");
        check(cable_running_step(&m, -1) == 0, "clamped at 0 above");
        r = cable_running_request(&m, 6);
        cable_running_apply(&m, r, 6, 10, rows, 4);
        check(cable_running_step(&m, 1) == 6 && cable_running_step(&m, 50) == 6, "no step past the end");
        check(cable_running_step(&m, -2) == 4 && cable_running_step(&m, -100) == 0, "back up");
        r = cable_running_request(&m, 0);
        cable_running_apply(&m, r, 0, 3, rows, 3);
        check(cable_running_step(&m, 5) == 0, "fewer rows than a page never steps");
        cable_running_apply(&m, r, 0, 4, rows, 4);
        check(cable_running_step(&m, 1) == 0, "exactly a page never steps");
    }

    // ── status words ──────────────────────────────────────────────────────────────────────────────────
    check(cable_running_status_parse("working") == RUNNING_WORKING, "working");
    check(cable_running_status_parse("question") == RUNNING_NEEDS_YOU, "question is needs you");
    check(cable_running_status_parse("idle") == RUNNING_IDLE, "idle");
    check(cable_running_status_parse("Working") == RUNNING_IDLE && cable_running_status_parse("") == RUNNING_IDLE &&
          cable_running_status_parse("done") == RUNNING_IDLE && cable_running_status_parse(NULL) == RUNNING_IDLE, "anything else is idle");

    // ── init is the clear ─────────────────────────────────────────────────────────────────────────────
    {
        cable_running_t m;
        cable_running_init(&m);
        uint32_t r = cable_running_request(&m, 0);
        rows_of(rows, 4, "c");
        cable_running_apply(&m, r, 0, 9, rows, 4);
        cable_running_init(&m);
        check(m.count == 0 && m.total == 0 && m.offset == 0 && m.request == 0 && !m.loaded && !m.rows[0].id[0], "init after use clears everything");
        check(!cable_running_apply(&m, 0, 0, 0, rows, 0), "serial 0 (none yet) is never a match for a reply");
    }

    printf("  %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
