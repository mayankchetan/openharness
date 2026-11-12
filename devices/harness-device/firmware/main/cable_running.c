#include "cable_running.h"

#include <string.h>

#define SERIAL_MAX 0x7fffffffu   // the wire's limit for `request`

void cable_running_init(cable_running_t *m)
{
    memset(m, 0, sizeof *m);
}

uint32_t cable_running_request(cable_running_t *m, int offset)
{
    m->request = (m->request == 0 || m->request >= SERIAL_MAX) ? 1 : m->request + 1;
    if (!m->loaded || offset != m->offset) m->loaded = false;   // the shown page stays "loaded" only for a refresh of the same offset
    return m->request;
}

bool cable_running_apply(cable_running_t *m, uint32_t request, int offset, int total, const cable_running_row_t *rows, int n)
{
    if (request == 0 || request != m->request) return false;   // 0 is "none yet", never a legal reply
    if (n < 0 || n > CABLE_RUNNING_ROWS || total < 0 || offset < 0) return false;
    if (n > 0 && (!rows || offset > total - n)) return false;   // offset + n > total, written so it cannot overflow
    for (int i = 0; i < n; i++) m->rows[i] = rows[i];
    for (int i = n; i < CABLE_RUNNING_ROWS; i++) memset(&m->rows[i], 0, sizeof m->rows[i]);
    m->count = n;
    m->offset = offset;
    m->total = total;
    m->loaded = true;
    return true;
}

int cable_running_step(const cable_running_t *m, int delta_rows)
{
    const int last = m->total > CABLE_RUNNING_ROWS ? m->total - CABLE_RUNNING_ROWS : 0;
    long target = (long)m->offset + delta_rows;
    if (target > last) target = last;
    if (target < 0) target = 0;
    return (int)target;
}

running_status_t cable_running_status_parse(const char *s)
{
    if (!s) return RUNNING_IDLE;
    if (!strcmp(s, "working")) return RUNNING_WORKING;
    if (!strcmp(s, "question")) return RUNNING_NEEDS_YOU;
    return RUNNING_IDLE;
}
