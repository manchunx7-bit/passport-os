#include "apps/voice/app_state.h"
#include <assert.h>
#include <stdio.h>

static app_state_t s;
static app_action_t actions[APP_ACT_MAX];
static uint8_t count;
static void ev(app_event_type_t type, int key, unsigned mv, uint64_t now) {
    app_event_t e = {.type=type, .u.key={key,mv}};
    app_state_reduce(&s,&e,now,actions,&count);
}
static void pc(bool audio, bool hotkey, uint64_t now) {
    app_event_t e = {.type=APP_EV_PC_STATUS, .u.pc_status={audio,hotkey}};
    app_state_reduce(&s,&e,now,actions,&count);
}
static void start(uint64_t now);
static void ptt(bool active, uint64_t now) {
    app_event_t e = {.type=APP_EV_PC_PTT, .u.pc_ptt={active}};
    app_state_reduce(&s,&e,now,actions,&count);
}
static void tick(uint64_t now) {
    app_event_t e = {.type=APP_EV_TICK};
    app_state_reduce(&s,&e,now,actions,&count);
}
static void assert_started(void) {
    assert(s.state==APP_ST_LISTENING && count==2);
    assert(actions[0].type==APP_ACT_SEND_VOICE_START && actions[1].type==APP_ACT_STREAM_START);
}
static void assert_stopped(void) {
    assert(s.state==APP_ST_READY && count==2 && !s.pc_ptt);
    assert(actions[0].type==APP_ACT_STREAM_STOP && actions[1].type==APP_ACT_SEND_VOICE_END);
}
static void pc_hotkey(void) {
    app_ui_snapshot_t snap;
    app_state_init(&s);
    // Not ready: PC hold is ignored exactly like the device key.
    ptt(true,10); assert(count==0 && s.state==APP_ST_READY);
    ev(APP_EV_LINK_UP,0,0,20); ptt(true,30); assert(count==0);
    pc(true,true,40);
    ptt(true,100); assert_started(); assert(s.pc_ptt);
    app_state_snapshot(&s,150,&snap); assert(snap.pc_ptt);
    // Renewals keep it open without re-sending voice.start.
    ptt(true,600); assert(count==0); pc(true,true,700);
    ptt(true,1100); assert(count==0);
    tick(2500); assert(count==0 && s.state==APP_ST_LISTENING);
    // A stray device UP release or press does not hijack the PC session.
    ev(APP_EV_KEY_PRESS,APP_BTN_UP,15,2510); assert(count==0);
    ev(APP_EV_KEY_RELEASE,APP_BTN_UP,2900,2520); assert(count==0 && s.pc_ptt);
    ptt(false,2600); assert_stopped();
    ptt(false,2610); assert(count==0);
    app_state_snapshot(&s,2620,&snap); assert(!snap.pc_ptt);
    // Lost release datagram: renewals stop, microphone closes after the timeout.
    pc(true,true,2700); ptt(true,2700); assert_started();
    tick(2700+APP_PC_PTT_TIMEOUT_MS); assert(count==0);
    tick(2701+APP_PC_PTT_TIMEOUT_MS); assert_stopped();
    // A device-key session is never started over or ended by the PC.
    pc(true,true,4300); start(4300);
    ptt(true,4400); assert(count==0 && !s.pc_ptt);
    ptt(false,4500); assert(count==0 && s.state==APP_ST_LISTENING);
    ev(APP_EV_KEY_RELEASE,APP_BTN_UP,2900,4600); assert_stopped();
    // PC readiness loss and link loss cancel the PC session.
    pc(true,true,4700); ptt(true,4700); assert_started();
    pc(false,true,4800); assert(count==2 && actions[0].type==APP_ACT_STREAM_CANCEL && !s.pc_ptt);
    pc(true,true,4900); ptt(true,4900); assert_started();
    ev(APP_EV_LINK_DOWN,0,0,5000); assert(count==2 && actions[0].type==APP_ACT_STREAM_CANCEL && !s.pc_ptt);
    // Talk cap: renewals must not reopen until the PC hotkey is released.
    ev(APP_EV_LINK_UP,0,0,6000); pc(true,true,6000); ptt(true,6000); assert_started();
    for (uint64_t t=6500; t<6000+APP_PTT_MAX_TALK_MS; t+=1000) { pc(true,true,t); ptt(true,t); }
    pc(true,true,6000+APP_PTT_MAX_TALK_MS); ptt(true,6000+APP_PTT_MAX_TALK_MS);
    tick(6000+APP_PTT_MAX_TALK_MS); assert_stopped();
    ptt(true,6100+APP_PTT_MAX_TALK_MS); assert(count==0 && s.state==APP_ST_READY);
    ptt(false,6200+APP_PTT_MAX_TALK_MS); assert(count==0);
    ptt(true,6300+APP_PTT_MAX_TALK_MS); assert_started();
}
static void start(uint64_t now) {
    ev(APP_EV_KEY_PRESS,APP_BTN_UP,15,now);
    assert(s.state==APP_ST_LISTENING && count==2);
    assert(actions[0].type==APP_ACT_SEND_VOICE_START);
    assert(actions[1].type==APP_ACT_STREAM_START);
}
int main(void) {
    app_state_init(&s);
    assert(s.state==APP_ST_READY && !s.link_up && !s.pc_ready);
    ev(APP_EV_KEY_PRESS,APP_BTN_UP,15,10); assert(count==0);
    ev(APP_EV_KEY_CLICK,APP_BTN_DOWN,2900,11); assert(count==0);
    ev(APP_EV_LINK_UP,0,0,20);
    ev(APP_EV_KEY_PRESS,APP_BTN_UP,15,30); assert(count==0);
    pc(false,true,40);
    ev(APP_EV_KEY_PRESS,APP_BTN_UP,15,50); assert(count==0);
    pc(true,true,60);
    ev(APP_EV_KEY_PRESS,APP_BTN_UP,2070,70); assert(count==0);
    start(100);
    ev(APP_EV_KEY_CLICK,APP_BTN_DOWN,2900,105); assert(count==0);
    ev(APP_EV_KEY_PRESS,APP_BTN_UP,15,110); assert(count==0);
    ev(APP_EV_KEY_LONG,APP_BTN_UP,15,900); assert(count==0);
    ev(APP_EV_KEY_LONG_UP,APP_BTN_UP,2900,1000);
    assert(s.state==APP_ST_READY && count==2);
    assert(actions[0].type==APP_ACT_STREAM_STOP && actions[1].type==APP_ACT_SEND_VOICE_END);
    ev(APP_EV_KEY_RELEASE,APP_BTN_UP,2900,1001); assert(count==0);
    ev(APP_EV_KEY_PRESS,APP_BTN_DOWN,1060,1005); assert(count==0);
    ev(APP_EV_KEY_RELEASE,APP_BTN_DOWN,2900,1006); assert(count==0);
    ev(APP_EV_KEY_CLICK,APP_BTN_DOWN,2900,1007);
    assert(count==1 && actions[0].type==APP_ACT_SEND_KEY_ACTION && actions[0].u.key_action.action==APP_KEY_ENTER);
    for(int b=0;b<3;b++) {
        if(b==APP_BTN_DOWN)continue;
        ev(APP_EV_KEY_CLICK,b,2900,1010); assert(count==0);
        ev(APP_EV_KEY_LONG,b,15,1020); assert(count==0);
    }
    ev(APP_EV_AGENT_STATUS,0,0,1100); assert(s.state==APP_ST_READY && count==0);
    ev(APP_EV_APPROVAL_REQUEST,0,0,1101); assert(s.state==APP_ST_READY && count==0);
    ev(APP_EV_TRANSCRIPT,0,0,1102); assert(s.state==APP_ST_READY && count==0);
    start(1200);
    pc(false,true,1300); assert(s.state==APP_ST_READY && count==2);
    assert(actions[0].type==APP_ACT_STREAM_CANCEL);
    pc(true,true,1400); start(1500);
    ev(APP_EV_LINK_DOWN,0,0,1600); assert(count==2 && !s.pc_ready && !s.link_up);
    ev(APP_EV_KEY_CLICK,APP_BTN_DOWN,2900,1601); assert(count==0);
    ev(APP_EV_LINK_UP,0,0,1700);
    ev(APP_EV_KEY_PRESS,APP_BTN_UP,15,1710); assert(count==0);
    pc(true,true,1800); start(1900);
    ev(APP_EV_TICK,0,0,5400); assert(count==2 && !s.pc_ready);
    pc(true,true,5500); start(5600);
    ev(APP_EV_AUDIO_ERROR,0,0,5700); assert(count==2 && s.state==APP_ST_READY);
    app_ui_snapshot_t snap; app_state_snapshot(&s,5701,&snap); assert(snap.toast[0]);
    pc(true,true,6000); start(6100);
    pc(true,true,APP_PTT_MAX_TALK_MS+6100);
    ev(APP_EV_TICK,0,0,APP_PTT_MAX_TALK_MS+6100); assert(count==2 && s.state==APP_ST_READY);
    app_state_init(&s); assert(!s.pc_ready && !s.link_up && s.state==APP_ST_READY);
    ev(APP_EV_INIT_FAILED,0,0,7000); assert(s.init_failed);
    ev(APP_EV_LINK_UP,0,0,7001); pc(true,true,7002);
    ev(APP_EV_KEY_PRESS,APP_BTN_UP,15,7003);
    assert(!s.link_up && !s.pc_ready && count==0 && s.state==APP_ST_READY);
    ev(APP_EV_KEY_CLICK,APP_BTN_DOWN,2900,7004); assert(count==0);
    app_state_snapshot(&s,7005,&snap); assert(snap.init_failed);
    app_state_init(&s); assert(!s.init_failed);
    ev(APP_EV_LINK_UP,0,0,7006); pc(true,true,7007); start(7008);
    pc_hotkey();
    puts("Voice remote: PC hotkey hold/renew/timeout/isolation/talk-cap PASS");
    puts("Voice remote: readiness, hold/release, duplicate events, faults, timeouts, retired actions PASS");
}
