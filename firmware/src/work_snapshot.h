#pragma once
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace cc_hud {
constexpr uint8_t kMaxWorkSessions = 8;
struct WorkSession {
    uint8_t index = 0;
    uint32_t key = 0;
    uint8_t client_id = 1, state = 0, kind = 0, phase = 0, flags = 0;
    uint8_t ctx_pct = 255, pending_questions = 0;
    uint32_t elapsed_s = 0;
    uint8_t plan_done = 0, plan_total = 0;
    uint8_t outcome = 0; uint32_t ended_at = 0, result_key = 0;
    char progress[145] = {}, attention[145] = {}, result[145] = {};
    char session_title[145] = {}, short_id[13] = {};
    char project[49] = {}, model[33] = {}, effort[13] = {}, detail[25] = {}, step[49] = {};
};

inline size_t workReadback(const WorkSession* s, uint8_t count, uint8_t* out) {
    out[0]=count;
    if (!count || !s) return 1;
    out[1]=s->state;out[2]=s->phase;out[3]=s->plan_done;out[4]=s->plan_total;out[5]=s->flags;
    for (int i=0;i<4;++i) {out[6+i]=s->key>>(8*i);out[10+i]=s->elapsed_s>>(8*i);}
    size_t pos=14;
    for (const char* text:{s->project,s->model,s->effort,s->detail,s->step}) {
        size_t len=std::strlen(text);out[pos++]=len;std::memcpy(out+pos,text,len);pos+=len;
    }
    out[pos++]=s->ctx_pct;out[pos++]=s->pending_questions;
    for (const char* text:{s->session_title,s->short_id}) {
        size_t len=std::strlen(text);out[pos++]=len;std::memcpy(out+pos,text,len);pos+=len;
    }
    return pos;
}

struct WorkNote {
    uint8_t index=0, part=0, outcome=0;
    uint32_t key=0, ended_at=0, result_key=0;
    char text[145]={};
};
inline size_t noteReadback(const WorkSession* s,uint8_t count,uint8_t* out) {
    out[0]=count;if(!s || !count)return 1;
    for(int i=0;i<4;++i){out[1+i]=s->key>>(8*i);out[6+i]=s->ended_at>>(8*i);out[10+i]=s->result_key>>(8*i);}
    out[5]=s->outcome;size_t pos=14;
    for(const char* text:{s->progress,s->attention,s->result}) {
        size_t n=std::strlen(text);out[pos++]=n;std::memcpy(out+pos,text,n);pos+=n;
    }
    return pos;
}

// 扩展条目的基础信息及三个文字分片全部到齐后，才发布同一代次。
class WorkAssembler {
public:
    bool accept(uint32_t generation,uint8_t count,const WorkSession* entry,
                WorkSession* output,uint8_t& output_count,const WorkNote* note=nullptr) {
        if(count>kMaxWorkSessions || (entry && entry->index>=count) || (note && (note->index>=count || note->part<1 || note->part>3)))return false;
        if(published_ && generation==published_generation_)return false;
        if(!staging_ || generation!=generation_) {
            generation_=generation;count_=count;staging_=true;
            for(uint8_t i=0;i<kMaxWorkSessions;++i){mask_[i]=0;rows_[i]=WorkSession{};}
        }
        if(count!=count_)return false;
        if(entry) {
            const uint8_t i=entry->index;
            // 补充分片允许先到；保留其内容，但拒绝不同会话混入。
            if(mask_[i] && rows_[i].key!=entry->key)return false;
            auto& r=rows_[i];
            char progress[145],attention[145],result[145];
            std::memcpy(progress,r.progress,145);std::memcpy(attention,r.attention,145);std::memcpy(result,r.result,145);
            auto outcome=r.outcome;auto ended=r.ended_at;auto result_key=r.result_key;
            r=*entry;
            if(mask_[i]&14){std::memcpy(r.progress,progress,145);std::memcpy(r.attention,attention,145);std::memcpy(r.result,result,145);r.outcome=outcome;r.ended_at=ended;r.result_key=result_key;}
            mask_[i]|=1;
        }
        if(note) {
            const uint8_t i=note->index;auto& r=rows_[i];
            if(mask_[i] && r.key!=note->key)return false;
            r.key=note->key;r.index=i;r.outcome=note->outcome;r.ended_at=note->ended_at;r.result_key=note->result_key;
            char* dest=note->part==1?r.progress:note->part==2?r.attention:r.result;
            std::memcpy(dest,note->text,145);mask_[i]|=uint8_t(1u<<note->part);
        }
        for(uint8_t i=0;i<count;++i)if(!(mask_[i]&1) || ((rows_[i].flags&8) && mask_[i]!=15))return false;
        for(uint8_t i=0;i<count;++i)output[i]=rows_[i];
        output_count=count;published_=true;published_generation_=generation;return true;
    }
private:
    WorkSession rows_[kMaxWorkSessions];
    uint8_t mask_[kMaxWorkSessions]={},count_=0;
    uint32_t generation_=0,published_generation_=0;
    bool staging_=false,published_=false;
};
}
