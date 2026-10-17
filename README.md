# mod-lorecompanions

Lore characters as party companions: Lady Jaina Proudmoore, Lady Sylvanas
Windrunner, Alexstrasza the Life-Binder, Princess Moira Bronzebeard, Lady
Liadrin, Tyrande Whisperwind, Prince Arthas Menethil, Thrall, Illidan
Stormrage, High Inquisitor Whitemane, Argent Confessor Paletress, Highlord
Tirion Fordring, Scarlet Commander Renault Mograine, High General Brigitte
Abbendis and Grand Inquisitor Isillien join your party as
[mod-playerbots](https://github.com/liyunfan1223/mod-playerbots) bots, wear
their own in-game models, stay at your party leader's level, and speak in
their own voices.

    .companion list               who there is, and who is with whom
    .companion summon jaina       she joins your party
    .companion dismiss [name]     one of them, or all of yours
    .companion say jaina idle     (GM) make her speak a line for a trigger now
    .companion reload             (admin) reload lore_companion_text

| | spec | model | as a character |
| --- | --- | --- | --- |
| Jaina | frost mage | 30863, Theramore | human |
| Sylvanas | marksman hunter, with a bat | 28213, the Banshee Queen | Forsaken |
| Alexstrasza | fire mage | 28227, her elf form, at 0.68 scale | blood elf |
| Moira | shadow priest | 8705, the princess in Blackrock Depths | dwarf |
| Liadrin | protection paladin, a tank | 16820, the Blood Knight Matriarch | blood elf |
| Tyrande | holy priest, a healer | 7274, the High Priestess in Darnassus | night elf |
| Arthas | retribution paladin | 24949, the prince at the Culling of Stratholme | human |
| Thrall | enhancement shaman | 4527, the Warchief in Orgrimmar, at 0.9 scale | orc |
| Illidan | combat rogue | 21135, the Betrayer of the Black Temple, at 0.35 scale | night elf |
| Whitemane | discipline priest | 2043, the High Inquisitor in the Scarlet Monastery, at 0.85 scale | human |
| Paletress | holy priest | 29083, the Argent Confessor at the tournament | human |
| Tirion | retribution paladin | 22209, the Highlord of the Argent Crusade, at 0.9 scale | human |
| Renault | protection paladin | 2042, the Scarlet Commander in the Monastery, at 0.9 scale | human |
| Brigitte | holy paladin | 24413, the High General of the Scarlet Onslaught, at 0.9 scale | human |
| Isillien | shadow priest | 10339, the Grand Inquisitor of Hearthglen | human |

The companions speak from late in Wrath of the Lich King: after the Argent
Tournament and the Trial of the Crusader, with the assault on Icecrown
Citadel about to begin. The Lich King still sits on the Frozen Throne.

Four of them come from earlier. Arthas is the prince before his fall,
Illidan is the Illidan of the Black Temple before it fell, Isillien is the
Grand Inquisitor before the day he struck down Taelan Fordring, and Renault
is the Scarlet Commander before his brother brought the corrupted Ashbringer
to the Monastery. All four were brought out of their own time by the bronze
dragonflight, and all four have been told how their stories end. Brigitte
Abbendis, who dies at New Hearthglen in the game's own story, is kept alive
here on purpose. Their lines and everyone else's are written that way; the
[lore reference](wiki/lore/README.md) has the details.

## How it works

**Built-in characters.** At startup the module makes sure an account
(`LoreCompanions.AccountName`, `LORECOMPANIONS` by default) exists with one
character per row of `lore_companion`, creating whatever is missing. The
password is random and never shown. "Sylvanas" and "Alexstrasza" are in the
client's reserved-name list, and the core refuses to log in a character with a
reserved name, so the account is given one RBAC permission, 17 ("skip
character creation reserved name check"), and nothing else. The same goes
for any other lore name on that list. The characters are not random bots, so
nothing logs them in except a summon: they never roam the world by
themselves.

**Summoning.** `.companion summon` logs the character in as an alt bot whose
master is the summoner. playerbots only lets a player command characters
from their own account, their guild or a linked account, so the summon first
links the companion account to the summoner's in `playerbots_account_links`.
playerbots then invites her into the summoner's party, and from there she
follows, fights and loots like any alt bot. A bot logs in wherever it last
logged out, so she is first saved at the summoner's position. Inside an
instance she arrives at her old spot instead and is teleported over once
she is in the world.

**Party only.** A companion who is no longer in her summoner's party (she was
kicked, or the summoner left) is sent home after `JoinGraceSeconds`, and a
dismissed one leaves the party as she goes. A summon is refused when the party
is full; she never turns a party into a raid. Each companion can be with one
player at a time.

**Logging out.** Companions log out with their summoner but keep their seats
in the party, and when the summoner logs back in, each one still in the party
is summoned again into her seat, even across a server restart. One who was
summoned by someone else in the meantime has left the party for theirs.

**Model.** The character's *native* display id is replaced with the lore
creature's on every login, before she is visible. Native rather than current
means anything that restores a player's appearance, such as a polymorph
ending, restores the lore model. Not everything comes from the model,
though: a player's weapons, and the shape of her armour, are drawn from her
visible item slots. So the module writes those slots too, in place of what
she has equipped:

- **Weapons:** the lore NPC's own (`MainHand`, `OffHand` and `Ranged` in
  `lore_companion`, from `WEAPONS` in the generator). Thrall carries the
  Doomhammer and Illidan his warglaives. Alexstrasza, Moira and Liadrin carry
  nothing as NPCs, so they show their own.
- **Armour shape** (`Look`, from `OUTFITS`): a humanoid model's outfit gives
  every piece its colours, but the helmet, shoulder pads, cloak and the hang
  of a tabard or robe come from the item in the slot. Each slot shows an
  item shaped like the NPC's piece, or nothing where the NPC wears nothing,
  so Tirion has no helmet and Whitemane's robe has its front flap. Creature
  models (Jaina, Sylvanas, Alexstrasza, Arthas, Illidan) don't use it.

Only the picture changes; the real gear, and its stats, is what she fights
with.

**Level.** A companion is always at her party leader's level, or her
summoner's if the leader can't be found. On joining, and whenever that level
changes (the leader levels up, or the lead passes to someone else), she is
rebuilt with playerbots' factory: her spells, her fixed spec (the
`AiPlayerbot.PremadeSpecName` named in `lore_companion.Spec`), gear, glyphs,
reagents and ammo. Sylvanas's pet is always a bat, the Vampiric Duskbat of
Tirisfal (`lore_companion.PetEntry`), instead of whatever playerbots would
pick; the same bat is kept from one level to the next. It is like playerbots' `init=auto`,
but it leaves out the random spec, the cleared quest log, guilds and arena
teams. Her own XP gain is switched off. Moving down a level clears her
spells first, so she can't keep abilities above it.

**Side.** With `FollowSummonerSide` on, the summoner's side is written into
the companion's [mod-factionchoice](https://github.com/tpyle/mod-factionchoice)
setting before she logs in. mod-factionchoice then applies the team, faction
and reputation rebase on login. That is why Sylvanas can walk through
Stormwind behind an Alliance character without the guards attacking her.

**Voice.** Lines come from `lore_companion_text`, written per character. Any
line from mod-botlore would be wrong in their mouths, so the module marks
each companion (`CustomData["LoreCompanion"]`) and mod-botlore leaves marked
bots alone. Lines are spoken in the universal language, because the parties
are usually mixed-faction.

| trigger | when | chance |
| --- | --- | --- |
| `summon`, `dismiss` | joining and leaving | always |
| `grow_stronger` | rebuilt at a higher level | always |
| `zone_enter` | entering a zone or area | `Chance.ZoneEnter` |
| `quest_accept`, `quest_complete` | the summoner's quests | `Chance.Quest*` |
| `combat_start`, `kill` | her fights; any kill by her or her summoner | `Chance.Combat*`, `Chance.Kill*` |
| `death`, `revived` | her own | `Chance.Death`, `Chance.Revived` |
| `ally_down` | the summoner dies | `Chance.AllyDown` |
| `idle` | out of combat, every `IdleMin`-`IdleMaxSeconds` | `Chance.Idle` |

`CooldownSeconds` paces each companion. `GroupGapSeconds` keeps a party's
companions from talking over one another. A line may carry a reply from
another companion, delivered `ReplyDelayMs` later; that is how the companions
talk to each other.

## The lines

The lines are written in `data/lines/<companion>.txt`, and
`tools/gen_lines.py` turns them, along with the companion table at the top
of the script, into `data/sql/db-world/base/lore_companion.sql`. The core's
updater applies that file at startup whenever its hash changes. To publish
lines without a restart, apply the file by hand and run `.companion reload`.

    @ trigger=zone_enter where=undercity
    Undercity. Terenas's crypts, now my halls. I find the irony sustains me.

    @ trigger=idle with=jaina
    Is it true you walked away from him at Stratholme, Proudmoore?
    > jaina: Yes. And I've regretted it, and been glad of it, every day since.

Filters: `where=` (named places, matched against zone and area), `foe=`
(creature names), `ctype=` (creature type), `rank=normal|notable`,
`side=alliance|horde` (the side she is fighting for), `with=` (another
companion in the party), `channel=say|emote|party|yell` and `weight=`. A line
that matches on place, foe, type, side or company outweighs a generic one but
doesn't silence it.

Placeholders: `%summoner %leader %target %quest %other %zone %area %name`.
There is deliberately no `%level`, and the generator rejects any line that
names a level number. A companion can notice she is stronger, but she
doesn't recite a statistic.

## Adding a companion

Add a row to `COMPANIONS` in `tools/gen_lines.py` with a free character
name, a race and class pair your realm allows, a creature display id, a
premade spec name for the class and, for a hunter, a pet's creature entry. Then write `data/lines/<id>.txt`, run the
generator and restart. The character is created on the next startup.

## Requirements

- mod-playerbots, which the module links against directly.
- mod-factionchoice for `FollowSummonerSide`. Without it that setting does
  nothing, and a companion keeps her race's side.
- A realm that allows the race and class pairs above (Forsaken hunter, blood
  elf mage, blood elf paladin, dwarf priest, night elf priest, human paladin,
  orc shaman, night elf rogue, human priest).
- `AllowTwoSide.Interaction.Group = 1` if companions are to join parties of
  the other faction.

## Licence

GNU Affero General Public License v3.0, the licence AzerothCore and its
modules use. See [LICENSE](LICENSE).
