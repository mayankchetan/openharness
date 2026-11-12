// The controls list's pure model (ui_habitat.c, "MENU"): three groups with a dim label line before each group's first visible row, the scroll rule that keeps
// a label from standing alone at the foot of the window, and the lookup that says which row a tap landed on. No LVGL, no globals, no clock: a host test
// (test_menu_list.c) runs every rule.
//
//   groups   THIS AGENT (acts on the agent on the glass), ON COMPUTER (reaches agents and text on the computer), DIAL (the dial itself). The table
//            is in group order. A row that needs a host feature is hidden when the host lacks it; a group with no visible row has no label.
//   lines    the label lines and the visible rows, in order. A label is a full list slot (the same height as a row), so it costs one of the four
//            visible lines (user decision 2026-11-12, plan D3).
//   scroll   one line a step. A window whose LAST line is a label is skipped (its rows would be below the fold), except the first window (offset 0).
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { MENU_GROUP_AGENT, MENU_GROUP_COMPUTER, MENU_GROUP_DIAL, MENU_GROUPS } menu_group_t;

typedef struct {
    uint8_t group;      // a menu_group_t; the table is sorted by it
    uint32_t feature;   // the host feature the row needs; 0 = always shown
} menu_def_t;

typedef struct {
    int8_t group;
    int16_t item;       // the index into the definitions; -1 = the group's label
} menu_line_t;

// Lines the model can hold: the table's rows plus one label per group (a compile-time check in ui_habitat.c).
#define MENU_MAX_LINES 16

// Builds the lines. Rows keep table order. A label comes before the first VISIBLE row of its group, and only when there is room for that row too.
// `supports` says whether the host has a feature (NULL: none). Returns the line count, at most `cap`.
int menu_build(const menu_def_t *defs, int n_defs, bool (*supports)(uint32_t), menu_line_t *out, int cap);

// The nearest valid offset at or below `offset`. Valid offsets are 0..max(0, n - rows) except a window whose last line is a label (offset 0 excepted).
int menu_clamp(const menu_line_t *lines, int n, int rows, int offset);

// Moves `steps` lines (negative: up), jumping over invalid offsets and clamping at both ends. The result is always a valid offset.
int menu_scroll(const menu_line_t *lines, int n, int rows, int offset, int steps);

// The definition index of the row drawn at `row` (0-based) of the window starting at `offset`; -1 for a label, a row past the end or an offset out of range.
int menu_item_at(const menu_line_t *lines, int n, int offset, int row);

// The Quiet row's count: whole minutes left until `until_ms`, rounded UP and kept within 1..15; 0 when no time is left. The dial has no wall clock (only its
// millisecond timer), so the row says "Quiet NN min left", never "until HH:MM". Wrap-safe: the two times are 32-bit milliseconds on the same clock.
int menu_quiet_minutes(uint32_t now_ms, uint32_t until_ms);
