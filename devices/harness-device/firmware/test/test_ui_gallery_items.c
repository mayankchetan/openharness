// The UI gallery's tour table (main/ui/habitat/ui_gallery_items.h): names, holds and the screens it covers.
// Compile with -I main/ui/habitat; no firmware needed.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ui_gallery_items.h"

typedef struct { const char *name; int animated; int hold_ms; const char *view, *source; } item_t;
#define NULL_STEP NULL
#define step_alert NULL
#define step_slide_panes NULL
#define step_slide_tabs NULL
#define step_slide_inbox NULL
#define step_reader_last NULL
#define step_level NULL
#define step_sleep NULL
#define step_cycle NULL
#define X(sym, name, anim, hold, view, source, step) {name, anim, hold, view, source},
static const item_t items[] = { UI_GALLERY_ITEMS(X) };
#undef X

int main(void)
{
    const int count = (int)(sizeof items / sizeof items[0]);
    assert(count >= 100);
    // View names (habitat_view_name): the 20 view_t values, HOME split by what render_home draws.
    static const char *const views[] = {
        "panes", "agent", "reader", "question", "choice", "answer_review", "inbox", "tabs", "machines", "voice",
        "selection", "form", "draft", "draft_options", "settings", "stop", "models", "message", "ota"};
    static const char *const home[] = {"brand", "home_rest", "home", "home_recap", "home_busy", "home_question", "workspace",
                                       "swarm"};
    for (unsigned v = 0; v < sizeof views / sizeof views[0]; v++) {
        int seen = 0;
        for (int i = 0; i < count; i++) seen += !strcmp(items[i].view, views[v]);
        assert(seen && "every view_t value has an item");
    }
    for (unsigned v = 0; v < sizeof home / sizeof home[0]; v++) {
        int seen = 0;
        for (int i = 0; i < count; i++) seen += !strcmp(items[i].view, home[v]);
        assert(seen && "every home face has an item");
    }
    int animated = 0;
    for (int i = 0; i < count; i++) {
        const item_t *it = &items[i];
        size_t n = strlen(it->name);
        assert(n >= 3 && n <= 19);   // uim_record_t.view is 20 bytes
        for (const char *c = it->name; *c; c++) assert((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_');
        for (int j = 0; j < i; j++) assert(strcmp(items[j].name, it->name));
        assert(it->hold_ms >= (it->animated ? 3000 : 2000) && it->hold_ms <= 15000);
        assert(it->source && it->source[0]);
        animated += it->animated;
    }
    assert(animated >= 40);
    long total = 0;
    for (int i = 0; i < count; i++) total += items[i].hold_ms;
    printf("ui gallery items: %d items (%d animated), every view_t value and home face covered, %ld s of holds PASS\n",
           count, animated, total / 1000);
    return 0;
}
