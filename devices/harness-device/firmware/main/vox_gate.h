// Safety gates of the VOX probe in review mode (VOX_REVIEW=1). Pure C, no IDF: unit-tested on the host.
// A review run must never send a transcript to an agent. Two gates:
//   vox_start_gate  before the first utterance: review must be possible, else the probe stops with reason=no_review
//   vox_reply_gate  on every reply: anything but a draft (or an error, which cannot have sent anything) stops the probe
// Both return NULL to continue, or the reason string for "VOX ABORT reason=<reason>".
#pragma once
#include <stdbool.h>
#include <string.h>

static inline const char *vox_start_gate(bool review_build, bool have_agent, bool draft_feature)
{
    if (!review_build) return NULL;                 // the plain image has no gate by design (never run by the lead)
    if (!have_agent || !draft_feature) return "no_review";
    return NULL;
}

static inline const char *vox_reply_gate(bool review_build, const char *reply_type)
{
    if (!review_build) return NULL;
    if (reply_type && !strcmp(reply_type, "voice.draft")) return NULL;
    if (reply_type && !strcmp(reply_type, "voice.error")) return NULL;
    return "sent_reply";                            // voice.transcript (the words went to an agent) or anything unknown
}
