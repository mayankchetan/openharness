#include "menu_list.h"

int menu_build(const menu_def_t *defs, int n_defs, bool (*supports)(uint32_t), menu_line_t *out, int cap)
{
    int n = 0, last_group = -1;
    if (!defs || !out || cap <= 0) return 0;
    for (int i = 0; i < n_defs; i++) {
        if (defs[i].feature && (!supports || !supports(defs[i].feature))) continue;
        const bool new_group = defs[i].group != last_group;
        if (n + (new_group ? 2 : 1) > cap) break;   // a label is written only with room for its first row
        if (new_group) {
            out[n++] = (menu_line_t){(int8_t)defs[i].group, -1};
            last_group = defs[i].group;
        }
        out[n++] = (menu_line_t){(int8_t)defs[i].group, (int16_t)i};
    }
    return n;
}

static int last_offset(int n, int rows)
{
    return n > rows ? n - rows : 0;
}

// Offset 0 is always valid; any other window must not END on a label.
static bool valid_offset(const menu_line_t *lines, int n, int rows, int offset)
{
    if (offset < 0 || offset > last_offset(n, rows)) return false;
    return offset == 0 || lines[offset + rows - 1].item != -1;
}

int menu_clamp(const menu_line_t *lines, int n, int rows, int offset)
{
    if (!lines || n <= 0 || rows <= 0 || offset <= 0) return 0;
    const int last = last_offset(n, rows);
    if (offset > last) offset = last;
    while (offset > 0 && !valid_offset(lines, n, rows, offset)) offset--;
    return offset;
}

int menu_scroll(const menu_line_t *lines, int n, int rows, int offset, int steps)
{
    offset = menu_clamp(lines, n, rows, offset);
    if (!lines || n <= 0 || rows <= 0 || !steps) return offset;
    const int dir = steps > 0 ? 1 : -1;
    const int last = last_offset(n, rows);
    for (long left = steps > 0 ? (long)steps : -(long)steps; left > 0; left--) {
        int next = offset + dir;
        while (next >= 0 && next <= last && !valid_offset(lines, n, rows, next)) next += dir;
        if (next < 0 || next > last) break;   // the end: never past it, and an overscroll builds nothing
        offset = next;
    }
    return offset;
}

int menu_item_at(const menu_line_t *lines, int n, int offset, int row)
{
    if (!lines || offset < 0 || row < 0) return -1;
    const long line = (long)offset + row;
    if (line >= n) return -1;
    return lines[line].item;   // -1 for a label
}
