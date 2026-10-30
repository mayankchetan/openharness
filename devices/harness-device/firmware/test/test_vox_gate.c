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
    /* plain build: no gate */
    assert(vox_start_gate(false, false, false) == NULL);
    assert(vox_reply_gate(false, "voice.transcript") == NULL);
    printf("test_vox_gate: PASS\n");
    return 0;
}
