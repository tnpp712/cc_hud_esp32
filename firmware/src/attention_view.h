#pragma once
#include <cstdio>
#include <cstring>
#include "work_snapshot.h"

namespace cc_hud {
enum AttentionKind:uint8_t {kAttentionNone,kAttentionQuestion,kAttentionApproval,kAttentionError,kAttentionUnknown};
inline AttentionKind attentionKind(const WorkSession& s) {
    if(s.state==3 && s.kind==3)return kAttentionError;
    if(s.pending_questions || (s.state==3 && s.kind==2))return kAttentionQuestion;
    if(s.state==3 && s.kind==1)return kAttentionApproval;
    return s.state==3?kAttentionUnknown:kAttentionNone;
}
inline const char* attentionTitle(AttentionKind kind) {
    switch(kind) {
        case kAttentionQuestion:return "待回答";
        case kAttentionApproval:return "待审批";
        case kAttentionError:return "执行失败";
        case kAttentionUnknown:return "需查看";
        default:return "";
    }
}
struct AttentionCounts {
    unsigned questions=0,approvals=0,errors=0,unknown=0;
    bool any()const{return questions || approvals || errors || unknown;}
    void format(char* text,size_t cap)const {
        text[0]=0;
        const unsigned counts[]={errors,approvals,questions,unknown};
        const char* names[]={"错误","待审批","待回答","需查看"};
        for(unsigned i=0;i<4;++i)if(counts[i]) {
            size_t used=std::strlen(text);
            if(used>=cap-1)break;
            std::snprintf(text+used,cap-used,"%s%s %u",used?"  ":"",names[i],counts[i]);
        }
    }
};
inline AttentionCounts attentionCounts(const WorkSession* rows,uint8_t count) {
    AttentionCounts c;
    for(uint8_t i=0;i<count;++i) {
        const auto& s=rows[i];
        if(s.pending_questions)c.questions+=s.pending_questions;
        else if(s.state==3 && s.kind==2)++c.questions;
        if(s.state==3 && s.kind==1)++c.approvals;
        if(s.state==3 && s.kind==3)++c.errors;
        if(attentionKind(s)==kAttentionUnknown)++c.unknown;
    }
    return c;
}
}
