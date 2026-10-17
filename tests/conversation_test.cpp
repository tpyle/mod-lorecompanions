// Unit tests for src/LC_conversation.h, which needs only the standard
// library. Build and run with any C++17 compiler, e.g.:
//
//     g++ -std=c++17 -Wall -Wextra -Werror -Isrc tests/conversation_test.cpp -o /tmp/t && /tmp/t
//
// or, without a local compiler, the same command inside
// `podman run --rm -v "$PWD":/src:Z -w /src docker.io/library/gcc:14 sh -c '...'`.

#include "LC_conversation.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using LoreCompanions::Reply;

namespace
{
    int failures = 0;

    template <typename T>
    void Expect(T const& actual, T const& expected, char const* what)
    {
        if (actual == expected)
            return;
        std::printf("FAIL: %s\n", what);
        ++failures;
    }

    void OtherSpeakersSkipsTheOpenerAndRepeats()
    {
        std::vector<Reply> const replies = {
            { "arthas", "a" }, { "jaina", "b" }, { "sylvanas", "c" }, { "arthas", "d" } };
        Expect(LoreCompanions::OtherSpeakers("jaina", replies),
            std::vector<std::string>{ "arthas", "sylvanas" }, "other speakers, in order of first line");
    }

    void OtherSpeakersOfNoReplies()
    {
        Expect(LoreCompanions::OtherSpeakers("jaina", {}), std::vector<std::string>{}, "no replies, nobody else");
    }

    void ReplyTimesAccumulate()
    {
        std::vector<Reply> const replies = { { "arthas", "12345" }, { "jaina", "1234567890" }, { "arthas", "" } };
        // base 1000, 10 per char: opening 3 chars -> +1030, then +1050, then +1100
        Expect(LoreCompanions::ReplyTimes("abc", replies, 500, 1000, 10),
            std::vector<uint64_t>{ 1530, 2580, 3680 }, "each reply waits for the line before it");
    }

    void ReplyTimesWithoutPerChar()
    {
        std::vector<Reply> const replies = { { "arthas", "long line indeed" }, { "jaina", "x" } };
        Expect(LoreCompanions::ReplyTimes("whatever", replies, 0, 3500, 0),
            std::vector<uint64_t>{ 3500, 7000 }, "a flat delay when perChar is 0");
    }

    void ReplyTimesOfNoReplies()
    {
        Expect(LoreCompanions::ReplyTimes("abc", {}, 100, 1000, 10), std::vector<uint64_t>{}, "nothing to schedule");
    }
}

int main()
{
    OtherSpeakersSkipsTheOpenerAndRepeats();
    OtherSpeakersOfNoReplies();
    ReplyTimesAccumulate();
    ReplyTimesWithoutPerChar();
    ReplyTimesOfNoReplies();

    if (failures)
        return EXIT_FAILURE;
    std::printf("conversation_test: all passed\n");
    return EXIT_SUCCESS;
}
