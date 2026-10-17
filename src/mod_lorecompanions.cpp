/*
 * mod-lorecompanions - lore characters as party companions
 *
 * Jaina, Sylvanas, Alexstrasza, Moira, Liadrin, Tyrande, Arthas, Thrall,
 * Illidan, Whitemane and Paletress (and whoever else is added to the
 * `lore_companion` table) are real characters on a hidden account that this
 * module owns. ".companion summon jaina" logs one in as a mod-playerbots alt
 * bot whose master is the summoner, so she joins the summoner's party and
 * fights, follows and loots the way any alt bot does. Nothing else ever logs
 * them in: they are not random bots, so they never wander the world on their
 * own, and a companion found outside her summoner's party is sent home.
 *
 * Three things make a companion more than an ordinary alt bot:
 *
 *   model  - the character's native display id is replaced with the lore
 *            creature's model (Jaina 30863, Sylvanas 28213, Alexstrasza
 *            28227). Native, not just current, so that anything that restores
 *            a player's display - a polymorph ending, an illusion fading -
 *            restores the lore model rather than the race's.
 *
 *   level  - a companion is always the party leader's level. On joining, and
 *            whenever the leader's level differs (the leader levels, or the
 *            lead passes to someone else), she is rebuilt for that level with
 *            playerbots' own factory: spells, her fixed talent spec, gear,
 *            glyphs, ammo, pet. XP gain is switched off; the leader's level
 *            is the only thing that moves hers.
 *
 *   voice  - her lines come from `lore_companion_text`, written for her and
 *            nobody else, keyed by trigger and filtered by place, foe, creature
 *            type, which side she is fighting for and which other companions
 *            are present. A line can carry a reply from another companion,
 *            which is how two of them talk to each other.
 *
 * Threading: script hooks can fire on map worker threads, so they only push
 * events onto a locked queue. Everything that touches a companion - speaking,
 * levelling, logging out - happens in WorldScript::OnUpdate, on the world
 * thread, while no map is updating.
 */

#include "AccountMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "Containers.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DataMap.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "QuestDef.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "SpellAuraDefines.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Tokenize.h"
#include "World.h"
#include "WorldSession.h"

#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "PlayerbotMgr.h"
#include "PlayerbotRepository.h"
#include "Playerbots.h"
#include "PlayerbotsDatabase.h"
#include "RandomPlayerbotMgr.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    // Trigger names, as they appear in lore_companion_text.Trigger
    constexpr char const* T_SUMMON         = "summon";
    constexpr char const* T_DISMISS        = "dismiss";
    constexpr char const* T_ZONE_ENTER     = "zone_enter";
    constexpr char const* T_QUEST_ACCEPT   = "quest_accept";
    constexpr char const* T_QUEST_COMPLETE = "quest_complete";
    constexpr char const* T_COMBAT_START   = "combat_start";
    constexpr char const* T_KILL           = "kill";
    constexpr char const* T_DEATH          = "death";
    constexpr char const* T_REVIVED        = "revived";
    constexpr char const* T_ALLY_DOWN      = "ally_down";
    constexpr char const* T_GROW_STRONGER  = "grow_stronger";
    constexpr char const* T_IDLE           = "idle";

    enum Channel : uint8
    {
        CHANNEL_SAY   = 0,
        CHANNEL_EMOTE = 1,
        CHANNEL_PARTY = 2,
        CHANNEL_YELL  = 3
    };

    enum RankFilter : int8
    {
        RANK_ANY     = -1,
        RANK_NORMAL  = 0,   // ordinary creatures
        RANK_NOTABLE = 1    // elites, rares and bosses
    };

    struct Config
    {
        bool        Enable             = true;
        std::string AccountName        = "LORECOMPANIONS";
        bool        FollowSummonerSide = true;
        uint32      MaxPerPlayer       = 4;
        uint32      GearQuality        = 0;     // 0 = match the leader's gear score
        uint32      JoinGraceSeconds   = 20;
        uint32      CooldownSeconds    = 45;
        uint32      GroupGapSeconds    = 12;
        uint32      IdleMinSeconds     = 240;
        uint32      IdleMaxSeconds     = 480;
        uint32      ReplyDelayMs       = 3500;
        uint32      QuestRangeYards    = 80;

        uint32 ChanceZoneEnter     = 60;
        uint32 ChanceQuestAccept   = 30;
        uint32 ChanceQuestComplete = 45;
        uint32 ChanceCombat        = 10;
        uint32 ChanceCombatNotable = 60;
        uint32 ChanceKill          = 6;
        uint32 ChanceKillNotable   = 60;
        uint32 ChanceDeath         = 100;
        uint32 ChanceRevived       = 80;
        uint32 ChanceAllyDown      = 80;
        uint32 ChanceIdle          = 100;
    } cfg;

    struct CompanionDef
    {
        std::string id;         // "jaina" - what commands and line files use
        std::string name;       // the character name
        std::string title;      // "Lady Jaina Proudmoore", for system messages
        uint8 race = 0;
        uint8 cls = 0;
        uint8 gender = 0;
        uint32 displayId = 0;
        float scale = 1.0f;
        std::string spec;       // a playerbots premade spec name, "frost pve"
        uint32 petEntry = 0;    // a hunter's pet, by creature entry; 0 lets playerbots choose
        // How she is drawn, slot by slot, in place of her real gear: the lore
        // NPC's weapons, and for her armour an item shaped like the NPC's
        // piece, or nothing. A slot not in lookMask shows what she wears.
        uint32 lookMask = 0;
        std::array<uint32, EQUIPMENT_SLOT_END> look = { };
        int32 specNo = -1;      // resolved against AiPlayerbot.PremadeSpecName
        ObjectGuid guid;        // empty when the character could not be set up
    };

    // Her main hand, off hand and ranged slot, in the order of lore_companion's
    // MainHand, OffHand and Ranged.
    static constexpr uint8 WeaponSlots[3] = { EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND, EQUIPMENT_SLOT_RANGED };

    struct Line
    {
        uint32 id = 0;
        std::string text;
        std::vector<uint32> places;         // zone or area ids
        std::vector<uint32> creatureTypes;
        std::vector<std::string> foes;      // lowercased creature names
        int8 rank = RANK_ANY;
        int8 side = -1;                     // TeamId the companion fights for
        std::string with;                   // another companion who must be present
        std::string replyBy;
        std::string replyText;
        uint8 channel = CHANNEL_SAY;
        uint32 weight = 1;
    };

    // What a line is matched against.
    struct Context
    {
        uint32 zoneId = 0;
        uint32 areaId = 0;
        std::string target;
        std::string targetLower;
        uint32 creatureType = 0;
        int8 rank = RANK_ANY;               // RANK_ANY: no creature involved
        std::string quest;
        std::string other;                  // a player's name, for ally_down
    };

    enum class Stage
    {
        Joining,    // logging in, waiting for playerbots to give her a master
        Ready,
        Leaving     // said goodbye; logged out once leaveAt passes
    };

    struct Active
    {
        size_t def = 0;
        ObjectGuid master;
        Stage stage = Stage::Joining;
        time_t since = 0;
        time_t nextLine = 0;
        time_t idleAt = 0;
        time_t leaveAt = 0;
        bool leaveParty = false;    // taken out of her party as she goes
        std::deque<uint32> recent;
    };

    enum class LoreEventType
    {
        Login,
        Logout,
        MasterLogin,
        Zone,
        QuestAccept,
        QuestComplete,
        Combat,
        Kill,
        Death,
        Revived
    };

    struct LoreEvent
    {
        LoreEventType type;
        ObjectGuid subject;
        Context context;
        time_t at = GameTime::GetGameTime().count();
    };

    struct PendingReply
    {
        ObjectGuid speaker;
        std::string text;
        uint8 channel = CHANNEL_SAY;
        uint64 dueMs = 0;
    };

    std::vector<CompanionDef> defs;
    std::unordered_map<ObjectGuid, size_t> defByGuid;   // written at startup only
    uint32 companionAccountId = 0;

    // "companion/trigger" -> lines
    std::unordered_map<std::string, std::vector<Line>> lines;

    // World thread only.
    std::unordered_map<ObjectGuid, Active> active;
    std::unordered_map<ObjectGuid, time_t> groupLastLine;   // keyed by master
    std::vector<PendingReply> replies;
    uint32 maintainTimer = 0;

    std::mutex eventLock;
    std::vector<LoreEvent> events;

    std::string Lower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
        return text;
    }

    std::vector<std::string> Split(std::string const& text, char separator)
    {
        std::vector<std::string> out;
        std::string token;
        std::istringstream stream(text);
        while (std::getline(stream, token, separator))
        {
            token.erase(0, token.find_first_not_of(" \t"));
            token.erase(token.find_last_not_of(" \t") + 1);
            if (!token.empty())
                out.push_back(token);
        }
        return out;
    }

    std::vector<uint32> SplitIds(std::string const& text)
    {
        std::vector<uint32> out;
        for (std::string const& token : Split(text, ','))
            out.push_back(uint32(std::strtoul(token.c_str(), nullptr, 10)));
        return out;
    }

    void LoadConfig()
    {
        cfg.Enable             = sConfigMgr->GetOption<bool>("LoreCompanions.Enable", true);
        cfg.AccountName        = sConfigMgr->GetOption<std::string>("LoreCompanions.AccountName", "LORECOMPANIONS");
        cfg.FollowSummonerSide = sConfigMgr->GetOption<bool>("LoreCompanions.FollowSummonerSide", true);
        cfg.MaxPerPlayer       = sConfigMgr->GetOption<uint32>("LoreCompanions.MaxPerPlayer", 4);
        cfg.GearQuality        = sConfigMgr->GetOption<uint32>("LoreCompanions.GearQuality", 0);
        cfg.JoinGraceSeconds   = sConfigMgr->GetOption<uint32>("LoreCompanions.JoinGraceSeconds", 20);
        cfg.CooldownSeconds    = sConfigMgr->GetOption<uint32>("LoreCompanions.CooldownSeconds", 45);
        cfg.GroupGapSeconds    = sConfigMgr->GetOption<uint32>("LoreCompanions.GroupGapSeconds", 12);
        cfg.IdleMinSeconds     = sConfigMgr->GetOption<uint32>("LoreCompanions.IdleMinSeconds", 240);
        cfg.IdleMaxSeconds     = std::max(cfg.IdleMinSeconds, sConfigMgr->GetOption<uint32>("LoreCompanions.IdleMaxSeconds", 480));
        cfg.ReplyDelayMs       = sConfigMgr->GetOption<uint32>("LoreCompanions.ReplyDelayMs", 3500);
        cfg.QuestRangeYards    = sConfigMgr->GetOption<uint32>("LoreCompanions.QuestRangeYards", 80);

        cfg.ChanceZoneEnter     = sConfigMgr->GetOption<uint32>("LoreCompanions.Chance.ZoneEnter", 60);
        cfg.ChanceQuestAccept   = sConfigMgr->GetOption<uint32>("LoreCompanions.Chance.QuestAccept", 30);
        cfg.ChanceQuestComplete = sConfigMgr->GetOption<uint32>("LoreCompanions.Chance.QuestComplete", 45);
        cfg.ChanceCombat        = sConfigMgr->GetOption<uint32>("LoreCompanions.Chance.Combat", 10);
        cfg.ChanceCombatNotable = sConfigMgr->GetOption<uint32>("LoreCompanions.Chance.CombatNotable", 60);
        cfg.ChanceKill          = sConfigMgr->GetOption<uint32>("LoreCompanions.Chance.Kill", 6);
        cfg.ChanceKillNotable   = sConfigMgr->GetOption<uint32>("LoreCompanions.Chance.KillNotable", 60);
        cfg.ChanceDeath         = sConfigMgr->GetOption<uint32>("LoreCompanions.Chance.Death", 100);
        cfg.ChanceRevived       = sConfigMgr->GetOption<uint32>("LoreCompanions.Chance.Revived", 80);
        cfg.ChanceAllyDown      = sConfigMgr->GetOption<uint32>("LoreCompanions.Chance.AllyDown", 80);
        cfg.ChanceIdle          = sConfigMgr->GetOption<uint32>("LoreCompanions.Chance.Idle", 100);
    }

    // ---------------------------------------------------------------------
    // Data
    // ---------------------------------------------------------------------

    void LoadDefs()
    {
        defs.clear();

        QueryResult result = WorldDatabase.Query(
            "SELECT Id, Name, Title, Race, Class, Gender, DisplayId, Scale, Spec, PetEntry, "
            "MainHand, OffHand, Ranged, Look "
            "FROM lore_companion WHERE Enabled = 1 ORDER BY SortOrder, Id");
        if (!result)
        {
            LOG_ERROR("module", "mod-lorecompanions: lore_companion is empty or missing");
            return;
        }

        do
        {
            Field* fields = result->Fetch();
            CompanionDef def;
            def.id        = Lower(fields[0].Get<std::string>());
            def.name      = fields[1].Get<std::string>();
            def.title     = fields[2].Get<std::string>();
            def.race      = fields[3].Get<uint8>();
            def.cls       = fields[4].Get<uint8>();
            def.gender    = fields[5].Get<uint8>();
            def.displayId = fields[6].Get<uint32>();
            def.scale     = fields[7].Get<float>();
            def.spec      = Lower(fields[8].Get<std::string>());
            def.petEntry  = fields[9].Get<uint32>();
            // Any lore weapon claims all three hands, so a 0 beside it is an
            // empty hand rather than her own weapon.
            uint32 const weapons[3] = { fields[10].Get<uint32>(), fields[11].Get<uint32>(), fields[12].Get<uint32>() };
            if (weapons[0] || weapons[1] || weapons[2])
            {
                for (uint8 i = 0; i < 3; ++i)
                {
                    def.lookMask |= 1u << WeaponSlots[i];
                    def.look[WeaponSlots[i]] = weapons[i];
                }
            }
            // "slot:entry slot:entry ...", by equipment slot; entry 0 is empty.
            std::string const look = fields[13].Get<std::string>();
            for (std::string_view pair : Acore::Tokenize(look, ' ', false))
            {
                std::vector<std::string_view> parts = Acore::Tokenize(pair, ':', false);
                Optional<uint32> slot = parts.size() == 2 ? Acore::StringTo<uint32>(parts[0]) : std::nullopt;
                Optional<uint32> entry = parts.size() == 2 ? Acore::StringTo<uint32>(parts[1]) : std::nullopt;
                if (!slot || !entry || *slot >= EQUIPMENT_SLOT_END)
                {
                    LOG_ERROR("module", "mod-lorecompanions: {} has a bad Look entry '{}'", def.id, pair);
                    continue;
                }
                def.lookMask |= 1u << *slot;
                def.look[*slot] = *entry;
            }
            defs.push_back(std::move(def));
        } while (result->NextRow());
    }

    uint32 LoadLines()
    {
        lines.clear();

        QueryResult result = WorldDatabase.Query(
            "SELECT Id, Companion, `Trigger`, Text, Places, CreatureTypes, Foes, `Rank`, Side, `With`, "
            "ReplyBy, ReplyText, Channel, Weight FROM lore_companion_text");
        if (!result)
        {
            LOG_ERROR("module", "mod-lorecompanions: lore_companion_text is empty or missing");
            return 0;
        }

        uint32 count = 0;
        do
        {
            Field* fields = result->Fetch();
            Line line;
            line.id            = fields[0].Get<uint32>();
            std::string const companion = Lower(fields[1].Get<std::string>());
            std::string const trigger   = fields[2].Get<std::string>();
            line.text          = fields[3].Get<std::string>();
            line.places        = SplitIds(fields[4].Get<std::string>());
            line.creatureTypes = SplitIds(fields[5].Get<std::string>());
            for (std::string const& foe : Split(fields[6].Get<std::string>(), '|'))
                line.foes.push_back(Lower(foe));
            line.rank          = fields[7].Get<int8>();
            line.side          = fields[8].Get<int8>();
            line.with          = Lower(fields[9].Get<std::string>());
            line.replyBy       = Lower(fields[10].Get<std::string>());
            line.replyText     = fields[11].Get<std::string>();
            line.channel       = fields[12].Get<uint8>();
            line.weight        = std::max<uint32>(fields[13].Get<uint32>(), 1);

            lines[companion + "/" + trigger].push_back(std::move(line));
            ++count;
        } while (result->NextRow());

        LOG_INFO("module", "mod-lorecompanions: loaded {} line(s)", count);
        return count;
    }

    CompanionDef const* FindDef(std::string const& key)
    {
        std::string const wanted = Lower(key);
        for (CompanionDef const& def : defs)
            if (def.id == wanted || Lower(def.name) == wanted)
                return &def;
        return nullptr;
    }

    size_t IndexOf(CompanionDef const* def)
    {
        return size_t(def - defs.data());
    }

    bool IsCompanion(ObjectGuid guid)
    {
        return defByGuid.count(guid) != 0;
    }

    bool IsHumanPlayer(Player* player)
    {
        return player && player->GetSession() && !player->GetSession()->IsHeadless();
    }

    // ---------------------------------------------------------------------
    // The hidden account and its characters
    // ---------------------------------------------------------------------

    std::string RandomPassword()
    {
        static char const alphabet[] = "abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        std::string out;
        for (int i = 0; i < 16; ++i)
            out += alphabet[urand(0, sizeof(alphabet) - 2)];
        return out;
    }

    // Lore names such as "Sylvanas" and "Alexstrasza" are in the client's
    // NamesReserved list ("Jaina" is not). Player::LoadFromDB checks the name on every login,
    // flags a reserved one for a forced rename and refuses the login, unless
    // the session may skip that check - so the companion account is given
    // exactly that permission, and nothing else.
    void AllowReservedNames()
    {
        LoginDatabase.DirectExecute(
            "INSERT IGNORE INTO rbac_account_permissions (accountId, permissionId, granted, realmId) "
            "VALUES ({}, {}, 1, -1)", companionAccountId, uint32(rbac::RBAC_PERM_SKIP_CHECK_CHARACTER_CREATION_RESERVEDNAME));
    }

    bool EnsureAccount()
    {
        companionAccountId = AccountMgr::GetId(cfg.AccountName);
        if (companionAccountId)
        {
            AllowReservedNames();
            return true;
        }

        // Nobody is meant to log into this account, so its password is thrown
        // away. A GM who wants in can set one with ".account set password".
        AccountOpResult const result = sAccountMgr->CreateAccount(cfg.AccountName, RandomPassword());
        if (result != AOR_OK)
        {
            LOG_ERROR("module", "mod-lorecompanions: could not create account {} (error {})",
                cfg.AccountName, uint32(result));
            return false;
        }

        // CreateAccount queues its insert; wait for it to land.
        for (int i = 0; i < 100 && !companionAccountId; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            companionAccountId = AccountMgr::GetId(cfg.AccountName);
        }

        if (!companionAccountId)
        {
            LOG_ERROR("module", "mod-lorecompanions: account {} was created but never appeared", cfg.AccountName);
            return false;
        }

        LOG_INFO("module", "mod-lorecompanions: created account {} ({})", cfg.AccountName, companionAccountId);
        AllowReservedNames();
        return true;
    }

    ObjectGuid CreateCharacter(CompanionDef const& def)
    {
        WorldSession* session = new WorldSession(companionAccountId, "", 0, nullptr, SEC_PLAYER,
            EXPANSION_WRATH_OF_THE_LICH_KING, time_t(0), sWorld->GetDefaultDbcLocale(), 0, false, false, 0);

        auto info = std::make_unique<CharacterCreateInfo>(def.name, def.race, def.cls, def.gender, 0, 0, 0, 0, 0);

        Player* player = new Player(session);
        player->GetMotionMaster()->Initialize();

        ObjectGuid guid;
        if (player->Create(sObjectMgr->GetGenerator<HighGuid::Player>().Generate(), info.get()))
        {
            player->setCinematic(2);
            player->SetAtLoginFlag(AT_LOGIN_NONE);
            player->SaveToDB(true, false);
            sCharacterCache->AddCharacterCacheEntry(player->GetGUID(), companionAccountId, player->GetName(),
                player->getGender(), player->getRace(), player->getClass(), player->GetLevel());
            guid = player->GetGUID();
        }
        else
            LOG_ERROR("module", "mod-lorecompanions: could not create {} (race {}, class {})",
                def.name, def.race, def.cls);

        player->CleanupsBeforeDelete();
        delete player;
        delete session;
        return guid;
    }

    void EnsureCharacters()
    {
        defByGuid.clear();
        bool created = false;

        for (size_t i = 0; i < defs.size(); ++i)
        {
            CompanionDef& def = defs[i];
            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(def.name);

            if (guid && sCharacterCache->GetCharacterAccountIdByGuid(guid) != companionAccountId)
            {
                LOG_ERROR("module", "mod-lorecompanions: the name {} belongs to a character on another account, "
                    "so {} is unavailable. Rename that character, or give the companion another name.",
                    def.name, def.title);
                continue;
            }

            if (!guid)
            {
                guid = CreateCharacter(def);
                if (!guid)
                    continue;

                LOG_INFO("module", "mod-lorecompanions: created {} ({})", def.title, def.name);
                created = true;
            }

            // A login refused before the permission existed left the rename
            // flag behind, and it would refuse every login after.
            CharacterDatabase.DirectExecute("UPDATE characters SET at_login = at_login & ~{} WHERE guid = {}",
                uint32(AT_LOGIN_RENAME), guid.GetCounter());

            def.guid = guid;
            defByGuid[guid] = i;
        }

        // The characters are only logged in when summoned, but a summon in the
        // first seconds after startup must not race the inserts.
        if (created)
            for (int i = 0; i < 100 && CharacterDatabase.QueueSize(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    int32 ResolveSpec(CompanionDef& def)
    {
        if (def.specNo >= 0)
            return def.specNo;

        for (int32 i = 0; i < MAX_SPECNO; ++i)
        {
            if (Lower(sPlayerbotAIConfig.premadeSpecName[def.cls][i]) == def.spec)
            {
                def.specNo = i;
                return i;
            }
        }

        LOG_ERROR("module", "mod-lorecompanions: no premade spec named '{}' for class {}; using the first one",
            def.spec, def.cls);
        def.specNo = 0;
        return 0;
    }

    // ---------------------------------------------------------------------
    // Model, side and level
    // ---------------------------------------------------------------------

    // Scale multiplies the model's own size from CreatureDisplayInfo:
    // Alexstrasza's is 1.75, Jaina's and Sylvanas's 1.2, a player race's 1.0.
    void ApplyModel(Player* bot, CompanionDef const& def)
    {
        bot->SetNativeDisplayId(def.displayId);

        // A transform aura (a polymorph, say) owns the current display while it
        // lasts; when it ends the core restores the native one, which is now
        // hers. SetDisplayId also sets the object scale, so they go together.
        if (!bot->HasAuraType(SPELL_AURA_TRANSFORM))
            bot->SetDisplayId(def.displayId, def.scale > 0.0f ? def.scale : 1.0f);
    }

    // The model does not settle everything she is drawn wearing. Its outfit
    // supplies her armour's colours, but her weapons, and the shape of her
    // armour - helmet, shoulder pads, cloak, the hang of a tabard or a robe -
    // are drawn from her visible item slots, so Tirion would wear whatever
    // helmet playerbots gave him. Those slots are written here in place of
    // her real gear - the picture only; the items she fights with, and their
    // stats, are untouched. Every redraw of a slot (equipping, a rebuild, mod-transmog)
    // goes through OnPlayerAfterSetVisibleItemSlot, which calls this again.

    void ApplyLookSlot(Player* bot, CompanionDef const& def, uint8 slot)
    {
        if (slot >= EQUIPMENT_SLOT_END || !(def.lookMask & (1u << slot)))
            return;
        if (bot->GetUInt32Value(PLAYER_VISIBLE_ITEM_1_ENTRYID + slot * 2) != def.look[slot])
        {
            bot->SetUInt32Value(PLAYER_VISIBLE_ITEM_1_ENTRYID + slot * 2, def.look[slot]);
            bot->SetUInt32Value(PLAYER_VISIBLE_ITEM_1_ENCHANTMENT + slot * 2, 0);
        }
    }

    void ApplyLook(Player* bot, CompanionDef const& def)
    {
        for (uint8 slot = 0; slot < EQUIPMENT_SLOT_END; ++slot)
            ApplyLookSlot(bot, def, slot);
    }

    bool LookDrifted(Player* bot, CompanionDef const& def)
    {
        for (uint8 slot = 0; slot < EQUIPMENT_SLOT_END; ++slot)
            if ((def.lookMask & (1u << slot)) && bot->GetUInt32Value(PLAYER_VISIBLE_ITEM_1_ENTRYID + slot * 2) != def.look[slot])
                return true;
        return false;
    }

    // Whether something has put her back in the racial model or reset her
    // size. Both happen routinely: RestoreDisplayId after a polymorph sets
    // scale 1, and so does InitStatsForLevel on every level change.
    bool ModelDrifted(Player* bot, CompanionDef const& def)
    {
        if (bot->GetNativeDisplayId() != def.displayId)
            return true;

        if (bot->HasAuraType(SPELL_AURA_TRANSFORM))
            return false;

        if (bot->GetDisplayId() != def.displayId)
            return true;

        float const scale = def.scale > 0.0f ? def.scale : 1.0f;
        return !bot->HasAuraType(SPELL_AURA_MOD_SCALE) && std::fabs(bot->GetObjectScale() - scale) > 0.001f;
    }

    // mod-factionchoice keeps a character's side in its player settings and
    // applies it - team, faction template and the reputation rebase that makes
    // NPCs friendly - on login. Writing the summoner's side there before the
    // companion logs in is what lets Sylvanas walk through Stormwind behind an
    // Alliance character without the guards cutting her down. The first value
    // is the side; the rest belongs to mod-factionchoice and is kept as it is.
    void PrepareSide(ObjectGuid guid, TeamId team)
    {
        std::vector<std::string> values;
        if (QueryResult result = CharacterDatabase.Query(
                "SELECT data FROM character_settings WHERE guid = {} AND source = 'mod-factionchoice'", guid.GetCounter()))
            values = Split(result->Fetch()[0].Get<std::string>(), ' ');

        if (values.empty())
            values.push_back("0");

        values[0] = team == TEAM_ALLIANCE ? "1" : "2";

        std::string data;
        for (std::string const& value : values)
            data += value + " ";

        CharacterDatabase.DirectExecute(
            "REPLACE INTO character_settings (guid, source, data) VALUES ({}, 'mod-factionchoice', '{}')",
            guid.GetCounter(), data);
    }

    // A bot logs in wherever it last logged out, which for a companion is
    // wherever her previous summoner left her. On an open-world map she is
    // simply saved at the summoner's feet before the login reads her back; an
    // instance needs a bind the login cannot assume, so there she arrives at
    // her old spot and is teleported over once she is in the world.
    void PlaceAt(ObjectGuid guid, Player* summoner)
    {
        MapEntry const* map = sMapStore.LookupEntry(summoner->GetMapId());
        if (!map || map->Instanceable())
            return;

        CharacterDatabase.DirectExecute(
            "UPDATE characters SET map = {}, instance_id = 0, position_x = {}, position_y = {}, position_z = {}, "
            "orientation = {}, zone = {} WHERE guid = {}",
            summoner->GetMapId(), summoner->GetPositionX(), summoner->GetPositionY(), summoner->GetPositionZ(),
            summoner->GetOrientation(), summoner->GetZoneId(), guid.GetCounter());
    }

    // True once she is standing with her summoner; until then, sends her there.
    bool BringTo(Player* bot, Player* summoner)
    {
        if (bot->IsBeingTeleported())
            return false;

        if (bot->GetMapId() == summoner->GetMapId() && bot->GetInstanceId() == summoner->GetInstanceId() &&
            bot->IsWithinDistInMap(summoner, 40.0f))
            return true;

        bot->TeleportTo(summoner->GetMapId(), summoner->GetPositionX(), summoner->GetPositionY(),
            summoner->GetPositionZ(), summoner->GetOrientation());
        return false;
    }

    // The level a companion should be: her party leader's, or her summoner's
    // when the leader cannot be seen (offline, or not a player at all).
    uint8 TargetLevel(Player* bot, Player* master)
    {
        if (Group* group = bot->GetGroup())
            if (Player* leader = ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID()))
                return leader->GetLevel();

        return master ? master->GetLevel() : bot->GetLevel();
    }

    // Gives a hunter companion her own pet - Sylvanas's bat - in place of
    // whatever playerbots would pick at random. A pet that is already the
    // right creature is kept, so its name and talents survive a level change;
    // anything else is put away the way playerbots replaces a bot's pet.
    void EnsurePet(Player* bot, CompanionDef const& def)
    {
        if (!def.petEntry || bot->getClass() != CLASS_HUNTER || bot->GetLevel() < 10)
            return;

        uint32 current = 0;
        if (Pet* pet = bot->GetPet())
            current = pet->GetEntry();
        else if (bot->GetPetStable() && bot->GetPetStable()->CurrentPet)
            current = bot->GetPetStable()->CurrentPet->CreatureId;

        if (current == def.petEntry)
            return;

        if (current)
        {
            bot->RemovePet(nullptr, PET_SAVE_AS_CURRENT);
            bot->RemovePet(nullptr, PET_SAVE_NOT_IN_SLOT);
        }

        if (bot->GetPetStable())
            bot->GetPetStable()->UnslottedPets.clear();

        Pet* pet = bot->CreateTamedPetFrom(def.petEntry, 0);
        if (!pet)
        {
            LOG_ERROR("module", "mod-lorecompanions: could not give {} her pet (creature {})", def.name, def.petEntry);
            return;
        }

        pet->SetUInt32Value(UNIT_FIELD_LEVEL, bot->GetLevel() - 1);
        pet->GetMap()->AddToMap(pet->ToCreature());
        pet->SetUInt32Value(UNIT_FIELD_LEVEL, bot->GetLevel());
        bot->SetMinion(pet, true);
        pet->InitTalentForLevel();
        pet->SavePetToDB(PET_SAVE_AS_CURRENT);
        bot->PetSpellInitialize();
    }

    // Rebuilds the companion for a level: what playerbots' "init" does, minus
    // the parts that would be wrong for her - a random spec, a cleared quest
    // log, random-bot guilds and arena teams, a pocketful of random gold.
    void Rebuild(Player* bot, CompanionDef& def, uint8 level, Player* gearSource)
    {
        uint8 const oldLevel = bot->GetLevel();

        uint32 quality = cfg.GearQuality;
        uint32 gearScore = 0;
        if (!quality)
        {
            // As "init=auto": no better geared than the people she travels with.
            quality = ITEM_QUALITY_LEGENDARY;
            gearScore = gearSource ? uint32(PlayerbotAI::GetMixedGearScore(gearSource, true, false, 12) *
                                            sPlayerbotAIConfig.autoInitEquipLevelLimitRatio) : 0;
            if (!gearScore)
                gearScore = 1;
        }

        if (bot->isDead())
            bot->ResurrectPlayer(1.0f, false);

        // Spells above the new level have to go when she comes down; the
        // relearn below gives back everything she is entitled to.
        if (level < oldLevel)
            bot->resetSpells();

        bot->GiveLevel(level);
        bot->InitStatsForLevel(true);
        bot->SetUInt32Value(PLAYER_XP, 0);
        bot->SetPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN);

        PlayerbotFactory factory(bot, level, quality, gearScore);
        bot->LearnDefaultSkills();
        factory.InitSkills();
        factory.InitClassSpells();
        factory.InitAvailableSpells();

        int32 const specNo = ResolveSpec(def);
        PlayerbotFactory::InitTalentsBySpecNo(bot, specNo, true);
        sRandomPlayerbotMgr.SetValue(bot->GetGUID().GetCounter(), "specNo", specNo + 1);
        factory.InitAvailableSpells();
        factory.InitSpecialSpells();

        factory.InitEquipment(false, true);
        factory.InitBags(false);
        factory.InitAmmo();
        factory.InitFood();
        factory.InitPotions();
        factory.InitReagents();
        if (level >= sPlayerbotAIConfig.minEnchantingBotLevel)
            factory.ApplyEnchantAndGemsNew();
        factory.InitGlyphs();
        factory.InitMounts();

        if (bot->getClass() == CLASS_HUNTER && level >= 10)
        {
            // Hers first; InitPet then only levels it and sets its autocasts.
            EnsurePet(bot, def);
            factory.InitPet();
            factory.InitPetTalents();
        }

        // Combat strategies are chosen from the talent tree, which just changed.
        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
        {
            PlayerbotRepository::instance().Reset(botAI);
            botAI->ResetStrategies(false);
        }

        // Enough to pay for flights and repairs alongside her summoner -
        // without it she cannot follow a flight path at all. Topped up, never
        // taken away: what she has looted is hers.
        uint32 const purse = uint32(level) * 2 * GOLD;
        if (bot->GetMoney() < purse)
            bot->SetMoney(purse);

        bot->SetFullHealth();
        if (bot->getPowerType() == POWER_MANA)
            bot->SetPower(POWER_MANA, bot->GetMaxPower(POWER_MANA));

        // InitStatsForLevel put her back to scale 1.
        ApplyModel(bot, def);

        bot->SaveToDB(false, false);

        LOG_INFO("module", "mod-lorecompanions: {} rebuilt from level {} to {}", def.name, oldLevel, level);
    }

    // ---------------------------------------------------------------------
    // Speech
    // ---------------------------------------------------------------------

    uint32 ChanceFor(std::string const& trigger, int8 rank)
    {
        bool const notable = rank == RANK_NOTABLE;
        if (trigger == T_ZONE_ENTER)     return cfg.ChanceZoneEnter;
        if (trigger == T_QUEST_ACCEPT)   return cfg.ChanceQuestAccept;
        if (trigger == T_QUEST_COMPLETE) return cfg.ChanceQuestComplete;
        if (trigger == T_COMBAT_START)   return notable ? cfg.ChanceCombatNotable : cfg.ChanceCombat;
        if (trigger == T_KILL)           return notable ? cfg.ChanceKillNotable : cfg.ChanceKill;
        if (trigger == T_DEATH)          return cfg.ChanceDeath;
        if (trigger == T_REVIVED)        return cfg.ChanceRevived;
        if (trigger == T_ALLY_DOWN)      return cfg.ChanceAllyDown;
        if (trigger == T_IDLE)           return cfg.ChanceIdle;
        return 100;
    }

    bool Contains(std::vector<uint32> const& list, uint32 value)
    {
        return std::find(list.begin(), list.end(), value) != list.end();
    }

    // Companions in the bot's group who are ready to speak.
    std::unordered_set<std::string> PresentCompanions(Player* bot)
    {
        std::unordered_set<std::string> present;
        Group* group = bot->GetGroup();
        if (!group)
            return present;

        for (auto const& [guid, state] : active)
            if (state.stage == Stage::Ready && group->IsMember(guid))
                present.insert(defs[state.def].id);

        return present;
    }

    Line const* PickLine(Active const& state, Player* bot, std::string const& trigger, Context const& context)
    {
        auto const found = lines.find(defs[state.def].id + "/" + trigger);
        if (found == lines.end())
            return nullptr;

        std::unordered_set<std::string> const present = PresentCompanions(bot);
        int8 const side = int8(bot->GetTeamId());

        std::vector<std::pair<Line const*, uint32>> candidates;
        uint32 total = 0;
        bool anyFresh = false;

        for (Line const& line : found->second)
        {
            uint32 score = line.weight;

            if (!line.places.empty())
            {
                if (!Contains(line.places, context.zoneId) && !Contains(line.places, context.areaId))
                    continue;
                score *= 6;
            }

            if (!line.foes.empty())
            {
                if (std::find(line.foes.begin(), line.foes.end(), context.targetLower) == line.foes.end())
                    continue;
                score *= 12;
            }

            if (!line.creatureTypes.empty())
            {
                if (!Contains(line.creatureTypes, context.creatureType))
                    continue;
                score *= 4;
            }

            if (line.rank != RANK_ANY && line.rank != context.rank)
                continue;

            if (line.side >= 0)
            {
                if (line.side != side)
                    continue;
                score *= 2;
            }

            if (!line.with.empty())
            {
                if (!present.count(line.with))
                    continue;
                score *= 4;
            }

            // A reply needs its speaker in the party, or the line is left
            // hanging.
            if (!line.replyBy.empty() && !present.count(line.replyBy))
                continue;

            bool const recent = std::find(state.recent.begin(), state.recent.end(), line.id) != state.recent.end();
            anyFresh |= !recent;
            candidates.emplace_back(&line, recent ? 0 : score);
            total += recent ? 0 : score;
        }

        if (candidates.empty())
            return nullptr;

        // Everything that matches was said recently: allow repeats rather than
        // silence, but still weighted.
        if (!anyFresh)
        {
            total = 0;
            for (auto& [line, score] : candidates)
            {
                score = line->weight;
                total += score;
            }
        }

        uint32 roll = urand(1, std::max<uint32>(total, 1));
        for (auto const& [line, score] : candidates)
        {
            if (roll <= score)
                return line;
            roll -= score;
        }

        return candidates.back().first;
    }

    std::string Substitute(std::string text, Player* bot, Context const& context, Player* master)
    {
        auto replace = [&text](std::string const& token, std::string const& value)
        {
            for (size_t pos = text.find(token); pos != std::string::npos; pos = text.find(token, pos + value.size()))
                text.replace(pos, token.size(), value);
        };

        auto areaName = [](uint32 id) -> std::string
        {
            AreaTableEntry const* area = sAreaTableStore.LookupEntry(id);
            if (!area)
                return "";
            char const* name = area->area_name[sWorld->GetDefaultDbcLocale()];
            return name && *name ? name : area->area_name[LOCALE_enUS];
        };

        Player* leader = nullptr;
        if (Group* group = bot->GetGroup())
            leader = ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID());

        replace("%summoner", master ? master->GetName() : "friend");
        replace("%leader", leader ? leader->GetName() : (master ? master->GetName() : "friend"));
        replace("%target", context.target);
        replace("%quest", context.quest);
        replace("%other", context.other);
        replace("%zone", areaName(context.zoneId));
        replace("%area", areaName(context.areaId ? context.areaId : context.zoneId));
        replace("%name", bot->GetName());
        return text;
    }

    void Emit(Player* bot, uint8 channel, std::string const& text)
    {
        // Universal: these are people who mean to be understood, and
        // cross-faction parties are the norm with them.
        switch (channel)
        {
            case CHANNEL_EMOTE:
                bot->TextEmote(text);
                break;
            case CHANNEL_YELL:
                bot->Yell(text, LANG_UNIVERSAL);
                break;
            case CHANNEL_PARTY:
                if (Group* group = bot->GetGroup())
                {
                    WorldPacket data;
                    ChatHandler::BuildChatPacket(data, CHAT_MSG_PARTY, LANG_UNIVERSAL, bot, bot, text);
                    group->BroadcastPacket(&data, false);
                    break;
                }
                [[fallthrough]];
            case CHANNEL_SAY:
            default:
                bot->Say(text, LANG_UNIVERSAL);
                break;
        }
    }

    // `forced` skips the chance roll and both cooldowns: a summon, a goodbye
    // or a test is never something to roll for.
    bool Speak(ObjectGuid guid, std::string const& trigger, Context const& context, bool forced = false)
    {
        auto const it = active.find(guid);
        if (it == active.end())
            return false;

        Active& state = it->second;
        Player* bot = ObjectAccessor::FindConnectedPlayer(guid);
        if (!bot || !bot->IsInWorld())
            return false;

        time_t const now = GameTime::GetGameTime().count();
        if (!forced)
        {
            if (state.stage != Stage::Ready || now < state.nextLine)
                return false;

            auto const last = groupLastLine.find(state.master);
            if (last != groupLastLine.end() && now - last->second < time_t(cfg.GroupGapSeconds))
                return false;

            if (urand(0, 99) >= ChanceFor(trigger, context.rank))
                return false;
        }

        Line const* line = PickLine(state, bot, trigger, context);
        if (!line)
            return false;

        Player* master = ObjectAccessor::FindConnectedPlayer(state.master);
        Emit(bot, line->channel, Substitute(line->text, bot, context, master));

        state.nextLine = now + urand(cfg.CooldownSeconds / 2, cfg.CooldownSeconds + cfg.CooldownSeconds / 2);
        state.recent.push_back(line->id);
        while (state.recent.size() > 8)
            state.recent.pop_front();
        groupLastLine[state.master] = now;

        if (!line->replyBy.empty())
        {
            for (auto& [otherGuid, other] : active)
            {
                if (defs[other.def].id != line->replyBy || other.stage != Stage::Ready)
                    continue;

                Player* replier = ObjectAccessor::FindConnectedPlayer(otherGuid);
                if (!replier)
                    break;

                PendingReply reply;
                reply.speaker = otherGuid;
                reply.text = Substitute(line->replyText, replier, context, master);
                reply.channel = line->channel == CHANNEL_EMOTE ? CHANNEL_SAY : line->channel;
                reply.dueMs = GameTime::GetGameTimeMS().count() + cfg.ReplyDelayMs;
                replies.push_back(std::move(reply));

                // She spoke too; nobody else should cut in.
                other.recent.push_back(line->id);
                groupLastLine[state.master] = now + cfg.ReplyDelayMs / IN_MILLISECONDS;
                break;
            }
        }

        LOG_DEBUG("module", "mod-lorecompanions: {} said line {} for {}", bot->GetName(), line->id, trigger);
        return true;
    }

    Context ContextAt(Player* bot)
    {
        Context context;
        bot->GetZoneAndAreaId(context.zoneId, context.areaId);
        return context;
    }

    void SetCreature(Context& context, Unit* unit)
    {
        if (!unit)
            return;

        context.target = unit->GetName();
        context.targetLower = Lower(context.target);

        if (Creature* creature = unit->ToCreature())
        {
            CreatureTemplate const* info = creature->GetCreatureTemplate();
            context.creatureType = info ? info->type : 0;
            bool const notable = info && (info->rank != CREATURE_ELITE_NORMAL || creature->isWorldBoss());
            context.rank = notable ? RANK_NOTABLE : RANK_NORMAL;
        }
        else
            context.rank = RANK_NORMAL;
    }

    // ---------------------------------------------------------------------
    // Summoning and dismissal
    // ---------------------------------------------------------------------

    uint32 CountFor(ObjectGuid master)
    {
        uint32 count = 0;
        for (auto const& [guid, state] : active)
            if (state.master == master && state.stage != Stage::Leaving)
                ++count;
        return count;
    }

    // A companion who goes with her party seat kept is one whose summoner
    // logged out: she rejoins when they log back in. Any other goodbye
    // gives the seat up, or she would come back after being sent home.
    void Dismiss(ObjectGuid guid, bool sayGoodbye, bool leaveParty = true)
    {
        auto const it = active.find(guid);
        if (it == active.end() || it->second.stage == Stage::Leaving)
            return;

        if (sayGoodbye && it->second.stage == Stage::Ready)
            if (Player* bot = ObjectAccessor::FindConnectedPlayer(guid))
                Speak(guid, T_DISMISS, ContextAt(bot), true);

        it->second.stage = Stage::Leaving;
        it->second.leaveParty = leaveParty;
        it->second.leaveAt = GameTime::GetGameTime().count() + (sayGoodbye ? 2 : 0);
    }

    void LogOut(ObjectGuid guid, ObjectGuid masterGuid)
    {
        if (Player* master = ObjectAccessor::FindConnectedPlayer(masterGuid))
            if (PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(master))
                if (mgr->GetPlayerBot(guid))
                {
                    mgr->LogoutPlayerBot(guid);
                    return;
                }

        if (sRandomPlayerbotMgr.GetPlayerBot(guid))
            sRandomPlayerbotMgr.LogoutPlayerBot(guid);
    }

    // `rejoining`: she is already in the summoner's party, holding her seat
    // from before they logged out, so a full party does not keep her out.
    std::string Summon(Player* player, CompanionDef const& def, bool rejoining = false)
    {
        if (!cfg.Enable)
            return "Lore companions are disabled.";

        if (!def.guid)
            return Acore::StringFormat("{} could not be set up on this realm; the server log says why.", def.title);

        if (!sPlayerbotAIConfig.enabled)
            return "Playerbots are disabled, and companions are playerbots.";

        if (player->InBattleground() || player->InArena())
            return "Companions do not answer in battlegrounds or arenas.";

        auto const existing = active.find(def.guid);
        if (existing != active.end() || ObjectAccessor::FindConnectedPlayer(def.guid))
        {
            if (existing != active.end() && existing->second.master == player->GetGUID())
                return Acore::StringFormat("{} is already with you.", def.title);

            std::string with = "someone else";
            if (existing != active.end())
                if (Player* other = ObjectAccessor::FindConnectedPlayer(existing->second.master))
                    with = other->GetName();
            return Acore::StringFormat("{} is travelling with {}.", def.title, with);
        }

        if (CountFor(player->GetGUID()) >= cfg.MaxPerPlayer)
            return Acore::StringFormat("You already have {} companion(s) with you.", cfg.MaxPerPlayer);

        // Party only: never a raid conversion to make room.
        if (Group* group = player->GetGroup())
        {
            if (group->isBGGroup() || group->isBFGroup() || group->isLFGGroup())
                return "Companions cannot join this kind of group.";

            if (!rejoining && !group->isRaidGroup() && group->GetMembersCount() >= MAXGROUPSIZE)
                return "Your party is full.";
        }

        PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(player);
        if (!mgr)
            return "You cannot command playerbots.";

        // playerbots only lets a player control characters on their own
        // account, their guild, or an account linked to theirs. The link is
        // what makes the companion account answer to anyone who summons.
        uint32 const masterAccount = player->GetSession()->GetAccountId();
        PlayerbotsDatabase.DirectExecute(
            "INSERT IGNORE INTO playerbots_account_links (account_id, linked_account_id) VALUES ({}, {})",
            companionAccountId, masterAccount);

        if (cfg.FollowSummonerSide)
            PrepareSide(def.guid, player->GetTeamId());

        PlaceAt(def.guid, player);

        Active state;
        state.def = IndexOf(&def);
        state.master = player->GetGUID();
        state.stage = Stage::Joining;
        state.since = GameTime::GetGameTime().count();
        active[def.guid] = state;

        mgr->AddPlayerBot(def.guid, masterAccount);
        return "";
    }

    // ---------------------------------------------------------------------
    // The world-thread loop
    // ---------------------------------------------------------------------

    // Every live companion whose summoner is `master`, ready to speak.
    std::vector<ObjectGuid> ReadyCompanionsOf(ObjectGuid master)
    {
        std::vector<ObjectGuid> out;
        for (auto const& [guid, state] : active)
            if (state.master == master && state.stage == Stage::Ready)
                out.push_back(guid);
        return out;
    }

    // Lets one companion of `master` comment, trying them in a random order so
    // the same one does not always get the first word.
    void SomeoneSays(ObjectGuid master, std::string const& trigger, Context const& context,
                     Player* within = nullptr)
    {
        std::vector<ObjectGuid> candidates = ReadyCompanionsOf(master);
        Acore::Containers::RandomShuffle(candidates);

        for (ObjectGuid const& guid : candidates)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(guid);
            if (!bot || !bot->IsAlive())
                continue;

            if (within && !bot->IsWithinDistInMap(within, float(cfg.QuestRangeYards)))
                continue;

            // One roll for the group, not one per companion, or three
            // companions would comment three times as often as one.
            Context local = context;
            if (local.zoneId == 0)
                bot->GetZoneAndAreaId(local.zoneId, local.areaId);
            Speak(guid, trigger, local);
            return;
        }
    }

    void HandleEvent(LoreEvent const& event)
    {
        switch (event.type)
        {
            case LoreEventType::Login:
            {
                if (active.count(event.subject))
                    return;

                // Logged in by something other than ".companion summon" -
                // ".playerbots bot add Jaina", say. Adopt her; her master is
                // whoever playerbots gives her, and the party rule applies.
                auto const def = defByGuid.find(event.subject);
                if (def == defByGuid.end())
                    return;

                Active state;
                state.def = def->second;
                state.since = GameTime::GetGameTime().count();
                active[event.subject] = state;
                return;
            }

            case LoreEventType::Logout:
            {
                // A logout that happened before her latest summon belongs to
                // the previous visit, not this one.
                auto const it = active.find(event.subject);
                if (it != active.end() && !(it->second.stage == Stage::Joining && it->second.since > event.at))
                    active.erase(it);
                return;
            }

            case LoreEventType::MasterLogin:
            {
                // Companions who logged out with their summoner are still in
                // the party, offline. Bring them back into their seats.
                Player* player = ObjectAccessor::FindConnectedPlayer(event.subject);
                if (!player || !player->IsInWorld())
                    return;

                Group* group = player->GetGroup();
                if (!group)
                    return;

                std::vector<size_t> seated;
                for (Group::MemberSlot const& slot : group->GetMemberSlots())
                {
                    auto const def = defByGuid.find(slot.guid);
                    if (def != defByGuid.end() && !active.count(slot.guid) && !ObjectAccessor::FindConnectedPlayer(slot.guid))
                        seated.push_back(def->second);
                }

                for (size_t index : seated)
                {
                    std::string const refusal = Summon(player, defs[index], true);
                    if (!refusal.empty())
                        ChatHandler(player->GetSession()).PSendSysMessage("{}", refusal);
                }
                return;
            }

            case LoreEventType::Zone:
                Speak(event.subject, T_ZONE_ENTER, event.context);
                return;

            case LoreEventType::QuestAccept:
            case LoreEventType::QuestComplete:
            {
                Player* player = ObjectAccessor::FindConnectedPlayer(event.subject);
                if (!player)
                    return;
                SomeoneSays(event.subject, event.type == LoreEventType::QuestAccept ? T_QUEST_ACCEPT : T_QUEST_COMPLETE,
                    event.context, player);
                return;
            }

            case LoreEventType::Combat:
                Speak(event.subject, T_COMBAT_START, event.context);
                return;

            case LoreEventType::Kill:
            {
                // Her own kill: she gets the first chance at it.
                if (active.count(event.subject))
                {
                    if (!Speak(event.subject, T_KILL, event.context))
                        if (event.context.rank == RANK_NOTABLE)
                            SomeoneSays(active[event.subject].master, T_KILL, event.context);
                    return;
                }

                SomeoneSays(event.subject, T_KILL, event.context);
                return;
            }

            case LoreEventType::Death:
            {
                if (active.count(event.subject))
                {
                    Speak(event.subject, T_DEATH, event.context);
                    return;
                }

                // The summoner fell.
                SomeoneSays(event.subject, T_ALLY_DOWN, event.context);
                return;
            }

            case LoreEventType::Revived:
                Speak(event.subject, T_REVIVED, event.context);
                return;
        }
    }

    void Maintain()
    {
        time_t const now = GameTime::GetGameTime().count();
        std::vector<std::pair<ObjectGuid, ObjectGuid>> logouts;

        for (auto& [guid, state] : active)
        {
            CompanionDef& def = defs[state.def];
            Player* bot = ObjectAccessor::FindConnectedPlayer(guid);

            if (state.stage == Stage::Leaving)
            {
                if (now >= state.leaveAt)
                {
                    if (state.leaveParty && bot)
                        if (Group* group = bot->GetGroup())
                            group->RemoveMember(guid);
                    logouts.emplace_back(guid, state.master);
                }
                continue;
            }

            if (!bot || !bot->IsInWorld())
            {
                // Still loading, or the login failed - and playerbots drops a
                // failed login without a word, so say something here.
                if (now - state.since > time_t(cfg.JoinGraceSeconds))
                {
                    LOG_ERROR("module", "mod-lorecompanions: {} never came into the world; her login was refused",
                        def.name);
                    if (Player* master = ObjectAccessor::FindConnectedPlayer(state.master))
                        ChatHandler(master->GetSession()).PSendSysMessage(
                            "{} could not come. The server log says why.", def.title);
                    logouts.emplace_back(guid, state.master);
                }
                continue;
            }

            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            Player* aiMaster = botAI ? botAI->GetMaster() : nullptr;

            if (state.stage == Stage::Joining)
            {
                if (!aiMaster)
                {
                    if (now - state.since > time_t(cfg.JoinGraceSeconds))
                    {
                        LOG_INFO("module", "mod-lorecompanions: {} has nobody to follow; sending her home", def.name);
                        Dismiss(guid, false, false);
                    }
                    continue;
                }

                state.master = aiMaster->GetGUID();
                if (!BringTo(bot, aiMaster))
                    continue;

                ApplyModel(bot, def);
                Rebuild(bot, def, TargetLevel(bot, aiMaster), aiMaster);

                state.stage = Stage::Ready;
                state.since = now;
                state.idleAt = now + urand(cfg.IdleMinSeconds, cfg.IdleMaxSeconds);
                state.nextLine = now + 20;
                Speak(guid, T_SUMMON, ContextAt(bot), true);
                continue;
            }

            Player* master = ObjectAccessor::FindConnectedPlayer(state.master);
            if (!master || aiMaster != master)
            {
                // A summoner who logged out keeps her seat for next time.
                Dismiss(guid, false, master != nullptr);
                continue;
            }

            // Party only. The invite is queued by playerbots at login, so give
            // it a moment before treating "no group" as "removed from it".
            Group* group = bot->GetGroup();
            if (!group || !group->IsMember(master->GetGUID()))
            {
                if (now - state.since > time_t(cfg.JoinGraceSeconds))
                {
                    LOG_INFO("module", "mod-lorecompanions: {} is no longer in {}'s party", def.name, master->GetName());
                    Dismiss(guid, true);
                }
                continue;
            }

            if (ModelDrifted(bot, def))
                ApplyModel(bot, def);
            if (LookDrifted(bot, def))
                ApplyLook(bot, def);

            uint8 const target = TargetLevel(bot, master);
            if (target != bot->GetLevel() && !bot->IsInCombat() && !master->IsInCombat() && !bot->IsInFlight())
            {
                bool const up = target > bot->GetLevel();
                Player* leader = ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID());
                Rebuild(bot, def, target, leader ? leader : master);
                if (up)
                    Speak(guid, T_GROW_STRONGER, ContextAt(bot), true);
            }

            if (now >= state.idleAt)
            {
                state.idleAt = now + urand(cfg.IdleMinSeconds, cfg.IdleMaxSeconds);
                if (!bot->IsInCombat() && bot->IsAlive() && !bot->IsInFlight())
                    Speak(guid, T_IDLE, ContextAt(bot));
            }
        }

        for (auto const& [guid, masterGuid] : logouts)
        {
            LogOut(guid, masterGuid);
            active.erase(guid);
        }

        // Masters who no longer have anyone with them.
        for (auto it = groupLastLine.begin(); it != groupLastLine.end();)
        {
            if (ReadyCompanionsOf(it->first).empty() && now - it->second > 600)
                it = groupLastLine.erase(it);
            else
                ++it;
        }
    }

    void DeliverReplies()
    {
        if (replies.empty())
            return;

        uint64 const nowMs = GameTime::GetGameTimeMS().count();
        for (auto it = replies.begin(); it != replies.end();)
        {
            if (it->dueMs > nowMs)
            {
                ++it;
                continue;
            }

            if (Player* bot = ObjectAccessor::FindConnectedPlayer(it->speaker))
                if (bot->IsInWorld() && active.count(it->speaker))
                    Emit(bot, it->channel, it->text);

            it = replies.erase(it);
        }
    }

    void Push(LoreEvent event)
    {
        std::lock_guard<std::mutex> guard(eventLock);
        events.push_back(std::move(event));
    }
}

class LoreCompanions_WorldScript : public WorldScript
{
public:
    LoreCompanions_WorldScript() : WorldScript("LoreCompanions_WorldScript",
        {
            WORLDHOOK_ON_AFTER_CONFIG_LOAD,
            WORLDHOOK_ON_STARTUP,
            WORLDHOOK_ON_UPDATE
        }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadConfig();
    }

    void OnStartup() override
    {
        if (!cfg.Enable)
            return;

        LoadDefs();
        LoadLines();

        if (!EnsureAccount())
            return;

        EnsureCharacters();
        LOG_INFO("module", "mod-lorecompanions: {} companion(s) ready on account {}", defByGuid.size(), cfg.AccountName);
    }

    void OnUpdate(uint32 diff) override
    {
        if (!cfg.Enable || defByGuid.empty())
            return;

        std::vector<LoreEvent> pending;
        {
            std::lock_guard<std::mutex> guard(eventLock);
            pending.swap(events);
        }

        for (LoreEvent const& event : pending)
            HandleEvent(event);

        DeliverReplies();

        if (maintainTimer > diff)
        {
            maintainTimer -= diff;
            return;
        }

        maintainTimer = 1000;
        if (!active.empty())
            Maintain();
    }
};

class LoreCompanions_PlayerScript : public PlayerScript
{
public:
    LoreCompanions_PlayerScript() : PlayerScript("LoreCompanions_PlayerScript",
        {
            PLAYERHOOK_ON_LOGIN,
            PLAYERHOOK_ON_LOGOUT,
            PLAYERHOOK_ON_UPDATE_ZONE,
            PLAYERHOOK_ON_PLAYER_QUEST_ACCEPT,
            PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST,
            PLAYERHOOK_ON_CREATURE_KILL,
            PLAYERHOOK_ON_CREATURE_KILLED_BY_PET,
            PLAYERHOOK_ON_PLAYER_JUST_DIED,
            PLAYERHOOK_ON_PLAYER_RESURRECT,
            PLAYERHOOK_ON_PLAYER_ENTER_COMBAT,
            PLAYERHOOK_ON_AFTER_SET_VISIBLE_ITEM_SLOT
        }) { }

    // Runs on her own map's thread, for her own fields; defs never change
    // after startup.
    void OnPlayerAfterSetVisibleItemSlot(Player* player, uint8 slot, Item* /*item*/) override
    {
        auto const def = defByGuid.find(player->GetGUID());
        if (def != defByGuid.end())
            ApplyLookSlot(player, defs[def->second], slot);
    }

    void OnPlayerLogin(Player* player) override
    {
        auto const def = defByGuid.find(player->GetGUID());
        if (def == defByGuid.end())
        {
            if (IsHumanPlayer(player))
                Push({ LoreEventType::MasterLogin, player->GetGUID(), {} });
            return;
        }

        // Before she is visible to anyone, so nobody sees the racial model.
        ApplyModel(player, defs[def->second]);
        ApplyLook(player, defs[def->second]);

        // Tells mod-botlore she has her own voice and wants none of its lines.
        player->CustomData.Set("LoreCompanion", new DataMap::Base());
        Push({ LoreEventType::Login, player->GetGUID(), {} });
    }

    void OnPlayerLogout(Player* player) override
    {
        if (IsCompanion(player->GetGUID()))
            Push({ LoreEventType::Logout, player->GetGUID(), {} });
    }

    void OnPlayerUpdateZone(Player* player, uint32 newZone, uint32 newArea) override
    {
        if (!IsCompanion(player->GetGUID()))
            return;

        LoreEvent event{ LoreEventType::Zone, player->GetGUID(), {} };
        event.context.zoneId = newZone;
        event.context.areaId = newArea;
        Push(std::move(event));
    }

    void OnPlayerQuestAccept(Player* player, Quest const* quest) override
    {
        QuestEvent(player, quest, LoreEventType::QuestAccept);
    }

    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
    {
        QuestEvent(player, quest, LoreEventType::QuestComplete);
    }

    void OnPlayerCreatureKill(Player* killer, Creature* killed) override
    {
        KillEvent(killer, killed);
    }

    void OnPlayerCreatureKilledByPet(Player* owner, Creature* killed) override
    {
        KillEvent(owner, killed);
    }

    void OnPlayerJustDied(Player* player) override
    {
        if (!IsCompanion(player->GetGUID()) && !IsHumanPlayer(player))
            return;

        LoreEvent event{ LoreEventType::Death, player->GetGUID(), {} };
        player->GetZoneAndAreaId(event.context.zoneId, event.context.areaId);
        event.context.other = player->GetName();
        Push(std::move(event));
    }

    void OnPlayerResurrect(Player* player, float /*restorePercent*/, bool& /*applySickness*/) override
    {
        if (!IsCompanion(player->GetGUID()))
            return;

        LoreEvent event{ LoreEventType::Revived, player->GetGUID(), {} };
        player->GetZoneAndAreaId(event.context.zoneId, event.context.areaId);
        Push(std::move(event));
    }

    void OnPlayerEnterCombat(Player* player, Unit* enemy) override
    {
        if (!IsCompanion(player->GetGUID()))
            return;

        LoreEvent event{ LoreEventType::Combat, player->GetGUID(), {} };
        player->GetZoneAndAreaId(event.context.zoneId, event.context.areaId);
        SetCreature(event.context, enemy);
        Push(std::move(event));
    }

private:
    static void QuestEvent(Player* player, Quest const* quest, LoreEventType type)
    {
        if (!quest || !IsHumanPlayer(player))
            return;

        LoreEvent event{ type, player->GetGUID(), {} };
        player->GetZoneAndAreaId(event.context.zoneId, event.context.areaId);
        event.context.quest = quest->GetTitle();
        Push(std::move(event));
    }

    static void KillEvent(Player* killer, Creature* killed)
    {
        if (!killer || !killed || (!IsCompanion(killer->GetGUID()) && !IsHumanPlayer(killer)))
            return;

        LoreEvent event{ LoreEventType::Kill, killer->GetGUID(), {} };
        killer->GetZoneAndAreaId(event.context.zoneId, event.context.areaId);
        SetCreature(event.context, killed);
        Push(std::move(event));
    }
};

class LoreCompanions_CommandScript : public CommandScript
{
public:
    LoreCompanions_CommandScript() : CommandScript("LoreCompanions_CommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable companionTable =
        {
            { "summon",  HandleSummon,  SEC_PLAYER,        Console::No },
            { "dismiss", HandleDismiss, SEC_PLAYER,        Console::No },
            { "list",    HandleList,    SEC_PLAYER,        Console::Yes },
            { "say",     HandleSay,     SEC_GAMEMASTER,    Console::Yes },
            { "reload",  HandleReload,  SEC_ADMINISTRATOR, Console::Yes }
        };

        static ChatCommandTable commandTable =
        {
            { "companion", companionTable }
        };

        return commandTable;
    }

    static bool HandleSummon(ChatHandler* handler, std::string name)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        CompanionDef const* def = FindDef(name);
        if (!def)
        {
            handler->PSendSysMessage("There is no companion called {}. \".companion list\" names them.", name);
            return true;
        }

        std::string const refusal = Summon(player, *def);
        if (!refusal.empty())
            handler->SendSysMessage(refusal);
        else
            handler->PSendSysMessage("You call for {}.", def->title);

        return true;
    }

    static bool HandleDismiss(ChatHandler* handler, Optional<std::string> name)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        CompanionDef const* only = nullptr;
        if (name && Lower(*name) != "all")
        {
            only = FindDef(*name);
            if (!only)
            {
                handler->PSendSysMessage("There is no companion called {}.", *name);
                return true;
            }
        }

        uint32 dismissed = 0;
        for (auto const& [guid, state] : active)
        {
            if (state.master != player->GetGUID() || state.stage == Stage::Leaving)
                continue;
            if (only && defs[state.def].guid != only->guid)
                continue;

            Dismiss(guid, true);
            ++dismissed;
        }

        if (!dismissed)
        {
            if (only)
                handler->PSendSysMessage("{} is not with you.", only->title);
            else
                handler->SendSysMessage("No companions are with you.");
        }

        return true;
    }

    static bool HandleList(ChatHandler* handler)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;

        for (CompanionDef const& def : defs)
        {
            std::string status = "available";
            if (!def.guid)
                status = "unavailable";
            else if (auto const it = active.find(def.guid); it != active.end())
            {
                if (player && it->second.master == player->GetGUID())
                    status = "with you";
                else if (Player* other = ObjectAccessor::FindConnectedPlayer(it->second.master))
                    status = "with " + other->GetName();
                else
                    status = "travelling";
            }

            handler->PSendSysMessage("{} - {} ({})", def.id, def.title, status);
        }

        if (defs.empty())
            handler->SendSysMessage("No companions are defined.");

        return true;
    }

    // ".companion say jaina idle" - makes an active companion speak a line for
    // a trigger now, ignoring chance and cooldowns. For checking new lines.
    static bool HandleSay(ChatHandler* handler, std::string name, std::string trigger)
    {
        CompanionDef const* def = FindDef(name);
        if (!def || !active.count(def->guid))
        {
            handler->SendSysMessage("That companion is not out in the world.");
            return true;
        }

        Player* bot = ObjectAccessor::FindConnectedPlayer(def->guid);
        if (!bot)
            return true;

        Context context = ContextAt(bot);
        if (Unit* target = bot->GetVictim())
            SetCreature(context, target);

        if (!Speak(def->guid, trigger, context, true))
            handler->PSendSysMessage("{} has no line for {} here.", def->title, trigger);

        return true;
    }

    static bool HandleReload(ChatHandler* handler)
    {
        uint32 const count = LoadLines();
        handler->PSendSysMessage("mod-lorecompanions: reloaded {} line(s).", count);
        return true;
    }
};

void AddLoreCompanionsScripts()
{
    new LoreCompanions_WorldScript();
    new LoreCompanions_PlayerScript();
    new LoreCompanions_CommandScript();
}
