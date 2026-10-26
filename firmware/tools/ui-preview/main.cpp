// 使用实际 LVGL 界面代码离屏渲染，用于刷机前检查布局与字体。
#include <cstdio>
#include <cstring>
#include "Arduino.h"
#include "Adafruit_ST7789.h"
#include "lvgl_ui.h"
#include <lvgl.h>
uint32_t preview_ms=1000;
uint16_t preview_pixels[240*240]={};
namespace cc_hud {Adafruit_ST7789& displayGetTft(){static Adafruit_ST7789 t;return t;}}
void save(const char* name){
    FILE* f=fopen(name,"wb");fprintf(f,"P6\n240 240\n255\n");
    for(auto p:preview_pixels){unsigned char c[]={static_cast<unsigned char>((p>>11)*255/31),static_cast<unsigned char>(((p>>5)&63)*255/63),static_cast<unsigned char>((p&31)*255/31)};fwrite(c,1,3,f);}
    fclose(f);
}
void render(cc_hud::LvglUiModel& m){for(int i=0;i<30;++i){preview_ms+=20;m.now_ms=preview_ms;cc_hud::lvglUiApply(m);cc_hud::lvglUiTick();}}
int main(){
    using namespace cc_hud;lvglUiInit();LvglUiModel m;m.ble_connected=true;m.work_count=3;m.total_sessions=3;m.busy_sessions=2;m.app_state=kAppStateTool;m.work_capture_ms=1000;
    for(int i=0;i<3;++i){auto& s=m.work[i];s.key=i+1;s.state=i==2?0:2;s.phase=2;s.flags=7;s.ctx_pct=42;s.elapsed_s=754;s.plan_done=2;s.plan_total=4;std::strcpy(s.project,"福利数据看板");std::strcpy(s.session_title,i?"评估福利看板指标数据支持":"更新假期看板进展与待办");std::strcpy(s.short_id,i?"B8D3":"A7C2");std::strcpy(s.model,"gpt-6-astra");std::strcpy(s.effort,"high");std::strcpy(s.detail,"Bash");std::strcpy(s.step,"检查状态链路");}
    std::strcpy(m.work[0].progress,"正在检查假期统计口径，核对数据来源。");
    m.work[0].flags=5;
    render(m);save("work.ppm");
    for(uint32_t i=0;i<lv_obj_get_child_count(lv_screen_active());++i){auto* o=lv_obj_get_child(lv_screen_active(),i);if(lv_obj_check_type(o,&lv_label_class))std::printf("%d,%d %dx%d %s\n",lv_obj_get_x(o),lv_obj_get_y(o),lv_obj_get_width(o),lv_obj_get_height(o),lv_label_get_text(o));}
    lv_mem_monitor_t monitor;lv_mem_monitor(&monitor);std::printf("内存剩余 %u\n",unsigned(monitor.free_size));
    m.work[0].pending_questions=2;std::strcpy(m.work[0].attention,"请选择统计口径：按自然年还是按财年？");
    render(m);save("pending-question.ppm");
    m.work[0].pending_questions=0;m.work[0].attention[0]=0;render(m);save("question-cleared.ppm");
    m.work[0].state=3;m.work[0].kind=1;m.work[0].phase=8;
    std::strcpy(m.work[0].attention,"确认是否允许发布本次修改，请回到 Codex 审批。");
    render(m);save("approval.ppm");
    m.work[0].state=2;m.work[0].kind=0;m.work[0].phase=2;m.work[0].attention[0]=0;
    m.work[2].outcome=1;m.work[2].ended_at=1792716000;m.work[2].result_key=777;
    std::strcpy(m.work[2].result,"已整理口径差异，仍需业务验收。未执行生产更新。");
    render(m);save("result.ppm");
    lvglUiManualAdvance();render(m);save("clock.ppm");
    lvglUiManualAdvance();render(m);save("first-session.ppm");
    lvglUiManualAdvance();render(m);save("second-session.ppm");
    m.work[0].pending_questions=2;m.work[0].state=2;
    m.work[1].state=3;m.work[1].kind=3;std::strcpy(m.work[1].attention,"验收命令执行失败，请回到 Codex 查看错误。");
    m.work[2].state=3;m.work[2].kind=1;
    render(m);save("error-mixed.ppm");
    lvglUiManualAdvance();render(m);lvglUiManualAdvance();m.quota.unix_ts=1792716000;m.quota.utc_offset_min=480;std::strcpy(m.quota.idle_status,"杭州 晴 23°C");render(m);save("clock-pending.ppm");
}
