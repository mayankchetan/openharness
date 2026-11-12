// The menu's pure model (dial-reach T10; menu_list.c), on a laptop: grouped lines, the scroll rule and the hit lookup.
//
//   devices/harness-device/firmware/test/run.sh
//
// The menu (the BOOT-hold list) has three groups (THIS AGENT, ON COMPUTER, DIAL) and each group has a dim label line before its first visible
// item. A label is a full list slot, so it costs one of the four visible lines. The scroll rule keeps a label from standing alone at the foot of
// the window (its rows would be below the fold): a window whose LAST line is a label is skipped, except the first window (offset 0).
#include "../main/ui/habitat/menu_list.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static int failures, checks;
static void check(int cond, const char *what)
{
    checks++;
    if (!cond) { printf("  FAIL %s\n", what); failures++; }
}

// The production table's shape: group and feature, in group order. Items 0..2 THIS AGENT, 3.. ON COMPUTER, then DIAL.
enum { F_FORM = 1, F_SELECTION = 2, F_VISIT = 4, F_RUNNING = 8 };
static unsigned mask;
static bool supports(uint32_t f) { return (mask & f) == f; }

static const menu_def_t T13[] = {   // before "All agents": ten items
    {MENU_GROUP_AGENT, 0}, {MENU_GROUP_AGENT, 0}, {MENU_GROUP_AGENT, F_VISIT},
    {MENU_GROUP_COMPUTER, F_FORM}, {MENU_GROUP_COMPUTER, F_FORM}, {MENU_GROUP_COMPUTER, F_SELECTION},
    {MENU_GROUP_DIAL, 0}, {MENU_GROUP_DIAL, 0}, {MENU_GROUP_DIAL, 0}, {MENU_GROUP_DIAL, 0},
};
static const menu_def_t T14[] = {   // with "All agents" first in ON COMPUTER: eleven items
    {MENU_GROUP_AGENT, 0}, {MENU_GROUP_AGENT, 0}, {MENU_GROUP_AGENT, F_VISIT},
    {MENU_GROUP_COMPUTER, F_RUNNING}, {MENU_GROUP_COMPUTER, F_FORM}, {MENU_GROUP_COMPUTER, F_FORM}, {MENU_GROUP_COMPUTER, F_SELECTION},
    {MENU_GROUP_DIAL, 0}, {MENU_GROUP_DIAL, 0}, {MENU_GROUP_DIAL, 0}, {MENU_GROUP_DIAL, 0},
};
#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define ROWS 4

// Compare a built list with a picture: 'L' a label line, a digit/letter an item index (0-9, a=10, b=11 ...).
static int same(const menu_line_t *lines, int n, const char *picture)
{
    if ((int)strlen(picture) != n) return 0;
    for (int i = 0; i < n; i++) {
        char c = picture[i];
        if (c == 'L') { if (lines[i].item != -1) return 0; }
        else {
            int want = c >= 'a' ? 10 + c - 'a' : c - '0';
            if (lines[i].item != want) return 0;
        }
    }
    return 1;
}
static int is_label(const menu_line_t *lines, int n, int i) { return i >= 0 && i < n && lines[i].item == -1; }
static int valid(const menu_line_t *lines, int n, int off)
{
    int last = n > ROWS ? n - ROWS : 0;
    if (off < 0 || off > last) return 0;
    return off == 0 || !is_label(lines, n, off + ROWS - 1);
}

int main(void)
{
    printf("menu_list\n");
    menu_line_t L[MENU_MAX_LINES];
    int n;

    // ── building ──────────────────────────────────────────────────────────────────────────────────────
    mask = F_FORM | F_SELECTION | F_VISIT;
    n = menu_build(T13, N(T13), supports, L, MENU_MAX_LINES);
    check(n == 13 && same(L, n, "L012L345L6789"), "all features: 13 lines, labels at 0, 4 and 8");
    check(L[0].group == MENU_GROUP_AGENT && L[4].group == MENU_GROUP_COMPUTER && L[8].group == MENU_GROUP_DIAL, "a label carries its group");
    check(L[1].group == MENU_GROUP_AGENT && L[7].group == MENU_GROUP_COMPUTER && L[12].group == MENU_GROUP_DIAL, "an item carries its group");

    mask = 0;
    n = menu_build(T13, N(T13), supports, L, MENU_MAX_LINES);
    check(n == 8 && same(L, n, "L01L6789"), "no feature: 8 lines, and the ON COMPUTER group and its label are gone");
    check(!is_label(L, n, 1) && is_label(L, n, 3), "the DIAL label follows THIS AGENT's two rows");

    mask = F_FORM;
    n = menu_build(T13, N(T13), supports, L, MENU_MAX_LINES);
    check(n == 11 && same(L, n, "L01L34L6789"), "only the FORM bit: a two-row ON COMPUTER group (Pick text hidden)");

    mask = F_SELECTION | F_FORM;
    n = menu_build(T13, N(T13), supports, L, MENU_MAX_LINES);
    check(n == 12 && same(L, n, "L01L345L6789"), "FORM and SELECTION without VISIT: THIS AGENT has two rows");

    mask = F_VISIT;
    n = menu_build(T13, N(T13), supports, L, MENU_MAX_LINES);
    check(n == 9 && same(L, n, "L012L6789"), "VISIT alone: the group of two feature rows stays hidden");

    {   // a group whose items are all hidden has no label, wherever it sits
        static const menu_def_t gated[] = {
            {MENU_GROUP_AGENT, F_VISIT}, {MENU_GROUP_AGENT, F_VISIT},
            {MENU_GROUP_COMPUTER, 0},
            {MENU_GROUP_DIAL, F_FORM},
        };
        mask = 0;
        n = menu_build(gated, N(gated), supports, L, MENU_MAX_LINES);
        check(n == 2 && same(L, n, "L2") && L[0].group == MENU_GROUP_COMPUTER, "a group with no visible row has no label");
        mask = F_VISIT | F_FORM;
        n = menu_build(gated, N(gated), supports, L, MENU_MAX_LINES);
        check(n == 7 && same(L, n, "L01L2L3"), "every group visible again");
    }

    n = menu_build(T13, N(T13), supports, L, 0);
    check(n == 0, "a zero capacity builds nothing");
    mask = F_FORM | F_SELECTION | F_VISIT;
    n = menu_build(T13, N(T13), supports, L, 5);
    check(n == 4 && same(L, n, "L012"), "the list is cut at the capacity, never written past it, and no label is left without a row");
    n = menu_build(T13, N(T13), supports, L, 6);
    check(n == 6 && same(L, n, "L012L3"), "a label is written only with room for its first row");
    n = menu_build(T13, 0, supports, L, MENU_MAX_LINES);
    check(n == 0, "no definitions, no lines");
    n = menu_build(T13, N(T13), NULL, L, MENU_MAX_LINES);
    check(n == 8 && same(L, n, "L01L6789"), "a NULL supports callback means no optional feature");

    // ── the 13-line list: scroll, clamp, hit lookup ───────────────────────────────────────────────────
    mask = F_FORM | F_SELECTION | F_VISIT;
    int n13 = menu_build(T13, N(T13), supports, L, MENU_MAX_LINES);
    {
        // valid offsets: 0 2 3 4 6 7 8 9 (1 ends on the ON COMPUTER label, 5 on the DIAL label)
        static const int want[] = {0, 2, 3, 4, 6, 7, 8, 9};
        int ok = 1, k = 0;
        for (int o = 0; o <= n13 - ROWS; o++) {
            int v = valid(L, n13, o);
            if (v) { if (k >= 8 || want[k++] != o) ok = 0; }
        }
        check(ok && k == 8, "the 13-line list has exactly the valid offsets 0 2 3 4 6 7 8 9");

        check(menu_scroll(L, n13, ROWS, 0, 1) == 2, "scroll +1 from 0 skips offset 1 and lands on 2");
        check(menu_scroll(L, n13, ROWS, 2, 1) == 3, "+1 again gives 3");
        check(menu_scroll(L, n13, ROWS, 3, 1) == 4, "3 to 4");
        check(menu_scroll(L, n13, ROWS, 4, 1) == 6, "4 skips 5 and lands on 6");
        check(menu_scroll(L, n13, ROWS, 0, 2) == 3, "two steps from 0 land on 3");
        check(menu_scroll(L, n13, ROWS, 0, 100) == 9, "a big step clamps at n - rows");
        check(menu_scroll(L, n13, ROWS, 9, 1) == 9 && menu_scroll(L, n13, ROWS, 9, 5) == 9, "no step past the end");
        check(menu_scroll(L, n13, ROWS, 2, -1) == 0, "-1 from 2 lands on 0 (skips 1)");
        check(menu_scroll(L, n13, ROWS, 6, -1) == 4, "-1 from 6 lands on 4 (skips 5)");
        check(menu_scroll(L, n13, ROWS, 0, -1) == 0 && menu_scroll(L, n13, ROWS, 0, -50) == 0, "an overscroll above stays at 0");
        check(menu_scroll(L, n13, ROWS, 9, -100) == 0, "a big step up clamps at 0");
        check(menu_scroll(L, n13, ROWS, 3, 0) == 3, "no steps, no move");
        // from every valid offset, any step lands on a valid offset, and the walk up then down is a path of valid offsets
        int all = 1;
        for (int o = 0; o <= n13 - ROWS; o++) {
            if (!valid(L, n13, o)) continue;
            for (int st = -12; st <= 12; st++) if (!valid(L, n13, menu_scroll(L, n13, ROWS, o, st))) all = 0;
        }
        check(all, "scroll always lands on a valid offset");

        check(menu_clamp(L, n13, ROWS, 1) == 0, "clamp: 1 is invalid, 0 is the nearest at or below");
        check(menu_clamp(L, n13, ROWS, 5) == 4, "clamp: 5 gives 4");
        check(menu_clamp(L, n13, ROWS, 99) == 9, "clamp: past the end gives the last valid offset");
        check(menu_clamp(L, n13, ROWS, -3) == 0, "clamp: negative gives 0");
        check(menu_clamp(L, n13, ROWS, 7) == 7 && menu_clamp(L, n13, ROWS, 0) == 0, "clamp: a valid offset is kept");
        check(menu_clamp(L, n13, ROWS, INT_MAX) == 9 && menu_clamp(L, n13, ROWS, INT_MIN) == 0, "clamp: extreme values");

        // the hit lookup: label rows and rows past the end give -1, every other row the item index, for EVERY valid offset
        int hits_ok = 1;
        for (int o = 0; o <= n13 - ROWS; o++) {
            if (!valid(L, n13, o)) continue;
            for (int r = 0; r < ROWS; r++) {
                int want = L[o + r].item;   // -1 for a label
                if (menu_item_at(L, n13, o, r) != want) hits_ok = 0;
            }
            if (menu_item_at(L, n13, o, ROWS) != (o + ROWS < n13 ? L[o + ROWS].item : -1)) hits_ok = 0;
        }
        check(hits_ok, "menu_item_at returns the item or -1 for a label, at every valid offset");
        check(menu_item_at(L, n13, 0, 0) == -1 && menu_item_at(L, n13, 0, 1) == 0, "the first window: a label, then Stop turn");
        check(menu_item_at(L, n13, 9, 3) == 9, "the last window ends on Settings");
        check(menu_item_at(L, n13, 20, 0) == -1 && menu_item_at(L, n13, 0, -1) == -1 && menu_item_at(L, n13, 0, 99) == -1, "rows out of range give -1");
        check(menu_item_at(L, n13, -1, 1) == -1, "a negative offset gives -1");
    }

    // ── the 8-line list (no optional feature) ─────────────────────────────────────────────────────────
    mask = 0;
    int n8 = menu_build(T13, N(T13), supports, L, MENU_MAX_LINES);
    {
        int ok = 1;
        for (int o = 0; o <= 4; o++) if (menu_clamp(L, n8, ROWS, o) != o) ok = 0;
        check(n8 == 8 && ok, "the 8-line list: every offset 0 to 4 is valid");
        check(menu_clamp(L, n8, ROWS, 5) == 4 && menu_clamp(L, n8, ROWS, 99) == 4, "the scroll limit follows the line count");
        check(menu_scroll(L, n8, ROWS, 0, 1) == 1 && menu_scroll(L, n8, ROWS, 0, 99) == 4, "scroll on 8 lines");
        // the DIAL label sits on line 3: window 0 ends on it and is the exempt first window, window 0 is valid
        check(is_label(L, n8, 3) && menu_clamp(L, n8, ROWS, 0) == 0, "the first window may end on a label");
        // a feature that appears after the menu was opened: the offset 9 of a 13-line list is invalid on 8 lines and clamps to 4
        check(menu_clamp(L, n8, ROWS, 9) == 4, "an offset kept from a longer list clamps");
    }
    // fewer lines than rows: always offset 0
    {
        static const menu_def_t tiny[] = {{MENU_GROUP_DIAL, 0}, {MENU_GROUP_DIAL, 0}};
        int nt = menu_build(tiny, N(tiny), supports, L, MENU_MAX_LINES);
        check(nt == 3 && menu_clamp(L, nt, ROWS, 2) == 0 && menu_scroll(L, nt, ROWS, 0, 3) == 0 && menu_scroll(L, nt, ROWS, 0, -3) == 0, "n <= rows always gives 0");
        check(menu_clamp(L, 0, ROWS, 5) == 0 && menu_scroll(L, 0, ROWS, 0, 5) == 0, "an empty list gives 0");
    }

    // ── the 14-line list with "All agents" (T7) ───────────────────────────────────────────────────────
    mask = F_FORM | F_SELECTION | F_VISIT | F_RUNNING;
    int n14 = menu_build(T14, N(T14), supports, L, MENU_MAX_LINES);
    {
        check(n14 == 14 && same(L, n14, "L012L3456L789a"), "with All agents: 14 lines, the row is line 5 and first in ON COMPUTER");
        static const int want[] = {0, 2, 3, 4, 5, 7, 8, 9, 10};
        int ok = 1, k = 0;
        for (int o = 0; o <= n14 - ROWS; o++) if (valid(L, n14, o)) { if (k >= 9 || want[k++] != o) ok = 0; }
        check(ok && k == 9, "the 14-line list has exactly the valid offsets 0 2 3 4 5 7 8 9 10");
        check(menu_scroll(L, n14, ROWS, 0, 1) == 2 && menu_scroll(L, n14, ROWS, 5, 1) == 7 && menu_scroll(L, n14, ROWS, 0, 99) == 10, "scroll on 14 lines");
        check(menu_clamp(L, n14, ROWS, 6) == 5 && menu_clamp(L, n14, ROWS, 1) == 0 && menu_clamp(L, n14, ROWS, 99) == 10, "clamp on 14 lines");
        int hits_ok = 1;
        for (int o = 0; o <= n14 - ROWS; o++) {
            if (!valid(L, n14, o)) continue;
            for (int r = 0; r < ROWS; r++) if (menu_item_at(L, n14, o, r) != L[o + r].item) hits_ok = 0;
        }
        check(hits_ok, "menu_item_at on 14 lines");
        // the RUNNING bit off: the row and nothing else goes
        mask = F_FORM | F_SELECTION | F_VISIT;
        int m = menu_build(T14, N(T14), supports, L, MENU_MAX_LINES);
        check(m == 13 && same(L, m, "L012L456L789a"), "without the RUNNING bit the row is gone and the rest keeps its order");
        mask = F_RUNNING;
        m = menu_build(T14, N(T14), supports, L, MENU_MAX_LINES);
        check(m == 10 && same(L, m, "L01L3L789a"), "RUNNING alone: ON COMPUTER holds only All agents");
    }

    // ── the Quiet row's minutes (T11): whole minutes left, rounded UP, 1..15, 0 once the time is over ────────
    {
        const uint32_t MIN = 60000u, t = 1000000u;
        check(menu_quiet_minutes(t, t + 15 * MIN) == 15, "quiet: 15 min left reads 15");
        check(menu_quiet_minutes(t, t + 14 * MIN + 1) == 15, "quiet: 14 min 1 ms left still reads 15 (rounded up)");
        check(menu_quiet_minutes(t, t + 14 * MIN) == 14, "quiet: exactly 14 min reads 14");
        check(menu_quiet_minutes(t, t + MIN + 1) == 2, "quiet: 60001 ms reads 2");
        check(menu_quiet_minutes(t, t + MIN) == 1, "quiet: 60000 ms reads 1");
        check(menu_quiet_minutes(t, t + 1) == 1, "quiet: 1 ms reads 1, never 0 while time is left");
        check(menu_quiet_minutes(t, t) == 0, "quiet: no time left reads 0");
        check(menu_quiet_minutes(t, t - 1) == 0 && menu_quiet_minutes(t, t - 20 * MIN) == 0, "quiet: past the end reads 0");
        check(menu_quiet_minutes(0xFFFFFF00u, 0xFFFFFF00u + 5 * MIN) == 5, "quiet: the 32-bit clock wraps (until is below now numerically)");
        check(menu_quiet_minutes(0xFFFFFF00u + 5 * MIN, 0xFFFFFF00u) == 0, "quiet: wrapped and past the end reads 0");
        check(menu_quiet_minutes(t, t + 16 * MIN) == 15 && menu_quiet_minutes(t, t + 100 * MIN) == 15, "quiet: never above 15");
        int ok = 1;
        for (uint32_t left = 1; left <= 15 * MIN; left += 977) {
            int m = menu_quiet_minutes(t, t + left);
            if (m < 1 || m > 15 || m != (int)((left + MIN - 1) / MIN)) ok = 0;
        }
        check(ok, "quiet: 1..15 and the ceiling of the minutes, over the whole run");
    }

    printf("  %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
