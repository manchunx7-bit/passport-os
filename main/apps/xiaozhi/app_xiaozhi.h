#pragma once

#include "apps/app_base.h"

#ifdef __cplusplus
extern "C" {
#endif

void xiaozhi_app_init(void);
void xiaozhi_app_start(void);
void xiaozhi_app_stop(void);
// Idempotent user request: connect if needed, then listen once hello/audio are
// ready. Only the control task touches the protocol and microphone.
void xiaozhi_app_request_conversation(void);
void xiaozhi_app_on_key(uint8_t btn, app_btn_event_t event, uint16_t mv);

extern const passport_app_t g_xiaozhi_app;

#ifdef __cplusplus
}
#endif
