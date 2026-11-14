#include "registry_platform.h"
#include <cstring>
#include "../main/launcher/app_registry.cc"
#include "apps/netease/netease_controls.h"
#include "launcher/ok_gesture.h"
static int starts,stops,keys;
static lv_obj_t *screens[APP_ID_COUNT]{};
static int conversation_requests=0;
void xiaozhi_app_request_conversation(){
 assert(app_registry_get_active()==APP_ID_XIAOZHI);
 assert(screens[APP_ID_XIAOZHI]);
 ++conversation_requests;
}
static bool release_heap_on_stop=false;
static void start(){++starts;screens[app_registry_get_active()]=lv_obj_create(nullptr);lv_screen_load(screens[app_registry_get_active()]);}
static void stop(){++stops;auto &screen=screens[app_registry_get_active()];if(screen){lv_obj_delete(screen);screen=nullptr;}
 if(release_heap_on_stop){mock_free=100000;mock_largest=40000;}}
static bool prepare(){return mock_ready;}
static bool key_opens_launcher=false;
static void key(uint8_t,app_btn_event_t,uint16_t){++keys;if(key_opens_launcher)app_registry_switch_to(APP_ID_LAUNCHER);}
static netease::Page music_page=netease::Page::Lists;
static int music_back_calls=0;
static bool music_back(){++music_back_calls;if(music_page==netease::Page::Lists)return false;music_page=netease::back_page(music_page,true);return true;}
static passport_app_t app(passport_app_id_t id){passport_app_t a{};a.id=id;a.name="test";a.start=start;a.stop=stop;a.on_key=key;a.prepare_stop=prepare;if(id==APP_ID_NETEASE)a.on_back=music_back;return a;}
const passport_app_t g_radio_app=app(APP_ID_RADIO),g_voice_app=app(APP_ID_VOICE),g_xiaozhi_app=app(APP_ID_XIAOZHI),
 g_wifi_app=app(APP_ID_WIFI),g_settings_app=app(APP_ID_SETTINGS),g_home_app=app(APP_ID_HOME),g_game_app=app(APP_ID_GAME),
 g_tetris_app=app(APP_ID_TETRIS),g_stopwatch_app=app(APP_ID_STOPWATCH),g_sound_jump_app=app(APP_ID_SOUND_JUMP),g_muse_app=app(APP_ID_MUSE),g_pomodoro_app=app(APP_ID_POMODORO),
 g_netease_app=app(APP_ID_NETEASE),g_reminders_app=app(APP_ID_REMINDERS),g_lists_app=app(APP_ID_LISTS);
static bool mock_reminder_active=false;
namespace reminders {
 bool alarm_active(){return mock_reminder_active;}
 bool intercept_key(uint8_t,int){return mock_reminder_active;}
 void dismiss_for_navigation(){mock_reminder_active=false;}
}
void launcher_ui_show(){start();} void launcher_ui_hide(){stop();}
void launcher_ui_on_key(uint8_t,app_btn_event_t){++keys;}
bool wifi_setup_can_leave(){return mock_setup;}
bool boot_chime_busy(){return mock_chime;}
static bool feedback_ready=true;
static int feedback_count=0;
static ui_sfx_kind_t last_feedback=UI_SFX_NAV;
void ui_sfx_play(ui_sfx_kind_t kind){++feedback_count;last_feedback=kind;}
bool ui_sfx_prepare_stop(){return feedback_ready;}
static int alarm_attempts=0;
bool ui_sfx_alarm_tick(){++alarm_attempts;return true;}
static char overlay_text[64];
void system_overlay_show(const char *text,uint32_t){std::snprintf(overlay_text,sizeof(overlay_text),"%s",text);}
void system_overlay_hide(){overlay_text[0]=0;}
int main(){
 // Audio/network sessions retain fast polling with the display off.
 for(int id=0;id<APP_ID_COUNT;++id){
  s_active_id.store(static_cast<passport_app_id_t>(id));
  s_screen_on.store(true);assert(!app_registry_background_idle());
  s_screen_on.store(false);
  const bool streaming=id==APP_ID_RADIO||id==APP_ID_NETEASE||id==APP_ID_VOICE||id==APP_ID_XIAOZHI||id==APP_ID_MUSE;
  assert(app_registry_background_idle()==!streaming);
 }
 s_active_id.store(APP_ID_LAUNCHER);s_screen_on.store(true);
 app_registry_init();app_registry_switch_to(APP_ID_HOME);assert(starts==1);
 mock_reminder_active=true;assert(!app_registry_sound_notice()&&alarm_attempts==0);mock_reminder_active=false;
 assert(mock_socket_cleanups==2);
 auto *old=mock_active;const int before_stops=stops;
 mock_ready=false;app_registry_switch_to(APP_ID_RADIO);
 for(int i=0;i<50;++i)app_registry_process_pending_switch();
 assert(starts==1&&stops==before_stops&&mock_active==old);
 app_registry_dispatch_key(BSP_BTN_UP,BTN_EVT_CLICK,0);assert(keys==0);
 // Latest request wins; unknown IDs cannot consume a valid pending request.
 app_registry_switch_to(APP_ID_XIAOZHI);app_registry_switch_to(APP_ID_COUNT);
 assert(s_pending_target==APP_ID_XIAOZHI);
 mock_ready=true;app_registry_process_pending_switch();
 assert(starts==2&&stops==before_stops+1&&app_registry_get_active()==APP_ID_XIAOZHI);
 assert(mock_screens.size()==1);
 // A busy display must not discard a request after its worker has stopped.
 mock_display_lock=false;app_registry_switch_to(APP_ID_HOME);assert(starts==2);
 mock_display_lock=true;app_registry_process_pending_switch();assert(starts==3);
 mock_chime=true;app_registry_switch_to(APP_ID_GAME);assert(starts==3);
 mock_chime=false;app_registry_process_pending_switch();assert(starts==4);
 // Sleep/wake preserves PM lock balance and consumes the waking gesture.
 app_registry_set_screen_timeout(1);mock_now+=2000000;app_registry_tick();assert(!app_registry_screen_is_on());
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_PRESS,0);assert(app_registry_screen_is_on());
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_CLICK,0);assert(keys==0);
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_RELEASE,0);assert(keys==0);
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_PRESS,0);app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_CLICK,0);assert(keys==2);
 assert(!overlay_text[0]);
 // Any key wakes after a long dark interval. Its full gesture is consumed,
 // including long OK, but the next ordinary press immediately reaches the app.
 for (uint8_t button : {BSP_BTN_UP, BSP_BTN_DOWN, BSP_BTN_OK}) {
  app_registry_set_screen_timeout(1);mock_now+=2000000;app_registry_tick();
  assert(!app_registry_screen_is_on()&&*s_display_awake_lock==0);
  mock_now+=11000000;
  const int before_wake=keys;const auto foreground=app_registry_get_active();
  app_registry_dispatch_key(button,BTN_EVT_PRESS,0);
  assert(app_registry_screen_is_on()&&*s_display_awake_lock==1);
  app_registry_dispatch_key(button,BTN_EVT_LONG,0);
  app_registry_dispatch_key(button,BTN_EVT_LONG_UP,0);
  app_registry_dispatch_key(button,BTN_EVT_CLICK,0);
  app_registry_dispatch_key(button,BTN_EVT_RELEASE,0);
  assert(keys==before_wake&&app_registry_get_active()==foreground&&!overlay_text[0]);
  app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_PRESS,0);
  app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_CLICK,0);
  assert(keys==before_wake+2&&app_registry_get_active()==foreground);
 }
 // Waking does not shorten the configured idle timeout to the old 10 s lock timer.
 app_registry_set_screen_timeout(300);mock_now+=11000000;app_registry_tick();
 assert(app_registry_screen_is_on()&&*s_display_awake_lock==1);
 mock_now+=290000000;app_registry_tick();
 assert(!app_registry_screen_is_on()&&*s_display_awake_lock==0);
 // A reminder after a long dark interval is immediately answerable as well.
 mock_now+=11000000;assert(app_registry_wake_for_notice());
 const int before_notice_key=keys;
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_PRESS,0);
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_CLICK,0);
 assert(keys==before_notice_key+2&&!overlay_text[0]);

 // Returning to the still-visible app during cancellation must supersede the
 // pending destination and restart the stopped worker, not drop the request.
 mock_ready=false;app_registry_switch_to(APP_ID_HOME);app_registry_switch_to(APP_ID_GAME);
 assert(s_pending_target==APP_ID_GAME);mock_ready=true;const int before_restart=starts;
 app_registry_process_pending_switch();assert(starts==before_restart+1&&app_registry_get_active()==APP_ID_GAME);
 // A reminder wakes the display without changing foreground or its worker.
 app_registry_set_screen_timeout(1);mock_now+=2000000;app_registry_tick();assert(!app_registry_screen_is_on());
 const int notice_starts=starts,notice_stops=stops;
 assert(app_registry_wake_for_notice());assert(app_registry_screen_is_on());
 assert(starts==notice_starts&&stops==notice_stops&&app_registry_get_active()==APP_ID_GAME);
 assert(*s_display_awake_lock==1);assert(app_registry_wake_for_notice());assert(*s_display_awake_lock==1);
 app_registry_switch_to(APP_ID_HOME);
 app_registry_dispatch_key(BSP_BTN_UP,BTN_EVT_PRESS,0);
 app_registry_dispatch_key(BSP_BTN_UP,BTN_EVT_CLICK,0);
 app_registry_dispatch_key(BSP_BTN_UP,BTN_EVT_RELEASE,0);
 assert(feedback_count==1);
 // Internal Back precedes the app-level exit, without restarting audio/UI.
 app_registry_switch_to(APP_ID_NETEASE);music_page=netease::Page::Player;
 const int music_starts=starts,music_stops=stops;
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_LONG,0);
 assert(music_page==netease::Page::Tracks&&app_registry_get_active()==APP_ID_NETEASE);
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_LONG_UP,0);
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_RELEASE,0);
 assert(music_back_calls==1&&starts==music_starts&&stops==music_stops);
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_LONG,0);
 assert(music_page==netease::Page::Lists&&app_registry_get_active()==APP_ID_NETEASE);
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_LONG_UP,0);
 assert(starts==music_starts&&stops==music_stops);
 // Once a cooperative shutdown is pending, do not call into its old model.
 mock_ready=false;app_registry_switch_to(APP_ID_LAUNCHER);
 const int back_before_shutdown=music_back_calls;
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_LONG,0);
 assert(music_back_calls==back_before_shutdown);
 mock_ready=true;app_registry_process_pending_switch();
 app_registry_switch_to(APP_ID_NETEASE);music_page=netease::Page::Lists;
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_LONG,0);
 assert(app_registry_get_active()==APP_ID_LAUNCHER);
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_LONG_UP,0);
 app_registry_switch_to(APP_ID_HOME);
 // Clean-launch policy is covered at the end; these flows start from a fresh heap.
 
 feedback_ready=false;app_registry_switch_to(APP_ID_RADIO);
 assert(app_registry_get_active()==APP_ID_HOME&&s_pending_target==APP_ID_RADIO);
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_CLICK,0);assert(feedback_count==1);
 feedback_ready=true;app_registry_process_pending_switch();assert(app_registry_get_active()==APP_ID_RADIO);
 app_registry_dispatch_key(BSP_BTN_UP,BTN_EVT_CLICK,0);assert(feedback_count==1);
 app_registry_switch_to(APP_ID_HOME);
 app_registry_set_screen_timeout(1);mock_now+=2000000;app_registry_tick();
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_PRESS,0);
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_CLICK,0);assert(feedback_count==1);
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_PRESS,0);
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_CLICK,0);assert(feedback_count==2);
 assert(last_feedback==UI_SFX_CONFIRM);
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_RELEASE,0);assert(feedback_count==2);
 app_registry_switch_to(APP_ID_LAUNCHER);
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_LONG,0);
 assert(app_registry_get_active()==APP_ID_HOME&&feedback_count==3&&last_feedback==UI_SFX_BACK);
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_LONG_UP,0);
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_RELEASE,0);assert(feedback_count==3);
 mock_ready=false;key_opens_launcher=true;
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_CLICK,0);
 assert(feedback_count==4&&s_pending_target==APP_ID_LAUNCHER);
 mock_ready=true;key_opens_launcher=false;app_registry_process_pending_switch();
 assert(app_registry_get_active()==APP_ID_LAUNCHER&&feedback_count==4);
 s_building_target=true;assert(!app_registry_wake_for_notice());assert(!app_registry_sound_notice());s_building_target=false;
 assert(app_registry_sound_notice()&&alarm_attempts==1);
 mock_mutex_available=false;assert(!app_registry_sound_notice());mock_mutex_available=true;
 mock_chime=true;assert(!app_registry_sound_notice());mock_chime=false;
 app_registry_switch_to(APP_ID_RADIO);assert(!app_registry_sound_notice()&&alarm_attempts==1);
 // Clean launch: heavy apps start in place while the heap fits them, however
 // often they are re-entered; light apps never restart.
 app_registry_switch_to(APP_ID_HOME);assert(mock_restarts==0);
 app_registry_switch_to(APP_ID_MUSE);assert(app_registry_get_active()==APP_ID_MUSE&&mock_restarts==0);
 app_registry_switch_to(APP_ID_HOME);app_registry_switch_to(APP_ID_XIAOZHI);
 app_registry_switch_to(APP_ID_HOME);app_registry_switch_to(APP_ID_RADIO);
 assert(app_registry_get_active()==APP_ID_RADIO&&mock_restarts==0);
 // All registered pages, including hidden setup and launcher. Double must
 // never forward a click to a confirmation dialog or toggle Xiaozhi twice.
 for(int id=0;id<APP_ID_COUNT;++id){
  app_registry_switch_to(static_cast<passport_app_id_t>(id));
  const int a=starts,b=stops,c=keys,requests=conversation_requests;
  app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_DOUBLE,600);
  assert(app_registry_get_active()==APP_ID_XIAOZHI&&keys==c);
  assert(starts==a+(id!=APP_ID_XIAOZHI)&&stops==b+(id!=APP_ID_XIAOZHI));
  assert(mock_screens.size()==1&&conversation_requests==requests+1);
 }
 // Full producer->registry path: a sleeping screen wakes on the first press,
 // then enters Xiaozhi on the second without delivering either click.
 app_registry_switch_to(APP_ID_HOME);
 app_registry_set_screen_timeout(1);mock_now+=2000000;app_registry_tick();
 OkGesture gesture;
 const auto emit=[](uint8_t b,app_btn_event_t e,uint16_t mv){app_registry_dispatch_key(b,e,mv);};
 const auto raw=[&](app_btn_event_t e,int delay){mock_now+=delay;
  gesture.event(BSP_BTN_OK,e,600,mock_now,app_registry_get_active(),emit);};
 const int wake_keys=keys;
 raw(BTN_EVT_PRESS,0);assert(app_registry_screen_is_on());
 raw(BTN_EVT_CLICK,80000);raw(BTN_EVT_RELEASE,0);
 assert(keys==wake_keys&&app_registry_get_active()==APP_ID_HOME);
 raw(BTN_EVT_PRESS,100000);raw(BTN_EVT_CLICK,80000);raw(BTN_EVT_RELEASE,0);
 assert(app_registry_get_active()==APP_ID_XIAOZHI&&keys==wake_keys+1);
 gesture.poll(mock_now+1000000,APP_ID_XIAOZHI,emit);assert(keys==wake_keys+1);
 app_registry_switch_to(APP_ID_REMINDERS);mock_reminder_active=true;
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_DOUBLE,600);
 assert(!mock_reminder_active&&app_registry_get_active()==APP_ID_XIAOZHI);
 // Slow audio/network shutdown and a busy LVGL lock cannot start the new
 // app before the old one has released its resources.
 app_registry_switch_to(APP_ID_NETEASE);mock_ready=false;
 const int a=starts,b=stops,requests=conversation_requests;
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_DOUBLE,600);
 for(int i=0;i<20;++i)app_registry_process_pending_switch();
 assert(starts==a&&stops==b&&s_pending_target==APP_ID_XIAOZHI);
 mock_ready=true;mock_display_lock=false;app_registry_process_pending_switch();
 assert(starts==a&&stops==b&&conversation_requests==requests);
 mock_display_lock=true;app_registry_process_pending_switch();
 assert(starts==a+1&&stops==b+1&&app_registry_get_active()==APP_ID_XIAOZHI);
 assert(conversation_requests==requests+1);
 // Phone data writes defer, rather than discard, the global request.
 app_registry_switch_to(APP_ID_WIFI);mock_setup=false;
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_DOUBLE,600);
 app_registry_process_pending_switch();
 assert(s_pending_target==APP_ID_XIAOZHI&&app_registry_get_active()==APP_ID_WIFI);
 mock_setup=true;app_registry_complete_setup();assert(s_pending_target==APP_ID_XIAOZHI);
 app_registry_process_pending_switch();assert(app_registry_get_active()==APP_ID_XIAOZHI);
 app_registry_complete_setup();assert(app_registry_get_active()==APP_ID_XIAOZHI);
 app_registry_switch_to(APP_ID_WIFI);app_registry_complete_setup();assert(app_registry_get_active()==APP_ID_HOME);
 // Superseding a deferred shortcut cancels its auto-listen intent. A later
 // ordinary menu entry must stay silent.
 app_registry_switch_to(APP_ID_NETEASE);mock_ready=false;
 const int cancelled=conversation_requests;
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_DOUBLE,600);
 app_registry_switch_to(APP_ID_HOME);mock_ready=true;app_registry_process_pending_switch();
 app_registry_switch_to(APP_ID_XIAOZHI);
 assert(conversation_requests==cancelled&&!s_start_xiaozhi_conversation);
 // A low reading while the old audio app is alive is not a reason to reboot:
 // only the RAM left AFTER its teardown decides the next launch.
 app_registry_switch_to(APP_ID_RADIO);mock_free=8000;mock_largest=4000;release_heap_on_stop=true;
 app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_DOUBLE,600);release_heap_on_stop=false;
 assert(app_registry_get_active()==APP_ID_XIAOZHI&&mock_restarts==0);
 // Insufficient total/contiguous RAM retires the source before restarting;
 // the clean-boot guard prevents a restart loop.
 for(bool fragmented:{false,true}){
  app_registry_switch_to(APP_ID_HOME);const int released=stops,not_started=starts;
  mock_largest=fragmented?15*1024:40000;mock_free=fragmented?100000:63*1024;
  try{app_registry_dispatch_key(BSP_BTN_OK,BTN_EVT_DOUBLE,600);assert(false);}catch(const MockRestart&){}
  assert(stops==released+1&&starts==not_started);
  assert(s_clean_launch[0]==kCleanTalkMagic);
  mock_reset_reason=ESP_RST_SW;assert(app_registry_take_clean_launch()==APP_ID_XIAOZHI);
  assert(s_start_xiaozhi_conversation);const int before_talk=conversation_requests;
  app_registry_switch_to(APP_ID_XIAOZHI);assert(app_registry_get_active()==APP_ID_XIAOZHI);
  assert(conversation_requests==before_talk+1&&!s_start_xiaozhi_conversation);
  mock_largest=40000;mock_free=100000;
 }
 assert(mock_restarts==2);mock_restarts=0;s_fresh_heap=false;
 // An allocation failure during a heavy session marks the heap as strained:
 // leaving is fine, the next heavy entry restarts into itself once.
 ++mock_alloc_failures;app_registry_switch_to(APP_ID_HOME);app_registry_switch_to(APP_ID_GAME);
 assert(mock_restarts==0);
 const int clean_stops=stops;
 try{app_registry_switch_to(APP_ID_XIAOZHI);assert(false);}catch(const MockRestart&){}
 assert(mock_restarts==1&&stops==clean_stops+1);
 // The panel explains the restart instead of freezing on the old app.
 assert(!std::strcmp(mock_notice,"正在整理内存\n马上打开 test")&&!s_building_target&&!mock_timer.paused);
 // Only a software reset with an intact record is honoured, and only once.
 mock_reset_reason=ESP_RST_SW;assert(app_registry_take_clean_launch()==APP_ID_XIAOZHI);
 assert(app_registry_take_clean_launch()==APP_ID_COUNT);
 s_clean_launch[0]=kCleanLaunchMagic;s_clean_launch[1]=APP_ID_RADIO^kCleanLaunchMagic;
 mock_reset_reason=ESP_RST_PANIC;assert(app_registry_take_clean_launch()==APP_ID_COUNT);
 s_clean_launch[0]=kCleanLaunchMagic;s_clean_launch[1]=APP_ID_GAME^kCleanLaunchMagic;
 mock_reset_reason=ESP_RST_SW;assert(app_registry_take_clean_launch()==APP_ID_COUNT);
 s_clean_launch[0]=0x12345678;s_clean_launch[1]=APP_ID_RADIO;assert(app_registry_take_clean_launch()==APP_ID_COUNT);
 s_clean_launch[0]=kCleanTalkMagic;s_clean_launch[1]=APP_ID_RADIO^kCleanTalkMagic;
 assert(app_registry_take_clean_launch()==APP_ID_COUNT&&!s_start_xiaozhi_conversation);
 s_clean_launch[0]=kCleanTalkMagic;s_clean_launch[1]=APP_ID_XIAOZHI^kCleanTalkMagic;
 mock_reset_reason=ESP_RST_PANIC;
 assert(app_registry_take_clean_launch()==APP_ID_COUNT&&!s_start_xiaozhi_conversation);
 mock_reset_reason=ESP_RST_SW;
 // Real RAM is fresh after the restart; the mock keeps state, so model that.
 s_heap_strained=false;s_fresh_heap=false;
 // Per-app needs: Xiaozhi fits in 20 KiB contiguous, the MP3 decoder does not.
 mock_largest=20*1024;
 app_registry_switch_to(APP_ID_HOME);app_registry_switch_to(APP_ID_XIAOZHI);
 assert(app_registry_get_active()==APP_ID_XIAOZHI&&mock_restarts==1);
 app_registry_switch_to(APP_ID_HOME);
 try{app_registry_switch_to(APP_ID_RADIO);assert(false);}catch(const MockRestart&){}
 assert(mock_restarts==2);
 // Total free is checked as well as the largest block.
 mock_largest=40000;mock_free=40*1024;
 try{app_registry_switch_to(APP_ID_MUSE);assert(false);}catch(const MockRestart&){}
 assert(mock_restarts==3);
 // The boot after a clean launch never restarts again, even if the heap is short.
 s_clean_launch[0]=kCleanLaunchMagic;s_clean_launch[1]=APP_ID_MUSE^kCleanLaunchMagic;
 assert(app_registry_take_clean_launch()==APP_ID_MUSE);
 app_registry_switch_to(APP_ID_MUSE);assert(app_registry_get_active()==APP_ID_MUSE&&mock_restarts==3);
 mock_largest=40000;mock_free=100000;
 mock_active=nullptr;for(auto *p:mock_screens)delete p;mock_screens.clear();
 delete s_switch_mutex;delete s_display_awake_lock;
 puts("Registry: pending worker gate, feedback ownership, single click, wake gesture, latest target and screen lifecycle, clean launch PASS");
}
