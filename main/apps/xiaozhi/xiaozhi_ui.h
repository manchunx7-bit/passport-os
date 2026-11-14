#pragma once

#include "apps/xiaozhi/xiaozhi_protocol.h"
#include <string>

namespace xiaozhi {

// UI 层只允许从"控制任务"调用；每个入口都在 LVGL 锁【内部】重新校验控件指针。
//
// 旧实现把 `if (!s_status_label || !lock())` 写在加锁之前：ui_hide() 会在持锁时
// 删除控件并把指针置空，而网络任务可能在检查通过之后、加锁之前被抢占 —— 随后拿
// 着已释放的 lv_obj 调用 lv_label_set_text()，这就是"对话过程中偶发闪退"的来源
// 之一。现在校验与使用都在同一把锁内，不存在这个窗口。

void ui_init();
void ui_show();
void ui_hide();

void ui_set_state(State state);                       // 顶部状态胶囊
void ui_set_emotion(const char *emotion);              // 服务端 llm.emotion → 表情
void ui_set_status_text(const std::string &text);      // 自定义状态文案（激活码/错误）
void ui_set_user_text(const std::string &text);        // 用户说的话（替换）
void ui_set_ai_text(const std::string &text);          // 小智回复（替换）
void ui_append_ai_text(const std::string &text);       // 小智回复（追加）
void ui_set_hint(const std::string &text);             // 底部按键提示
void ui_clear_text();
void ui_set_volume(unsigned percent);

} // namespace xiaozhi
