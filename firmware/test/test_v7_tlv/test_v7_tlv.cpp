#include <unity.h>
#include <cstdint>
#include <cstddef>
#include "v7_tlv.h"

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

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_known_tags);
    RUN_TEST(test_unknown_tag_skipped);
    RUN_TEST(test_len_overflow_errors);
    RUN_TEST(test_fragment_rejected);
    RUN_TEST(test_parse_intervention_kind);
    RUN_TEST(test_parse_session_list);
    return UNITY_END();
}
