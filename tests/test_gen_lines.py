"""Unit tests for tools/gen_lines.py's line-file parser.

    python3 -m unittest discover -s tests
"""

import importlib.util
import pathlib
import textwrap
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
_spec = importlib.util.spec_from_file_location("gen_lines", ROOT / "tools" / "gen_lines.py")
gen_lines = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gen_lines)

PATH = pathlib.Path("jaina.txt")


def parse(text, companion="jaina"):
    return gen_lines.parse_lines(PATH, companion, textwrap.dedent(text))


class ParseLinesTest(unittest.TestCase):
    def test_plain_lines_share_their_header(self):
        rows = parse("""
            @ trigger=kill foe=onyxia
            First.
            Second.
        """)
        self.assertEqual([r["text"] for r in rows], ["First.", "Second."])
        self.assertTrue(all(r["foes"] == "onyxia" and r["replies"] == [] for r in rows))

    def test_single_reply(self):
        rows = parse("""
            @ trigger=idle with=sylvanas
            Opening.
            > sylvanas: Answer.
        """)
        self.assertEqual(rows[0]["replies"], [("sylvanas", "Answer.")])

    def test_conversation_back_and_forth_and_third_speaker(self):
        rows = parse("""
            @ trigger=idle with=arthas
            One.
            > arthas: Two.
            > jaina: Three.
            > sylvanas: Four.
            Next line.
        """)
        self.assertEqual(rows[0]["replies"], [("arthas", "Two."), ("jaina", "Three."), ("sylvanas", "Four.")])
        self.assertEqual(rows[1]["text"], "Next line.")
        self.assertEqual(rows[1]["replies"], [])

    def test_comments_and_blank_lines_inside_a_conversation(self):
        rows = parse("""
            @ trigger=idle with=arthas
            One.

            # an aside
            > arthas: Two.
        """)
        self.assertEqual(rows[0]["replies"], [("arthas", "Two.")])

    def test_conversation_without_with_is_allowed(self):
        rows = parse("""
            @ trigger=zone_enter where=stratholme
            One.
            > arthas: Two.
        """)
        self.assertEqual(rows[0]["with"], "")
        self.assertEqual(rows[0]["places"], str(gen_lines.PLACES["stratholme"]))

    def test_reply_after_header_is_refused(self):
        with self.assertRaisesRegex(gen_lines.LineError, "must follow the line"):
            parse("""
                @ trigger=idle
                > arthas: Nobody spoke first.
            """)

    def test_reply_after_a_new_header_is_refused(self):
        with self.assertRaisesRegex(gen_lines.LineError, "must follow the line"):
            parse("""
                @ trigger=idle
                One.
                @ trigger=idle with=arthas
                > arthas: Answers the wrong header.
            """)

    def test_opener_cannot_answer_herself_first(self):
        with self.assertRaisesRegex(gen_lines.LineError, "cannot answer herself"):
            parse("""
                @ trigger=idle
                One.
                > jaina: Me again.
            """)

    def test_same_speaker_twice_in_a_row_is_refused(self):
        with self.assertRaisesRegex(gen_lines.LineError, "cannot answer herself"):
            parse("""
                @ trigger=idle
                One.
                > arthas: Two.
                > arthas: Two again.
            """)

    def test_unknown_speaker(self):
        with self.assertRaisesRegex(gen_lines.LineError, "no companion called 'varian'"):
            parse("""
                @ trigger=idle
                One.
                > varian: Two.
            """)

    def test_bad_reply_syntax(self):
        with self.assertRaisesRegex(gen_lines.LineError, "written '> companion: text'"):
            parse("""
                @ trigger=idle
                One.
                > Arthas says hello
            """)

    def test_too_many_replies(self):
        speakers = ["arthas", "jaina"] * gen_lines.MAX_REPLIES
        body = "\n".join(f"> {who}: Line {i}." for i, who in enumerate(speakers[:gen_lines.MAX_REPLIES + 1]))
        with self.assertRaisesRegex(gen_lines.LineError, "at most"):
            parse("@ trigger=idle\nOne.\n" + body)

    def test_max_replies_is_allowed(self):
        speakers = ["arthas", "jaina"] * gen_lines.MAX_REPLIES
        body = "\n".join(f"> {who}: Line {i}." for i, who in enumerate(speakers[:gen_lines.MAX_REPLIES]))
        rows = parse("@ trigger=idle\nOne.\n" + body)
        self.assertEqual(len(rows[0]["replies"]), gen_lines.MAX_REPLIES)

    def test_with_must_speak_in_the_conversation(self):
        with self.assertRaisesRegex(gen_lines.LineError, "with=sylvanas"):
            parse("""
                @ trigger=idle with=sylvanas
                One.
                > arthas: Two.
            """)

    def test_with_on_a_line_without_replies_is_fine(self):
        rows = parse("""
            @ trigger=idle with=sylvanas
            Just a remark about her.
        """)
        self.assertEqual(rows[0]["with"], "sylvanas")

    def test_reply_text_is_checked(self):
        with self.assertRaisesRegex(gen_lines.LineError, "unknown placeholder"):
            parse("""
                @ trigger=idle
                One.
                > arthas: Hello %nobody.
            """)
        with self.assertRaisesRegex(gen_lines.LineError, "level"):
            parse("""
                @ trigger=idle
                One.
                > arthas: I am level 80.
            """)

    def test_line_before_header(self):
        with self.assertRaisesRegex(gen_lines.LineError, "before any @ header"):
            parse("Orphan.\n")

    def test_error_names_file_and_line(self):
        with self.assertRaisesRegex(gen_lines.LineError, r"^jaina\.txt:3: "):
            parse("@ trigger=idle\nOne.\n> jaina: Me.\n")


class ConversationFileTest(unittest.TestCase):
    def parse(self, text):
        return gen_lines.parse_lines(pathlib.Path("arthas_jaina.txt"), None, textwrap.dedent(text))

    def test_each_line_names_its_opener(self):
        rows = self.parse("""
            @ trigger=zone_enter where=stratholme
            jaina: Arthas. Don't.
            > arthas: You don't know what I'm going to say.
            > jaina: I know exactly what you're going to say.
            arthas: Another one, opened by me.
            > jaina: Fine.
        """)
        self.assertEqual([r["companion"] for r in rows], ["jaina", "arthas"])
        self.assertEqual(rows[0]["text"], "Arthas. Don't.")
        self.assertEqual(rows[0]["replies"][-1], ("jaina", "I know exactly what you're going to say."))

    def test_opener_cannot_answer_herself(self):
        with self.assertRaisesRegex(gen_lines.LineError, "cannot answer herself"):
            self.parse("""
                @ trigger=idle
                jaina: One.
                > jaina: Two.
            """)

    def test_line_without_opener_is_refused(self):
        with self.assertRaisesRegex(gen_lines.LineError, "written 'companion: text'"):
            self.parse("""
                @ trigger=idle
                Nobody said this.
            """)

    def test_unknown_opener(self):
        with self.assertRaisesRegex(gen_lines.LineError, "no companion called 'varian'"):
            self.parse("""
                @ trigger=idle
                varian: Hello.
            """)

    def test_colon_inside_text_is_kept(self):
        rows = self.parse("""
            @ trigger=idle
            jaina: Three things: duty, grief, and tea.
            > arthas: In that order?
        """)
        self.assertEqual(rows[0]["text"], "Three things: duty, grief, and tea.")


class LengthTest(unittest.TestCase):
    def test_line_at_the_limit(self):
        rows = parse("@ trigger=idle\n" + "a" * gen_lines.MAX_BYTES + "\n")
        self.assertEqual(len(rows[0]["text"]), gen_lines.MAX_BYTES)

    def test_line_over_the_limit(self):
        with self.assertRaisesRegex(gen_lines.LineError, "at most"):
            parse("@ trigger=idle\n" + "a" * (gen_lines.MAX_BYTES + 1) + "\n")

    def test_limit_counts_bytes(self):
        with self.assertRaisesRegex(gen_lines.LineError, "at most"):
            parse("@ trigger=idle\nOne.\n> arthas: " + "é" * (gen_lines.MAX_BYTES // 2 + 1) + "\n")


class HeaderTest(unittest.TestCase):
    def test_unknown_filter(self):
        with self.assertRaisesRegex(gen_lines.LineError, "unknown filter"):
            parse("@ trigger=idle mood=grim\nOne.\n")

    def test_unknown_place(self):
        with self.assertRaisesRegex(gen_lines.LineError, "unknown place"):
            parse("@ trigger=zone_enter where=atlantis\nOne.\n")

    def test_header_needs_trigger(self):
        with self.assertRaisesRegex(gen_lines.LineError, "needs a trigger"):
            parse("@ where=stormwind\nOne.\n")

    def test_unknown_with(self):
        with self.assertRaisesRegex(gen_lines.LineError, "unknown companion"):
            parse("@ trigger=idle with=varian\nOne.\n")


class ShippedLinesTest(unittest.TestCase):
    def test_every_line_file_builds(self):
        rows = gen_lines.load()
        self.assertGreater(len(rows), 0)
        for row in rows:
            for who, _ in row["replies"]:
                self.assertIn(who, gen_lines.COMPANION_IDS)


if __name__ == "__main__":
    unittest.main()
