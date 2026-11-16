/* Host test for savedata_param_sfo.h (Decision 129). Not part of any console build.
 *   g++ -std=c++17 -Wall -Wextra -O1 native/adapters/savedata_param_sfo_host_test.cpp -o /tmp/sfo_test && /tmp/sfo_test */
#include "savedata_param_sfo.h"

#include <cstdio>
#include <cstring>

static int g_failures = 0;
#define EXPECT(condition) do { if (!(condition)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); ++g_failures; } } while (0)

using namespace savedata_sfo;

static Params make_params()
{
    static Params params;
    std::memset(&params, 0, sizeof params);
    std::strcpy(params.title, "inFAMOUS");
    std::strcpy(params.sub_title, "Chapter 3");
    std::strcpy(params.detail, "Empire City, Neon district");
    params.attribute = 0;
    std::strcpy(params.list_param, "SLOT01");
    std::strcpy(params.directory, "BCUS98119-SAVE00");
    const uint8_t id[secure_id_bytes] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    EXPECT(secure_set(params, "GAME.SAV", id));
    return params;
}

static void test_round_trip()
{
    const Params &written = make_params();
    static uint8_t bytes[0x4000];
    const size_t size = encode(written, bytes, sizeof bytes);
    EXPECT(size > sfo_header_bytes);
    EXPECT(bytes[0] == 0 && bytes[1] == 'P' && bytes[2] == 'S' && bytes[3] == 'F');
    EXPECT(get_le32(bytes + 4) == sfo_version);
    EXPECT(get_le32(bytes + 16) == sfo_fixed_entries + 1);
    EXPECT(get_le32(bytes + 12) % 4 == 0);
    static Params read;
    EXPECT(decode(bytes, size, read));
    EXPECT(!std::strcmp(read.title, "inFAMOUS"));
    EXPECT(!std::strcmp(read.sub_title, "Chapter 3"));
    EXPECT(!std::strcmp(read.detail, "Empire City, Neon district"));
    EXPECT(!std::strcmp(read.list_param, "SLOT01"));
    EXPECT(!std::strcmp(read.directory, "BCUS98119-SAVE00"));
    EXPECT(read.secure_count == 1);
    EXPECT(!std::strcmp(read.secure[0].name, "GAME.SAV"));
    EXPECT(read.secure[0].id[0] == 1 && read.secure[0].id[15] == 16);
    EXPECT(file_type_of(read, "GAME.SAV") == file_type_secure);
    EXPECT(file_type_of(read, "OPTIONS.DAT") == file_type_normal);
    EXPECT(file_type_of(read, "ICON0.PNG") == file_type_icon0);
}

static void test_keys_sorted()
{
    const Params &written = make_params();
    static uint8_t bytes[0x4000];
    const size_t size = encode(written, bytes, sizeof bytes);
    const uint32_t key_table = get_le32(bytes + 8), count = get_le32(bytes + 16);
    const char *previous = nullptr;
    for (uint32_t index = 0; index < count; ++index) {
        const char *key = reinterpret_cast<const char *>(bytes + key_table + get_le16(bytes + sfo_header_bytes + index * sfo_index_bytes));
        if (previous) EXPECT(std::strcmp(previous, key) < 0);
        previous = key;
    }
    EXPECT(size > 0);
}

static void test_full_strings_are_terminated()
{
    static Params params;
    std::memset(&params, 0, sizeof params);
    std::memset(params.title, 'T', title_bytes - 1);
    std::memset(params.list_param, 'L', list_param_bytes - 1);
    static uint8_t bytes[0x4000];
    const size_t size = encode(params, bytes, sizeof bytes);
    static Params read;
    EXPECT(decode(bytes, size, read));
    EXPECT(std::strlen(read.title) == title_bytes - 1);
    EXPECT(std::strlen(read.list_param) == list_param_bytes - 1);
}

static void test_short_capacity_and_bad_input()
{
    const Params &written = make_params();
    uint8_t small[64];
    EXPECT(encode(written, small, sizeof small) == 0);
    static Params read;
    EXPECT(!decode(small, 4, read));
    const uint8_t not_sfo[sfo_header_bytes] = {'X', 'P', 'S', 'F'};
    EXPECT(!decode(not_sfo, sizeof not_sfo, read));
    uint8_t lying[sfo_header_bytes] = {0, 'P', 'S', 'F'};
    put_le32(lying + 16, 1000);
    EXPECT(!decode(lying, sizeof lying, read));
}

static void test_secure_list()
{
    static Params params;
    std::memset(&params, 0, sizeof params);
    const uint8_t id[secure_id_bytes] = {};
    for (uint32_t index = 0; index < secure_file_max; ++index) {
        char name[file_name_bytes];
        std::snprintf(name, sizeof name, "F%03u.DAT", unsigned(index));
        EXPECT(secure_set(params, name, id));
    }
    EXPECT(!secure_set(params, "ONEMORE.DAT", id));
    EXPECT(secure_set(params, "F000.DAT", id));
    secure_remove(params, "F000.DAT");
    EXPECT(params.secure_count == secure_file_max - 1);
    EXPECT(secure_find(params, "F000.DAT") < 0);
    EXPECT(secure_find(params, "F112.DAT") == int(secure_file_max - 2));
}

static void test_names()
{
    EXPECT(dir_name_valid("BCUS98119-SAVE00"));
    EXPECT(dir_name_valid("ABCDEFGHIJKLMNOPQRSTUVWXYZ01234"));
    EXPECT(!dir_name_valid("ABCDEFGHIJKLMNOPQRSTUVWXYZ012345"));
    EXPECT(!dir_name_valid(""));
    EXPECT(!dir_name_valid("../X"));
    EXPECT(!dir_name_valid("A/B"));
    EXPECT(file_name_valid("GAME.SAV"));
    EXPECT(file_name_valid("ABCDEFGH.DAT"));
    EXPECT(!file_name_valid("ABCDEFGHI.DAT"));
    EXPECT(!file_name_valid("PARAM.SFO"));
    EXPECT(!file_name_valid(".."));
    EXPECT(!file_name_valid("A/B"));
    EXPECT(!file_name_valid("A B"));
    EXPECT(prefix_matches("", "ANY"));
    EXPECT(prefix_matches(nullptr, "ANY"));
    EXPECT(prefix_matches("BCUS98119", "BCUS98119-SAVE00"));
    EXPECT(prefix_matches("BCES00609|BCUS98119", "BCUS98119-SAVE00"));
    EXPECT(!prefix_matches("BCES00609|BCUS98118", "BCUS98119-SAVE00"));
    EXPECT(content_file_name(file_type_pic1) && !std::strcmp(content_file_name(file_type_pic1), "PIC1.PNG"));
    EXPECT(!content_file_name(file_type_secure));
    EXPECT(!content_file_name(9));
}

int main()
{
    test_round_trip();
    test_keys_sorted();
    test_full_strings_are_terminated();
    test_short_capacity_and_bad_input();
    test_secure_list();
    test_names();
    std::printf(g_failures ? "SAVEDATA_SFO_TEST FAIL failures=%d\n" : "SAVEDATA_SFO_TEST PASS failures=%d\n", g_failures);
    return g_failures ? 1 : 0;
}
