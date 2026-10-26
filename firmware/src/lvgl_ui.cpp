// Codex 工作页：项目、模型、阶段、计划与耗时同屏；空闲进入时钟。
#include <Arduino.h>
#include <Adafruit_ST7789.h>
#include <lvgl.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include "config.h"
#include "display.h"
#include "lvgl_ui.h"
#include "codex_frames.h"
#include "tool_icons.h"
LV_FONT_DECLARE(font_cn_20);
LV_FONT_DECLARE(font_cn_24);
LV_FONT_DECLARE(font_cn_16);

namespace cc_hud {
namespace {
constexpr int kBufRows = 40;
alignas(4) uint8_t g_buf_a[kScreenWidth * kBufRows * 2], g_buf_b[kScreenWidth * kBufRows * 2];
const lv_color_t C_FG = lv_color_hex(0xEFFAF5), C_MUTED = lv_color_hex(0x93A5A0);
const lv_color_t C_ACCENT = lv_color_hex(0x63E6BE), C_RED = lv_color_hex(0xFF6B7A);
lv_obj_t *g_work, *g_idle, *g_project, *g_session_title, *g_short_id, *g_model, *g_phase, *g_detail, *g_plan, *g_duration, *g_sessions;
lv_obj_t *g_dot, *g_position, *g_battery, *g_icon, *g_anim, *g_alert, *g_metrics, *g_pending;
lv_obj_t *g_pet, *g_clock, *g_date, *g_status;
lv_image_dsc_t g_icon_dsc[kIconCount], g_frame_dsc[kCodexFrameCount];
const void* g_frames[kCodexFrameCount];
WorkSession g_rows[kMaxWorkSessions];
uint8_t g_count = 0;
uint32_t g_current_key = 0, g_manual_key = 0, g_manual_until = 0, g_last_active = 0, g_wait_key = 0;
bool g_on_clock = false, g_manual_clock = false, g_initialized = false, g_was_active = false;
constexpr uint32_t kManualHoldMs = 10000, kClockAfterMs = 180000;

void flush_cb(lv_display_t* disp, const lv_area_t* a, uint8_t* pixels) {
    displayGetTft().drawRGBBitmap(a->x1, a->y1, reinterpret_cast<uint16_t*>(pixels),
                                  a->x2-a->x1+1, a->y2-a->y1+1);
    lv_display_flush_ready(disp);
}
uint32_t tick_cb() { return millis(); }
void setText(lv_obj_t* label, const char* value) {
    if (std::strcmp(lv_label_get_text(label), value)) lv_label_set_text(label, value);
}
lv_obj_t* screen() {
    auto* s = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s, lv_color_black(), 0);
    lv_obj_set_style_pad_all(s, 0, 0); lv_obj_set_style_border_width(s, 0, 0);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    return s;
}
lv_obj_t* label(lv_obj_t* parent, int x, int y, int width, const lv_font_t* font, lv_color_t color) {
    auto* l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y); lv_obj_set_width(l, width);
    lv_obj_set_style_text_font(l, font, 0); lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP); lv_label_set_text(l, "");
    return l;
}
void imageDescriptor(lv_image_dsc_t& d, const uint16_t* px, int w, int h) {
    std::memset(&d, 0, sizeof(d));
    d.header.magic=LV_IMAGE_HEADER_MAGIC; d.header.cf=LV_COLOR_FORMAT_RGB565;
    d.header.w=w; d.header.h=h; d.header.stride=w*2;
    d.data_size=w*h*2; d.data=reinterpret_cast<const uint8_t*>(px);
}
struct PetSprite { const char* normal; const char* blink; };
constexpr PetSprite kPetSprites[kPetMoodCount] = {
    {"(=^.^=)", "(=^_^=)"},
    {"(='.'=)", "(=-.-=)"},
    {"(=u.u=)", "(=u_u=)"},
    {"(=>.<=)", "(=>_<=)"},
    {"(=T.T=)", "(=T_T=)"},
};
struct PetMove {
    uint32_t retarget_min, retarget_max;
    int16_t  smooth;
    bool     stop_centre;
};
constexpr PetMove kPetMove[kPetMoodCount] = {
    {3500, 6500, 12, false},
    {3000, 6000, 14, false},
    {4500, 8500, 20, false},
    {1200, 2500,  6, false},
    {99999, 99999, 1, true},
};
constexpr int16_t kPetMinX = 30, kPetMaxX = 150;  // label x range (obj left)
int16_t  g_pet_x = 90, g_pet_target = 90;
uint32_t g_pet_next_retarget = 0;
volatile int8_t g_pet_mood = kPetMoodHappy;

void pet_timer_cb(lv_timer_t*) {
    if (!g_on_clock || g_pet == nullptr) return;
    const uint32_t now = millis();
    int8_t m = g_pet_mood;
    if (m < 0 || m >= kPetMoodCount) m = kPetMoodHappy;
    const PetMove& mv = kPetMove[m];

    if (mv.stop_centre) {
        g_pet_target = (kPetMinX + kPetMaxX) / 2;
    } else if (now >= g_pet_next_retarget) {
        g_pet_target = static_cast<int16_t>(random(kPetMinX, kPetMaxX + 1));
        g_pet_next_retarget = now +
            static_cast<uint32_t>(random(mv.retarget_min, mv.retarget_max + 1));
    }
    const int32_t delta = g_pet_target - g_pet_x;
    int16_t nx = static_cast<int16_t>(g_pet_x + delta / mv.smooth);
    if (nx == g_pet_x && delta != 0) nx += (delta > 0) ? 1 : -1;
    g_pet_x = nx;

    // Blink + bob.
    const bool blink = (now % 4500u) >= 4250u;
    const int16_t bob = (mv.stop_centre) ? 0 : ((now / 500u) & 1);
    const PetSprite& sp = kPetSprites[m];
    lv_label_set_text_static(g_pet, blink ? sp.blink : sp.normal);
    lv_obj_set_pos(g_pet, g_pet_x, 14 + bob * 2);
}

void composeIdle(const QuotaSnapshot& q, uint64_t now_ms,
                 char* clk, size_t clk_n, char* dt, size_t dt_n) {
    if (q.unix_ts == 0) {
        snprintf(clk, clk_n, "--:--");
        snprintf(dt, dt_n, "等待 BLE 校时");
        return;
    }
    uint32_t el = 0;
    if (q.time_capture_ms > 0 && now_ms >= q.time_capture_ms) {
        el = static_cast<uint32_t>((now_ms - q.time_capture_ms) / 1000ULL);
    }
    time_t local = static_cast<time_t>(q.unix_ts) + el +
                   static_cast<time_t>(q.utc_offset_min) * 60;
    struct tm tmv;
    gmtime_r(&local, &tmv);
    snprintf(clk, clk_n, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    static const char* W[] = {"日","一","二","三","四","五","六"};
    snprintf(dt, dt_n, "%02d-%02d 周%s", tmv.tm_mon + 1, tmv.tm_mday, W[tmv.tm_wday % 7]);
}

void buildWork() {
    g_work=screen();
    auto* brand=label(g_work,12,8,68,&lv_font_montserrat_14,C_ACCENT); setText(brand,"Codex");
    g_battery=label(g_work,85,8,50,&lv_font_montserrat_14,C_MUTED);
    g_position=label(g_work,153,8,55,&lv_font_montserrat_14,C_MUTED);
    g_pending=label(g_work,88,5,124,&font_cn_16,C_RED);
    g_dot=lv_obj_create(g_work); lv_obj_set_pos(g_dot,218,10); lv_obj_set_size(g_dot,8,8);
    lv_obj_set_style_radius(g_dot,LV_RADIUS_CIRCLE,0); lv_obj_set_style_border_width(g_dot,0,0);
    lv_obj_remove_flag(g_dot,LV_OBJ_FLAG_SCROLLABLE);
    g_project=label(g_work,12,27,158,&font_cn_16,C_MUTED);
    g_short_id=label(g_work,172,28,56,&lv_font_montserrat_14,C_ACCENT);
    lv_obj_set_style_text_align(g_short_id,LV_TEXT_ALIGN_RIGHT,0);
    g_session_title=label(g_work,12,48,216,&font_cn_20,C_FG);
    lv_label_set_long_mode(g_session_title,LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_long_mode(g_project,LV_LABEL_LONG_SCROLL_CIRCULAR);
    g_model=label(g_work,12,75,216,&lv_font_montserrat_14,C_MUTED);
    g_metrics=label(g_work,12,94,216,&font_cn_16,C_MUTED);
    auto* sep=lv_obj_create(g_work); lv_obj_set_pos(sep,12,116); lv_obj_set_size(sep,216,1);
    lv_obj_set_style_border_width(sep,0,0); lv_obj_set_style_bg_color(sep,lv_color_hex(0x193F33),0);
    g_phase=label(g_work,12,121,174,&font_cn_24,C_FG);
    g_detail=label(g_work,12,151,174,&font_cn_16,C_MUTED);
    g_plan=label(g_work,12,173,216,&font_cn_16,C_MUTED);
    lv_label_set_long_mode(g_plan,LV_LABEL_LONG_SCROLL_CIRCULAR);
    g_duration=label(g_work,12,196,216,&font_cn_16,C_FG);
    g_sessions=label(g_work,12,217,216,&font_cn_16,C_MUTED);
    g_icon=lv_image_create(g_work); lv_obj_set_pos(g_icon,188,120);
    g_anim=lv_animimg_create(g_work); lv_obj_set_pos(g_anim,188,120); lv_obj_set_size(g_anim,40,40);
    lv_animimg_set_src(g_anim,const_cast<const void**>(g_frames),kCodexFrameCount);
    lv_animimg_set_duration(g_anim,1200); lv_animimg_set_repeat_count(g_anim,LV_ANIM_REPEAT_INFINITE);
    lv_animimg_start(g_anim);
}
void buildClock() {
    g_idle=screen();
    g_pet=label(g_idle,90,14,150,&lv_font_montserrat_24,C_ACCENT);
    g_clock=label(g_idle,0,72,240,&lv_font_montserrat_48,C_FG);
    g_date=label(g_idle,0,136,240,&font_cn_20,C_MUTED);
    g_status=label(g_idle,8,190,224,&font_cn_20,C_ACCENT);
    for(auto* l:{g_clock,g_date,g_status}) lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);
    lv_timer_create(pet_timer_cb,30,nullptr);
}
const char* phaseName(const WorkSession& s) {
    if(s.pending_questions && s.state==kAppStateIdle) return "待回答";
    if(s.phase==13) return "等待状态";
    if(s.state==kAppStateWaiting) return s.kind==1?"待批准":s.kind==2?"待回答":"等待处理";
    if(s.state==kAppStateIdle) return (s.flags&1)?"本轮结束":"就绪";
    switch(s.phase) {
        case 2:return "运行命令"; case 3:return "修改文件"; case 4:return "读取文件";
        case 5:return "检索资料"; case 6:return "调用工具"; case 7:return "使用工具";
        case 10:return "运行测试"; case 11:return "更新计划"; case 12:return "检查权限";
        default:return "思考中";
    }
}
int iconFor(const char* detail) {
    int best=kIconCount-1; size_t longest=0;
    for(int i=0;i<kIconMapLen;++i) {
        size_t n=std::strlen(kIconMap[i].prefix);
        if(n>longest && !std::strncmp(detail,kIconMap[i].prefix,n)){best=kIconMap[i].icon_idx;longest=n;}
    }
    return best;
}
void applyWork(const LvglUiModel& m, int index) {
    char buf[160];
    const bool valid=index>=0 && index<m.work_count;
    WorkSession fallback;
    fallback.state=static_cast<uint8_t>(m.app_state<0?kAppStateIdle:m.app_state);
    fallback.phase=fallback.state==kAppStateTool?7:fallback.state;
    fallback.kind=m.app_intervention;
    std::strncpy(fallback.detail,m.app_detail,sizeof(fallback.detail)-1);
    const WorkSession& s=valid?m.work[index]:fallback;
    setText(g_project,valid && s.project[0]?s.project:"等待会话信息");
    setText(g_session_title,s.session_title[0]?s.session_title:"未命名会话");
    snprintf(buf,sizeof(buf),"#%s",s.short_id[0]?s.short_id:"----");
    setText(g_short_id,buf);
    const int code_width=static_cast<int>(std::strlen(buf))*10;
    lv_obj_set_width(g_short_id,code_width);lv_obj_set_x(g_short_id,228-code_width);
    lv_obj_set_width(g_project,208-code_width);
    snprintf(buf,sizeof(buf),"%s",s.model[0]?s.model:"--");
    setText(g_model,buf);
    char context[8];
    if((s.flags&4) && s.ctx_pct<=100) snprintf(context,sizeof(context),"%u%%",s.ctx_pct);
    else snprintf(context,sizeof(context),"--");
    snprintf(buf,sizeof(buf),"强度 %s  上下文已用 %s",s.effort[0]?s.effort:"--",context);
    setText(g_metrics,buf);
    if(s.pending_questions) snprintf(buf,sizeof(buf),"待回答 %u",s.pending_questions);
    else buf[0]=0;
    setText(g_pending,buf);
    setText(g_phase,phaseName(s));
    lv_obj_set_style_text_color(g_phase,s.state==kAppStateWaiting?C_RED:C_FG,0);
    if(s.detail[0] && std::strcmp(s.detail,"turn-complete"))
        snprintf(buf,sizeof(buf),"%s %s",s.state==kAppStateThinking?"最近":"工具",s.detail);
    else buf[0]=0;
    setText(g_detail,buf);
    if(s.flags&2) snprintf(buf,sizeof(buf),"步骤 %u/%u%s%s",s.plan_done,s.plan_total,s.step[0]?" ":"",s.step);
    else buf[0]=0;
    setText(g_plan,buf);
    if(s.flags&1) {
        uint32_t sec=s.elapsed_s;
        if(s.state!=kAppStateIdle && m.now_ms>=m.work_capture_ms) sec+=(m.now_ms-m.work_capture_ms)/1000;
        if(sec<3600) snprintf(buf,sizeof(buf),"本轮 %um%02us",unsigned(sec/60),unsigned(sec%60));
        else snprintf(buf,sizeof(buf),"本轮 %uh%02um",unsigned(sec/3600),unsigned((sec/60)%60));
    } else snprintf(buf,sizeof(buf),"本轮 --");
    setText(g_duration,buf);
    snprintf(buf,sizeof(buf),"%u 会话  %u 忙碌",m.total_sessions,m.busy_sessions); setText(g_sessions,buf);
    if(s.pending_questions) buf[0]=0;
    else if(valid) snprintf(buf,sizeof(buf),"%u/%u",index+1,m.work_count); else snprintf(buf,sizeof(buf),"--");
    setText(g_position,buf);
    if(!s.pending_questions && m.battery_pct!=255) snprintf(buf,sizeof(buf),"%u%%",m.battery_pct); else buf[0]=0;
    setText(g_battery,buf);
    lv_obj_set_style_bg_color(g_dot,m.ble_connected?C_ACCENT:lv_color_hex(0x555555),0);
    lv_obj_add_flag(g_icon,LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(g_anim,LV_OBJ_FLAG_HIDDEN);
    if(s.state==kAppStateThinking && s.phase!=12) lv_obj_remove_flag(g_anim,LV_OBJ_FLAG_HIDDEN);
    else if(s.state==kAppStateTool) {
        lv_image_set_src(g_icon,&g_icon_dsc[iconFor(s.detail)]);lv_obj_remove_flag(g_icon,LV_OBJ_FLAG_HIDDEN);
    }
}
void alertOpacity(void* obj,int32_t value){lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj),value,0);}
void alertDone(lv_anim_t*){lv_obj_add_flag(g_alert,LV_OBJ_FLAG_HIDDEN);}
}

void lvglUiInit() {
    lv_init();lv_tick_set_cb(tick_cb);
    auto* d=lv_display_create(kScreenWidth,kScreenHeight);lv_display_set_flush_cb(d,flush_cb);
    lv_display_set_buffers(d,g_buf_a,g_buf_b,sizeof(g_buf_a),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_obj_set_style_bg_color(lv_screen_active(),lv_color_black(),0);
    for(int i=0;i<kIconCount;++i)imageDescriptor(g_icon_dsc[i],kIcons[i],40,40);
    for(int i=0;i<kCodexFrameCount;++i){imageDescriptor(g_frame_dsc[i],kCodexFrames[i],40,40);g_frames[i]=&g_frame_dsc[i];}
    buildWork();buildClock();
    g_alert=lv_obj_create(lv_layer_top());lv_obj_set_size(g_alert,240,240);lv_obj_set_pos(g_alert,0,0);
    lv_obj_set_style_bg_color(g_alert,C_RED,0);lv_obj_set_style_border_width(g_alert,0,0);lv_obj_set_style_radius(g_alert,0,0);
    lv_obj_add_flag(g_alert,LV_OBJ_FLAG_HIDDEN);lv_screen_load(g_work);
    g_last_active=millis();
    Serial.println("[LVGL] Codex 工作页与时钟页已启动");
}
void lvglUiTick(){lv_timer_handler();}
void lvglUiFlashAlert(){
    lv_obj_remove_flag(g_alert,LV_OBJ_FLAG_HIDDEN);
    lv_anim_t a;lv_anim_init(&a);lv_anim_set_var(&a,g_alert);lv_anim_set_exec_cb(&a,alertOpacity);
    lv_anim_set_values(&a,0,255);lv_anim_set_duration(&a,kAlertFlashIntervalMs);
    lv_anim_set_playback_duration(&a,kAlertFlashIntervalMs);lv_anim_set_repeat_count(&a,kAlertFlashCycles);
    lv_anim_set_completed_cb(&a,alertDone);lv_anim_start(&a);
}
void lvglUiManualAdvance(){
    int next=0;
    if(!g_on_clock) {
        for(uint8_t i=0;i<g_count;++i)if(g_rows[i].key==g_current_key){next=i+1;break;}
        if(!g_count)next=1;
    }
    g_manual_clock=next>=g_count && !g_on_clock;
    g_manual_key=!g_manual_clock && g_count?g_rows[next<g_count?next:0].key:0;
    g_manual_until=millis()+kManualHoldMs;
}
void lvglUiApply(const LvglUiModel& m){
    uint32_t now=static_cast<uint32_t>(m.now_ms);
    g_count=m.work_count;for(uint8_t i=0;i<g_count;++i)g_rows[i]=m.work[i];
    bool active=m.app_state==kAppStateThinking || m.app_state==kAppStateTool || m.app_state==kAppStateWaiting;
    if(active){g_last_active=now;if(!g_was_active)g_manual_until=0;}
    g_was_active=active;
    uint32_t wait_key=0;
    for(uint8_t i=0;i<g_count;++i)if(g_rows[i].state==kAppStateWaiting || g_rows[i].pending_questions){wait_key=g_rows[i].key;break;}
    if(wait_key && wait_key!=g_wait_key)g_manual_until=0;
    g_wait_key=wait_key;
    bool manual=g_manual_until && static_cast<int32_t>(g_manual_until-now)>0;
    bool clock=manual?g_manual_clock:(m.idle_mode || (!active && now-g_last_active>=kClockAfterMs));
    int index=g_count?0:-1;
    if(manual && !clock)for(uint8_t i=0;i<g_count;++i)if(g_rows[i].key==g_manual_key){index=i;break;}
    if(!g_initialized || clock!=g_on_clock){lv_screen_load(clock?g_idle:g_work);g_initialized=true;}
    g_on_clock=clock;g_current_key=index>=0?g_rows[index].key:0;g_pet_mood=m.mood;
    if(clock){
        char clk[8],date[40];composeIdle(m.quota,m.now_ms,clk,sizeof(clk),date,sizeof(date));
        setText(g_clock,clk);setText(g_date,date);setText(g_status,m.quota.idle_status);
    } else applyWork(m,index);
}
}
