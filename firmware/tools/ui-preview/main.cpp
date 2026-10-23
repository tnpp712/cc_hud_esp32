// 使用实际 LVGL 界面代码离屏渲染，用于刷机前检查布局与字体。
#include <cstdio>
#include <cstring>
#include "Arduino.h"
#include "Adafruit_ST7789.h"
#include "lvgl_ui.h"
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
    for(int i=0;i<3;++i){auto& s=m.work[i];s.key=i+1;s.state=i==2?0:2;s.phase=2;s.flags=7;s.ctx_pct=42;s.elapsed_s=754;s.plan_done=2;s.plan_total=4;std::strcpy(s.project,i?"另一个项目":"cc_hud_esp32");std::strcpy(s.model,"gpt-6-astra");std::strcpy(s.effort,"high");std::strcpy(s.detail,"Bash");std::strcpy(s.step,"检查状态链路");}
    render(m);save("work.ppm");
    m.work[0].pending_questions=1;render(m);save("pending-question.ppm");
    m.work[0].pending_questions=0;
    m.app_state=kAppStateWaiting;m.work[0].state=3;m.work[0].kind=1;m.work[0].phase=8;render(m);save("waiting.ppm");
    lvglUiManualAdvance();render(m);save("second-session.ppm");
    lvglUiManualAdvance();render(m);lvglUiManualAdvance();m.quota.unix_ts=1792716000;m.quota.utc_offset_min=480;std::strcpy(m.quota.idle_status,"杭州 晴 23°C");render(m);save("clock.ppm");
}
