#define CATCH_CONFIG_MAIN

#include "Query.h"
#include "QueryParser.h"
#include "Utils.h"
#include "lib/Catch.h"

TEST_CASE("Test query parser", "[queryparser]") {
    SECTION("Simple atomic") {
        Query q = parse_query("\"test\"");
        REQUIRE(q.get_type() == QueryType::PRIMITIVE);
        REQUIRE(q.as_value() == "test");
    }

    SECTION("Logical or") {
        Query q = parse_query("\"test\" || \"lol!\"");
        REQUIRE(q.get_type() == QueryType::OR);
        REQUIRE(q.as_queries().size() == 2);
        REQUIRE(q.as_queries()[0].as_value() == "test");
        REQUIRE(q.as_queries()[1].as_value() == "lol!");
    }
}

TEST_CASE("Test get_trigrams", "[gram3]") {
    std::string str;
    std::vector<TriGram> gram3;

    SECTION("String len < 3") {
        str = "";
        gram3 = get_trigrams((const uint8_t *)str.c_str(), str.length());
        REQUIRE(gram3.size() == 0);
        str = "a";
        gram3 = get_trigrams((const uint8_t *)str.c_str(), str.length());
        REQUIRE(gram3.size() == 0);
        str = "aa";
        gram3 = get_trigrams((const uint8_t *)str.c_str(), str.length());
        REQUIRE(gram3.size() == 0);
    }

    SECTION("String len 3") {
        str = "abc";
        gram3 = get_trigrams((const uint8_t *)str.c_str(), str.length());
        REQUIRE(gram3[0] == (('a' << 16U) | ('b' << 8U) | 'c'));
        REQUIRE(gram3.size() == 1);
    }

    SECTION("String len 4") {
        str = "abcd";
        gram3 = get_trigrams((const uint8_t *)str.c_str(), str.length());
        REQUIRE(gram3[0] == (('a' << 16U) | ('b' << 8U) | 'c'));
        REQUIRE(gram3[1] == (('b' << 16U) | ('c' << 8U) | 'd'));
        REQUIRE(gram3.size() == 2);
    }
}

TEST_CASE("Compress run symmetry", "[compress_run]") {
    std::stringstream ss;

    std::vector<FileId> fids;
    int last_fid = 0;

    srand(1337);

    for (int i = 0; i < 1000; i++) {
        if (rand() % 100 < 80) {
            last_fid += rand() % 120;
        } else {
            last_fid += rand() % 100000;
        }

        fids.push_back(last_fid);
    }

    compress_run(fids, ss);

    std::string s = ss.str();
    const auto *ptr = (uint8_t *)s.data();

    std::vector<FileId> read_fids = read_compressed_run(ptr, ptr + s.length());

    REQUIRE(fids.size() == read_fids.size());

    for (int i = 0; i < fids.size(); i++) {
        REQUIRE(fids[i] == read_fids[i]);
    }
}
