#!/usr/bin/env python3
"""
Build data/sql/db-world/base/lore_companion.sql from the companion table below
and the line files in data/lines/.

    python3 tools/gen_lines.py

The core's database updater applies the result at the next startup (a module's
data/sql/db-world file is re-applied whenever its hash changes). To publish
lines without a restart, apply the file by hand and run ".companion reload".

A line file is named after the companion who speaks it - data/lines/jaina.txt
holds Jaina's lines - and reads like this:

    # a comment
    @ trigger=kill foe=onyxia
    The line itself. Every line under a header shares its filters.
    Another line with the same filters.

    @ trigger=idle with=sylvanas
    Jaina says this, only when Sylvanas is in the party...
    > sylvanas: ...and Sylvanas answers it a few seconds later.

Filters (all optional except trigger; a bad value stops the build):

    trigger=  summon dismiss zone_enter quest_accept quest_complete combat_start
              kill death revived ally_down grow_stronger idle
    where=    place names from PLACES, comma separated; matches zone or area
    foe=      creature names, comma separated, spaces written as underscores
    ctype=    creature types from CREATURE_TYPES, comma separated
    rank=     normal | notable (elite, rare or boss)
    side=     alliance | horde - the side she is fighting for (her summoner's)
    with=     another companion who must be in the party
    channel=  say | emote | party | yell
    weight=   a whole number, default 1

Placeholders: %summoner %leader %target %quest %other %zone %area %name.
%level does not exist and is refused: a companion never states a level as a
number. Growing stronger is something she notices, not a statistic she reads.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
LINES = ROOT / "data" / "lines"
OUT = ROOT / "data" / "sql" / "db-world" / "base" / "lore_companion.sql"

# id, name, title, race, class, gender, display, scale, spec, pet, sort
#   race:   1 human, 5 undead, 10 blood elf
#   class:  3 hunter, 8 mage
#   gender: 1 female
#   spec:   an AiPlayerbot.PremadeSpecName for that class
#   pet:    a hunter's pet by creature entry, 0 to let playerbots pick.
#           1554 is the Vampiric Duskbat of Tirisfal Glades - the Forsaken's
#           own bats, and an ordinary tameable family, so no exotic talent.
COMPANIONS = [
    ("jaina",       "Jaina",       "Lady Jaina Proudmoore",       1,  8, 1, 30863, 1.0, "frost pve", 0,    1),
    ("sylvanas",    "Sylvanas",    "Lady Sylvanas Windrunner",    5,  3, 1, 28213, 1.0, "mm pve",    1554, 2),
    ("alexstrasza", "Alexstrasza", "Alexstrasza the Life-Binder", 10, 8, 1, 28227, 1.0, "fire pve",  0,    3),
]

TRIGGERS = {
    "summon", "dismiss", "zone_enter", "quest_accept", "quest_complete", "combat_start",
    "kill", "death", "revived", "ally_down", "grow_stronger", "idle",
}

# Zone and area ids from AreaTable.dbc. A line written for a zone also fires in
# all of its areas, because a companion's zone is checked as well as her area.
PLACES = {
    # Kalimdor
    "durotar": 14, "orgrimmar": 1637, "barrens": 17, "dustwallow": 15, "theramore": 513,
    "brackenwall": 496, "onyxias_lair": 2159, "ashenvale": 331, "thunder_bluff": 1638,
    "darnassus": 1657, "hyjal": 616,
    # Eastern Kingdoms
    "elwynn": 12, "northshire": 9, "goldshire": 87, "stormwind": 1519, "westfall": 40,
    "duskwood": 10, "ironforge": 1537, "wetlands": 11, "grim_batol": 1037,
    "tirisfal": 85, "deathknell": 154, "brill": 159, "undercity": 1497, "silverpine": 130,
    "hillsbrad": 267, "southshore": 271, "tarren_mill": 272, "alterac": 36, "lordamere": 170,
    "arathi": 45, "hinterlands": 47, "western_plaguelands": 28, "andorhal": 193,
    "hearthglen": 190, "caer_darrow": 2298, "eastern_plaguelands": 139,
    "lights_hope": 2268, "stratholme": 2017, "stratholme_city": 2279, "badlands": 3,
    "searing_gorge": 51, "burning_steppes": 46, "blackrock_mountain": 25,
    "blackrock_spire": 1583, "blackrock_depths": 1584, "quelthalas": 2037,
    "eversong": 3430, "sunstrider_isle": 3431, "silvermoon": 3487, "ghostlands": 3433,
    "windrunner_village": 3491, "windrunner_spire": 3492, "quel_danas": 4080,
    "sunwell_plateau": 4075,
    # Outland
    "hellfire": 3483, "shattrath": 3703, "netherstorm": 3523, "shadowmoon": 3520,
    "exodar": 3557,
    # Northrend
    "borean": 3537, "coldarra": 4024, "nexus": 4265, "howling_fjord": 495,
    "vengeance_landing": 4000, "new_agamand": 3991, "dragonblight": 65,
    "wyrmrest": 4161, "ruby_dragonshrine": 4168, "wrathgate": 4172,
    "agmars_hammer": 4165, "wintergarde": 4177, "new_hearthglen": 4180,
    "venomspite": 4186, "grizzly_hills": 394, "zuldrak": 66, "sholazar": 3711,
    "storm_peaks": 67, "crystalsong": 2817, "dalaran": 4395, "icecrown": 210,
    "icecrown_citadel": 4812, "culling_of_stratholme": 4100,
}

CREATURE_TYPES = {
    "beast": 1, "dragonkin": 2, "demon": 3, "elemental": 4, "giant": 5, "undead": 6,
    "humanoid": 7, "critter": 8, "mechanical": 9,
}

RANKS = {"normal": 0, "notable": 1}
SIDES = {"alliance": 0, "horde": 1}
CHANNELS = {"say": 0, "emote": 1, "party": 2, "yell": 3}
PLACEHOLDERS = {"%summoner", "%leader", "%target", "%quest", "%other", "%zone", "%area", "%name"}
COMPANION_IDS = {c[0] for c in COMPANIONS}


def fail(path, number, message):
    sys.exit(f"{path.name}:{number}: {message}")


def check_text(path, number, text):
    if "%level" in text:
        fail(path, number, "%level is not allowed - a companion never states a level as a number")
    for token in re.findall(r"%[a-z_]+", text):
        if token not in PLACEHOLDERS:
            fail(path, number, f"unknown placeholder {token}")
    if re.search(r"\blevel\s*\d", text, re.IGNORECASE):
        fail(path, number, "a line may not name a level number")


def parse_header(path, number, text):
    filters = {"trigger": None, "places": "", "ctypes": "", "foes": "", "rank": -1,
               "side": -1, "with": "", "channel": 0, "weight": 1}
    for token in text[1:].split():
        if "=" not in token:
            fail(path, number, f"expected key=value, got {token!r}")
        key, value = token.split("=", 1)
        values = [v for v in value.split(",") if v]
        if key == "trigger":
            if value not in TRIGGERS:
                fail(path, number, f"unknown trigger {value!r}")
            filters["trigger"] = value
        elif key == "where":
            for v in values:
                if v not in PLACES:
                    fail(path, number, f"unknown place {v!r} - add it to PLACES")
            filters["places"] = ",".join(str(PLACES[v]) for v in values)
        elif key == "ctype":
            for v in values:
                if v not in CREATURE_TYPES:
                    fail(path, number, f"unknown creature type {v!r}")
            filters["ctypes"] = ",".join(str(CREATURE_TYPES[v]) for v in values)
        elif key == "foe":
            filters["foes"] = "|".join(v.replace("_", " ") for v in values)
        elif key == "rank":
            if value not in RANKS:
                fail(path, number, f"rank is normal or notable, not {value!r}")
            filters["rank"] = RANKS[value]
        elif key == "side":
            if value not in SIDES:
                fail(path, number, f"side is alliance or horde, not {value!r}")
            filters["side"] = SIDES[value]
        elif key == "with":
            if value not in COMPANION_IDS:
                fail(path, number, f"unknown companion {value!r}")
            filters["with"] = value
        elif key == "channel":
            if value not in CHANNELS:
                fail(path, number, f"unknown channel {value!r}")
            filters["channel"] = CHANNELS[value]
        elif key == "weight":
            if not value.isdigit() or int(value) < 1:
                fail(path, number, f"weight must be a positive whole number")
            filters["weight"] = int(value)
        else:
            fail(path, number, f"unknown filter {key!r}")
    if not filters["trigger"]:
        fail(path, number, "a header needs a trigger")
    return filters


def load():
    rows = []
    for path in sorted(LINES.glob("*.txt")):
        companion = path.stem
        if companion not in COMPANION_IDS:
            sys.exit(f"{path.name}: no companion called {companion!r}")
        filters = None
        for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            text = raw.strip()
            if not text or text.startswith("#"):
                continue
            if text.startswith("@"):
                filters = parse_header(path, number, text)
                continue
            if filters is None:
                fail(path, number, "a line before any @ header")
            if text.startswith(">"):
                match = re.match(r">\s*([a-z]+)\s*:\s*(.+)", text)
                if not match:
                    fail(path, number, "a reply is written '> companion: text'")
                who, reply = match.groups()
                if who not in COMPANION_IDS or who == companion:
                    fail(path, number, f"{who!r} cannot reply here")
                if not rows or rows[-1]["companion"] != companion or rows[-1]["reply_by"]:
                    fail(path, number, "a reply must follow the line it answers")
                if filters["with"] and filters["with"] != who:
                    fail(path, number, f"the header says with={filters['with']}, but {who} replies")
                check_text(path, number, reply)
                rows[-1]["reply_by"] = who
                rows[-1]["reply_text"] = reply
                continue
            check_text(path, number, text)
            rows.append(dict(filters, companion=companion, text=text, reply_by="", reply_text=""))
    return rows


def sql(value):
    if isinstance(value, (int, float)):
        return str(value)
    return "'" + str(value).replace("\\", "\\\\").replace("'", "''") + "'"


def main():
    rows = load()
    out = [
        "-- ---------------------------------------------------------------------------",
        "-- mod-lorecompanions: who the companions are, and everything they say.",
        "--",
        "-- GENERATED by tools/gen_lines.py from data/lines/*.txt - edit those, then",
        "-- re-run it. Applied by the core's updater at startup; idempotent.",
        "-- ---------------------------------------------------------------------------",
        "",
        "DROP TABLE IF EXISTS `lore_companion`;",
        "CREATE TABLE `lore_companion` (",
        "  `Id` VARCHAR(32) NOT NULL,",
        "  `Name` VARCHAR(12) NOT NULL COMMENT 'character name',",
        "  `Title` VARCHAR(64) NOT NULL,",
        "  `Race` TINYINT UNSIGNED NOT NULL,",
        "  `Class` TINYINT UNSIGNED NOT NULL,",
        "  `Gender` TINYINT UNSIGNED NOT NULL,",
        "  `DisplayId` INT UNSIGNED NOT NULL COMMENT 'CreatureDisplayInfo id of the lore model',",
        "  `Scale` FLOAT NOT NULL DEFAULT 1,",
        "  `Spec` VARCHAR(32) NOT NULL COMMENT 'AiPlayerbot.PremadeSpecName for the class',",
        "  `PetEntry` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'hunter pet creature entry, 0 = playerbots picks',",
        "  `SortOrder` TINYINT UNSIGNED NOT NULL DEFAULT 0,",
        "  `Enabled` TINYINT UNSIGNED NOT NULL DEFAULT 1,",
        "  PRIMARY KEY (`Id`)",
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;",
        "",
        "INSERT INTO `lore_companion` (`Id`, `Name`, `Title`, `Race`, `Class`, `Gender`, `DisplayId`, `Scale`, `Spec`, `PetEntry`, `SortOrder`) VALUES",
        ",\n".join("(" + ", ".join(sql(v) for v in c) + ")" for c in COMPANIONS) + ";",
        "",
        "DROP TABLE IF EXISTS `lore_companion_text`;",
        "CREATE TABLE `lore_companion_text` (",
        "  `Id` INT UNSIGNED NOT NULL AUTO_INCREMENT,",
        "  `Companion` VARCHAR(32) NOT NULL,",
        "  `Trigger` VARCHAR(32) NOT NULL,",
        "  `Text` TEXT NOT NULL,",
        "  `Places` VARCHAR(255) NOT NULL DEFAULT '' COMMENT 'zone or area ids, comma separated',",
        "  `CreatureTypes` VARCHAR(64) NOT NULL DEFAULT '',",
        "  `Foes` VARCHAR(255) NOT NULL DEFAULT '' COMMENT 'creature names, | separated',",
        "  `Rank` TINYINT NOT NULL DEFAULT -1 COMMENT '-1 any, 0 normal, 1 elite/rare/boss',",
        "  `Side` TINYINT NOT NULL DEFAULT -1 COMMENT '-1 any, 0 Alliance, 1 Horde',",
        "  `With` VARCHAR(32) NOT NULL DEFAULT '' COMMENT 'companion who must be present',",
        "  `ReplyBy` VARCHAR(32) NOT NULL DEFAULT '',",
        "  `ReplyText` TEXT NOT NULL,",
        "  `Channel` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 say, 1 emote, 2 party, 3 yell',",
        "  `Weight` INT UNSIGNED NOT NULL DEFAULT 1,",
        "  PRIMARY KEY (`Id`),",
        "  KEY `idx_companion_trigger` (`Companion`, `Trigger`)",
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;",
        "",
    ]
    columns = "(`Companion`, `Trigger`, `Text`, `Places`, `CreatureTypes`, `Foes`, `Rank`, `Side`, `With`, `ReplyBy`, `ReplyText`, `Channel`, `Weight`)"
    for start in range(0, len(rows), 200):
        chunk = rows[start:start + 200]
        out.append(f"INSERT INTO `lore_companion_text` {columns} VALUES")
        values = []
        for r in chunk:
            values.append("(" + ", ".join(sql(v) for v in (
                r["companion"], r["trigger"], r["text"], r["places"], r["ctypes"], r["foes"],
                r["rank"], r["side"], r["with"], r["reply_by"], r["reply_text"], r["channel"], r["weight"])) + ")")
        out.append(",\n".join(values) + ";")
        out.append("")
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(out), encoding="utf-8")

    counts = {}
    for r in rows:
        counts.setdefault(r["companion"], {}).setdefault(r["trigger"], 0)
        counts[r["companion"]][r["trigger"]] += 1
    for companion, triggers in sorted(counts.items()):
        total = sum(triggers.values())
        detail = ", ".join(f"{t} {n}" for t, n in sorted(triggers.items()))
        print(f"{companion}: {total} lines ({detail})")
    replies = sum(1 for r in rows if r["reply_by"])
    print(f"{len(rows)} lines, {replies} with replies -> {OUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
