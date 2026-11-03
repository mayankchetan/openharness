// The UI gallery's tour, as data (DEVICE_UI_GALLERY): one X() row per item, in tour order.
//   X(sym, "name", animated, hold_ms, "view", "source", step)
//     sym      setup_<sym>() in ui_gallery.inc drives the model into the state
//     name     the item name in the log (UIM_TOUR ... name=<name>) and the UIM line's view= (<= 19 chars)
//     animated 1 = something moves on its own (hold is at least one full cycle and at least 3 s), 0 = a still screen (2 s)
//     hold_ms  how long the state is shown before UIM_TOUR shot_ready
//     view     what habitat_view_name() must say once the state is up (the tour logs a mismatch)
//     source   the function in ui_habitat.c / focus.c that draws it (the coverage checklist resolves it to file:line)
//     step     NULL or step_<x>(t_ms): called every 100 ms while the item is shown (carousel slides, level meter, ...)
// Host test test_ui_gallery_items.c reads this table without the firmware.
//
// Pet cycles behind the holds (pets.c, steps x step_ms): work claude 1.4 s, chef alt build 1.8 s, codex 3.4 s, muse 1.1 s,
// fo 1.8 s; relax 1.3 / 2.9 / 2.4 / 2.9 s; listen 3.4 / 1.2 / 1.7 / 1.7 s; send 2.4 / 2.1 / 2.8 / 2.2 s; the alert pops
// in and rings for 5.1 s (46 x 110 ms).
#define UI_GALLERY_ITEMS(X) \
    /* the connection and the home face */ \
    X(brand,            "brand",            0, 2000, "brand",         "render_brand",            NULL) \
    X(brand_loading,    "brand_loading",    0, 2000, "brand",         "render_brand",            NULL) \
    X(home_rest,        "home_rest",        0, 2000, "home_rest",     "render_home",             NULL) \
    X(idle_claude,      "idle_claude",      1, 4000, "home",          "relaxing_scene",          NULL) \
    X(idle_codex,       "idle_codex",       1, 4000, "home",          "relaxing_scene",          NULL) \
    X(idle_muse,        "idle_muse",        1, 4000, "home",          "relaxing_scene",          NULL) \
    X(idle_fo,          "idle_fo",          1, 4000, "home",          "relaxing_scene",          NULL) \
    X(idle_nopet,       "idle_nopet",       0, 2000, "home",          "render_home",             NULL) \
    X(recap_claude,     "recap_claude",     0, 2000, "home_recap",    "render_home",             NULL) \
    X(recap_fo,         "recap_fo",         1, 4000, "home_recap",    "render_home",             NULL) \
    X(recap_nopet,      "recap_nopet",      0, 2000, "home_recap",    "render_home",             NULL) \
    X(busy_claude_chef, "busy_claude_chef", 1, 4000, "home_busy",     "working_scene",           NULL) \
    X(busy_claude_build,"busy_claude_build",1, 4000, "home_busy",     "working_scene",           NULL) \
    X(busy_codex,       "busy_codex",       1, 4000, "home_busy",     "working_scene",           NULL) \
    X(busy_muse,        "busy_muse",        1, 4000, "home_busy",     "working_scene",           NULL) \
    X(busy_fo,          "busy_fo",          1, 4000, "home_busy",     "working_scene",           NULL) \
    X(busy_nopet,       "busy_nopet",       1, 3000, "home_busy",     "render_home",             NULL) \
    X(asking_claude,    "asking_claude",    1, 4000, "home_question", "pet_state",               NULL) \
    X(asking_codex,     "asking_codex",     1, 4000, "home_question", "pet_state",               NULL) \
    X(asking_muse,      "asking_muse",      1, 4000, "home_question", "pet_state",               NULL) \
    X(asking_fo,        "asking_fo",        1, 4000, "home_question", "pet_state",               NULL) \
    X(asking_nopet,     "asking_nopet",     0, 2000, "home_question", "render_home",             NULL) \
    X(bell_idle,        "bell_idle",        0, 2000, "home",          "focus_bell",              NULL) \
    X(alert_claude,     "alert_claude",     1, 7500, "home_busy",     "alert_scene",             step_alert) \
    X(alert_codex,      "alert_codex",      1, 7500, "home_busy",     "alert_scene",             step_alert) \
    X(alert_muse,       "alert_muse",       1, 7500, "home_busy",     "alert_scene",             step_alert) \
    X(alert_fo,         "alert_fo",         1, 7500, "home_busy",     "alert_scene",             step_alert) \
    X(done_claude,      "done_claude",      1, 3000, "home_recap",    "character_mood",          NULL) \
    X(done_fo,          "done_fo",          1, 3000, "home_recap",    "character_mood",          NULL) \
    X(booped_claude,    "booped_claude",    1, 3000, "home",          "character_mood",          NULL) \
    X(booped_fo,        "booped_fo",        1, 3000, "home",          "character_mood",          NULL) \
    X(nap,              "nap",              1, 3000, "home",          "character_mood",          NULL) \
    X(try_again,        "try_again",        0, 2000, "home",          "render_home",             NULL) \
    X(carry_footer,     "carry_footer",     0, 2000, "home",          "render_home",             NULL) \
    X(carry_error,      "carry_error",      0, 2000, "home",          "render_home",             NULL) \
    X(visit_footer,     "visit_footer",     0, 2000, "home",          "render_home",             NULL) \
    X(workspace_preview,"workspace_preview",0, 2000, "workspace",     "render_workspace_preview",NULL) \
    /* the Swarms (Super Tab faces) */ \
    X(swarm_working,    "swarm_working",    1, 4000, "swarm",         "render_swarm_face",       NULL) \
    X(swarm_asking,     "swarm_asking",     1, 4000, "swarm",         "render_swarm_face",       NULL) \
    X(swarm_stuck,      "swarm_stuck",      1, 4000, "swarm",         "render_swarm_face",       NULL) \
    X(swarm_recap,      "swarm_recap",      0, 2000, "swarm",         "render_swarm_face",       NULL) \
    X(swarm_loading,    "swarm_loading",    0, 2000, "swarm",         "render_swarm_face",       NULL) \
    X(swarm_empty,      "swarm_empty",      0, 2000, "swarm",         "render_focus_panes",      NULL) \
    /* panes, tabs, inbox, machines, controls, models */ \
    X(panes,            "panes",            0, 2000, "panes",         "render_focus_panes",      NULL) \
    X(panes_slide,      "panes_slide",      1, 3500, "panes",         "focus_carousel",          step_slide_panes) \
    X(panes_empty,      "panes_empty",      0, 2000, "panes",         "render_focus_panes",      NULL) \
    X(panes_loading,    "panes_loading",    0, 2000, "panes",         "render_focus_panes",      NULL) \
    X(tabs,             "tabs",             0, 2000, "tabs",          "render_focus_tabs",       NULL) \
    X(tabs_slide,       "tabs_slide",       1, 3500, "tabs",          "focus_carousel",          step_slide_tabs) \
    X(tabs_empty,       "tabs_empty",       0, 2000, "tabs",          "render_focus_tabs",       NULL) \
    X(inbox_first,      "inbox_first",      0, 2000, "inbox",         "render_focus_inbox",      NULL) \
    X(inbox_slide,      "inbox_slide",      1, 3500, "inbox",         "render_focus_inbox",      step_slide_inbox) \
    X(inbox_last,       "inbox_last",       0, 2000, "inbox",         "render_focus_inbox",      NULL) \
    X(inbox_question,   "inbox_question",   0, 2000, "inbox",         "inbox_page",              NULL) \
    X(inbox_failed,     "inbox_failed",     0, 2000, "inbox",         "inbox_page",              NULL) \
    X(inbox_empty,      "inbox_empty",      0, 2000, "inbox",         "render_focus_inbox",      NULL) \
    X(machines,         "machines",         0, 2000, "machines",      "render_list",             NULL) \
    X(machines_empty,   "machines_empty",   0, 2000, "machines",      "render_list",             NULL) \
    X(settings_1,       "settings_1",       0, 2000, "settings",      "render_settings",         NULL) \
    X(settings_2,       "settings_2",       0, 2000, "settings",      "render_settings",         NULL) \
    X(link_wifi,        "link_wifi",        0, 2000, "link",          "render_link",             NULL) \
    X(link_keep,        "link_keep",        0, 2000, "link",          "render_link",             NULL) \
    X(link_none,        "link_none",        0, 2000, "link",          "render_link",             NULL) \
    X(phone_off,        "phone_off",        0, 2000, "phone",         "render_phone",            NULL) \
    X(phone_unpaired,   "phone_unpaired",   0, 2000, "phone",         "render_phone",            NULL) \
    X(phone_paired,     "phone_paired",     0, 2000, "phone",         "render_phone",            NULL) \
    X(phone_connected,  "phone_connected",  0, 2000, "phone",         "render_phone",            NULL) \
    X(phone_waiting,    "phone_waiting",    0, 2000, "phone",         "render_phone",            NULL) \
    X(passkey,          "passkey",          0, 2000, "passkey",       "render_passkey",          NULL) \
    X(passkey_zeros,    "passkey_zeros",    0, 2000, "passkey",       "render_passkey",          NULL) \
    X(forget_ask,       "forget_ask",       0, 2000, "forget",        "render_forget",           NULL) \
    X(models_loading,   "models_loading",   0, 2000, "models",        "habitat_scene_take",      NULL) \
    X(models,           "models",           0, 2000, "models",        "habitat_scene_take",      NULL) \
    X(models_empty,     "models_empty",     0, 2000, "models",        "habitat_scene_take",      NULL) \
    /* the reader */ \
    X(reader_p1,        "reader_p1",        0, 2000, "reader",        "habitat_scene_take",      NULL) \
    X(reader_p2,        "reader_p2",        0, 2000, "reader",        "habitat_scene_take",      NULL) \
    X(reader_last,      "reader_last",      0, 2000, "reader",        "habitat_scene_take",      step_reader_last) \
    /* questions */ \
    X(q_loading,        "q_loading",        0, 2000, "question",      "render_question",         NULL) \
    X(q_text,           "q_text",           0, 2000, "question",      "render_question",         NULL) \
    X(q_choice_only,    "q_choice_only",    0, 2000, "question",      "render_question",         NULL) \
    X(q_error,          "q_error",          0, 2000, "question",      "render_question",         NULL) \
    X(q_unsupported,    "q_unsupported",    0, 2000, "question",      "render_question",         NULL) \
    X(choices,          "choices",          0, 2000, "choice",        "render_choices",          NULL) \
    X(choices_multi,    "choices_multi",    0, 2000, "choice",        "render_choices",          NULL) \
    X(answer_review,    "answer_review",    0, 2000, "answer_review", "render_answer_review",    NULL) \
    X(answer_draft,     "answer_draft",     0, 2000, "answer_review", "render_answer_review",    NULL) \
    X(answer_pending,   "answer_pending",   0, 2000, "answer_review", "render_answer_review",    NULL) \
    /* voice (the microphone is never started: ui_gallery.inc stands in for the recorder's state) */ \
    X(voice_starting,   "voice_starting",   1, 3000, "voice",         "render_voice",            NULL) \
    X(voice_listen_claude,"voice_listen_claude",1, 4000, "voice",     "listening_scene",         step_level) \
    X(voice_listen_codex,"voice_listen_codex",1, 4000, "voice",       "listening_scene",         step_level) \
    X(voice_listen_muse,"voice_listen_muse",1, 4000, "voice",         "listening_scene",         step_level) \
    X(voice_listen_fo,  "voice_listen_fo",  1, 4000, "voice",         "listening_scene",         step_level) \
    X(voice_listen_nopet,"voice_listen_nopet",1, 3000, "voice",       "render_voice",            step_level) \
    X(voice_write_claude,"voice_write_claude",1, 3500, "voice",       "sending_scene",           NULL) \
    X(voice_write_codex,"voice_write_codex",1, 3500, "voice",         "sending_scene",           NULL) \
    X(voice_write_muse, "voice_write_muse", 1, 3500, "voice",         "sending_scene",           NULL) \
    X(voice_write_fo,   "voice_write_fo",   1, 3500, "voice",         "sending_scene",           NULL) \
    X(voice_write_nopet,"voice_write_nopet",1, 3000, "voice",         "render_voice",            NULL) \
    X(voice_find,       "voice_find",       1, 3000, "voice",         "render_voice",            NULL) \
    X(voice_draft,      "voice_draft",      1, 3000, "voice",         "render_voice",            NULL) \
    /* selection, find / new, drafts */ \
    X(sel_ready,        "sel_ready",        0, 2000, "selection",     "render_selection",        NULL) \
    X(sel_pending,      "sel_pending",      0, 2000, "selection",     "render_selection",        NULL) \
    X(sel_error,        "sel_error",        0, 2000, "selection",     "render_selection",        NULL) \
    X(sel_search,       "sel_search",       0, 2000, "selection",     "render_selection",        NULL) \
    X(sel_nomatch,      "sel_nomatch",      0, 2000, "selection",     "render_selection",        NULL) \
    X(sel_carry_wait,   "sel_carry_wait",   0, 2000, "selection",     "render_selection",        NULL) \
    X(form_find,        "form_find",        0, 2000, "form",          "render_form",             NULL) \
    X(form_new,         "form_new",         0, 2000, "form",          "render_form",             NULL) \
    X(form_busy,        "form_busy",        0, 2000, "form",          "render_form",             NULL) \
    X(form_error,       "form_error",       0, 2000, "form",          "render_form",             NULL) \
    X(form_opening,     "form_opening",     0, 2000, "form",          "render_form",             NULL) \
    X(draft_page,       "draft_page",       0, 2000, "draft",         "render_draft",            NULL) \
    X(draft_scrolled,   "draft_scrolled",   0, 2000, "draft",         "render_draft",            NULL) \
    X(draft_locked,     "draft_locked",     0, 2000, "draft",         "render_draft",            NULL) \
    X(draft_options,    "draft_options",    0, 2000, "draft_options", "render_draft_options",    NULL) \
    /* prompts, messages, the update screen, sleep, and the arc limits */ \
    X(stop,             "stop",             0, 2000, "stop",          "habitat_scene_take",      NULL) \
    X(msg_toast,        "msg_toast",        0, 2000, "message",       "habitat_scene_take",      NULL) \
    X(msg_long,         "msg_long",         0, 2000, "message",       "habitat_scene_take",      NULL) \
    X(msg_turn_error,   "msg_turn_error",   0, 2000, "message",       "ui_turn_error",           NULL) \
    X(ota,              "ota",              0, 2000, "ota",           "render_brand",            NULL) \
    X(sleep_wake,       "sleep_wake",       1, 6500, "home",          "display_sleep",           step_sleep) \
    X(agents_cycle,     "agents_cycle",     1,12000, "agent",         "ht_arc_title_face",       step_cycle) \
    X(arc_worst,        "arc_worst",        0, 2500, "synthetic",     "arc_prepare",             NULL)
