#pragma once
#include "work_snapshot.h"

namespace cc_hud {
enum WorkPage:uint8_t {kPageDetail,kPageClock};
class WorkNavigation {
public:
    WorkPage page=kPageDetail;
    uint32_t key=0;
    bool manual=false;
    void advance(const WorkSession* rows,uint8_t count,uint32_t now) {
        if(page==kPageClock) {
            // 时钟再短按回到会话，并恢复自动选择。
            manual=false;page=kPageDetail;key=count?rows[0].key:0;
            selected_at_=now;result_until_=0;return;
        }
        manual=true;
        int index=find(rows,count,key);
        if(index+1<count){key=rows[index+1].key;return;}
        page=kPageClock;
    }
    int update(const WorkSession* rows,uint8_t count,uint32_t now,bool idle) {
        int current=find(rows,count,key),waiting=-1,best=-1,finished=-1;
        for(uint8_t i=0;i<count;++i) {
            if(rows[i].pending_questions || rows[i].state==3){if(waiting<0)waiting=i;}
            if(!(rows[i].flags&16) && best<0)best=i;
            if(rows[i].outcome && rows[i].ended_at && !seen(rows[i].result_key)) {
                mark(rows[i].result_key);if(finished<0 || rows[i].ended_at>rows[finished].ended_at)finished=i;
            }
        }
        if(manual) {
            if(page==kPageDetail && current<0){if(count){key=rows[0].key;current=0;}else page=kPageClock;}
            return page==kPageDetail?current:-1;
        }
        if(waiting>=0){select(rows[waiting],now);return waiting;}
        if(finished>=0){select(rows[finished],now);result_until_=now+20000;return finished;}
        if(page==kPageDetail && current>=0 && static_cast<int32_t>(result_until_-now)>0)return current;
        if(page==kPageDetail && current>=0 && now-selected_at_<8000)return current;
        if(idle){page=kPageClock;return -1;}
        if(best>=0){select(rows[best],now);return best;}
        page=kPageClock;return -1;
    }
private:
    uint32_t selected_at_=0,result_until_=0,seen_[32]={};uint8_t next_seen_=0;
    static int find(const WorkSession* rows,uint8_t count,uint32_t key){for(uint8_t i=0;i<count;++i)if(rows[i].key==key)return i;return -1;}
    void select(const WorkSession& row,uint32_t now){if(page!=kPageDetail || key!=row.key)selected_at_=now;page=kPageDetail;key=row.key;}
    bool seen(uint32_t value){if(!value)return true;for(auto v:seen_)if(v==value)return true;return false;}
    void mark(uint32_t value){seen_[next_seen_++%32]=value;}
};
}
