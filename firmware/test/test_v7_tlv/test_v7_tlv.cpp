#include <unity.h>
#include <cstdint>
#include <cstddef>
#include <utility>
#include "v7_tlv.h"
#include "quota_gauge.h"
#include "work_navigation.h"
#include "attention_view.h"
#include "led_policy.h"
#include "work_frame_fixture.h"

using namespace cc_hud;

void test_parse_known_tags() {
    // 头 + TAG_FIVE_H_USED_PCT(0x01)=40 + TAG_TITLE(0x0B)="Hi"
    const uint8_t buf[] = {0x0B,0,0,1, 0x01,1,40, 0x0B,2,'H','i'};
    V7Fields f{};
    TEST_ASSERT_EQUAL(V7_OK, parseV7Tlv(buf, sizeof(buf), f));
    TEST_ASSERT_TRUE(f.has_five_h_used);
    TEST_ASSERT_EQUAL_UINT8(40, f.five_h_used);
    TEST_ASSERT_EQUAL_STRING("Hi", f.title);
}

void test_unknown_tag_skipped() {
    // 未知 tag 0xC0 len=2,后跟已知 AGG_STATE(0x20)=2
    const uint8_t buf[] = {0x0B,0,0,1, 0xC0,2,0xAA,0xBB, 0x20,1,2};
    V7Fields f{};
    TEST_ASSERT_EQUAL(V7_OK, parseV7Tlv(buf, sizeof(buf), f));
    TEST_ASSERT_TRUE(f.has_agg_state);
    TEST_ASSERT_EQUAL_UINT8(2, f.agg_state);
}

void test_len_overflow_errors() {
    const uint8_t buf[] = {0x0B,0,0,1, 0x01,9,40};   // len=9 越界
    V7Fields f{};
    TEST_ASSERT_EQUAL(V7_ERR_LEN, parseV7Tlv(buf, sizeof(buf), f));
}

void test_fragment_rejected() {
    const uint8_t buf[] = {0x0B,0,0,2, 0x01,1,40};   // total=2
    V7Fields f{};
    TEST_ASSERT_EQUAL(V7_ERR_FRAGMENT, parseV7Tlv(buf, sizeof(buf), f));
}

void test_parse_intervention_kind() {
    // 头 + AGG_STATE(0x20)=3(waiting) + INTERVENTION_KIND(0x40)=2(question)
    const uint8_t buf[] = {0x0B,0,0,1, 0x20,1,3, 0x40,1,2};
    V7Fields f{};
    TEST_ASSERT_EQUAL(V7_OK, parseV7Tlv(buf, sizeof(buf), f));
    TEST_ASSERT_TRUE(f.has_agg_state);
    TEST_ASSERT_EQUAL_UINT8(3, f.agg_state);
    TEST_ASSERT_TRUE(f.has_intervention);
    TEST_ASSERT_EQUAL_UINT8(2, f.intervention_kind);
}

void test_parse_session_list() {
    // 头 + SESSION_LIST_COUNT=2 + 2 条 SESSION_ENTRY
    const uint8_t buf[] = {0x0B,0,0,1,
        0x60,1, 2,
        0x61,5, 0,0,1,0,0,                       // idx0 client0 state1 kind0 title""
        0x61,10, 1,1,3,2,5,'c','o','d','e','x'};  // idx1 client1 state3 kind2 "codex"
    V7Fields f{};
    TEST_ASSERT_EQUAL(V7_OK, parseV7Tlv(buf, sizeof(buf), f));
    TEST_ASSERT_EQUAL_UINT8(2, f.session_count);
    TEST_ASSERT_EQUAL_UINT8(1, f.sessions[1].client_id);
    TEST_ASSERT_EQUAL_UINT8(3, f.sessions[1].state);
    TEST_ASSERT_EQUAL_UINT8(2, f.sessions[1].kind);
    TEST_ASSERT_EQUAL_STRING("codex", f.sessions[1].title);
}

void test_metrics_valid_and_legacy_default() {
    const uint8_t buf[] = {0x0B,0,0,1, 0x0C,1,22, 0x03,1,7};
    V7Fields f{};
    TEST_ASSERT_EQUAL(V7_OK, parseV7Tlv(buf, sizeof(buf), f));
    TEST_ASSERT_TRUE(f.has_metrics_valid);
    TEST_ASSERT_EQUAL_UINT8(22, f.metrics_valid);
    TEST_ASSERT_EQUAL_UINT8(7, f.seven_d_used);
    V7Fields legacy{};
    TEST_ASSERT_EQUAL_UINT8(63, legacy.metrics_valid);
}

void test_work_frames_are_atomic_and_metadata_matches() {
    V7Fields a{},b{};
    TEST_ASSERT_EQUAL(V7_OK,parseV7Tlv(kWorkFrame0,sizeof(kWorkFrame0),a));
    TEST_ASSERT_EQUAL(V7_OK,parseV7Tlv(kWorkFrame1,sizeof(kWorkFrame1),b));
    TEST_ASSERT_EQUAL_STRING("项目",a.work.project);
    TEST_ASSERT_EQUAL_STRING("model",a.work.model);
    TEST_ASSERT_EQUAL_STRING("检查",a.work.step);
    TEST_ASSERT_EQUAL_UINT32(65,a.work.elapsed_s);
    WorkAssembler assembler; WorkSession rows[kMaxWorkSessions]; uint8_t count=0;
    TEST_ASSERT_FALSE(assembler.accept(42,2,&b.work,rows,count));
    TEST_ASSERT_EQUAL_UINT8(0,count);
    TEST_ASSERT_TRUE(assembler.accept(42,2,&a.work,rows,count));
    TEST_ASSERT_EQUAL_UINT8(2,count);
    TEST_ASSERT_EQUAL_UINT32(17,rows[0].key);
    TEST_ASSERT_EQUAL_UINT8(3,rows[1].state);
    TEST_ASSERT_FALSE(assembler.accept(42,2,&a.work,rows,count));
    TEST_ASSERT_TRUE(assembler.accept(43,0,nullptr,rows,count));
    TEST_ASSERT_EQUAL_UINT8(0,count);
}
void test_work_rejects_truncated_fields_and_mixed_generation() {
    V7Fields f{};
    TEST_ASSERT_EQUAL(V7_ERR_LEN,parseV7Tlv(kWorkFrame0,sizeof(kWorkFrame0)-1,f));
    WorkAssembler assembler; WorkSession rows[kMaxWorkSessions],a,b; uint8_t count=0;
    a.index=0;a.key=100;b.index=1;b.key=200;
    TEST_ASSERT_FALSE(assembler.accept(1,2,&a,rows,count));
    TEST_ASSERT_FALSE(assembler.accept(2,2,&b,rows,count));
    TEST_ASSERT_EQUAL_UINT8(0,count);
    a.key=300;
    TEST_ASSERT_TRUE(assembler.accept(2,2,&a,rows,count));
    TEST_ASSERT_EQUAL_UINT32(300,rows[0].key);
}
void test_gauge_means_remaining() {
    TEST_ASSERT_EQUAL_UINT8(24,remainingPixels(0,24));
    TEST_ASSERT_EQUAL_UINT8(18,remainingPixels(25,24));
    TEST_ASSERT_EQUAL_UINT8(0,remainingPixels(100,24));
    TEST_ASSERT_EQUAL_UINT8(0,remainingPixels(255,24));
}
void test_work_v2_keeps_activity_and_pending_separate() {
    V7Fields f{};
    TEST_ASSERT_EQUAL(V7_OK,parseV7Tlv(kWorkFrameV2,sizeof(kWorkFrameV2),f));
    TEST_ASSERT_EQUAL_UINT8(2,f.work.state);
    TEST_ASSERT_EQUAL_UINT8(2,f.work.phase);
    TEST_ASSERT_EQUAL_UINT8(37,f.work.ctx_pct);
    TEST_ASSERT_EQUAL_UINT8(2,f.work.pending_questions);
    uint8_t data[200];size_t size=workReadback(&f.work,1,data);
    TEST_ASSERT_EQUAL_UINT8(37,data[size-4]);
    TEST_ASSERT_EQUAL_UINT8(2,data[size-3]);
}
void test_work_v2_rejects_invalid_context() {
    uint8_t data[sizeof(kWorkFrameV2)];
    std::memcpy(data,kWorkFrameV2,sizeof(data));data[sizeof(data)-2]=101;
    V7Fields f{};
    TEST_ASSERT_EQUAL(V7_ERR_LEN,parseV7Tlv(data,sizeof(data),f));
}

void test_work_v3_identity_and_truncation() {
    V7Fields f{};
    TEST_ASSERT_EQUAL(V7_OK,parseV7Tlv(kWorkFrameV3,sizeof(kWorkFrameV3),f));
    TEST_ASSERT_EQUAL_STRING("修复待回答提醒",f.work.session_title);
    TEST_ASSERT_EQUAL_STRING("A7C2",f.work.short_id);
    TEST_ASSERT_EQUAL_UINT8(37,f.work.ctx_pct);
    uint8_t data[384];size_t size=workReadback(&f.work,1,data);
    TEST_ASSERT_TRUE(size<sizeof(data));
    TEST_ASSERT_EQUAL_MEMORY("A7C2",data+size-4,4);
    TEST_ASSERT_EQUAL(V7_ERR_LEN,parseV7Tlv(kWorkFrameV3,sizeof(kWorkFrameV3)-1,f));
}

void test_experience_requires_all_note_parts_and_roundtrips() {
    V7Fields base{},p{},a{},r{};
    TEST_ASSERT_EQUAL(V7_OK,parseV7Tlv(kExperienceFrame0,sizeof(kExperienceFrame0),base));
    TEST_ASSERT_EQUAL(V7_OK,parseV7Tlv(kExperienceFrame1,sizeof(kExperienceFrame1),p));
    TEST_ASSERT_EQUAL(V7_OK,parseV7Tlv(kExperienceFrame2,sizeof(kExperienceFrame2),a));
    TEST_ASSERT_EQUAL(V7_OK,parseV7Tlv(kExperienceFrame3,sizeof(kExperienceFrame3),r));
    WorkAssembler assembler;WorkSession rows[kMaxWorkSessions];uint8_t count=0;
    TEST_ASSERT_FALSE(assembler.accept(100,1,nullptr,rows,count,&a.note));
    TEST_ASSERT_FALSE(assembler.accept(100,1,&base.work,rows,count));
    TEST_ASSERT_FALSE(assembler.accept(100,1,nullptr,rows,count,&r.note));
    TEST_ASSERT_TRUE(assembler.accept(100,1,nullptr,rows,count,&p.note));
    TEST_ASSERT_EQUAL_STRING("确认发布范围",rows[0].attention);
    TEST_ASSERT_EQUAL_STRING("核对指标口径",rows[0].progress);
    TEST_ASSERT_EQUAL_UINT32(456,rows[0].result_key);
    uint8_t data[464];auto size=noteReadback(rows,count,data);
    TEST_ASSERT_TRUE(size<sizeof(data));TEST_ASSERT_EQUAL_UINT8(1,data[5]);
    TEST_ASSERT_FALSE(assembler.accept(100,1,nullptr,rows,count,&p.note));
}
void test_navigation_holds_manual_and_auto_selection() {
    WorkSession rows[3];rows[0].key=10;rows[0].state=2;rows[1].key=20;rows[1].state=1;rows[2].key=30;rows[2].flags=16;
    WorkNavigation nav;
    TEST_ASSERT_EQUAL(0,nav.update(rows,3,1000,false));
    std::swap(rows[0],rows[1]);
    TEST_ASSERT_EQUAL(1,nav.update(rows,3,2000,false));
    TEST_ASSERT_EQUAL(0,nav.update(rows,3,10000,false));
    nav.advance(rows,3,11000);TEST_ASSERT_EQUAL(kPageDetail,nav.page);
    TEST_ASSERT_EQUAL_UINT32(rows[1].key,nav.key);TEST_ASSERT_TRUE(nav.manual);
    rows[0].pending_questions=1;
    TEST_ASSERT_EQUAL(1,nav.update(rows,3,90000,false));
    nav.advance(rows,3,91000);TEST_ASSERT_EQUAL_UINT32(rows[2].key,nav.key);
    nav.advance(rows,3,92000);TEST_ASSERT_EQUAL(kPageClock,nav.page);
    nav.advance(rows,3,93000);TEST_ASSERT_FALSE(nav.manual);TEST_ASSERT_EQUAL(kPageDetail,nav.page);
    TEST_ASSERT_EQUAL(0,nav.update(rows,3,93001,false));
}
void test_direct_navigation_empty_removed_and_idle_result() {
    WorkSession rows[1];rows[0].key=10;rows[0].flags=16;
    WorkNavigation nav;nav.advance(rows,0,1000);TEST_ASSERT_EQUAL(kPageClock,nav.page);
    nav.advance(rows,1,2000);TEST_ASSERT_EQUAL(0,nav.update(rows,1,2001,true));
    TEST_ASSERT_EQUAL(kPageDetail,nav.page);
    nav.manual=true;nav.key=99;
    TEST_ASSERT_EQUAL(0,nav.update(rows,1,4000,false));
    TEST_ASSERT_EQUAL_UINT32(10,nav.key);
    nav.update(rows,0,5000,true);TEST_ASSERT_EQUAL(kPageClock,nav.page);
}
void test_attention_labels_separate_questions_approvals_and_errors() {
    WorkSession rows[3];rows[0].state=2;rows[0].phase=2;rows[0].pending_questions=2;
    rows[1].state=3;rows[1].kind=1;rows[2].state=3;rows[2].kind=3;
    TEST_ASSERT_EQUAL_STRING("待回答",attentionTitle(attentionKind(rows[0])));
    TEST_ASSERT_EQUAL_UINT8(2,rows[0].state);
    TEST_ASSERT_EQUAL_STRING("待审批",attentionTitle(attentionKind(rows[1])));
    TEST_ASSERT_EQUAL_STRING("执行失败",attentionTitle(attentionKind(rows[2])));
    auto counts=attentionCounts(rows,3);char text[128];counts.format(text,sizeof(text));
    TEST_ASSERT_EQUAL_STRING("错误 1  待审批 1  待回答 2",text);
    rows[0].pending_questions=0;
    TEST_ASSERT_EQUAL(kAttentionNone,attentionKind(rows[0]));
    TEST_ASSERT_EQUAL_UINT32(0,attentionCounts(rows,3).questions);
    rows[0].state=3;TEST_ASSERT_EQUAL_STRING("需查看",attentionTitle(attentionKind(rows[0])));
}
void test_result_card_is_shown_once_and_waiting_has_priority() {
    WorkSession rows[2];rows[0].key=1;rows[0].state=2;rows[1].key=2;rows[1].outcome=1;rows[1].ended_at=123;rows[1].result_key=456;
    WorkNavigation nav;
    TEST_ASSERT_EQUAL(1,nav.update(rows,2,1000,false));
    TEST_ASSERT_EQUAL(1,nav.update(rows,2,10000,false));
    TEST_ASSERT_EQUAL(0,nav.update(rows,2,22000,false));
    TEST_ASSERT_EQUAL(0,nav.update(rows,2,23000,false));
    rows[1].pending_questions=1;TEST_ASSERT_EQUAL(1,nav.update(rows,2,24000,false));
}
void test_led_waiting_error_and_normal_states_are_distinct() {
    TEST_ASSERT_EQUAL(kLedModeWaiting,ledRingModeForAppState(3,2));
    TEST_ASSERT_EQUAL(kLedModeWaiting,ledRingModeForAppState(3,1));
    TEST_ASSERT_EQUAL(kLedModeError,ledRingModeForAppState(3,3));
    TEST_ASSERT_EQUAL(kLedModeWorking,ledRingModeForAppState(2,0));
    TEST_ASSERT_EQUAL(kLedModeIdleDone,ledRingModeForAppState(0,0));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_experience_requires_all_note_parts_and_roundtrips);
    RUN_TEST(test_navigation_holds_manual_and_auto_selection);
    RUN_TEST(test_direct_navigation_empty_removed_and_idle_result);
    RUN_TEST(test_attention_labels_separate_questions_approvals_and_errors);
    RUN_TEST(test_result_card_is_shown_once_and_waiting_has_priority);
    RUN_TEST(test_led_waiting_error_and_normal_states_are_distinct);
    RUN_TEST(test_parse_known_tags);
    RUN_TEST(test_unknown_tag_skipped);
    RUN_TEST(test_len_overflow_errors);
    RUN_TEST(test_fragment_rejected);
    RUN_TEST(test_parse_intervention_kind);
    RUN_TEST(test_parse_session_list);
    RUN_TEST(test_metrics_valid_and_legacy_default);
    RUN_TEST(test_work_frames_are_atomic_and_metadata_matches);
    RUN_TEST(test_work_rejects_truncated_fields_and_mixed_generation);
    RUN_TEST(test_gauge_means_remaining);
    RUN_TEST(test_work_v2_keeps_activity_and_pending_separate);
    RUN_TEST(test_work_v2_rejects_invalid_context);
    RUN_TEST(test_work_v3_identity_and_truncation);
    return UNITY_END();
}
