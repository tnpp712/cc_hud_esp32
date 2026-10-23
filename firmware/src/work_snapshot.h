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
    return pos;
}

// 只有同一代次的全部会话到齐才发布，避免新旧会话信息混合。
class WorkAssembler {
public:
    bool accept(uint32_t generation, uint8_t count, const WorkSession* entry,
                WorkSession* output, uint8_t& output_count) {
        if (count > kMaxWorkSessions || (entry && entry->index >= count)) return false;
        if (published_ && generation == published_generation_) return false;
        if (!staging_ || generation != generation_) {
            generation_ = generation; count_ = count; mask_ = 0; staging_ = true;
        }
        if (count != count_) return false;
        if (entry) { rows_[entry->index] = *entry; mask_ |= uint16_t(1u << entry->index); }
        if (mask_ != (1u << count) - 1u) return false;
        for (uint8_t i = 0; i < count; ++i) output[i] = rows_[i];
        output_count = count;
        published_ = true; published_generation_ = generation;
        return true;
    }
private:
    WorkSession rows_[kMaxWorkSessions];
    uint32_t generation_ = 0, published_generation_ = 0;
    uint8_t count_ = 0;
    uint16_t mask_ = 0;
    bool staging_ = false, published_ = false;
};
}
