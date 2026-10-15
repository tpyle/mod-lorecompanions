# mod-lorecompanions

Lore characters as party companions: Lady Jaina Proudmoore, Lady Sylvanas
Windrunner and Alexstrasza the Life-Binder join your party as
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
| Alexstrasza | fire mage | 28227, her elf form | blood elf |

## How it works

**Built-in characters.** At startup the module makes sure an account
(`LoreCompanions.AccountName`, `LORECOMPANIONS` by default) exists with one
character per row of `lore_companion`, creating whatever is missing. The
password is random and never shown. "Sylvanas" and "Alexstrasza" are in the
client's reserved-name list, and the core refuses to log in a character with a
reserved name, so the account is given one RBAC permission, 17 ("skip
character creation reserved name check"), and nothing else. The characters are not random bots, so
nothing logs them in except a summon: they never roam the world by
themselves.

**Summoning.** `.companion summon` logs the character in as an alt bot whose
master is the summoner. playerbots only lets a player command characters
from their own account, their guild or a linked account, so the summon first
links the companion account to the summoner's in `playerbots_account_links`.
playerbots then invites her into the summoner's party, and from there she
follows, fights and loots like any alt bot.

**Party only.** A companion who is no longer in her summoner's party (she was
kicked, or the summoner left) is sent home after `JoinGraceSeconds`, and so is
one whose summoner logs out. A summon is refused when the party is full; she
never turns a party into a raid. Each companion can be with one player at a
time.

**Model.** The character's *native* display id is replaced with the lore
creature's on every login, before she is visible. Native rather than current
means anything that restores a player's appearance, such as a polymorph
ending, restores the lore model. The model draws its own clothes and
weapons, so her equipped gear doesn't show.

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
another companion, delivered `ReplyDelayMs` later; that is how Jaina and
Sylvanas talk to each other.

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
  elf mage).
- `AllowTwoSide.Interaction.Group = 1` if companions are to join parties of
  the other faction.

## Licence

GNU Affero General Public License v3.0, the licence AzerothCore and its
modules use. See [LICENSE](LICENSE).
