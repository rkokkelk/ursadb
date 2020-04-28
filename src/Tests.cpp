#define CATCH_CONFIG_MAIN

#include <cstdlib>
#include <variant>

#include "catch/Catch.h"
#include "libursa/BitmapIndexBuilder.h"
#include "libursa/Database.h"
#include "libursa/DatabaseSnapshot.h"
#include "libursa/FlatIndexBuilder.h"
#include "libursa/IndexBuilder.h"
#include "libursa/OnDiskDataset.h"
#include "libursa/OnDiskIndex.h"
#include "libursa/Query.h"
#include "libursa/QueryGraph.h"
#include "libursa/QueryParser.h"
#include "libursa/ResultWriter.h"
#include "libursa/Utils.h"

TriGram gram3_pack(const char (&s)[4]) {
    TriGram v0 = (uint8_t)s[0];
    TriGram v1 = (uint8_t)s[1];
    TriGram v2 = (uint8_t)s[2];
    return (v0 << 16U) | (v1 << 8U) | (v2 << 0U);
}

TriGram text4_pack(const char (&s)[5]) {
    TriGram v0 = get_b64_value(s[0]);
    TriGram v1 = get_b64_value(s[1]);
    TriGram v2 = get_b64_value(s[2]);
    TriGram v3 = get_b64_value(s[3]);
    return (v0 << 18U) | (v1 << 12U) | (v2 << 6U) | (v3 << 0U);
}

template <typename T>
T parse(std::string query_text) {
    Command cmd = parse_command(query_text);
    REQUIRE(std::holds_alternative<T>(cmd));
    return std::move(std::get<T>(cmd));
}

QString mqs(const std::string &str) {
    QString out;
    for (const auto &c : str) {
        out.emplace_back(QToken::single(c));
    }
    return out;
}

TEST_CASE("packing 3grams", "[internal]") {
    // pay attention to the input, this covers unexpected sign extension
    REQUIRE(gram3_pack("\xCC\xBB\xAA") == (TriGram)0xCCBBAAU);
    REQUIRE(gram3_pack("\xAA\xBB\xCC") == (TriGram)0xAABBCCU);
    REQUIRE(gram3_pack("abc") == (TriGram)0x616263);
}

TEST_CASE("select hexstring without spaces", "[queryparser]") {
    auto cmd = parse<SelectCommand>("select{4d534d};");
    REQUIRE(cmd.get_query() == q(mqs("MSM")));
}

TEST_CASE("select hexstring with spaces", "[queryparser]") {
    auto cmd = parse<SelectCommand>("select { 4d 53 4d };");
    REQUIRE(cmd.get_query() == q(mqs("MSM")));
}

TEST_CASE("select hexstring with mixed spaces", "[queryparser]") {
    auto cmd = parse<SelectCommand>("select { 4d 534d };");
    REQUIRE(cmd.get_query() == q(mqs("MSM")));
}

TEST_CASE("select hexstring with full wildcard", "[queryparser]") {
    QString expect;
    expect.emplace_back(std::move(QToken::single(0x4d)));
    expect.emplace_back(std::move(QToken::wildcard()));
    expect.emplace_back(std::move(QToken::single(0x4d)));

    auto cmd = parse<SelectCommand>("select { 4d ?? 4d };");
    REQUIRE(cmd.get_query() == q(std::move(expect)));
}

TEST_CASE("select hexstring with high wildcard", "[queryparser]") {
    QString expect;
    expect.emplace_back(std::move(QToken::single(0x4d)));
    expect.emplace_back(std::move(QToken::high_wildcard(0x03)));
    expect.emplace_back(std::move(QToken::single(0x4d)));

    auto cmd = parse<SelectCommand>("select { 4d ?3 4d };");
    REQUIRE(cmd.get_query() == q(std::move(expect)));
}

TEST_CASE("select hexstring with low wildcard", "[queryparser]") {
    QString expect;
    expect.emplace_back(std::move(QToken::single(0x4d)));
    expect.emplace_back(std::move(QToken::low_wildcard(0x50)));
    expect.emplace_back(std::move(QToken::single(0x4d)));

    auto cmd = parse<SelectCommand>("select { 4d 5? 4d };");
    REQUIRE(cmd.get_query() == q(std::move(expect)));
}

TEST_CASE("select hexstring with explicit options", "[queryparser]") {
    QString expect;
    expect.emplace_back(std::move(QToken::single(0x4d)));
    expect.emplace_back(std::move(QToken::with_values({0x51, 0x52})));
    expect.emplace_back(std::move(QToken::single(0x4d)));

    auto cmd = parse<SelectCommand>("select { 4d (51 | 52) 4d };");
    REQUIRE(cmd.get_query() == q(std::move(expect)));
}

TEST_CASE("select hexstring with mixed explicit options", "[queryparser]") {
    QString expect;
    expect.emplace_back(std::move(QToken::single(0x4d)));
    expect.emplace_back(std::move(QToken::with_values(
        {0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b,
         0x5c, 0x5d, 0x5e, 0x5f, 0x70})));
    expect.emplace_back(std::move(QToken::single(0x4d)));

    auto cmd = parse<SelectCommand>("select { 4d (5? | 70) 4d };");
    REQUIRE(cmd.get_query() == q(std::move(expect)));
}

TEST_CASE("select literal", "[queryparser]") {
    auto cmd = parse<SelectCommand>("select \"MSM\";");
    REQUIRE(cmd.get_query() == q(std::move(mqs("MSM"))));
}

TEST_CASE("select literal with hex escapes", "[queryparser]") {
    auto cmd = parse<SelectCommand>("select \"\\x4d\\x53\\x4d\";");
    REQUIRE(cmd.get_query() == q(std::move(mqs("MSM"))));
}

TEST_CASE("select literal with uppercase hex escapes", "[queryparser]") {
    auto cmd = parse<SelectCommand>("select \"\\x4D\\x53\\x4D\";");
    REQUIRE(cmd.get_query() == q(std::move(mqs("MSM"))));
}

TEST_CASE("select char escapses", "[queryparser]") {
    auto cmd = parse<SelectCommand>("select \"\\n\\t\\r\\f\\b\\\\\\\"\";");
    REQUIRE(cmd.get_query() == q(std::move(mqs("\n\t\r\f\b\\\""))));
}

TEST_CASE("select OR", "[queryparser]") {
    auto cmd = parse<SelectCommand>("select \"test\" | \"cat\";");
    std::vector<Query> expect_or;
    expect_or.emplace_back(q(mqs("test")));
    expect_or.emplace_back(q(mqs("cat")));
    REQUIRE(cmd.get_query() == q_or(std::move(expect_or)));
}

TEST_CASE("select AND", "[queryparser]") {
    auto cmd = parse<SelectCommand>("select \"test\" & \"cat\";");
    std::vector<Query> expect_and;
    expect_and.emplace_back(q(mqs("test")));
    expect_and.emplace_back(q(mqs("cat")));
    REQUIRE(cmd.get_query() == q_and(std::move(expect_and)));
}

TEST_CASE("select operator order", "[queryparser]") {
    auto cmd =
        parse<SelectCommand>("select \"cat\" | \"dog\" & \"msm\" | \"monk\";");

    std::vector<Query> expect_or;
    expect_or.emplace_back(q(std::move(mqs("msm"))));
    expect_or.emplace_back(q(std::move(mqs("monk"))));

    std::vector<Query> expect_and;
    expect_and.emplace_back(q(std::move(mqs("dog"))));
    expect_and.emplace_back(q_or(std::move(expect_or)));

    std::vector<Query> expect_final;
    expect_final.emplace_back(q(std::move(mqs("cat"))));
    expect_final.emplace_back(q_and(std::move(expect_and)));

    REQUIRE(cmd.get_query() == q_or(std::move(expect_final)));
}

TEST_CASE("select min .. of", "[queryparser]") {
    auto cmd =
        parse<SelectCommand>("select min 2 of (\"xyz\", \"cat\", \"hmm\");");
    std::vector<Query> expect_min;
    expect_min.emplace_back(q(std::move(mqs("xyz"))));
    expect_min.emplace_back(q(std::move(mqs("cat"))));
    expect_min.emplace_back(q(std::move(mqs("hmm"))));

    REQUIRE(cmd.get_query() == q_min_of(2, std::move(expect_min)));
}

TEST_CASE("select literal without iterator", "[queryparser]") {
    auto cmd = parse<SelectCommand>("select \"MSM\";");
    REQUIRE(cmd.get_query() == q(std::move(mqs("MSM"))));
    REQUIRE(cmd.iterator_requested() == false);
}

TEST_CASE("select literal into iterator", "[queryparser]") {
    auto cmd = parse<SelectCommand>("select into iterator \"MSM\";");
    REQUIRE(cmd.get_query() == q(std::move(mqs("MSM"))));
    REQUIRE(cmd.iterator_requested() == true);
}

TEST_CASE("select literal with taints", "[queryparser]") {
    auto cmd = parse<SelectCommand>("select with taints [\"hmm\"] \"MSM\";");
    REQUIRE(cmd.get_query() == q(std::move(mqs("MSM"))));
    REQUIRE(cmd.get_taints() == std::set<std::string>{"hmm"});
}

TEST_CASE("compact all command", "[queryparser]") {
    auto cmd = parse<CompactCommand>("compact all;");
    REQUIRE(cmd.get_type() == CompactType::All);
}

TEST_CASE("compact smart command", "[queryparser]") {
    auto cmd = parse<CompactCommand>("compact smart;");
    REQUIRE(cmd.get_type() == CompactType::Smart);
}

TEST_CASE("index command with default types", "[queryparser]") {
    auto cmd = parse<IndexCommand>("index \"cat\";");
    REQUIRE(cmd.get_paths() == std::vector<std::string>{"cat"});
    REQUIRE(cmd.get_index_types() == std::vector{IndexType::GRAM3});
    REQUIRE(cmd.ensure_unique() == true);
}

TEST_CASE("index command with type override", "[queryparser]") {
    auto cmd = parse<IndexCommand>("index \"cat\" with [text4, wide8];");
    REQUIRE(cmd.get_paths() == std::vector<std::string>{"cat"});
    REQUIRE(cmd.get_index_types() ==
            std::vector{IndexType::TEXT4, IndexType::WIDE8});
}

TEST_CASE("index command with nocheck", "[queryparser]") {
    auto cmd = parse<IndexCommand>("index \"cat\" nocheck;");
    REQUIRE(cmd.get_paths() == std::vector<std::string>{"cat"});
    REQUIRE(cmd.ensure_unique() == false);
}

TEST_CASE("index command with empty type override", "[queryparser]") {
    auto cmd = parse<IndexCommand>("index \"cat\" with [];");
    REQUIRE(cmd.get_paths() == std::vector<std::string>{"cat"});
    REQUIRE(cmd.get_index_types().empty());
}

TEST_CASE("index command with escapes", "[queryparser]") {
    auto cmd = parse<IndexCommand>("index \"\\x63\\x61\\x74\";");
    REQUIRE(cmd.get_paths() == std::vector<std::string>{"cat"});
}

TEST_CASE("index command with hexstring", "[queryparser]") {
    auto cmd = parse<IndexCommand>("index {63 61 74};");
    REQUIRE(cmd.get_paths() == std::vector<std::string>{"cat"});
}

TEST_CASE("index command with multiple paths", "[queryparser]") {
    auto cmd = parse<IndexCommand>("index \"aaa\" \"bbb\";");
    REQUIRE(cmd.get_paths() == std::vector<std::string>{"aaa", "bbb"});
}

TEST_CASE("index from command", "[queryparser]") {
    auto cmd = parse<IndexFromCommand>("index from list \"aaa\";");
    REQUIRE(cmd.get_path_list_fname() == "aaa");
    REQUIRE(cmd.get_index_types() == std::vector{IndexType::GRAM3});
    REQUIRE(cmd.ensure_unique() == true);
}

TEST_CASE("index from command with type override", "[queryparser]") {
    auto cmd = parse<IndexFromCommand>("index from list \"aaa\" with [hash4];");
    REQUIRE(cmd.get_path_list_fname() == "aaa");
    REQUIRE(cmd.get_index_types() == std::vector{IndexType::HASH4});
}

TEST_CASE("index from command with nocheck", "[queryparser]") {
    auto cmd = parse<IndexFromCommand>("index from list \"aaa\" nocheck;");
    REQUIRE(cmd.get_path_list_fname() == "aaa");
    REQUIRE(cmd.ensure_unique() == false);
}

TEST_CASE("dataset.taint command", "[queryparser]") {
    auto cmd = parse<TaintCommand>("dataset \"xyz\" taint \"hmm\";");
    REQUIRE(cmd.get_dataset() == "xyz");
    REQUIRE(cmd.get_mode() == TaintMode::Add);
    REQUIRE(cmd.get_taint() == "hmm");
}

TEST_CASE("dataset.untaint command", "[queryparser]") {
    auto cmd = parse<TaintCommand>("dataset \"xyz\" untaint \"hmm\";");
    REQUIRE(cmd.get_dataset() == "xyz");
    REQUIRE(cmd.get_mode() == TaintMode::Clear);
    REQUIRE(cmd.get_taint() == "hmm");
}

TEST_CASE("iterator.pop command", "[queryparser]") {
    auto cmd = parse<IteratorPopCommand>("iterator \"xyz\" pop 3;");
    REQUIRE(cmd.get_iterator_id() == "xyz");
    REQUIRE(cmd.elements_to_pop() == 3);
}

TEST_CASE("reindex command", "[queryparser]") {
    auto cmd = parse<ReindexCommand>("reindex \"xyz\" with [wide8];");
    REQUIRE(cmd.dataset_id() == "xyz");
    REQUIRE(cmd.get_index_types() == std::vector{IndexType::WIDE8});
}

TEST_CASE("topology command", "[queryparser]") {
    parse<TopologyCommand>("topology;");
}

TEST_CASE("ping command", "[queryparser]") { parse<PingCommand>("ping;"); }

TEST_CASE("status command", "[queryparser]") {
    parse<StatusCommand>("status;");
}

TEST_CASE("get_trigrams", "[ngrams]") {
    std::string str;
    std::vector<TriGram> gram3;

    SECTION("String len < 3") {
        str = "";
        gram3 = get_trigrams((const uint8_t *)str.c_str(), str.length());
        REQUIRE(gram3.empty());
        str = "a";
        gram3 = get_trigrams((const uint8_t *)str.c_str(), str.length());
        REQUIRE(gram3.empty());
        str = "aa";
        gram3 = get_trigrams((const uint8_t *)str.c_str(), str.length());
        REQUIRE(gram3.empty());
    }

    SECTION("String len 3") {
        str = "abc";
        gram3 = get_trigrams((const uint8_t *)str.c_str(), str.length());
        REQUIRE(gram3[0] == gram3_pack("abc"));
        REQUIRE(gram3.size() == 1);
    }

    SECTION("String len 4") {
        str = "abcd";
        gram3 = get_trigrams((const uint8_t *)str.c_str(), str.length());
        REQUIRE(gram3[0] == gram3_pack("abc"));
        REQUIRE(gram3[1] == gram3_pack("bcd"));
        REQUIRE(gram3.size() == 2);
    }
}

TEST_CASE("get_b64grams", "[ngrams]") {
    std::string str;
    std::vector<TriGram> gram3;

    str = "";
    gram3 = get_b64grams((const uint8_t *)str.c_str(), str.length());
    REQUIRE(gram3.empty());
    str = "a";
    gram3 = get_b64grams((const uint8_t *)str.c_str(), str.length());
    REQUIRE(gram3.empty());
    str = "ab";
    gram3 = get_b64grams((const uint8_t *)str.c_str(), str.length());
    REQUIRE(gram3.empty());
    str = "abc";
    gram3 = get_b64grams((const uint8_t *)str.c_str(), str.length());
    REQUIRE(gram3.empty());
    str = "abcd";
    gram3 = get_b64grams((const uint8_t *)str.c_str(), str.length());
    REQUIRE(gram3.size() == 1);
    REQUIRE(gram3[0] == text4_pack("abcd"));
    str = "abcde";
    gram3 = get_b64grams((const uint8_t *)str.c_str(), str.length());
    REQUIRE(gram3.size() == 2);
    REQUIRE(gram3[0] == text4_pack("abcd"));
    REQUIRE(gram3[1] == text4_pack("bcde"));
    str = "abcde\xAAXghi";
    gram3 = get_b64grams((const uint8_t *)str.c_str(), str.length());
    REQUIRE(gram3.size() == 3);
    REQUIRE(gram3[0] == text4_pack("abcd"));
    REQUIRE(gram3[1] == text4_pack("bcde"));
    REQUIRE(gram3[2] == text4_pack("Xghi"));
}

TEST_CASE("get_wide_b64grams", "[ngrams]") {
    std::string str;
    std::vector<TriGram> gram3;

    str = "";
    gram3 = get_wide_b64grams((const uint8_t *)str.c_str(), str.length());
    REQUIRE(gram3.empty());
    str = "a";
    gram3 = get_wide_b64grams((const uint8_t *)str.c_str(), str.length());
    REQUIRE(gram3.empty());
    str = "ab";
    gram3 = get_wide_b64grams((const uint8_t *)str.c_str(), str.length());
    REQUIRE(gram3.empty());
    str = "abcdefg";
    gram3 = get_wide_b64grams((const uint8_t *)str.c_str(), str.length());
    REQUIRE(gram3.empty());
    str = std::string("\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", 10);
    gram3 = get_wide_b64grams((const uint8_t *)str.c_str(), str.length());
    REQUIRE(gram3.empty());
    str = std::string("a\0b\0c\0d\0", 8);
    gram3 = get_wide_b64grams((const uint8_t *)str.c_str(), str.length());
    REQUIRE(gram3.size() == 1);
    REQUIRE(gram3[0] == text4_pack("abcd"));
    str = std::string("a\0b\0c\0d\0e\0", 10);
    gram3 = get_wide_b64grams((const uint8_t *)str.c_str(), str.length());
    REQUIRE(gram3.size() == 2);
    REQUIRE(gram3[0] == text4_pack("abcd"));
    REQUIRE(gram3[1] == text4_pack("bcde"));
    str = std::string("\0a\0b\0c\0d", 8);
    gram3 = get_wide_b64grams((const uint8_t *)str.c_str(), str.length());
    REQUIRE(gram3.empty());
    str = std::string("\0a\0b\0c\0d\0", 9);
    gram3 = get_wide_b64grams((const uint8_t *)str.c_str(), str.length());
    REQUIRE(gram3.size() == 1);
    REQUIRE(gram3[0] == text4_pack("abcd"));
}

TEST_CASE("get_h4grams", "[ngrams]") {
    std::string str;
    std::vector<TriGram> gram3;

    str = "";
    gram3 = get_h4grams((const uint8_t *)str.c_str(), str.length());
    REQUIRE(gram3.empty());
    str = "a";
    gram3 = get_h4grams((const uint8_t *)str.c_str(), str.length());
    REQUIRE(gram3.empty());
    str = "ab";
    gram3 = get_h4grams((const uint8_t *)str.c_str(), str.length());
    REQUIRE(gram3.empty());
    str = "abc";
    gram3 = get_h4grams((const uint8_t *)str.c_str(), str.length());
    REQUIRE(gram3.empty());
    str = "abcd";
    gram3 = get_h4grams((const uint8_t *)str.c_str(), str.length());
    REQUIRE(gram3.size() == 1);
    REQUIRE(gram3[0] == (gram3_pack("abc") ^ gram3_pack("bcd")));
    str = "abcde";
    gram3 = get_h4grams((const uint8_t *)str.c_str(), str.length());
    REQUIRE(gram3.size() == 2);
    REQUIRE(gram3[0] == (gram3_pack("abc") ^ gram3_pack("bcd")));
    REQUIRE(gram3[1] == (gram3_pack("bcd") ^ gram3_pack("cde")));
}

TEST_CASE("Compress run symmetry", "[compress_run]") {
    std::stringstream ss;

    std::vector<FileId> fids;
    int last_fid = 0;

    srand(1337);

    for (int i = 0; i < 1000; i++) {
        if (rand() % 100 < 80) {
            last_fid += 1 + rand() % 120;
        } else {
            last_fid += 1 + rand() % 100000;
        }

        fids.push_back(last_fid);
    }

    compress_run(fids, ss);

    std::string s = ss.str();
    const auto *ptr = (uint8_t *)s.data();

    std::vector<FileId> read_fids = read_compressed_run(ptr, ptr + s.length());

    REQUIRE(fids.size() == read_fids.size());

    for (unsigned int i = 0; i < fids.size(); i++) {
        REQUIRE(fids[i] == read_fids[i]);
    }
}

TEST_CASE("Compress run sanity", "[compress_run]") {
    std::stringstream ss;
    std::vector<FileId> fids = {1, 2, 5, 8, 256 + 8 + 1};
    compress_run(fids, ss);
    std::string s = ss.str();

    REQUIRE(s == std::string("\x01\x00\x02\x02\x80\x02", 6));
}

std::vector<std::string> basic_test_payload{
    "kjhg", "\xA1\xA2\xA3\xA4\xA5\xA6\xA7\xA8", "",
    "\xA1\xA2Xbcde\xA3\xA4\xA5\xA6\xA7systXm32\xA5Xcdef\xA6\xA7",
    "\xAA\xAA\xAA\xAA\xAA\xAAXm32\xA5Xd\xAA\xAA\xAA\xAA\xAA\xAA"};

void add_payload(IndexBuilder *builder,
                 const std::vector<std::string> &payload) {
    std::string contents;

    for (int i = 0; i < payload.size(); i++) {
        builder->add_file(i + 1,
                          reinterpret_cast<const uint8_t *>(payload[i].data()),
                          payload[i].size());
    }
}

void check_query_is_everything(const OnDiskIndex &ndx,
                               const std::string &query) {
    REQUIRE(ndx.query_str(mqs(query)).is_everything());
}

void check_query(const OnDiskIndex &ndx, const std::string &query,
                 const std::vector<uint32_t> &results) {
    REQUIRE(ndx.query_str(mqs(query)).vector() == results);
}

void check_test_payload_gram3(const OnDiskIndex &ndx) {
    check_query_is_everything(ndx, "");
    check_query_is_everything(ndx, "a");
    check_query_is_everything(ndx, "ab");
    check_query(ndx, "kjhg", {1});
    check_query(ndx, "\xA1\xA2\xA3", {2});
    check_query(ndx, "m32\xA5X", {4, 5});
    check_query(ndx, "Xm32\xA5X", {4, 5});
    check_query(ndx, "Xm32\xA5s", {});
    check_query(ndx, "Xbcdef", {4});
    check_query(ndx, "\xA4\xA5\xA6\xA7", {2, 4});
}

void check_test_payload_text4(const OnDiskIndex &ndx) {
    check_query_is_everything(ndx, "");
    check_query_is_everything(ndx, "a");
    check_query_is_everything(ndx, "ab");
    check_query_is_everything(ndx, "abc");
    check_query(ndx, "Xbcd", {4});
    check_query(ndx, "Xbcdef", {4});
    check_query(ndx, "m32\xA5X", {});
    check_query(ndx, "Xm32\xA5X", {4, 5});
    check_query_is_everything(ndx, "\xA1\xA2\xA3");
    check_query_is_everything(ndx, "d\xA6\xA7");
    check_query_is_everything(ndx, "\xA4\xA5\xA6\xA7");
}

// RAII helper for OnDiskIndex.
template <typename BuilderT>
class OnDiskIndexTest {
    OnDiskIndex index_;

    static std::string build_and_get_fname(IndexType type) {
        fs::path test_fname = fs::temp_directory_path();
        test_fname /= "test_ndx_" + random_hex_string(8);
        BuilderT builder(type);
        add_payload(&builder, basic_test_payload);
        builder.save(test_fname);
        return test_fname;
    }

   public:
    OnDiskIndexTest(IndexType type) : index_(build_and_get_fname(type)) {}

    ~OnDiskIndexTest() { fs::remove(index_.get_fpath()); }

    const OnDiskIndex &get() const { return index_; }
};

TEST_CASE("BitmapIndexBuilder for gram3", "[index_builder]") {
    OnDiskIndexTest<BitmapIndexBuilder> index(IndexType::GRAM3);
    check_test_payload_gram3(index.get());
}

TEST_CASE("BitmapIndexBuilder for text4", "[index_builder]]") {
    OnDiskIndexTest<BitmapIndexBuilder> index(IndexType::TEXT4);
    check_test_payload_text4(index.get());
}

TEST_CASE("FlatIndexBuilder for gram3", "[index_builder]") {
    OnDiskIndexTest<FlatIndexBuilder> index(IndexType::GRAM3);
    check_test_payload_gram3(index.get());
}

TEST_CASE("FlatIndexBuilder for text4", "[index_builder]") {
    OnDiskIndexTest<FlatIndexBuilder> index(IndexType::TEXT4);
    check_test_payload_text4(index.get());
}

void make_query(Database &db, std::string query_str,
                std::set<std::string> expected_out) {
    TaskSpec *task_spec = db.allocate_task();
    Task task(task_spec);
    auto cmd = parse<SelectCommand>(query_str);
    InMemoryResultWriter out;
    db.snapshot().execute(cmd.get_query(), {}, &task, &out);
    db.commit_task(*task_spec, task.changes());

    std::vector<std::string> out_fixed;
    for (const auto &x : out.get()) {
        std::string xx = x.substr(x.find_last_of("/") + 1);
        xx.resize(xx.size() - 4);
        out_fixed.push_back(xx);
    }

    std::set<std::string> out_set(out_fixed.begin(), out_fixed.end());
    REQUIRE(out_set == expected_out);
}

TEST_CASE("Test internal_pick_common", "[internal_pick_common]") {
    std::vector<FileId> source1 = {1, 2, 3};
    REQUIRE(internal_pick_common(1, {&source1}) ==
            std::vector<FileId>{1, 2, 3});
    REQUIRE(internal_pick_common(2, {&source1}) == std::vector<FileId>{});

    std::vector<FileId> source2 = {3, 4, 5};
    REQUIRE(internal_pick_common(1, {&source1, &source2}) ==
            std::vector<FileId>{1, 2, 3, 4, 5});
    REQUIRE(internal_pick_common(2, {&source1, &source2}) ==
            std::vector<FileId>{3});

    std::vector<FileId> source3 = {1, 2, 3};
    REQUIRE(internal_pick_common(1, {&source1, &source3}) ==
            std::vector<FileId>{1, 2, 3});
    REQUIRE(internal_pick_common(2, {&source1, &source3}) ==
            std::vector<FileId>{1, 2, 3});

    std::vector<FileId> source4 = {4, 5, 6};
    REQUIRE(internal_pick_common(1, {&source1, &source4}) ==
            std::vector<FileId>{1, 2, 3, 4, 5, 6});
    REQUIRE(internal_pick_common(2, {&source1, &source4}) ==
            std::vector<FileId>{});

    REQUIRE(internal_pick_common(1, {&source1, &source2, &source4}) ==
            std::vector<FileId>{1, 2, 3, 4, 5, 6});
    REQUIRE(internal_pick_common(2, {&source1, &source2, &source4}) ==
            std::vector<FileId>{3, 4, 5});
    REQUIRE(internal_pick_common(3, {&source1, &source2, &source4}) ==
            std::vector<FileId>{});

    REQUIRE(internal_pick_common(1, {&source1, &source2, &source3}) ==
            std::vector<FileId>{1, 2, 3, 4, 5});
    REQUIRE(internal_pick_common(2, {&source1, &source2, &source3}) ==
            std::vector<FileId>{1, 2, 3});
    REQUIRE(internal_pick_common(3, {&source1, &source2, &source3}) ==
            std::vector<FileId>{3});

    std::vector<FileId> source5 = {};
    REQUIRE(internal_pick_common(1, {&source5}) == std::vector<FileId>{});
    REQUIRE(internal_pick_common(2, {&source5, &source5}) ==
            std::vector<FileId>{});
    REQUIRE(internal_pick_common(1, {&source1, &source5}) ==
            std::vector<FileId>{1, 2, 3});
    REQUIRE(internal_pick_common(1, {&source5, &source1}) ==
            std::vector<FileId>{1, 2, 3});
    REQUIRE(internal_pick_common(2, {&source1, &source1, &source5}) ==
            std::vector<FileId>{1, 2, 3});
    REQUIRE(internal_pick_common(2, {&source1, &source5, &source1}) ==
            std::vector<FileId>{1, 2, 3});
}

QueryGraph make_kot() {
    // Basically:  k -> o -> t
    return QueryGraph::from_qstring(mqs("kot"));
}

QueryGraph make_caet() {
    //                  a
    // Basically:  c -<   >- t
    //                  e
    QString expect;
    expect.emplace_back(std::move(QToken::single('c')));
    expect.emplace_back(std::move(QToken::with_values({'a', 'e'})));
    expect.emplace_back(std::move(QToken::single('t')));
    return QueryGraph::from_qstring(expect);
}

TEST_CASE("Simple query graph parse", "[query_graphs]") {
    auto graph{make_kot()};

    REQUIRE(graph.size() == 3);
}

TEST_CASE("Graph parse with wildcard", "[query_graphs]") {
    auto graph{make_caet()};

    REQUIRE(graph.size() == 4);
}

TEST_CASE("Simple graph join", "[query_graphs]") {
    auto graph{make_kot()};

    SECTION("With kot") {
        graph.join(std::move(make_kot()));
        REQUIRE(graph.size() == 7);
    }

    SECTION("With caet") {
        graph.join(std::move(make_caet()));
        REQUIRE(graph.size() == 8);
    }
}

QueryFunc make_oracle(std::string accepting) {
    return [accepting](uint32_t gram1) {
        if (accepting.find(static_cast<char>(gram1)) != std::string::npos) {
            return QueryResult::everything();
        }
        return QueryResult::empty();
    };
}

TEST_CASE("Test basic query", "[query_graphs]") {
    auto graph{make_kot()};

    REQUIRE(graph.run(make_oracle("tok")).is_everything());
    REQUIRE(!graph.run(make_oracle("abc")).is_everything());
}

TEST_CASE("Test wildcard query", "[query_graphs]") {
    auto graph{make_caet()};

    REQUIRE(graph.run(make_oracle("cat")).is_everything());
    REQUIRE(graph.run(make_oracle("cet")).is_everything());
    REQUIRE(!graph.run(make_oracle("abc")).is_everything());
}
