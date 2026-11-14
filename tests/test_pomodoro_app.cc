// Real application handlers and heartbeat; LVGL/BSP only are mocked.
#include "../main/apps/pomodoro/app_pomodoro.cc"

passport_app_id_t app_registry_get_active(){return static_cast<passport_app_id_t>(mock_foreground);}
bool app_registry_screen_is_on(){return mock_awake;}
static bool sound_available=false;
static unsigned sound_notices=0, sound_cancels=0;
bool app_registry_sound_notice(){assert(mock_locks>0);if(!sound_available)return false;++sound_notices;return true;}
void ui_sfx_cancel_alarm(){++sound_cancels;}
bool app_registry_wake_for_notice(){
    assert(mock_locks>0);
    if(!mock_allow_wake)return false;
    ++mock_wakes;mock_awake=true;return true;
}

static void press(uint8_t key){g_pomodoro_app.on_key(key,BTN_EVT_CLICK,0);assert(mock_locks==0);}
int main(){
    start();assert(mock_timers==1&&mock_objects>10);
    const auto page_objects=mock_objects;
    press(BSP_BTN_OK);assert(s_clock.status==POMODORO_RUNNING);
    mock_now+=10000000;tick(nullptr);assert(s_time->text=="24:50");
    press(BSP_BTN_DOWN);assert(s_reset_confirm&&s_clock.status==POMODORO_RUNNING);
    press(BSP_BTN_UP);assert(!s_reset_confirm);
    press(BSP_BTN_OK);assert(s_clock.status==POMODORO_PAUSED);
    const auto paused=s_clock.remaining_us;
    mock_now+=60000000;tick(nullptr);assert(s_clock.remaining_us==paused);
    press(BSP_BTN_DOWN);press(BSP_BTN_OK);
    assert(s_clock.status==POMODORO_READY&&s_clock.remaining_us==POMODORO_FOCUS_US);
    press(BSP_BTN_OK);stop();
    assert(mock_objects==0&&mock_timers==0&&s_clock.status==POMODORO_RUNNING);
    mock_now+=20000000;pomodoro_service_tick();assert(mock_objects==0&&mock_timers==0);
    // Completion survives heap pressure and a busy screen transition, with no
    // allocation or wake until both conditions permit the single notice.
    mock_now=s_clock.deadline_us;mock_heap=7000;pomodoro_service_tick();
    assert(s_clock.status==POMODORO_FINISHED&&s_clock.notice_pending&&s_clock.completed==1);
    assert(mock_objects==0&&mock_wakes==0);
    mock_heap=100000;mock_largest=2000;pomodoro_service_tick();assert(mock_objects==0&&s_clock.notice_pending);
    mock_largest=40000;mock_allow_wake=false;pomodoro_service_tick();assert(mock_objects==0&&s_clock.notice_pending);
    mock_allow_wake=true;mock_awake=false;pomodoro_service_tick();
    assert(!s_clock.notice_pending&&mock_objects==1&&mock_timers==0&&mock_wakes==1&&mock_awake);
    assert(mock_foreground==APP_ID_XIAOZHI);
    assert(mock_top.children[0]->text=="番茄钟 · 专注完成");
    for(int i=0;i<20;++i)pomodoro_service_tick();assert(mock_objects==1&&mock_wakes==1);
    mock_now+=10000000;pomodoro_service_tick();assert(mock_objects==0&&mock_top.children.empty());
    assert(s_clock.sound_pending && !sound_notices);
    sound_available=true;pomodoro_service_tick();
    assert(!s_clock.sound_pending && sound_notices==1);
    pomodoro_service_tick();assert(sound_notices==1);
    // The clock's own page needs no overlay allocation when its interval ends.
    s_clock=POMODORO_INITIALIZER;mock_foreground=APP_ID_POMODORO;start();press(BSP_BTN_OK);
    mock_awake=false;const auto old_time=s_time->text;
    mock_now=s_clock.deadline_us;tick(nullptr);assert(s_time->text==old_time);
    pomodoro_service_tick();assert(mock_awake&&s_time->text=="00:00"&&!s_clock.notice_pending);
    assert(mock_objects==page_objects&&mock_top.children.empty());stop();
    // A visible-page refresh may finish the interval before the heartbeat.
    // Its visual acknowledgement must not consume the independent sound.
    s_clock=POMODORO_INITIALIZER;start();press(BSP_BTN_OK);
    mock_now=s_clock.deadline_us;tick(nullptr);assert(s_clock.sound_pending);
    s_clock.notice_pending=false;const auto before_sound=sound_notices;
    pomodoro_service_tick();assert(!s_clock.sound_pending&&sound_notices==before_sound+1);
    // Acknowledging a delayed reminder starts the next interval silently.
    sound_available=false;press(BSP_BTN_OK);mock_now=s_clock.deadline_us;tick(nullptr);
    assert(s_clock.sound_pending);const auto before_cancel=sound_cancels;
    press(BSP_BTN_OK);assert(!s_clock.sound_pending&&sound_cancels==before_cancel+1);stop();
    // Repeated entry/exit cannot accumulate UI objects or timers. The same
    // paused/running model persists, independently of the page lifetime.
    s_clock=POMODORO_INITIALIZER;
    for(int i=0;i<100;++i){start();assert(mock_objects==page_objects&&mock_timers==1);stop();assert(!mock_objects&&!mock_timers);}
    mock_lock_available=false;pomodoro_service_tick();assert(mock_locks==0);
    mock_lock_available=true;
    // Voice can control the actual clock without creating a UI or stealing
    // foreground. All operations share the same lock as keys and heartbeat.
    s_clock=POMODORO_INITIALIZER;mock_foreground=APP_ID_XIAOZHI;sound_available=false;
    PomodoroSnapshot snapshot;pomodoro_command_result_t result;
    mock_lock_available=false;
    assert(!pomodoro_control(POMODORO_START,0,&snapshot,&result));
    assert(s_clock.status==POMODORO_READY);
    mock_lock_available=true;
    assert(pomodoro_control(POMODORO_START,0,&snapshot,&result));
    assert(result==POMODORO_COMMAND_OK&&snapshot.status==POMODORO_RUNNING&&snapshot.remaining_seconds==1500);
    assert(!mock_objects&&!mock_timers&&!mock_locks&&mock_foreground==APP_ID_XIAOZHI);
    mock_now+=7000000;
    assert(pomodoro_control(POMODORO_PAUSE,0,&snapshot,&result)&&snapshot.remaining_seconds==1493);
    mock_now+=60000000;
    assert(pomodoro_control(POMODORO_QUERY,0,&snapshot,&result)&&snapshot.remaining_seconds==1493);
    start();assert(s_time->text=="24:53");
    assert(pomodoro_control(POMODORO_RESUME,0,&snapshot,&result)&&s_clock.status==POMODORO_RUNNING);
    stop();mock_now=s_clock.deadline_us;mock_awake=false;
    pomodoro_service_tick();assert(mock_awake&&s_clock.completed==1&&s_clock.sound_pending);
    assert(mock_foreground==APP_ID_XIAOZHI);
    mock_foreground=APP_ID_HOME;sound_available=true;
    const auto sounds=sound_notices;
    pomodoro_service_tick();assert(sound_notices==sounds+1&&!s_clock.sound_pending);
    assert(pomodoro_control(POMODORO_RESET,0,&snapshot,&result));
    assert(!mock_objects&&!mock_timers&&snapshot.status==POMODORO_READY&&snapshot.completed==1);
    puts("Pomodoro application: real key handlers, reset/cancel, background, low-memory deferral, transition deferral, one notice, timeout/wake, no foreground switch and 100 leak-free UI cycles PASS");
}
