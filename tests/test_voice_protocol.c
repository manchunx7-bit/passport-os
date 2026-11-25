// Host test: PC -> device JSON lines for the voice remote (pc.status / pc.ptt).
#include "apps/voice/app_protocol.h"
#include "apps/voice/app_events.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

// app_protocol.c links the event queue; the parser under test never posts.
void app_event_post(const app_event_t *ev) { (void)ev; }
esp_err_t app_event_post_important(const app_event_t *ev, uint32_t timeout_ms) {
    (void)ev; (void)timeout_ms; return ESP_OK;
}

static bool parse(const char *line, app_event_t *ev) {
    memset(ev, 0, sizeof(*ev));
    return app_protocol_parse(line, strlen(line), ev);
}

int main(void) {
    app_event_t ev;
    assert(parse("{\"type\":\"pc.ptt\",\"active\":true}\n", &ev));
    assert(ev.type == APP_EV_PC_PTT && ev.u.pc_ptt.active);
    assert(parse("{\"type\":\"pc.ptt\",\"active\":false}", &ev));
    assert(ev.type == APP_EV_PC_PTT && !ev.u.pc_ptt.active);
    // Missing or non-bool field must not be read as a press.
    assert(!parse("{\"type\":\"pc.ptt\"}", &ev));
    assert(!parse("{\"type\":\"pc.ptt\",\"active\":1}", &ev));
    assert(!parse("{\"type\":\"pc.ptt\",\"active\":\"true\"}", &ev));
    assert(!parse("{\"type\":\"PC.PTT\",\"active\":true}", &ev));
    // Existing readiness heartbeat still parses.
    assert(parse("{\"type\":\"pc.status\",\"audioReady\":true,\"hotkeyReady\":true}", &ev));
    assert(ev.type == APP_EV_PC_STATUS && ev.u.pc_status.audio_ready && ev.u.pc_status.hotkey_ready);
    puts("Voice protocol: pc.ptt / pc.status parsing PASS");
    return 0;
}
