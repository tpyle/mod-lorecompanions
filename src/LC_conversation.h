/*
 * mod-lorecompanions - conversations between companions
 *
 * A line in lore_companion_text may open a conversation: the replies in
 * lore_companion_reply that follow it, in order, each spoken by a companion
 * (another one, or the opener again). This header holds the parts of that
 * which need nothing from the core, so they can be tested on their own.
 */

#ifndef MOD_LORECOMPANIONS_CONVERSATION_H
#define MOD_LORECOMPANIONS_CONVERSATION_H

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace LoreCompanions
{
    /// One reply in a conversation: who says it, and what, before placeholders.
    struct Reply
    {
        std::string speaker;    // a lore_companion.Id
        std::string text;
    };

    /**
     * The companions other than `opener` who speak somewhere in `replies`,
     * each once, in the order they first speak. A conversation is only
     * started when all of them are in the party.
     */
    inline std::vector<std::string> OtherSpeakers(std::string const& opener, std::vector<Reply> const& replies)
    {
        std::vector<std::string> speakers;
        for (Reply const& reply : replies)
            if (reply.speaker != opener && std::find(speakers.begin(), speakers.end(), reply.speaker) == speakers.end())
                speakers.push_back(reply.speaker);
        return speakers;
    }

    /**
     * When each reply is due, in milliseconds on the same clock as `nowMs`.
     * Every reply waits `baseMs`, plus `perCharMs` for each character of the
     * line before it, so a long line is given time to be read.
     */
    inline std::vector<uint64_t> ReplyTimes(std::string const& opening, std::vector<Reply> const& replies,
        uint64_t nowMs, uint32_t baseMs, uint32_t perCharMs)
    {
        std::vector<uint64_t> times;
        times.reserve(replies.size());
        uint64_t due = nowMs;
        size_t previous = opening.size();
        for (Reply const& reply : replies)
        {
            due += baseMs + uint64_t(perCharMs) * previous;
            times.push_back(due);
            previous = reply.text.size();
        }
        return times;
    }
}

#endif
