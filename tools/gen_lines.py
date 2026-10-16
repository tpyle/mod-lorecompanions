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
#   race:   1 human, 2 orc, 3 dwarf, 4 night elf, 5 undead, 10 blood elf
#   class:  2 paladin, 3 hunter, 4 rogue, 5 priest, 7 shaman, 8 mage
#   gender: 0 male, 1 female
#   scale:  multiplies the model's own size. Alexstrasza's model is drawn at
#           1.75 (Jaina's and Sylvanas's at 1.2, player races at 1.0), so 0.68
#           brings her to about 1.19: a little over a blood elf, level with
#           the other two. Moira, Liadrin and Tyrande wear their own NPC
#           outfits on their race's model, at its natural size. Thrall's
#           Warchief model is drawn at 1.2, so 0.9 brings him to about 1.08:
#           a little over an ordinary orc rather than a head above. Illidan's Black Temple model is drawn at 1.5 and stands
#           about three times a night elf's height, wings included, so 0.35
#           brings him to a little over a night elf. Whitemane's Scarlet
#           Monastery model is drawn at 1.25, so 0.85 brings her to about
#           1.06; Paletress is at her natural size. Tirion, Mograine and
#           Abbendis are drawn at 1.2, so 0.9 brings them to about 1.08;
#           Isillien is at his natural size.
#   spec:   an AiPlayerbot.PremadeSpecName for that class
#   pet:    a hunter's pet by creature entry, 0 to let playerbots pick.
#           1554 is the Vampiric Duskbat of Tirisfal Glades - the Forsaken's
#           own bats, and an ordinary tameable family, so no exotic talent.
COMPANIONS = [
    ("jaina",       "Jaina",       "Lady Jaina Proudmoore",       1,  8, 1, 30863, 1.0, "frost pve", 0,    1),
    ("sylvanas",    "Sylvanas",    "Lady Sylvanas Windrunner",    5,  3, 1, 28213, 1.0, "mm pve",    1554, 2),
    ("alexstrasza", "Alexstrasza", "Alexstrasza the Life-Binder", 10, 8, 1, 28227, 0.68, "fire pve", 0,    3),
    ("moira",       "Moira",       "Princess Moira Bronzebeard",  3,  5, 1, 8705,  1.0, "shadow pve", 0,  4),
    ("liadrin",     "Liadrin",     "Lady Liadrin",                10, 2, 1, 16820, 1.0, "prot pve",  0,    5),
    ("tyrande",     "Tyrande",     "Tyrande Whisperwind",         4,  5, 1, 7274,  1.0, "holy pve",  0,    6),
    ("arthas",      "Arthas",      "Prince Arthas Menethil",      1,  2, 0, 24949, 1.0, "ret pve",   0,    7),
    ("thrall",      "Thrall",      "Thrall, Warchief of the Horde", 2, 7, 0, 4527, 0.9, "enh pve",   0,    8),
    ("illidan",     "Illidan",     "Illidan Stormrage",           4,  4, 0, 21135, 0.35, "combat pve", 0,  9),
    ("whitemane",   "Whitemane",   "High Inquisitor Whitemane",   1,  5, 1, 2043,  0.85, "disc pve",  0,    10),
    ("paletress",   "Paletress",   "Argent Confessor Paletress",  1,  5, 1, 29083, 1.0, "holy pve",  0,    11),
    ("tirion",      "Tirion",      "Highlord Tirion Fordring",    1,  2, 0, 22209, 0.9, "ret pve",   0,    12),
    ("renault",     "Renault",     "Scarlet Commander Renault Mograine", 1, 2, 0, 2042, 0.9, "prot pve", 0, 13),
    ("brigitte",    "Brigitte",    "High General Brigitte Abbendis", 1, 2, 1, 24413, 0.9, "holy pve",  0,    14),
    ("isillien",    "Isillien",    "Grand Inquisitor Isillien",   1,  5, 0, 10339, 1.0, "shadow pve", 0,   15),
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
    "darnassus": 1657, "hyjal": 616, "teldrassil": 141, "dolanaar": 186, "shadowglen": 188,
    "ruttheran": 702, "darkshore": 148, "auberdine": 442, "astranaar": 415, "moonglade": 493,
    "nighthaven": 2361, "feralas": 357, "felwood": 361, "winterspring": 618, "azshara": 16,
    "silithus": 1377, "ahnqiraj": 3428, "tanaris": 440, "caverns_of_time": 1941,
    "hyjal_summit": 3606, "valley_of_trials": 363, "razor_hill": 362, "echo_isles": 368,
    "demon_fall_canyon": 435,
    # Eastern Kingdoms
    "dun_morogh": 1, "kharanos": 131, "gnomeregan": 133, "loch_modan": 38, "thelsamar": 144,
    "aerie_peak": 348, "ruins_of_thaurissan": 250, "molten_core": 2717, "tranquillien": 3488,
    "deatholme": 3500, "magisters_terrace": 4095, "zulaman": 3805, "durnholde": 275,
    "old_hillsbrad": 2367, "uthers_tomb": 196, "tyrs_hand": 2266, "ruins_of_lordaeron": 153,
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
    "exodar": 3557, "tempest_keep": 3845, "aldor_rise": 3896, "scryers_tier": 3898,
    "black_temple": 3959, "karabor": 3756, "hellfire_citadel": 3545, "nagrand": 3518,
    "garadar": 3613, "throne_of_elements": 3615,
    # Northrend
    "borean": 3537, "coldarra": 4024, "nexus": 4265, "howling_fjord": 495,
    "vengeance_landing": 4000, "new_agamand": 3991, "dragonblight": 65,
    "wyrmrest": 4161, "ruby_dragonshrine": 4168, "wrathgate": 4172,
    "agmars_hammer": 4165, "wintergarde": 4177, "new_hearthglen": 4180,
    "venomspite": 4186, "grizzly_hills": 394, "zuldrak": 66, "sholazar": 3711,
    "storm_peaks": 67, "crystalsong": 2817, "dalaran": 4395, "icecrown": 210,
    "icecrown_citadel": 4812, "culling_of_stratholme": 4100, "halls_of_stone": 4264,
    "halls_of_lightning": 4272, "ulduar": 4273, "argent_tournament": 4658,
    "scarlet_monastery": 796, "crusaders_pinnacle": 4580, "trial_of_the_champion": 4723,
    "warsong_hold": 4129, "naxxramas": 3456, "mardenholde": 203, "ebon_hold": 4281,
    "scarlet_enclave": 4298, "utgarde_keep": 3983, "frozen_throne": 4859, "halls_of_reflection": 4820,
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

# The weapons she is drawn holding: main hand, off hand, ranged, by item entry -
# the lore NPC's own creature_equip_template items. Only the picture changes;
# she fights with the gear she is given. 0 leaves that hand empty. A companion
# not listed here (her NPC carries nothing) shows her real weapons.
WEAPONS = {
    "jaina":     (2177,  12869, 0),      # Ornate Mage Staff, a glowing blue book
    "sylvanas":  (2179,  0,     42775),  # her scimitar and her bow
    "tyrande":   (5598,  0,     14105),  # three-bladed glaive, white bow
    "arthas":    (37579, 0,     0),      # the prince's mace
    "thrall":    (12183, 0,     0),      # the Doomhammer
    "illidan":   (32632, 32633, 0),      # the Warglaives of Azzinoth
    "whitemane": (28738, 0,     0),      # Benediction
    "paletress": (12591, 0,     0),      # her holy staff
    "tirion":    (13262, 0,     0),      # the Ashbringer
    "renault":   (7706,  12933, 0),      # his mace, and a Scarlet shield
    "brigitte":  (38216, 12933, 0),      # her axe and Scarlet shield
    "isillien":  (12591, 0,     0),      # a holy staff, as in Hearthglen
}
for _id in WEAPONS:
    if _id not in COMPANION_IDS:
        sys.exit(f"WEAPONS names {_id!r}, who is not in COMPANIONS")

# The shape of the armour she is drawn wearing, by item entry; 0 leaves the
# slot empty. A humanoid model's outfit (CreatureDisplayInfoExtra) supplies the
# colours of every piece, but a player is drawn with the meshes and attached
# models of her visible items: a helmet, shoulder pads, a cloak, the flap of a
# tabard or the skirt of a robe. So each slot names an item whose
# ItemDisplayInfo has the same models and geosets as the NPC's piece - the
# same colours too, where one exists - or 0 where the NPC wears nothing there.
# Creature models (Jaina, Sylvanas, Alexstrasza, Arthas, Illidan) draw no
# armour from items and are not listed. Unmatched pieces: Renault's red
# blindfold and Paletress's white shoulders are left empty; Brigitte's rust
# and Isillien's Crusader shoulders are drawn in black and in crimson.
SLOTS = {"head": 0, "shoulders": 2, "shirt": 3, "chest": 4, "waist": 5, "legs": 6,
         "feet": 7, "wrists": 8, "hands": 9, "back": 14, "tabard": 18}
_BARE = {slot: 0 for slot in SLOTS}
OUTFITS = {
    "moira":     dict(_BARE, legs=153),
    "liadrin":   dict(_BARE, shoulders=22429, feet=843, hands=1965, tabard=746),
    "tyrande":   dict(_BARE, head=10123, legs=153),
    "thrall":    dict(_BARE, shoulders=32868, feet=43, hands=203),
    "whitemane": dict(_BARE, head=7720, shoulders=7059, tabard=746),
    "paletress": dict(_BARE, head=7520, tabard=746),
    "tirion":    dict(_BARE, shoulders=33699, feet=843, hands=1965, back=29369, tabard=746),
    "renault":   dict(_BARE, shoulders=21683, feet=843, hands=2422, tabard=746),
    "brigitte":  dict(_BARE, shoulders=30979, feet=43, hands=1965, tabard=746),
    "isillien":  dict(_BARE, head=7720, shoulders=10237, legs=153, hands=1965),
}
for _id, _outfit in OUTFITS.items():
    if _id not in COMPANION_IDS:
        sys.exit(f"OUTFITS names {_id!r}, who is not in COMPANIONS")
    for _slot in _outfit:
        if _slot not in SLOTS:
            sys.exit(f"OUTFITS gives {_id} an unknown slot {_slot!r}")


def look(companion):
    outfit = OUTFITS.get(companion, {})
    return " ".join(f"{SLOTS[slot]}:{entry}" for slot, entry in sorted(outfit.items(), key=lambda kv: SLOTS[kv[0]]))


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
        "  `MainHand` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'item entry drawn in her main hand, 0 = her own',",
        "  `OffHand` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'off hand; with MainHand or Ranged set, 0 = empty',",
        "  `Ranged` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'ranged slot; with MainHand or OffHand set, 0 = empty',",
        "  `Look` VARCHAR(255) NOT NULL DEFAULT '' COMMENT 'equipment slot:item entry pairs drawn in place of her armour, 0 = empty',",
        "  `Enabled` TINYINT UNSIGNED NOT NULL DEFAULT 1,",
        "  PRIMARY KEY (`Id`)",
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;",
        "",
        "INSERT INTO `lore_companion` (`Id`, `Name`, `Title`, `Race`, `Class`, `Gender`, `DisplayId`, `Scale`, `Spec`, `PetEntry`, `SortOrder`, `MainHand`, `OffHand`, `Ranged`, `Look`) VALUES",
        ",\n".join("(" + ", ".join(sql(v) for v in c + WEAPONS.get(c[0], (0, 0, 0)) + (look(c[0]),)) + ")" for c in COMPANIONS) + ";",
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
