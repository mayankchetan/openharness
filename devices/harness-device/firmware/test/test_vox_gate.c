// Host unit test of main/vox_gate.h (the safety gates of the VOX probe in review mode). Run through test_vox_gate.py.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "vox_gate.h"

int main(void)
{
    /* start gate, review build: only agent + draft feature passes */
    assert(vox_start_gate(true, true, true) == NULL);
    assert(!strcmp(vox_start_gate(true, true, false), "no_review"));   /* draft feature not advertised */
    assert(!strcmp(vox_start_gate(true, false, true), "no_review"));   /* no agent: the daemon refuses a review without one */
    assert(!strcmp(vox_start_gate(true, false, false), "no_review"));
    /* reply gate, review build */
    assert(vox_reply_gate(true, "voice.draft") == NULL);
    assert(vox_reply_gate(true, "voice.error") == NULL);               /* an error cannot have sent anything */
    assert(!strcmp(vox_reply_gate(true, "voice.transcript"), "sent_reply"));
    assert(!strcmp(vox_reply_gate(true, "voice.question"), "sent_reply"));
    assert(!strcmp(vox_reply_gate(true, ""), "sent_reply"));
    assert(!strcmp(vox_reply_gate(true, NULL), "sent_reply"));
    /* stop gate, review build: after the capture delay and before audio_client_stop(). The review flag was set right after the start. */
    assert(vox_stop_gate(true, true, true) == NULL);                    /* flag set, still recording: stop it as planned */
    assert(!strcmp(vox_stop_gate(true, false, true), "review_not_set"));  /* the flag is gone: voice.end would send the words */
    assert(!strcmp(vox_stop_gate(true, false, false), "review_not_set"));
    assert(!strcmp(vox_stop_gate(true, true, false), "ended_early")); /* a tap or BOOT press ended the turn inside the 4 s */
    /* plain build: no gate */
    assert(vox_start_gate(false, false, false) == NULL);
    assert(vox_reply_gate(false, "voice.transcript") == NULL);
    assert(vox_stop_gate(false, false, false) == NULL);
    printf("test_vox_gate: PASS\n");
    return 0;
}
