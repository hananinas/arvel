#if LV_BUILD_TEST
#include "../lvgl.h"

#include "unity/unity.h"

static const char color_cmd = LV_TXT_COLOR_CMD[0];

#if LV_TXT_LINE_BREAK_LONG_LEN == 0
static uint32_t wrapping_glyph_calls;

static bool counted_glyph_dsc(const lv_font_t * font, lv_font_glyph_dsc_t * dsc,
                              uint32_t letter, uint32_t next)
{
    LV_UNUSED(font);
    wrapping_glyph_calls++;
    return (LV_FONT_DEFAULT)->get_glyph_dsc(LV_FONT_DEFAULT, dsc, letter, next);
}

void test_txt_long_word_wrapping_has_linear_glyph_work(void)
{
    char word[8193];
    memset(word, 'x', sizeof(word) - 1);
    word[sizeof(word) - 1] = '\0';
    lv_font_t font = *LV_FONT_DEFAULT;
    font.get_glyph_dsc = counted_glyph_dsc;
    lv_coord_t glyph_width = lv_font_get_glyph_width(LV_FONT_DEFAULT, 'x', 'x');
    wrapping_glyph_calls = 0;
    uint32_t offset = 0;
    while(offset < sizeof(word) - 1) {
        lv_coord_t width = 0;
        uint32_t bytes = _lv_txt_get_next_line(word + offset, &font, 0, 8 * glyph_width + 1,
                                             &width, LV_TEXT_FLAG_NONE);
        TEST_ASSERT_EQUAL_UINT32(8, bytes);
        TEST_ASSERT_EQUAL_INT(8 * glyph_width, width);
        offset += bytes;
    }
    TEST_ASSERT_EQUAL_UINT32(sizeof(word) - 1, offset);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(2 * (sizeof(word) - 1), wrapping_glyph_calls);
}

void test_txt_forced_wrap_preserves_utf8_and_word_boundaries(void)
{
    lv_coord_t width = 0;
    lv_coord_t glyph_width = lv_font_get_glyph_width(LV_FONT_DEFAULT, 0xe9, 0xe9);
    uint32_t bytes = _lv_txt_get_next_line("éééé", LV_FONT_DEFAULT, 0, 2 * glyph_width + 1,
                                         &width, LV_TEXT_FLAG_NONE);
    TEST_ASSERT_EQUAL_UINT32(4, bytes);
    TEST_ASSERT_EQUAL_INT(2 * glyph_width, width);

    glyph_width = lv_font_get_glyph_width(LV_FONT_DEFAULT, 'x', 'x');
    bytes = _lv_txt_get_next_line("xx xxxxx", LV_FONT_DEFAULT, 0, 3 * glyph_width,
                                 &width, LV_TEXT_FLAG_NONE);
    TEST_ASSERT_EQUAL_UINT32(3, bytes);
}
#endif

void test_txt_should_identify_valid_start_of_command(void)
{
    uint32_t character = color_cmd;
    lv_text_cmd_state_t state = LV_TEXT_CMD_STATE_WAIT;

    bool is_cmd = _lv_txt_is_cmd(&state, character);

    TEST_ASSERT_TRUE(is_cmd);
    TEST_ASSERT_EQUAL_UINT8(state, LV_TEXT_CMD_STATE_PAR);
}

void test_txt_should_identify_invalid_start_of_command(void)
{
    uint32_t character = '$';
    lv_text_cmd_state_t state = LV_TEXT_CMD_STATE_WAIT;

    bool is_cmd = _lv_txt_is_cmd(&state, character);

    TEST_ASSERT_FALSE(is_cmd);
    TEST_ASSERT_EQUAL_UINT8(state, LV_TEXT_CMD_STATE_WAIT);
}

void test_txt_should_identify_scaped_command_in_parameter(void)
{
    uint32_t character = color_cmd;
    lv_text_cmd_state_t state = LV_TEXT_CMD_STATE_PAR;

    bool is_cmd = _lv_txt_is_cmd(&state, character);

    TEST_ASSERT_FALSE(is_cmd);
    TEST_ASSERT_EQUAL_UINT8(state, LV_TEXT_CMD_STATE_WAIT);
}

void test_txt_should_skip_color_parameter_in_parameter(void)
{
    uint32_t character = '$';
    lv_text_cmd_state_t state = LV_TEXT_CMD_STATE_PAR;

    bool is_cmd = _lv_txt_is_cmd(&state, character);

    TEST_ASSERT_TRUE(is_cmd);
    TEST_ASSERT_EQUAL_UINT8(state, LV_TEXT_CMD_STATE_PAR);
}

void test_txt_should_reset_state_when_receiving_color_cmd_while_processing_commands(void)
{
    uint32_t character = color_cmd;
    lv_text_cmd_state_t state = LV_TEXT_CMD_STATE_IN;

    bool is_cmd = _lv_txt_is_cmd(&state, character);

    TEST_ASSERT_TRUE(is_cmd);
    TEST_ASSERT_EQUAL_UINT8(state, LV_TEXT_CMD_STATE_WAIT);
}

void test_txt_should_identify_space_after_parameter(void)
{
    uint32_t character = ' ';
    lv_text_cmd_state_t state = LV_TEXT_CMD_STATE_PAR;

    bool is_cmd = _lv_txt_is_cmd(&state, character);

    TEST_ASSERT_TRUE(is_cmd);
    TEST_ASSERT_EQUAL_UINT8(state, LV_TEXT_CMD_STATE_IN);
}

void test_txt_should_insert_string_into_another(void)
{
    const char * msg = "Hello ";
    const char * suffix = "World";
    char target[20] = {0};
    size_t msg_len = strlen(msg);

    strcpy(target, msg);

    _lv_txt_ins(target, msg_len, suffix);

    TEST_ASSERT_EQUAL_STRING("Hello World", target);
}

void test_txt_should_handle_null_pointers_when_inserting(void)
{
    const char * msg = "Hello ";
    char target[20] = {0};
    size_t msg_len = strlen(msg);

    strcpy(target, msg);

    _lv_txt_ins(target, msg_len, NULL);

    TEST_ASSERT_EQUAL_STRING("Hello ", target);
}

void test_txt_cut_should_handle_null_pointer_to_txt(void)
{
    _lv_txt_cut(NULL, 0, 6);
}

void test_txt_cut_happy_path(void)
{
    char msg[] = "Hello World";

    _lv_txt_cut(msg, 0, 6);

    TEST_ASSERT_EQUAL_STRING("World", msg);
}

void test_txt_cut_should_handle_len_longer_than_string_length(void)
{
    char msg[] = "Hello World";

    _lv_txt_cut(msg, 0, 30);

    TEST_ASSERT_EQUAL_UINT8(msg[0], 0x00);
}

void test_txt_get_encoded_next_should_decode_valid_ascii(void)
{
    char msg[] = "Hello World!";
    uint32_t result = 0;

    result = _lv_txt_encoded_next(msg, NULL);

    TEST_ASSERT_EQUAL_UINT32((uint32_t) 'H', result);
}

void test_txt_get_encoded_next_detect_valid_2_byte_input(void)
{
    char msg[] = "\xc3\xb1";
    uint32_t result = 0;

    result = _lv_txt_encoded_next(msg, NULL);

    TEST_ASSERT_EQUAL_UINT32(241, result);
}

void test_txt_get_encoded_next_detect_invalid_2_byte_input(void)
{
    char msg[] = "\xc3\x28";
    uint32_t result = 0;

    result = _lv_txt_encoded_next(msg, NULL);

    TEST_ASSERT_EQUAL_UINT32(0, result);
}

void test_txt_get_encoded_next_detect_valid_3_byte_input(void)
{
    char msg[] = "\xe2\x82\xa1";
    uint32_t result = 0;

    result = _lv_txt_encoded_next(msg, NULL);

    TEST_ASSERT_EQUAL_UINT32(8353, result);
}

void test_txt_get_encoded_next_detect_invalid_3_byte_input(void)
{
    char msg[] = "\xe2\x28\xa1";
    uint32_t result = 0;

    result = _lv_txt_encoded_next(msg, NULL);

    TEST_ASSERT_EQUAL_UINT32(0, result);
}

void test_txt_get_encoded_next_detect_valid_4_byte_input(void)
{
    char msg[] = "\xf0\x90\x8c\xbc";
    uint32_t result = 0;

    result = _lv_txt_encoded_next(msg, NULL);

    TEST_ASSERT_EQUAL_UINT32(66364, result);
}

void test_txt_get_encoded_next_detect_invalid_4_byte_input(void)
{
    char msg[] = "\xf0\x28\x8c\x28";
    uint32_t result = 0;

    result = _lv_txt_encoded_next(msg, NULL);

    TEST_ASSERT_EQUAL_UINT32(0, result);
}

/* See #2615 for more information */
void test_txt_next_line_should_handle_empty_string(void)
{
    const lv_font_t * font_ptr = NULL;
    lv_coord_t letter_space = 0;
    lv_coord_t max_width = 0;
    lv_text_flag_t flag = LV_TEXT_FLAG_NONE;

    uint32_t next_line = _lv_txt_get_next_line("", font_ptr, letter_space, max_width, NULL, flag);

    TEST_ASSERT_EQUAL_UINT32(0, next_line);
}

#endif
