// THE UI GALLERY (-DDEVICE_UI_GALLERY=1): a UI-only image that walks every screen and every animation of the firmware once
// per tour, on a fixture model, so the renderer's limits and the internal RAM of each screen can be read off the dial.
//
// Included at the end of ui_habitat.c, because the tour drives the model the way its own callers do (view(), the ui_*
// setters, the ht_* state machines) and those are file-local. Nothing here is compiled otherwise.
//
// What runs: board, display, renderer, touch, this UI, the cable link (hello, keepalive, debug.shot), and the tour task.
// What does not: audio and the codecs, ptt, beep, the refresh task, any agent or session logic, OTA, radios. The tour
// never writes NVS, never starts the microphone (gallery_recording stands in for the recorder, ui_habitat.c top) and never
// speaks to the daemon beyond log lines; queue() drops every action, so a touch during the tour changes nothing outside
// the glass.
//
// The tour (ui_gallery_items.h): for each item, reset the model to a quiet home face, begin a UIM record for the item,
// drive the model into the item's state, hold it for its natural duration (a full animation cycle, at least 3 s; 2 s for a
// still screen), log "UIM_TOUR shot_ready" and wait up to 4 s for the host's debug.shot (or any other debug command) to
// finish, then write the item's UIM line and "UIM_TOUR item=... end". The first tour starts 20 s after boot and repeats
// after a pause.
#include "ui_gallery.h"
#include "uim.h"
#include "ui_gallery_items.h"
#include "../../cable_debug.h"
#include "../../ram_telemetry.h"

#ifndef UI_GALLERY_START_S
#define UI_GALLERY_START_S 20     // first tour this many seconds after boot (the daemon has said welcome by then)
#endif
#ifndef UI_GALLERY_PAUSE_S
#define UI_GALLERY_PAUSE_S 120    // between the end of one tour and the start of the next
#endif
#define SHOT_WAIT_MS 4000

static const char *TAG_TOUR = "tour";

// ── the fixture: sixteen agents, the longest names the model takes, one engine each ──────────────────────────────────
static char G_ID[16][8];
static const char *const G_ENGINE[16] = {"claude", "codex", "muse", "fo", "cursor", "opencode", "grok", "copilot",
                                         "amp", "devin", "kilo", "pi", "hermes", "agy", "commandcode", "zzz"};
static const char *const G_NAME[16] = {
    "Harness dial", "Cable link refactor", "Muse sketchbook", "fo field notes", "Review the diff", "Docs pass",
    "Investigate flaky websocket reconnects!",            // 39 bytes: CABLE_NAME_MAX - 1
    "\xe1\xba\xa4\xe1\xba\xa6\xe1\xba\xa8\xe1\xba\xaa\xe1\xba\xac\xe1\xba\xbe\xe1\xbb\x80\xe1\xbb\x82\xe1\xbb\x84"
    "\xe1\xbb\x86\xe1\xbb\x90\xe1\xbb\x92\xe1\xbb\x94",   // 13 stacked Vietnamese capitals, 39 bytes
    "Amp review", "Devin deploy", "Kilo bench", "Pi notes", "Hermes relay", "Agy sandbox", "Command code", "Unknown engine"};
// 24 stacked Vietnamese capitals (72 bytes): the arc mask's worst case (terminal.c ARC_MASK_BYTES comment). An agent name
// cannot be this long (39 bytes), so the arc_worst item draws it straight on both arcs.
static const char G_VIET24[] =
    "\xe1\xba\xa4\xe1\xba\xa6\xe1\xba\xa8\xe1\xba\xaa\xe1\xba\xac\xe1\xba\xbe\xe1\xbb\x80\xe1\xbb\x82\xe1\xbb\x84"
    "\xe1\xbb\x86\xe1\xbb\x90\xe1\xbb\x92\xe1\xbb\x94\xe1\xbb\x96\xe1\xbb\x98\xe1\xbb\x9a\xe1\xbb\x9c\xe1\xbb\x9e"
    "\xe1\xbb\xa0\xe1\xbb\xa2\xe1\xbb\xa8\xe1\xbb\xaa\xe1\xbb\xac\xe1\xbb\xae";
static const char G_RECAP[] =
    "Rewrote the cable link reader into three small functions, added a regression test for the 32 KiB ring wrap, and "
    "rebuilt the dial image. All 66 host checks pass.";
// 945 bytes: about ten reader pages at five rows each.
static const char G_FULL[] =
    "Rebuilt the cable link reader around three small functions: one that waits for bytes, one that decodes a frame, and "
    "one that hands the frame to the session. The 32 KiB receive ring now keeps its read index in a single atomic word, so "
    "the wrap case no longer needs a lock. I added a regression test that fills the ring to the byte, drains it in odd "
    "sized chunks, and checks every payload against a generated vector. Next I ran the host suite: sixty six checks pass, "
    "and the three that fail on this Windows machine fail the same way on the base commit. The dial image builds with no "
    "new warnings. Internal RAM at boot is unchanged, and the reader task stack still has more than a kilobyte spare under "
    "load. What is left: the pet pack path still copies each slice twice, which is worth a look once the wireless link "
    "exists. I did not touch the protocol, the hello fields or the debug commands. Tell me if you want the change split "
    "into two commits.";
static const char *const G_SUMMARY[8] = {
    "Refactored the cable link reader and added a ring wrap test.",
    "Build failed: component main has 3 undefined references.",
    "Which branch do we ship, main or the release candidate?",
    "Done. 14 files changed, 212 insertions, 38 deletions; host tests pass.",
    "Needs your answer: delete the old pet packs from the dial?",
    "A very long result that goes on and on so the inbox card has to wrap across four lines and then cut the last one with "
    "an ellipsis because the glass is small and round and only so many words fit between the chords of the circle.",
    "ok",
    "The turn failed: tool call timed out after 120 s."};

// COPY() of a literal trips the compiler's strnlen bound check once inlined; snprintf does not.
#define GCOPY(dst, src) snprintf((dst), sizeof(dst), "%s", (src))
static void g_wait(uint32_t ms_) { vTaskDelay(pdMS_TO_TICKS(ms_)); }
static const char *g_id(int i) { return G_ID[i]; }

// The agent at `i` goes quiet: no work, no recap, no question.
static void g_agent_quiet(int i)
{
    agent_t *a = &s.agents[i];
    a->busy = false;
    a->tool[0] = 0;
    a->preview[0] = a->full[0] = 0;
    a->has_event = a->recap_ready = false;
    a->tokens = 0;
    pane_memory_t *m = pane_memory(a->id, false);
    if (m) {
        m->busy = m->awaiting_result = m->dismissed = m->live_summary = false;
        m->activity[0] = m->preview[0] = m->full[0] = 0;
    }
}
static void g_busy(int i, const char *tool) { ui_project_emit(g_id(i), "g-sess", "processing", tool, NULL); }
static void g_recap(int i)
{
    ui_project_emit(g_id(i), "g-sess", "done", G_FULL, G_RECAP);
    ui_project_full(g_id(i), G_FULL, false);
}

// The inbox: `ask` >= 0 gives that agent an open question; `others` unread notices from other agents (0..23).
static void g_notices(int ask, int others)
{
    static EXT_RAM_BSS_ATTR cable_notif_t rows[NOTICES];
    memset(rows, 0, sizeof rows);
    int n = 0;
    if (ask >= 0) {
        cable_notif_t *r = &rows[n++];
        GCOPY(r->agent_id, g_id(ask)); GCOPY(r->name, G_NAME[ask]); GCOPY(r->machine, "This computer");
        GCOPY(r->summary, G_SUMMARY[2]); GCOPY(r->read_token, "ask-0"); r->question = true;
    }
    for (int k = 0; k < others && n < NOTICES; k++) {
        int agent = 1 + k % 15;
        cable_notif_t *r = &rows[n];
        if (agent == ask) continue;
        if (k < 15) GCOPY(r->agent_id, g_id(agent)); else snprintf(r->agent_id, sizeof r->agent_id, "x%02d", k);
        if (k < 15) GCOPY(r->name, G_NAME[agent]); else snprintf(r->name, sizeof r->name, "Agent %02d", k);
        GCOPY(r->machine, k % 3 ? "This computer" : "Studio Mac");
        GCOPY(r->summary, G_SUMMARY[k % 8]);
        snprintf(r->read_token, sizeof r->read_token, "n-%d", k);
        r->question = k % 7 == 3;
        r->failed = k % 11 == 5;
        n++;
    }
    ui_notif_replace(rows, n);
}

// ── the model's other fixtures ───────────────────────────────────────────────────────────────────────────────────────
static void g_swarm_put(const char *s1, const char *s2, const char *s3, const char *s4)
{
    static EXT_RAM_BSS_ATTR ht_supertab_t t;
    const char *state[4] = {s1, s2, s3, s4};
    static const char *const project[4] = {"Cable link", "Dial UI", "Docs", "Bench"};
    static const char *const engine[4] = {"claude", "codex", "muse", "fo"};
    static const char *const lead[4] = {"L1", "L2", "L3", "L4"};
    ht_supertab_init(&t, "sw1", "", "Release swarm", 5);
    for (int i = 0; i < 4; i++) {
        ht_lead_in_t in = {.id = lead[i], .project_id = project[i], .project = project[i], .engine = engine[i],
                           .owner = "Mayank", .local = G_ID[i + 1], .state = state[i], .order = i,
                           .since = i == 0 ? 125 : -1};
        ht_supertab_lead_add(&t, &in);
    }
    ui_supertab(&t);
    ht_supertab_init(&t, "sw2", "", "Empty swarm", 0);
    ui_supertab(&t);
}
static void g_swarm_go(const char *tab, const char *lead)
{
    display_lock();
    swarm_enter(ht_supertabs_find(&swarm, tab), lead, false);
    display_unlock();
}
static void g_roster(void)
{
    ui_projects_bulk_begin();
    for (int i = 0; i < 16; i++) {
        ui_project_set_name(g_id(i), G_NAME[i]);
        ui_project_set_engine(g_id(i), G_ENGINE[i]);
        ui_project_set_machine(g_id(i), i % 3 ? "m-local" : "m-studio", i % 3 ? "This computer" : "Studio Mac");
    }
    ui_projects_bulk_end();
}
static void g_tabs(void)
{
    static const char *const tab_names[6] = {"Main", "Docs and design", "Bench", "A very long workspace name that wraps",
                                             "Ana's sandbox", "Release"};
    static cable_swarm_t tabs[6];
    for (int i = 0; i < 6; i++) {
        snprintf(tabs[i].id, sizeof tabs[i].id, "t%d", i);
        GCOPY(tabs[i].name, tab_names[i]);
        tabs[i].agents = tabs[i].panes = 3 + i;
    }
    ui_swarms_replace(tabs, 6, "t0");
    g_swarm_put("work", "work", "done", "done");
    static const char ids[2][ID_MAX] = {"sw1", "sw2"};
    ui_supertabs_ids(ids, 2);
}
static void g_machines(void)
{
    static cable_machine_t machines[10];
    static const char *const mstate[4] = {"ready", "offline", "unknown", "needs-link"};
    for (int i = 0; i < 10; i++) {
        snprintf(machines[i].id, sizeof machines[i].id, "m%d", i);
        snprintf(machines[i].name, sizeof machines[i].name, i == 0 ? "This computer" : i == 3 ? "A machine with a very long name" : "Machine %d", i);
        GCOPY(machines[i].state, mstate[i % 4]);
        machines[i].local = i == 0;
    }
    ui_machines_replace(machines, 10, "m0", NULL);
}
static void g_fixture(void)
{
    for (int i = 0; i < 16; i++) snprintf(G_ID[i], sizeof G_ID[i], "g%02d", i);
    ui_set_connected(true);
    g_roster();
    g_tabs();
    g_machines();
    display_lock();
    static const char *const runtime[8] = {"claude:opus-4@high", "claude:opus-4@medium", "claude:sonnet-4@high",
                                           "claude:sonnet-4@low", "codex:gpt-5@high", "codex:gpt-5@medium",
                                           "muse:muse-1@default", "fo:fo-1@default"};
    for (int i = 0; i < 8; i++) snprintf(s.models[i].id, sizeof s.models[i].id, "runtime-v1:g-sess:%s", runtime[i]);
    display_unlock();
}

// ── a quiet home face between items ──────────────────────────────────────────────────────────────────────────────────
static void g_reset(void)
{
    if (display_is_asleep()) display_wake();
    display_lock();
    gallery_recording = gallery_active = false;
    gallery_level = 0;
    gallery_synthetic = NULL;
    voice_close();
    s.voice_open = s.voice_review = s.voice_review_preview = s.voice_search = s.voice_carry = s.voice_draft_append = false;
    s.voice_return = HOME;
    s.voice_retry_until = 0;
    ht_selection_close(&selection);
    ht_carry_close(&carry);
    ht_visit_close(&visit);
    ht_form_reset(&form);
    ht_draft_reset(&draft);
    ht_workspace_cancel_touch(&workspace);
    ht_workspace_cancel_request(&workspace);
    s.q.valid = s.q.loading = s.q.pending = s.q.uncertain = false;
    s.q.error[0] = s.q.speech_error[0] = 0;
    s.q.revision++;
    s.nap = false;
    s.pet_pose = 0;
    s.error_until = 0;
    s.loading = false;
    s.connected = true;
    s.pending_machine[0] = 0;
    s.model_request = false;
    if (swarm_mode()) { sw.tab[0] = sw.lead[0] = 0; sw.at = 0; ht_supertabs_recap_open(&swarm, "", "", ms()); }
    for (int i = 0; i < 16; i++) g_agent_quiet(i);
    s.bell_unread = 0;
    s.notice_ms = 0;
    display_unlock();
    ui_supertab_wait_close(g_id(2), "supertab:r1");
    if (ui_project_count() != 16) { ui_project_clear_all(); g_roster(); }
    g_tabs();         // an item may have emptied them (tabs_empty)
    g_machines();     // likewise (machines_empty)
    g_notices(-1, 0);
    display_lock();
    s.active = 0;
    view(HOME);
    s.offset = 0;
    change();
    display_unlock();
}

// ── the setups ───────────────────────────────────────────────────────────────────────────────────────────────────────
#define SETUP(sym) static void setup_##sym(void)
#define LOCKED(code) do { display_lock(); code; change(); display_unlock(); } while (0)

static void g_show(int agent) { LOCKED(s.active = agent; view(HOME)); }
static void g_idle(int agent) { g_show(agent); }
static void g_with_recap(int agent) { g_recap(agent); g_show(agent); }
static void g_working(int agent, const char *tool, bool alt)
{
    g_show(agent);
    g_busy(agent, tool);
    // The builder or the chef, not a coin flip (focus.c ht_focus_pick_working keeps what is set for this agent).
    display_lock();
    memset(&s.working_pick, 0, sizeof s.working_pick);
    s.working_pick.shown = true;
    s.working_pick.alt = alt;
    snprintf(s.working_pick.agent, sizeof s.working_pick.agent, "%s", g_id(agent));
    change();
    display_unlock();
}
static void g_asking(int agent) { g_notices(agent, 0); g_show(agent); }

SETUP(brand) { LOCKED(s.connected = false; view(HOME)); }
SETUP(brand_loading) { LOCKED(s.loading = true; view(HOME)); }
SETUP(home_rest) { LOCKED(s.active = -1; view(HOME)); }
SETUP(idle_claude) { g_idle(0); }
SETUP(idle_codex) { g_idle(1); }
SETUP(idle_muse) { g_idle(2); }
SETUP(idle_fo) { g_idle(3); }
SETUP(idle_nopet) { g_idle(4); }
SETUP(recap_claude) { g_with_recap(0); }
SETUP(recap_fo) { g_with_recap(3); }
SETUP(recap_nopet) { g_with_recap(4); }
SETUP(busy_claude_chef) { g_working(0, "Editing cable_link.c", false); }
SETUP(busy_claude_build) { g_working(0, "Editing cable_link.c", true); }
SETUP(busy_codex) { g_working(1, "Running the host tests", false); }
SETUP(busy_muse) { g_working(2, "Sketching", false); }
SETUP(busy_fo) { g_working(3, "Taking notes", false); }
SETUP(busy_nopet) { g_working(4, "Reviewing the diff", false); }
SETUP(asking_claude) { g_asking(0); }
SETUP(asking_codex) { g_asking(1); }
SETUP(asking_muse) { g_asking(2); }
SETUP(asking_fo) { g_asking(3); }
SETUP(asking_nopet) { g_asking(4); }
SETUP(bell_idle) { g_notices(-1, 3); g_show(3); }
// The alert: the agent works, and notices arrive 1.5 s in (step_alert), so the bubble pops in and then rings.
SETUP(alert_claude) { g_working(0, "Editing cable_link.c", false); }
SETUP(alert_codex) { g_working(1, "Running the host tests", false); }
SETUP(alert_muse) { g_working(2, "Sketching", false); }
SETUP(alert_fo) { g_working(3, "Taking notes", false); }
static void step_alert(uint32_t t)
{
    static bool sent;
    if (t < 100) sent = false;
    if (!sent && t >= 1500) { sent = true; g_notices(-1, 3); }
}
static void g_pose(int pose, int agent, bool recap)
{
    if (recap) g_recap(agent);
    LOCKED(s.active = agent; view(HOME); s.pet_pose = pose; s.pet_until = ms() + 4500);
}
SETUP(done_claude) { g_pose(3, 0, true); }
SETUP(done_fo) { g_pose(3, 3, true); }
SETUP(booped_claude) { g_pose(1, 0, false); }
SETUP(booped_fo) { g_pose(1, 3, false); }
SETUP(nap) { LOCKED(s.nap = true; s.nap_until = ms() + 900000; s.active = 0; view(HOME)); }
SETUP(try_again) { LOCKED(s.voice_retry_until = ms() + 20000; view(HOME)); }
SETUP(carry_footer)
{
    display_lock();
    ht_carry_open(&carry, "carry-g1", g_id(0), "sel-1", 1, ms(), carry_emit, NULL);
    ht_carry_reply(&carry, "carry-g1", carry.request, true, "Cable link refactor", "static void reader_task(void *arg)", 1, 250000, NULL, ms());
    view(HOME);
    change();
    display_unlock();
}
SETUP(carry_error)
{
    display_lock();
    ht_carry_open(&carry, "carry-g2", g_id(0), "sel-2", 1, ms(), carry_emit, NULL);
    ht_carry_reply(&carry, "carry-g2", carry.request, false, NULL, NULL, 0, 0, "Carried text expired. Select it again.", ms());
    view(HOME);
    change();
    display_unlock();
}
SETUP(visit_footer)
{
    display_lock();
    ht_visit_open(&visit, "visit-g1", g_id(1), ms(), visit_emit, NULL);
    ht_visit_reply(&visit, "visit-g1", visit.request, true, "Back to Harness dial");
    view(HOME);
    change();
    display_unlock();
}
SETUP(workspace_preview)
{
    // view(HOME) first: it calls input_cancel(), which clears the drag flags (the first run landed on plain "home").
    LOCKED(view(HOME); workspace.touching = workspace.moved = true; workspace.cancelled = false; workspace.choice = 1;
           workspace.origin = 0; workspace.count = s.tab_count);
}

// ── Swarms ──────────────────────────────────────────────────────────────────────────────────────────────────────────
SETUP(swarm_working) { g_swarm_go("sw1", "L1"); }
SETUP(swarm_asking)
{
    g_swarm_put("work", "ask", "done", "done");   // L2 asks: the wait text of the harness it runs on shows on the face
    ui_supertab_wait(g_id(2), "supertab:r1", "Release swarm", false, G_SUMMARY[2]);
    g_swarm_go("sw1", "L2");
}
SETUP(swarm_stuck)
{
    g_swarm_put("work", "work", "stuck", "done");
    g_swarm_go("sw1", "L3");
}
SETUP(swarm_recap)
{
    g_swarm_go("sw1", "L4");
    ui_supertab_recap("sw1", "L4", G_RECAP, ms(), false);
}
SETUP(swarm_loading) { g_swarm_go("sw1", "L4"); }
SETUP(swarm_empty) { g_swarm_go("sw2", NULL); }

// ── lists ───────────────────────────────────────────────────────────────────────────────────────────────────────────
static void g_open_panes(void) { LOCKED(dispatch((action_t){.kind = A_AGENTS})); }
SETUP(panes) { g_open_panes(); }
static void step_slide_panes(uint32_t t)
{
    if (t % 1000 < 100 && s.view == AGENTS) {
        display_lock();
        ht_tab_carousel_go(&pane_carousel, (ht_tab_carousel_index(&pane_carousel) + 1) % (s.count ? s.count : 1), ms());
        change();
        display_unlock();
    }
}
SETUP(panes_slide) { g_open_panes(); }
SETUP(panes_empty) { g_swarm_go("sw2", NULL); g_open_panes(); }   // a Swarm with no projects: "No panes in this tab."
SETUP(panes_loading) { ui_project_clear_all(); LOCKED(s.loading = true; dispatch((action_t){.kind = A_AGENTS})); }   // g_reset refills the roster
SETUP(tabs) { LOCKED(tabs_open()); }
static void step_slide_tabs(uint32_t t)
{
    if (t % 1000 < 100 && s.view == TABS) {
        display_lock();
        int total = tabs_total();
        ht_tab_carousel_go(&tab_carousel, (ht_tab_carousel_index(&tab_carousel) + 1) % (total ? total : 1), ms());
        change();
        display_unlock();
    }
}
SETUP(tabs_slide) { LOCKED(tabs_open()); }
SETUP(tabs_empty)
{
    ui_swarms_replace(NULL, 0, "");
    ui_supertabs_ids((const char (*)[ID_MAX])NULL, 0);   // the Swarms are pages of the tab list too
    LOCKED(tabs_open());
}
static void g_open_inbox(int index)
{
    g_notices(-1, 23);
    display_lock();
    ht_tab_carousel_reset(&inbox_carousel, s.notice_count, index);
    view(INBOX);
    s.offset = index;
    change();
    display_unlock();
}
SETUP(inbox_first) { g_open_inbox(0); }
static void step_slide_inbox(uint32_t t)
{
    if (t % 1000 < 100 && s.view == INBOX) {
        display_lock();
        ht_tab_carousel_go(&inbox_carousel, (ht_tab_carousel_index(&inbox_carousel) + 1) % (s.notice_count ? s.notice_count : 1), ms());
        change();
        display_unlock();
    }
}
SETUP(inbox_slide) { g_open_inbox(0); }
SETUP(inbox_last) { g_open_inbox(22); }
static void g_inbox_find(bool question, bool failed)
{
    g_open_inbox(0);
    display_lock();
    for (int i = 0; i < s.notice_count; i++)
        if ((question && s.notice[i].question) || (failed && s.notice[i].failed)) {
            ht_tab_carousel_reset(&inbox_carousel, s.notice_count, i);
            s.offset = i;
            break;
        }
    change();
    display_unlock();
}
SETUP(inbox_question) { g_inbox_find(true, false); }
SETUP(inbox_failed) { g_inbox_find(false, true); }
SETUP(inbox_empty) { LOCKED(ht_tab_carousel_reset(&inbox_carousel, 0, 0); view(INBOX)); }
SETUP(machines) { LOCKED(view(MACHINES)); }
SETUP(machines_empty) { LOCKED(ui_machines_clear(); view(MACHINES)); }
SETUP(settings_1) { LOCKED(view(SETTINGS)); }
SETUP(settings_2) { LOCKED(view(SETTINGS); s.offset = 5); }
SETUP(models_loading) { LOCKED(GCOPY(s.model_agent, g_id(0)); s.model_count = -1; view(MODELS)); }
SETUP(models) { LOCKED(GCOPY(s.model_agent, g_id(0)); s.model_count = 8; view(MODELS)); }
SETUP(models_empty) { LOCKED(GCOPY(s.model_agent, g_id(0)); s.model_count = 0; view(MODELS)); }

// ── the reader ──────────────────────────────────────────────────────────────────────────────────────────────────────
static void g_reader(int offset)
{
    g_recap(0);
    LOCKED(s.active = 0; view(READER); s.offset = offset);
}
SETUP(reader_p1) { g_reader(0); }
SETUP(reader_p2) { g_reader(5); }
SETUP(reader_last) { g_reader(0); }
static void step_reader_last(uint32_t t)
{
    // The row count is known after the first frame (s.reader_rows); then jump to the last page, once.
    static bool done;
    if (t < 100) done = false;
    if (!done && t >= 600 && s.reader_rows > 5) {
        done = true;
        LOCKED(s.offset = ((s.reader_rows - 1) / 5) * 5);
    }
}

// ── questions: four questions of six options, two that take a spoken answer ──────────────────────────────────────────
static void g_question(int index, bool valid, bool supported, bool loading)
{
    display_lock();
    uint32_t revision = s.q.revision + 1;
    memset(&s.q, 0, sizeof s.q);
    s.q.revision = revision;
    GCOPY(s.q.agent, g_id(0)); GCOPY(s.q.name, G_NAME[0]); GCOPY(s.q.request, "g-req"); GCOPY(s.q.token, "g-tok");
    GCOPY(s.q.fetch, "g-fetch");
    s.q.deadline = ms() + 120000;   // the tick must not time the fixture out
    s.q.count = 4;
    s.q.index = index;
    for (int k = 0; k < 4; k++) {
        question_item_t *q = &s.q.item[k];
        snprintf(q->key, sizeof q->key, "k%d", k);
        snprintf(q->prompt, sizeof q->prompt,
                 k == 0 ? "Which branch should the cable link refactor land on, and should the regression test for the "
                          "ring wrap go in the same commit or in its own?"
                        : "Question %d: pick how the dial should handle the next step.", k + 1);
        q->multi = k == 1;
        q->can_text = k == 0 || k == 2;
        q->count = OPTION_MAX;
        for (int o = 0; o < OPTION_MAX; o++)
            snprintf(q->options[o], sizeof q->options[o],
                     o == 0 ? "Land it on main now, with the test in the same commit and the change log updated."
                            : "Option %d for question %d", o + 1, k + 1);
    }
    s.q.valid = valid;
    s.q.supported = supported;
    s.q.loading = loading;
    display_unlock();
}
SETUP(q_loading) { g_question(0, false, true, true); LOCKED(view(QUESTION)); }
SETUP(q_text) { g_question(0, true, true, false); LOCKED(view(QUESTION)); }
SETUP(q_choice_only) { g_question(3, true, true, false); LOCKED(view(QUESTION)); }
SETUP(q_error)
{
    g_question(0, false, true, false);
    LOCKED(GCOPY(s.q.error, "Answered elsewhere."); view(QUESTION));
}
SETUP(q_unsupported) { g_question(0, true, false, false); LOCKED(view(QUESTION)); }
SETUP(choices) { g_question(0, true, true, false); LOCKED(view(CHOICE)); }
SETUP(choices_multi)
{
    g_question(1, true, true, false);
    LOCKED(s.q.item[1].selected = 0x15; s.q.choice = 2; view(CHOICE));
}
SETUP(answer_review)
{
    g_question(1, true, true, false);
    LOCKED(s.q.item[1].selected = 0x05;
           GCOPY(s.q.item[1].answer, "Option 1 for question 2\n\nOption 3 for question 2"); view(ANSWER_REVIEW));
}
SETUP(answer_draft)
{
    g_question(0, true, true, false);
    LOCKED(GCOPY(s.q.item[0].draft, "d1");
           GCOPY(s.q.item[0].answer, "Land it on main now, and put the regression test in its own commit so it can be reverted alone.");
           view(ANSWER_REVIEW));
}
SETUP(answer_pending)
{
    g_question(1, true, true, false);
    LOCKED(s.q.item[1].selected = 0x01; GCOPY(s.q.item[1].answer, "Option 1 for question 2");
           s.q.pending = s.q.uncertain = true; view(ANSWER_REVIEW));
}

// ── voice: the recorder's state is faked; the screens are the real ones ──────────────────────────────────────────────
static void g_voice(int agent, bool recording, bool starting, bool waiting, view_t back)
{
    display_lock();
    s.voice_open = true;
    s.voice_started = ms();
    s.voice_wait_until = ms() + 120000;
    s.voice_return = back;   // set before view(VOICE): view() refuses to leave a draft or a form for anything but its voice
    s.voice_start_pending = starting;
    s.voice_waiting = waiting;
    s.voice_review = waiting;
    s.voice_search = false;
    gallery_recording = recording;
    gallery_active = true;
    GCOPY(s.voice_target, G_NAME[agent]);
    GCOPY(s.voice_engine, G_ENGINE[agent]);
    view(VOICE);
    change();
    display_unlock();
}
static void step_level(uint32_t t)
{
    static const unsigned wave[8] = {0, 1, 2, 4, 3, 2, 1, 0};   // a spoken phrase, one step per 150 ms
    unsigned level = wave[(t / 150) % 8];
    if (level != gallery_level) { gallery_level = level; display_lock(); change(); display_unlock(); }
}
SETUP(voice_starting) { g_voice(0, false, true, false, HOME); }
SETUP(voice_listen_claude) { g_voice(0, true, false, false, HOME); }
SETUP(voice_listen_codex) { g_voice(1, true, false, false, HOME); }
SETUP(voice_listen_muse) { g_voice(2, true, false, false, HOME); }
SETUP(voice_listen_fo) { g_voice(3, true, false, false, HOME); }
SETUP(voice_listen_nopet) { g_voice(4, true, false, false, HOME); }
SETUP(voice_write_claude) { g_voice(0, false, false, true, HOME); }
SETUP(voice_write_codex) { g_voice(1, false, false, true, HOME); }
SETUP(voice_write_muse) { g_voice(2, false, false, true, HOME); }
SETUP(voice_write_fo) { g_voice(3, false, false, true, HOME); }
SETUP(voice_write_nopet) { g_voice(4, false, false, true, HOME); }
SETUP(voice_find)
{
    display_lock();
    ht_selection_open(&selection, "pick-g1", g_id(0), ms(), selection_emit, NULL);
    selection.deadline = ms() + 120000;
    display_unlock();
    g_voice(0, false, false, true, SELECTION);
    LOCKED(s.voice_search = true);
}
static void g_draft_page(bool locked, const char *error, bool long_text)
{
    static EXT_RAM_BSS_ATTR ht_draft_page_t page;
    memset(&page, 0, sizeof page);
    GCOPY(page.id, "draft-g1"); GCOPY(page.agent, g_id(0)); GCOPY(page.name, G_NAME[0]);
    GCOPY(page.context, "claude on This computer");
    GCOPY(page.text, long_text
         ? "Please rebuild the dial image with the new telemetry, flash the second dial, and compare the internal free "
           "memory of both boots. Report the numbers per screen. If any screen uses more than eight kilobytes of "
           "internal RAM for its own data, say which structure holds it and what moving it to PSRAM would cost in "
           "raster time. Do not change the protocol. Keep the answer short, and put the table first so I can read it "
           "on the dial while I walk to the bench."
         : "Please rebuild the dial image and report the free memory.");
    page.revision = 1; page.position = 1; page.total = 3; page.active = true; page.can_send = true; page.can_undo = true;
    page.locked = locked;
    if (error) GCOPY(page.error, error);
    display_lock();
    ht_draft_open(&draft, &page, draft_emit, NULL);
    display_unlock();
}
SETUP(voice_draft)
{
    g_draft_page(false, NULL, false);
    g_voice(0, false, false, true, DRAFT);
    LOCKED(s.voice_draft_append = false);
}

// ── selection, find / new, drafts ────────────────────────────────────────────────────────────────────────────────────
static void g_selection(bool reply, bool ok, const char *query, int match, int matches, int rows)
{
    display_lock();
    ht_selection_open(&selection, "pick-g1", g_id(0), ms(), selection_emit, NULL);
    if (reply)
        ht_selection_reply_search(&selection, selection.request, "pick-g1", ok, 1,
                                  "static void reader_task(void *arg)\n{\n    (void)arg;\n    for (;;) read_one_frame();",
                                  rows, false, ok ? NULL : "The app did not answer. Choose again.", query, match, matches, ms());
    selection.deadline = ms() + 120000;   // no tick may time it out
    s.active = 0;
    view(SELECTION);
    change();
    display_unlock();
}
SETUP(sel_ready) { g_selection(true, true, NULL, 0, 0, 4); }
SETUP(sel_pending) { g_selection(false, true, NULL, 0, 0, 0); }
SETUP(sel_error) { g_selection(true, false, NULL, 0, 0, 0); }
SETUP(sel_search) { g_selection(true, true, "reader", 2, 5, 2); }
SETUP(sel_nomatch) { g_selection(true, true, "zzzz", 0, 0, 0); }
SETUP(sel_carry_wait)
{
    g_selection(true, true, NULL, 0, 0, 4);
    LOCKED(ht_carry_open(&carry, "carry-g3", g_id(0), "pick-g1", selection.revision, ms(), carry_emit, NULL);
           carry.deadline = ms() + 120000);
}
static void g_form(const char *id, bool reply, bool ok, bool busy, const char *error)
{
    static EXT_RAM_BSS_ATTR ht_form_page_t page;
    memset(&page, 0, sizeof page);
    bool find = !strncmp(id, "find-", 5);
    GCOPY(page.title, find ? "Find Harness" : "New Harness");
    GCOPY(page.label, find ? G_NAME[1] : "claude in a new tab");
    GCOPY(page.previous, find ? G_NAME[0] : "");
    GCOPY(page.detail, find ? "codex on This computer, 3 minutes ago" : "Starts in the current workspace");
    GCOPY(page.status, "Starting in a new tab...");
    GCOPY(page.action, find ? "open" : "start");
    if (error) GCOPY(page.error, error);
    page.revision = 1; page.position = 2; page.total = 7; page.active = true; page.busy = busy; page.enabled = !busy;
    page.can_query = true;
    display_lock();
    ht_form_open(&form, id, ms(), form_emit, NULL);
    form.deadline = ms() + 120000;
    if (reply) ht_form_reply(&form, id, form.request, ok, &page, ms());
    form.poll = ms() + 3600000;   // no poll may be sent, or time out, while the tour holds the page
    view(FORM);
    change();
    display_unlock();
}
SETUP(form_find) { g_form("find-g1", true, true, false, NULL); }
SETUP(form_new) { g_form("form-g1", true, true, false, NULL); }
SETUP(form_busy) { g_form("form-g2", true, true, true, NULL); }
SETUP(form_error) { g_form("find-g2", true, true, false, "Could not open that harness: it has closed."); }
SETUP(form_opening) { g_form("find-g3", false, true, false, NULL); }
SETUP(draft_page) { g_draft_page(false, NULL, false); LOCKED(view(DRAFT)); }
SETUP(draft_scrolled) { g_draft_page(false, NULL, true); LOCKED(view(DRAFT); s.offset = 3); }
SETUP(draft_locked) { g_draft_page(true, "The harness is busy. Check the terminal.", false); LOCKED(view(DRAFT)); }
SETUP(draft_options) { g_draft_page(false, NULL, false); LOCKED(view(DRAFT_OPTIONS)); }

// ── prompts, messages, the update screen ─────────────────────────────────────────────────────────────────────────────
SETUP(stop) { g_busy(0, "Editing cable_link.c"); LOCKED(GCOPY(s.stop_agent, g_id(0)); view(STOP)); }
SETUP(msg_toast) { ui_cable_toast("The link to your computer is ready."); }
SETUP(msg_long)
{
    ui_show_error("Link this machine",
                  "On that computer:\nharness link create\n\nOn this computer:\nharness link import\n\nThen reconnect the cable.");
}
SETUP(msg_turn_error) { ui_turn_error(g_id(1), G_SUMMARY[7]); }
SETUP(ota) { LOCKED(view(OTA)); }

// ── sleep, the agent roster, and the arc worst case ──────────────────────────────────────────────────────────────────
SETUP(sleep_wake) { g_idle(0); }
static void step_sleep(uint32_t t)
{
    static bool slept, woke;
    if (t < 100) slept = woke = false;
    if (!slept && t >= 1000) { slept = true; display_sleep(); }
    if (!woke && t >= 3500) { woke = true; display_wake(); }
}
SETUP(agents_cycle) { LOCKED(s.active = 0; view(AGENT)); }
static void step_cycle(uint32_t t)
{
    static int last = -1;
    int k = (int)((t / 700) % 16);
    if (t < 100) last = -1;
    if (k != last) { last = k; LOCKED(s.active = k; view(AGENT)); }
}
static void synthetic_arcs(ht_scene_t *f)
{
    ht_arc_title_face(f, FG, G_VIET24, &ht_arc_inter_prop);
    ht_arc_status_face(f, FG, G_VIET24, &ht_arc_inter_lower);
}
SETUP(arc_worst) { LOCKED(gallery_synthetic = synthetic_arcs); }

// ── the tour ─────────────────────────────────────────────────────────────────────────────────────────────────────────
typedef struct {
    const char *name;
    bool animated;
    uint32_t hold_ms;
    const char *view;
    void (*setup)(void);
    void (*step)(uint32_t);
} g_item_t;
#define G_ROW(sym, name, anim, hold, view, source, step) {name, anim, hold, view, setup_##sym, step},
static const g_item_t G_ITEMS[] = { UI_GALLERY_ITEMS(G_ROW) };
#define G_COUNT ((int)(sizeof G_ITEMS / sizeof G_ITEMS[0]))

// What the tour cannot show, and why. One "UIM_TOUR skipped" line each, then the names in the done line.
static const struct { const char *name, *why; } G_SKIPS[] = {
    {"voice_review", "'Release to review' is recomputed every tick from a finger held 650 ms (surface_tick)"},
    {"celebration", "ui_companion_celebrate is inert: follow_companion is off under HABITAT_FOCUS_ONLY"},
    {"custom_pet_packs", "packs arrive from the desktop over the cable; there is no fixture without it"},
    {"alert_custom_pack", "the code-drawn alert exists for custom packs only"},
    {"other_skins", "tim, tux, octopus and the command faces are not built (HABITAT_FOCUS_ONLY)"},
    {"pressed_states", "control highlight and scroll inertia need a finger on the glass"},
    {"lock_screen", "ui_lock_* are empty stubs and there is no view"},
};
#define G_SKIP_COUNT ((int)(sizeof G_SKIPS / sizeof G_SKIPS[0]))

static void tour_item(int n, const g_item_t *it, char *bad, size_t bad_cap)
{
    g_reset();
    g_wait(300);
    ESP_LOGI(TAG_TOUR, "UIM_TOUR item=%d/%d name=%s begin kind=%s hold_ms=%u", n, G_COUNT, it->name,
             it->animated ? "animated" : "still", (unsigned)it->hold_ms);
    uim_item_begin(it->name);
    it->setup();
    g_wait(300);
    display_lock();
    const char *seen = habitat_view_name();
    bool match = !strcmp(seen, it->view);
    display_unlock();
    if (!match) {
        ESP_LOGW(TAG_TOUR, "UIM_TOUR item=%d/%d name=%s MISMATCH expected_view=%s got=%s", n, G_COUNT, it->name, it->view, seen);
        size_t used = strlen(bad);
        if (used + strlen(it->name) + 2 < bad_cap) snprintf(bad + used, bad_cap - used, "%s%s", used ? "," : "", it->name);
    }
    uint32_t t0 = ms(), t;
    while ((t = ms() - t0) < it->hold_ms) {
        if (it->step) it->step(t);
        g_wait(100);
    }
    ESP_LOGI(TAG_TOUR, "UIM_TOUR shot_ready name=%s", it->name);
    uint32_t done0 = cable_debug_commands_done(), waited = 0;
    while (waited < SHOT_WAIT_MS && cable_debug_commands_done() == done0) {
        if (it->step) it->step(ms() - t0);
        g_wait(50);
        waited += 50;
    }
    bool shot = cable_debug_commands_done() != done0;
    uim_item_end();   // the item's UIM line, here and now
    ESP_LOGI(TAG_TOUR, "UIM_TOUR item=%d/%d name=%s end shot=%d shot_wait_ms=%u", n, G_COUNT, it->name, shot, (unsigned)waited);
}

static void tour_task(void *arg)
{
    (void)arg;
    while (esp_timer_get_time() < (int64_t)UI_GALLERY_START_S * 1000000) g_wait(200);
    g_fixture();
    uint32_t hold_total = 0;
    for (int i = 0; i < G_COUNT; i++) hold_total += G_ITEMS[i].hold_ms;
    // Per item: 0.6 s of reset and settling, the hold, and the shot wait (about 1.5 s with a host answering, 4 s without).
    uint32_t min_s = (hold_total + G_COUNT * (600 + 1500)) / 1000, max_s = (hold_total + G_COUNT * (600 + SHOT_WAIT_MS)) / 1000;
    for (int cycle = 1;; cycle++) {
        ESP_LOGI(TAG_TOUR, "UIM_TOUR cycle=%d begin items=%d est_s=%u..%u pause_s=%u", cycle, G_COUNT, (unsigned)min_s,
                 (unsigned)max_s, (unsigned)UI_GALLERY_PAUSE_S);
        for (int k = 0; k < G_SKIP_COUNT; k++)
            ESP_LOGI(TAG_TOUR, "UIM_TOUR skipped name=%s reason=%s", G_SKIPS[k].name, G_SKIPS[k].why);
        char bad[200] = "";
        int64_t began = esp_timer_get_time();
        for (int i = 0; i < G_COUNT; i++) tour_item(i + 1, &G_ITEMS[i], bad, sizeof bad);
        g_reset();
        char skipped[200] = "";
        for (int k = 0; k < G_SKIP_COUNT; k++) {
            size_t used = strlen(skipped);
            snprintf(skipped + used, sizeof skipped - used, "%s%s", used ? "," : "", G_SKIPS[k].name);
        }
        ESP_LOGI(TAG_TOUR, "UIM_TOUR done covered=%d skipped=%s mismatched=%s cycle_s=%d", G_COUNT, skipped,
                 bad[0] ? bad : "none", (int)((esp_timer_get_time() - began) / 1000000));
        g_wait(UI_GALLERY_PAUSE_S * 1000);
    }
}

void ui_gallery_start(void)
{
    ESP_LOGW(TAG_TOUR, "UI GALLERY BUILD: %d items, first tour %d s after boot; touch does nothing; no audio, no NVS writes",
             G_COUNT, UI_GALLERY_START_S);
    assert(xTaskCreate(tour_task, "ui_tour", UI_GALLERY_TOUR_STACK, NULL, 3, NULL) == pdPASS);
}
