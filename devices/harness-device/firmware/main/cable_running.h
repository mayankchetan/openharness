// The "All agents" page model, with no LVGL, no cJSON and no ESP-IDF in it (dial-reach T6).
//
// The dial holds ONE page of at most four rows, never the whole list of running agents (the list can be longer than the dial's 16
// tiles). It asks the daemon for a page with `running.get {offset, request}` and the daemon answers `running.page {request, offset,
// total, rows}`. Every request carries a serial; a reply for any other serial is dropped, because two quick drags send two
// requests and the older answer can arrive last.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Rows in one page. Equals RUNNING_PAGE_SIZE in cli/src/cable/runningAgents.ts (test_running_empty.py compares the two).
#define CABLE_RUNNING_ROWS 4

typedef enum { RUNNING_IDLE, RUNNING_WORKING, RUNNING_NEEDS_YOU } running_status_t;

// Sizes follow the wire: an id is 47 + nul like ID_MAX, a name is at most 95 bytes + nul, an engine at most 15 + nul.
typedef struct {
    char id[48];
    char name[96];
    char engine[16];
    running_status_t status;
} cable_running_row_t;

typedef struct {
    cable_running_row_t rows[CABLE_RUNNING_ROWS];
    int count, offset, total;
    uint32_t request;   // serial of the latest request; 0 = none yet
    bool loaded;        // a page for `request` has been applied
} cable_running_t;

// Clears everything. It is also the "session down" clear.
void cable_running_init(cable_running_t *m);

// Starts a request: returns its serial, 1..0x7fffffff (the wire's range; it wraps from 0x7fffffff to 1, never 0). `loaded` becomes false only
// when `offset` is not the offset of the page now shown, so a refresh of the same page keeps it on screen.
uint32_t cable_running_request(cable_running_t *m, int offset);

// Applies a reply. False (and the model untouched) when the reply is not for the latest request, or is invalid: more than CABLE_RUNNING_ROWS
// rows, a negative total, offset or count, or rows that run past the total. The reply's own offset is the one that counts (the daemon clamps).
bool cable_running_apply(cable_running_t *m, uint32_t request, int offset, int total, const cable_running_row_t *rows, int n);

// The offset a drag of `delta_rows` rows leads to from the page now shown, clamped to 0..max(0, total - CABLE_RUNNING_ROWS).
int cable_running_step(const cable_running_t *m, int delta_rows);

// "working" and "question" (needs you); anything else, NULL included, is idle.
running_status_t cable_running_status_parse(const char *s);
