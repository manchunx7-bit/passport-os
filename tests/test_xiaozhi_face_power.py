"""Exercise the production face timer across blanking and waking."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'main/apps/xiaozhi/xiaozhi_ui.cc').read_text(encoding='utf-8')
a=s.index('void face_tick(');b=s.index('\nvoid set_badge_color',a)
c=s.index('bool companion_needed()');d=s.index('\nvoid conversation_layout',c)
code=r'''
#include <cassert>
#include <string>
#include "apps/xiaozhi/xiaozhi_level.h"
struct lv_timer_t {int period;};
struct Obj {bool hidden;int redraws;};
static Obj screen{},companion{},badge{};
static Obj *s_screen=&screen,*s_companion=&companion,*s_status_badge=&badge;
static bool visible=true;
static bool app_registry_screen_is_on(){return visible;}
static void lv_timer_set_period(lv_timer_t *t,int ms){t->period=ms;}
static constexpr int LV_OBJ_FLAG_HIDDEN=1;
static bool lv_obj_has_flag(Obj *o,int){return o->hidden;}
static void lv_obj_invalidate(Obj *o){++o->redraws;}
namespace face {
using xiaozhi::face::Waveform;
using xiaozhi::face::kWaveBars;
constexpr int kFramePeriodMs=50;
struct Pose {int frame;uint8_t bars[kWaveBars]{};
 bool operator==(const Pose &p)const{
  for(int i=0;i<kWaveBars;++i)if(bars[i]!=p.bars[i])return false;
  return frame==p.frame;
 }};
struct Animator {int calls=0;bool change=true;Pose step(int){++calls;return {change?calls:0};}};
}
static face::Animator s_face_animator;
static face::Pose s_face_pose{};
static face::Waveform s_waveform;
enum class State {Idle, Listening, Thinking, Speaking};
static State s_face_state;
static std::string s_user_text,s_ai_reply_buffer;
static bool s_text_expanded=false;
static int face_activity(State state){return static_cast<int>(state);}
struct AudioService {static uint8_t VisualLevel(){return 80;}};
'''+s[a:b]+s[c:d]+r'''
int main(){
 lv_timer_t timer{};
 face_tick(&timer);assert(timer.period==50&&companion.redraws==1);
 visible=false;
 for(int i=0;i<300;++i)face_tick(&timer);
 assert(timer.period==1000&&s_face_animator.calls==1&&companion.redraws==1&&badge.redraws==1);
 visible=true;face_tick(&timer);
 assert(timer.period==50&&s_face_animator.calls==2&&companion.redraws==2);
 companion.hidden=true;face_tick(&timer);assert(companion.redraws==2&&badge.redraws==3);
 s_face_animator.change=false;s_face_pose={0};face_tick(&timer);assert(badge.redraws==3);
 s_screen=nullptr;face_tick(&timer);assert(s_face_animator.calls==4);
 s_screen=&screen;s_face_state=State::Speaking;companion.hidden=false;
 face_tick(&timer);assert(s_face_pose.bars[face::kWaveBars-1]==80);
 const int calls=s_face_animator.calls;
 visible=false;face_tick(&timer);
 assert(s_face_animator.calls==calls&&s_face_pose.bars[face::kWaveBars-2]==0);
 visible=true;s_face_state=State::Idle;face_tick(&timer);
 for(auto bar:s_face_pose.bars)assert(bar==0);
 assert(companion_needed());
 s_user_text="transcript";assert(!companion_needed());
 for(State state:{State::Listening,State::Thinking,State::Speaking}){
  s_face_state=state;assert(companion_needed());
  s_text_expanded=true;assert(!companion_needed());s_text_expanded=false;
 }
 s_face_state=State::Idle;s_user_text.clear();s_ai_reply_buffer="reply";
 assert(!companion_needed());
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp)/'face.cc';p.write_text(code)
 subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(root/'main'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
print('Face timer: blanking suppresses animation/redraw, wake restores cadence, hidden/unchanged/deleted UI PASS')
