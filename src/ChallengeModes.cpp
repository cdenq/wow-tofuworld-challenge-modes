/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "ChallengeModes.h"
#include "Corpse.h"
#include "DBCStores.h"
#include "Group.h"
#include "Mail.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "Random.h"
#include <sstream>

#if __has_include("RandomPlayerbotMgr.h")
#include "RandomPlayerbotMgr.h"
#define TOFU_HAS_PLAYERBOTS 1
#endif

#if __has_include("IndividualProgression.h")
#include "IndividualProgression.h"
#define TOFU_HAS_INDIVIDUAL_PROGRESSION 1
#endif

namespace
{
    std::string const SETTING_SOURCE = "mod-challenge-modes";

    // The challenges a player can pick or hold, in the order the shrine and the login line list them. HARDCORE_DEAD and SETTING_COMPLETED are state flags, not challenges.
    std::array<ChallengeModeSettings, 10> const ALL_CHALLENGES =
    {
        SETTING_HARDCORE, SETTING_SELF_CRAFTED, SETTING_ITEM_QUALITY_LEVEL, SETTING_ONE_RATE, SETTING_QUEST_XP_ONLY, SETTING_PROGRESSION,
        SETTING_SEMI_HARDCORE, SETTING_SLOW_XP_GAIN, SETTING_VERY_SLOW_XP_GAIN, SETTING_IRON_MAN
    };

    std::string const NO_GROUP_MESSAGE = "Characters on a challenge (other than Progression) can only group with other challenge characters. You cannot fill your group with random bots while on a challenge: make alts on the same challenges, log them in, and party with them instead.";

    void SendSys(Player* player, std::string const& text)
    {
        if (player && player->GetSession())
            ChatHandler(player->GetSession()).SendSysMessage(text);
    }

    bool IsChallengeIndex(uint8 index)
    {
        return index < CHALLENGE_MODE_COUNT && index != HARDCORE_DEAD;
    }

    // reward_group keys of mod-tcg-vendors (REWARD_GROUPS in src/mod_tcg_vendors.cpp at lightninjay/mod-tcg-vendors b24b511). Used only to warn at startup; an unlisted key is still issued, since a newer mod-tcg-vendors may add keys.
    std::unordered_set<std::string> const KNOWN_TCG_GROUPS =
    {
        "TCG_TABARD_OF_FLAME", "TCG_HIPPOGRYPH_HATCHLING", "TCG_RIDING_TURTLE", "TCG_PICNIC_BASKET", "TCG_BANANA_CHARM", "TCG_IMP_IN_A_BALL",
        "TCG_GOBLIN_GUMBO_KETTLE", "TCG_FISHING_CHAIR", "TCG_SPECTRAL_TIGER", "TCG_PAPER_FLYING_MACHINE", "TCG_ROCKET_CHICKEN", "TCG_DRAGON_KITE",
        "TCG_X51_NETHER_ROCKET", "TCG_PET_BISCUIT", "TCG_GOBLIN_WEATHER_MACHINE", "TCG_PATH_OF_ILLIDAN", "TCG_DISCO", "TCG_SOUL_TRADER_BEACON",
        "TCG_PARTY_GRENADE", "TCG_FLAG_OF_OWNERSHIP", "TCG_BIG_BATTLE_BEAR", "TCG_SANDBOX_TIGER", "TCG_EPIC_PURPLE_SHIRT", "TCG_FOAM_SWORD_RACK",
        "TCG_PATH_OF_CENARIUS", "TCG_OGRE_PINATA", "TCG_MAGIC_ROOSTER_EGG", "TCG_SCOURGEWAR_MINIMOUNT", "TCG_TUSKARR_KITE", "TCG_SPECTRAL_TIGER_CUB",
        "TCG_LANDROS_GIFT_BOX", "TCG_INSTANT_STATUE_PEDESTAL", "TCG_BLAZING_HIPPOGRYPH", "TCG_PAINT_BOMB", "TCG_ETHEREAL_PORTAL", "TCG_WOOLY_WHITE_RHINO",
        "TCG_TABARD_OF_FROST", "TCG_PERPETUAL_PURPLE_FIREWORK", "TCG_CARVED_OGRE_IDOL", "TCG_TABARD_OF_THE_ARCANE", "TCG_TABARD_OF_BRILLIANCE", "TCG_TABARD_OF_THE_DEFENDER",
        "TCG_TABARD_OF_FURY", "TCG_TABARD_OF_NATURE", "TCG_TABARD_OF_THE_VOID", "TCG_LANDROS_PET_BOX", "BLIZZCON_MURKY", "BLIZZCON_MURLOC_COSTUME",
        "BLIZZCON_BIG_BLIZZARD_BEAR", "PROMO_GURKY", "PROMO_ORANGE_MURLOC_EGG", "PROMO_WHITE_MURLOC_EGG", "PROMO_HEAVY_MURLOC_EGG", "PROMO_MURKIMUS_SPEAR",
        "PROMO_ZERGLING_LEASH", "PROMO_PANDA_COLLAR", "PROMO_DIABLO_STONE", "PROMO_NETHERWHELP", "PROMO_FROSTYS_COLLAR", "WWI_TYRAELS_HILT",
        "PROMO_WARBOT_KEY", "PROMO_ENCHANTED_ONYX", "PROMO_CORE_HOUND_PUP", "PROMO_GRYPHON_HATCHLING", "PROMO_WIND_RIDER_CUB", "PROMO_PANDAREN_MONK",
        "PROMO_LIL_PHYLACTERY", "PROMO_LIL_XT", "PROMO_MINI_THOR", "PROMO_ONYXIAN_WHELPLING"
    };

    // Same alphabet and layout as mod-tcg-vendors' GenerateRandomCode: 16 characters without I, L, O, 0, and 1, shown as XXXX-XXXX-XXXX-XXXX.
    std::string GenerateTcgCode()
    {
        static char const alphabet[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
        std::string code;
        code.reserve(19);
        for (uint8 i = 0; i < 16; ++i)
        {
            if (i && i % 4 == 0)
                code += '-';
            code += alphabet[urand(0, sizeof(alphabet) - 2)];
        }
        return code;
    }

    // Upper-case letters, digits, and underscores only, so the key is safe inside the INSERT text.
    bool IsValidTcgGroupKey(std::string const& key)
    {
        if (key.empty() || key.size() > 64)
            return false;
        for (char c : key)
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
                return false;
        return true;
    }

    // Letters, digits, and underscores only, so the table name is safe inside the SQL text.
    bool IsValidTableName(std::string const& name)
    {
        if (name.empty() || name.size() > 64)
            return false;
        for (char c : name)
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
                return false;
        return true;
    }

    void ReplaceAll(std::string& text, std::string const& from, std::string const& to)
    {
        if (from.empty())
            return;
        size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos)
        {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
    }

    // Conf strings cannot hold a line break, so "\n" written in the conf becomes one.
    std::string ExpandLineBreaks(std::string text)
    {
        ReplaceAll(text, "\\n", "\n");
        return text;
    }

    // Player-made characters, altbots included. Random bots and addclass bots are not player-made.
    bool IsPlayerMadeCharacter(Player* player)
    {
#ifdef TOFU_HAS_PLAYERBOTS
        if (sRandomPlayerbotMgr.IsRandomBot(player) || sRandomPlayerbotMgr.IsAddclassBot(player))
            return false;
#endif
        return player != nullptr;
    }
}

ChallengeModes* ChallengeModes::instance()
{
    static ChallengeModes instance;
    return &instance;
}

bool ChallengeModes::challengeEnabledForPlayer(ChallengeModeSettings setting, Player* player) const
{
    if (!player || !enabled() || !challengeEnabled(setting))
    {
        return false;
    }
    return player->GetPlayerSetting(SETTING_SOURCE, setting).value;
}

bool ChallengeModes::challengeEnabled(ChallengeModeSettings setting) const
{
    if (!IsChallengeIndex(setting))
        return false;
    return definitions[setting].enable;
}

uint32 ChallengeModes::getDisableLevel(ChallengeModeSettings setting) const
{
    if (!IsChallengeIndex(setting))
        return 0;
    return definitions[setting].disableLevel;
}

float ChallengeModes::getXpBonusForChallenge(ChallengeModeSettings setting) const
{
    if (!IsChallengeIndex(setting))
        return 1.0f;
    return definitions[setting].xpMultiplier;
}

ChallengeDefinition const* ChallengeModes::getDefinition(ChallengeModeSettings setting) const
{
    if (!IsChallengeIndex(setting))
        return nullptr;
    return &definitions[setting];
}

std::string const& ChallengeModes::getDisplayName(ChallengeModeSettings setting) const
{
    static std::string const unknown = "Unknown";
    if (!IsChallengeIndex(setting))
        return unknown;
    return definitions[setting].displayName;
}

bool ChallengeModes::hasRestrictingChallenge(Player* player) const
{
    if (!player || !enabled())
        return false;
    for (ChallengeModeSettings setting : ALL_CHALLENGES)
    {
        if (setting == SETTING_PROGRESSION)
            continue;
        if (challengeEnabledForPlayer(setting, player))
            return true;
    }
    return false;
}

bool ChallengeModes::hasPersonalXpLock(Player* player) const
{
    return challengeEnabledForPlayer(SETTING_ONE_RATE, player) || challengeEnabledForPlayer(SETTING_QUEST_XP_ONLY, player);
}

std::string ChallengeModes::activeChallengeList(Player* player) const
{
    std::string list;
    for (ChallengeModeSettings setting : ALL_CHALLENGES)
    {
        if (!challengeEnabledForPlayer(setting, player))
            continue;
        if (!list.empty())
            list += ", ";
        list += getDisplayName(setting);
    }
    return list;
}

bool ChallengeModes::isSelfCraftedExempt(ItemTemplate const* proto) const
{
    if (!proto)
        return false;
    // Shirts and tabards
    if (proto->InventoryType == INVTYPE_BODY || proto->InventoryType == INVTYPE_TABARD)
        return true;
    // Bags and quivers are containers, not gear
    if (proto->Class == ITEM_CLASS_CONTAINER || proto->Class == ITEM_CLASS_QUIVER || proto->InventoryType == INVTYPE_BAG || proto->InventoryType == INVTYPE_QUIVER)
        return true;
    // Tools: fishing poles, and every item a profession spell requires as a tool (Mining Pick, Blacksmith Hammer, Arclight Spanner, and so on)
    if (proto->Class == ITEM_CLASS_WEAPON && proto->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE)
        return true;
    if (professionToolItems.count(proto->ItemId))
        return true;
    // Items that are themselves needed for a quest (worn to see or interact with something, handed in, or started from)
    if (proto->Class == ITEM_CLASS_QUEST || proto->Bonding == BIND_QUEST_ITEM || proto->StartQuest != 0 || questNeededItems.count(proto->ItemId))
        return true;
    // Starting weapons and gear
    if (startingGearItems.count(proto->ItemId))
        return true;
    return selfCraftedExtraExemptItems.count(proto->ItemId) != 0;
}

bool ChallengeModes::isEquipAllowed(Player* player, Item* item, std::string* reason) const
{
    if (!player || !item)
        return true;
    ItemTemplate const* proto = item->GetTemplate();
    if (!proto)
        return true;

    if (challengeEnabledForPlayer(SETTING_SELF_CRAFTED, player) && !isSelfCraftedExempt(proto) && item->GetGuidValue(ITEM_FIELD_CREATOR) != player->GetGUID())
    {
        if (reason)
            *reason = getDisplayName(SETTING_SELF_CRAFTED);
        return false;
    }

    if (challengeEnabledForPlayer(SETTING_ITEM_QUALITY_LEVEL, player) && proto->Class != ITEM_CLASS_CONTAINER && proto->Class != ITEM_CLASS_QUIVER && proto->Quality > itemQualityMax)
    {
        if (reason)
            *reason = getDisplayName(SETTING_ITEM_QUALITY_LEVEL);
        return false;
    }

    if (challengeEnabledForPlayer(SETTING_IRON_MAN, player) && proto->Quality > ITEM_QUALITY_NORMAL)
    {
        if (reason)
            *reason = getDisplayName(SETTING_IRON_MAN);
        return false;
    }

    return true;
}

// Moves every equipped item the player's challenges forbid into the bags, or into the mailbox when the bags are full. Mirrors Player::AutoUnequipOffhandIfNeed.
void ChallengeModes::enforceEquipment(Player* player) const
{
    if (!player || !player->IsInWorld())
        return;

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
            continue;

        std::string reason;
        if (isEquipAllowed(player, item, &reason))
            continue;

        std::string const itemName = item->GetTemplate()->Name1;
        ItemPosCountVec dest;
        InventoryResult msg = player->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false);
        if (msg == EQUIP_ERR_OK)
        {
            player->RemoveItem(INVENTORY_SLOT_BAG_0, slot, true);
            player->StoreItem(dest, item, true);
            SendSys(player, "[" + itemName + "] was unequipped and moved to your bags: it breaks your " + reason + " challenge.");
        }
        else
        {
            player->MoveItemFromInventory(INVENTORY_SLOT_BAG_0, slot, true);
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            item->DeleteFromInventoryDB(trans);
            item->SaveToDB(trans);
            MailDraft("Unequipped item", "This item breaks your " + reason + " challenge, and your bags were full, so it was mailed to you.").AddItem(item).SendMailTo(trans, player, MailSender(player, MAIL_STATIONERY_GM), MAIL_CHECK_MASK_COPIED);
            CharacterDatabase.CommitTransaction(trans);
            SendSys(player, "[" + itemName + "] was unequipped and mailed to you: it breaks your " + reason + " challenge.");
        }
    }
}

// Equipping can happen inside code that still holds the item (bot gear commands, item loading), so the check runs on the next player update instead of inside the equip call.
void ChallengeModes::scheduleEquipmentCheck(Player* player) const
{
    if (!player)
        return;
    ObjectGuid const guid = player->GetGUID();
    player->m_Events.AddEventAtOffset([guid]()
    {
        if (Player* target = ObjectAccessor::FindPlayer(guid))
            sChallengeModes->enforceEquipment(target);
    }, 100ms);
}

bool ChallengeModes::isCompleted(Player* player, ChallengeModeSettings setting) const
{
    if (!player || !IsChallengeIndex(setting))
        return false;
    return (player->GetPlayerSetting(SETTING_SOURCE, SETTING_COMPLETED).value & (1u << setting)) != 0;
}

void ChallengeModes::markCompleted(Player* player, ChallengeModeSettings setting) const
{
    if (!player || !IsChallengeIndex(setting))
        return;
    uint32 mask = player->GetPlayerSetting(SETTING_SOURCE, SETTING_COMPLETED).value;
    player->UpdatePlayerSetting(SETTING_SOURCE, SETTING_COMPLETED, mask | (1u << setting));
}

void ChallengeModes::grantLevelRewards(Player* player, ChallengeModeSettings setting, uint8 level) const
{
    ChallengeDefinition const* def = getDefinition(setting);
    if (!player || !def)
        return;

    if (auto it = def->titleRewards.find(level); it != def->titleRewards.end())
    {
        for (uint32 titleId : it->second)
        {
            CharTitlesEntry const* titleInfo = sCharTitlesStore.LookupEntry(titleId);
            if (!titleInfo)
            {
                LOG_ERROR("module", "mod-challenge-modes: invalid title ID {} in {}.TitleRewards", titleId, def->confName);
                continue;
            }
            player->SetTitle(titleInfo);
        }
    }

    if (auto it = def->talentRewards.find(level); it != def->talentRewards.end() && it->second)
    {
        player->RewardExtraBonusTalentPoints(it->second);
        player->InitTalentForLevel();
        SendSys(player, def->displayName + " reward: " + std::to_string(it->second) + " bonus talent point(s).");
    }

    if (auto it = def->achievementRewards.find(level); it != def->achievementRewards.end())
    {
        for (uint32 achievementId : it->second)
        {
            AchievementEntry const* achievementInfo = sAchievementStore.LookupEntry(achievementId);
            if (!achievementInfo)
            {
                LOG_ERROR("module", "mod-challenge-modes: invalid achievement ID {} in {}.AchievementReward", achievementId, def->confName);
                continue;
            }
            player->CompletedAchievement(achievementInfo);
        }
    }

    if (auto it = def->itemRewards.find(level); it != def->itemRewards.end() && !it->second.empty())
    {
        std::vector<std::pair<uint32, uint32>> mailItems;
        for (ChallengeItemReward const& reward : it->second)
            mailItems.emplace_back(reward.itemId, reward.count);
        player->SendItemRetrievalMail(mailItems);
        SendSys(player, def->displayName + " reward: item(s) sent to your mailbox.");
    }

    // One reward mail per challenge and level: the gold is attached and the redemption codes are in the body text.
    uint32 gold = 0;
    if (auto it = def->goldRewards.find(level); it != def->goldRewards.end())
        gold = it->second;

    std::vector<std::pair<std::string, std::string>> codes; // reward_group, code
    if (auto it = def->tcgCodeRewards.find(level); it != def->tcgCodeRewards.end() && !it->second.empty())
    {
        if (!tcgCodesActive())
            LOG_ERROR("module", "mod-challenge-modes: {} has TCG code rewards at level {}, but TCG codes are off (ChallengeModes.TcgCodes.Enable = 0, or the table was not found at startup). No code was issued to {}.", def->confName, level, player->GetName());
        else if (!IsPlayerMadeCharacter(player))
            LOG_INFO("module", "mod-challenge-modes: {} is a random or addclass bot, so no TCG code was issued for {} level {}.", player->GetName(), def->confName, level);
        else
            for (std::string const& group : it->second)
                codes.emplace_back(group, GenerateTcgCode());
    }

    if (!gold && codes.empty())
        return;

    std::string const levelText = std::to_string(level);
    std::string subject = def->rewardMailSubject;
    ReplaceAll(subject, "{MODE}", def->displayName);
    ReplaceAll(subject, "{LEVEL}", levelText);

    std::string codeText;
    if (!codes.empty())
    {
        codeText = codes.size() == 1 ? "\n\nYour redemption code:" : "\n\nYour redemption codes:";
        for (auto const& [group, code] : codes)
            codeText += "\n" + code;
        codeText += "\n\nRedeem at a TCG vendor. Each code works once.";
    }

    std::string body = def->rewardMailBody;
    ReplaceAll(body, "{MODE}", def->displayName);
    ReplaceAll(body, "{LEVEL}", levelText);
    if (body.find("{CODES}") != std::string::npos)
        ReplaceAll(body, "{CODES}", codeText);
    else
        body += codeText; // a code is never dropped because the body lost its placeholder

    // The codes and the mail are saved in one transaction: either the player gets the mail and every code in it exists, or neither happens.
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    for (auto const& [group, code] : codes)
        trans->Append("INSERT INTO `{}` (code, reward_group, redeemed, account_id, character_guid, redeemed_date) VALUES ('{}', '{}', 0, NULL, NULL, NULL)", tcgCodesTable, code, group);
    MailDraft draft(subject, body);
    if (gold)
        draft.AddMoney(gold);
    draft.SendMailTo(trans, MailReceiver(player), MailSender(player, MAIL_STATIONERY_GM));
    CharacterDatabase.CommitTransaction(trans);

    for (auto const& [group, code] : codes)
        LOG_INFO("module", "mod-challenge-modes: issued TCG code {} ({}) to {} (guid {}, account {}) for {} level {}.", code, group, player->GetName(), player->GetGUID().GetCounter(), player->GetSession() ? player->GetSession()->GetAccountId() : 0, def->confName, level);

    if (gold && !codes.empty())
        SendSys(player, def->displayName + " reward: " + std::to_string(gold / 10000) + " gold and " + std::to_string(codes.size()) + " redemption code(s) sent to your mailbox.");
    else if (gold)
        SendSys(player, def->displayName + " reward: " + std::to_string(gold / 10000) + " gold sent to your mailbox.");
    else
        SendSys(player, def->displayName + " reward: " + std::to_string(codes.size()) + " redemption code(s) sent to your mailbox.");
}

void ChallengeModes::checkTcgCodesTable()
{
    tcgCodesAvailable = false;
    if (!tcgCodesConfigured)
        return;

    if (!IsValidTableName(tcgCodesTable))
    {
        LOG_ERROR("module", "mod-challenge-modes: ChallengeModes.TcgCodes.Table \"{}\" is not a valid table name. TCG code rewards are off.", tcgCodesTable);
        return;
    }

    if (!CharacterDatabase.Query("SHOW TABLES LIKE '{}'", tcgCodesTable))
    {
        LOG_ERROR("module", "mod-challenge-modes: table {} was not found in the characters database (is mod-tcg-vendors installed?). TCG code rewards are off until the next start.", tcgCodesTable);
        return;
    }

    tcgCodesAvailable = true;
    LOG_INFO("module", "mod-challenge-modes: TCG code rewards are on (table {}).", tcgCodesTable);
}

void ChallengeModes::validateTcgCodeRewards() const
{
    for (ChallengeDefinition const& def : definitions)
        for (auto const& [level, groups] : def.tcgCodeRewards)
            for (std::string const& group : groups)
                if (!KNOWN_TCG_GROUPS.count(group))
                    LOG_WARN("module", "mod-challenge-modes: {}.TcgCodeRewards level {} uses reward_group {}, which mod-tcg-vendors b24b511 does not define. Codes are still issued, but the vendor rejects them unless the key exists (check with: python tools/generate_codes.py --list-groups).", def.confName, level, group);
}

void ChallengeModes::buildStartupData()
{
    questNeededItems.clear();
    for (auto const& [questId, quest] : sObjectMgr->GetQuestTemplates())
    {
        if (!quest)
            continue;
        for (uint32 itemId : quest->RequiredItemId)
            if (itemId)
                questNeededItems.insert(itemId);
        for (uint32 itemId : quest->ItemDrop)
            if (itemId)
                questNeededItems.insert(itemId);
        if (quest->GetSrcItemId())
            questNeededItems.insert(quest->GetSrcItemId());
    }

    startingGearItems.clear();
    for (uint32 i = 0; i < sCharStartOutfitStore.GetNumRows(); ++i)
    {
        CharStartOutfitEntry const* outfit = sCharStartOutfitStore.LookupEntry(i);
        if (!outfit)
            continue;
        for (int32 itemId : outfit->ItemId)
            if (itemId > 0)
                startingGearItems.insert(uint32(itemId));
    }
    for (uint32 race = 1; race < 32; ++race) // GetPlayerInfo returns null past the last race
    {
        for (uint32 cls = 1; cls < MAX_CLASSES; ++cls)
        {
            PlayerInfo const* info = sObjectMgr->GetPlayerInfo(race, cls);
            if (!info)
                continue;
            for (PlayerCreateInfoItem const& createItem : info->item)
                startingGearItems.insert(createItem.item_id);
        }
    }

    professionToolItems.clear();
    for (uint32 spellId = 1; spellId < sSpellMgr->GetSpellInfoStoreSize(); ++spellId)
    {
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            continue;
        for (uint32 toolId : spellInfo->Totem)
            if (toolId)
                professionToolItems.insert(toolId);
    }

    LOG_INFO("module", "mod-challenge-modes: Self Crafted exemptions loaded: {} quest items, {} starting gear items, {} profession tools.", questNeededItems.size(), startingGearItems.size(), professionToolItems.size());
}

class ChallengeModes_WorldScript : public WorldScript
{
public:
    ChallengeModes_WorldScript()
        : WorldScript("ChallengeModes_WorldScript")
    {}

    void OnBeforeConfigLoad(bool reload) override
    {
        LoadConfig();
        // The table can only appear with a build and restart, so a reload keeps the startup result and only re-checks the keys.
        if (reload)
            sChallengeModes->validateTcgCodeRewards();
    }

    void OnStartup() override
    {
        sChallengeModes->buildStartupData();
        sChallengeModes->checkTcgCodesTable();
        sChallengeModes->validateTcgCodeRewards();
    }

private:
    static std::vector<std::vector<std::string>> SplitEntries(std::string const& configString)
    {
        std::vector<std::vector<std::string>> entries;
        std::string delimitedValue;
        std::stringstream configIdStream(configString);
        // Entries are separated by commas; the tokens inside an entry by spaces
        while (std::getline(configIdStream, delimitedValue, ','))
        {
            std::stringstream entryStream(delimitedValue);
            std::vector<std::string> tokens;
            std::string token;
            while (entryStream >> token)
                tokens.push_back(token);
            if (!tokens.empty())
                entries.push_back(tokens);
        }
        return entries;
    }

    static uint32 ToUInt(std::string const& text)
    {
        return uint32(std::strtoul(text.c_str(), nullptr, 10));
    }

    static void LoadLevelList(std::map<uint8, std::vector<uint32>>& target, std::string const& configString)
    {
        target.clear();
        for (auto const& tokens : SplitEntries(configString))
            if (tokens.size() >= 2)
                target[uint8(ToUInt(tokens[0]))].push_back(ToUInt(tokens[1]));
    }

    static void LoadLevelValue(std::map<uint8, uint32>& target, std::string const& configString)
    {
        target.clear();
        for (auto const& tokens : SplitEntries(configString))
            if (tokens.size() >= 2)
                target[uint8(ToUInt(tokens[0]))] += ToUInt(tokens[1]);
    }

    static void LoadItems(std::map<uint8, std::vector<ChallengeItemReward>>& target, std::string const& configString, uint32 defaultCount)
    {
        target.clear();
        for (auto const& tokens : SplitEntries(configString))
        {
            if (tokens.size() < 2)
                continue;
            uint32 count = tokens.size() >= 3 ? ToUInt(tokens[2]) : defaultCount;
            target[uint8(ToUInt(tokens[0]))].push_back({ ToUInt(tokens[1]), count ? count : 1 });
        }
    }

    static void LoadTcgCodes(std::map<uint8, std::vector<std::string>>& target, std::string const& configString, std::string const& confName)
    {
        target.clear();
        for (auto const& tokens : SplitEntries(configString))
        {
            if (tokens.size() < 2)
                continue;
            uint8 level = uint8(ToUInt(tokens[0]));
            if (!level || !IsValidTcgGroupKey(tokens[1]))
            {
                LOG_ERROR("module", "mod-challenge-modes: skipped {}.TcgCodeRewards entry \"{} {}\": the format is \"level REWARD_GROUP\" with a key of A-Z, 0-9, and _.", confName, tokens[0], tokens[1]);
                continue;
            }
            target[level].push_back(tokens[1]);
        }
    }

    static void LoadIdSet(std::unordered_set<uint32>& target, std::string const& configString)
    {
        target.clear();
        std::string normalized = configString;
        for (char& c : normalized)
            if (c == ',')
                c = ' ';
        std::stringstream stream(normalized);
        std::string token;
        while (stream >> token)
            if (uint32 id = ToUInt(token))
                target.insert(id);
    }

    static void LoadDefinition(ChallengeModeSettings setting, std::string const& confName, std::string const& displayName, bool defaultEnable, uint32 defaultDisableLevel, float defaultXp)
    {
        ChallengeDefinition& def = sChallengeModes->definitions[setting];
        def.confName = confName;
        def.displayName = displayName;
        def.enable = sConfigMgr->GetOption<bool>(confName + ".Enable", defaultEnable);
        def.disableLevel = sConfigMgr->GetOption<uint32>(confName + ".DisableLevel", defaultDisableLevel);
        def.xpMultiplier = sConfigMgr->GetOption<float>(confName + ".XPMultiplier", defaultXp);
        def.itemRewardAmount = sConfigMgr->GetOption<uint32>(confName + ".ItemRewardAmount", 1);
        LoadLevelList(def.titleRewards, sConfigMgr->GetOption<std::string>(confName + ".TitleRewards", ""));
        LoadLevelValue(def.talentRewards, sConfigMgr->GetOption<std::string>(confName + ".TalentRewards", ""));
        LoadItems(def.itemRewards, sConfigMgr->GetOption<std::string>(confName + ".ItemRewards", ""), def.itemRewardAmount);
        LoadLevelValue(def.goldRewards, sConfigMgr->GetOption<std::string>(confName + ".GoldRewards", ""));
        LoadLevelList(def.achievementRewards, sConfigMgr->GetOption<std::string>(confName + ".AchievementReward", ""));
        LoadTcgCodes(def.tcgCodeRewards, sConfigMgr->GetOption<std::string>(confName + ".TcgCodeRewards", ""), confName);
        def.rewardMailSubject = ExpandLineBreaks(sConfigMgr->GetOption<std::string>(confName + ".RewardMail.Subject", "{MODE} reward"));
        def.rewardMailBody = ExpandLineBreaks(sConfigMgr->GetOption<std::string>(confName + ".RewardMail.Body", "Congratulations on reaching level {LEVEL} on the {MODE} challenge.{CODES}"));
    }

    static void LoadConfig()
    {
        sChallengeModes->challengesEnabled = sConfigMgr->GetOption<bool>("ChallengeModes.Enable", false);
        if (!sChallengeModes->enabled())
            return;

        sChallengeModes->groupRestriction = sConfigMgr->GetOption<bool>("ChallengeModes.GroupRestriction", true);
        sChallengeModes->enforceEquipmentEnabled = sConfigMgr->GetOption<bool>("ChallengeModes.EnforceEquipment", true);
        sChallengeModes->itemQualityMax = sConfigMgr->GetOption<uint32>("ItemQualityLevel.MaxQuality", ITEM_QUALITY_UNCOMMON);
        sChallengeModes->tcgCodesConfigured = sConfigMgr->GetOption<bool>("ChallengeModes.TcgCodes.Enable", false);
        sChallengeModes->tcgCodesTable = sConfigMgr->GetOption<std::string>("ChallengeModes.TcgCodes.Table", "account_tcg_codes");

        LoadDefinition(SETTING_HARDCORE, "Hardcore", "Hardcore", true, 80, 1.0f);
        LoadDefinition(SETTING_SEMI_HARDCORE, "SemiHardcore", "Semi-Hardcore", false, 0, 1.0f);
        LoadDefinition(SETTING_SELF_CRAFTED, "SelfCrafted", "Self Crafted", true, 80, 1.0f);
        LoadDefinition(SETTING_ITEM_QUALITY_LEVEL, "ItemQualityLevel", "Green Quality", true, 80, 1.0f);
        LoadDefinition(SETTING_SLOW_XP_GAIN, "SlowXpGain", "Slow XP", false, 0, 0.50f);
        LoadDefinition(SETTING_VERY_SLOW_XP_GAIN, "VerySlowXpGain", "Very Slow XP", false, 0, 0.25f);
        LoadDefinition(SETTING_QUEST_XP_ONLY, "QuestXpOnly", "Quest Only", true, 80, 2.0f);
        LoadDefinition(SETTING_IRON_MAN, "IronMan", "Iron Man", false, 0, 1.0f);
        LoadDefinition(SETTING_ONE_RATE, "OneRate", "One Rate", true, 80, 1.0f);
        LoadDefinition(SETTING_PROGRESSION, "Progression", "Progression", true, 0, 1.0f);

        LoadIdSet(sChallengeModes->hardcoreBlockedSpells, sConfigMgr->GetOption<std::string>("Hardcore.BlockedSpells", "19752"));
        LoadIdSet(sChallengeModes->selfCraftedExtraExemptItems, sConfigMgr->GetOption<std::string>("SelfCrafted.ExemptItems", ""));
    }
};

class ChallengeMode : public PlayerScript
{
public:
    explicit ChallengeMode(const char *scriptName,
                           ChallengeModeSettings settingName)
            : PlayerScript(scriptName), settingName(settingName)
    { }

    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* /*victim*/, uint8 /*xpSource*/) override
    {
        if (!sChallengeModes->challengeEnabledForPlayer(settingName, player))
        {
            return;
        }
        amount *= sChallengeModes->getXpBonusForChallenge(settingName);
    }

    // Rewards every level crossed (a Level-Up Token or a GM command can skip levels), then ends the challenge at its DisableLevel.
    void OnPlayerLevelChanged(Player* player, uint8 oldlevel) override
    {
        if (!sChallengeModes->challengeEnabledForPlayer(settingName, player))
        {
            return;
        }

        uint8 const level = player->GetLevel();
        for (uint32 rewardLevel = uint32(oldlevel) + 1; rewardLevel <= level; ++rewardLevel)
            sChallengeModes->grantLevelRewards(player, settingName, uint8(rewardLevel));

        uint32 const disableLevel = sChallengeModes->getDisableLevel(settingName);
        if (disableLevel && disableLevel <= level)
        {
            player->UpdatePlayerSetting(SETTING_SOURCE, settingName, 0);
            sChallengeModes->markCompleted(player, settingName);
            SendSys(player, "Challenge complete: " + sChallengeModes->getDisplayName(settingName) + ". Its restrictions are lifted.");
            if (!sChallengeModes->hasRestrictingChallenge(player))
                SendSys(player, "You have no challenges left that limit grouping. You can group with anyone.");
        }
    }

protected:
    ChallengeModeSettings settingName;
};

class ChallengeMode_Hardcore : public ChallengeMode
{
public:
    ChallengeMode_Hardcore() : ChallengeMode("ChallengeMode_Hardcore", SETTING_HARDCORE) {}

    static void KickDead(Player* player)
    {
        if (player->GetSession())
            player->GetSession()->KickPlayer(std::string("This Hardcore character has died and can no longer be played."));
    }

    void OnPlayerLogin(Player* player) override
    {
        if (!sChallengeModes->challengeEnabledForPlayer(SETTING_HARDCORE, player) || !player->GetPlayerSetting(SETTING_SOURCE, HARDCORE_DEAD).value)
        {
            return;
        }
        player->KillPlayer();
        KickDead(player);
    }

    void OnPlayerJustDied(Player* player) override
    {
        if (!sChallengeModes->challengeEnabledForPlayer(SETTING_HARDCORE, player))
        {
            return;
        }
        player->UpdatePlayerSetting(SETTING_SOURCE, HARDCORE_DEAD, 1);
        SendSys(player, "Your Hardcore character has died. This character can no longer be played.");
    }

    void OnPlayerReleasedGhost(Player* player) override
    {
        if (!sChallengeModes->challengeEnabledForPlayer(SETTING_HARDCORE, player))
        {
            return;
        }
        player->UpdatePlayerSetting(SETTING_SOURCE, HARDCORE_DEAD, 1);
        KickDead(player);
    }

    void OnPlayerPVPKill(Player* /*killer*/, Player* killed) override
    {
        if (!sChallengeModes->challengeEnabledForPlayer(SETTING_HARDCORE, killed))
        {
            return;
        }
        killed->UpdatePlayerSetting(SETTING_SOURCE, HARDCORE_DEAD, 1);
    }

    void OnPlayerKilledByCreature(Creature* /*killer*/, Player* killed) override
    {
        if (!sChallengeModes->challengeEnabledForPlayer(SETTING_HARDCORE, killed))
        {
            return;
        }
        killed->UpdatePlayerSetting(SETTING_SOURCE, HARDCORE_DEAD, 1);
    }

    // Last line of defense: resurrection spells are refused in ChallengeModes_SpellScript, but direct revives (GM .revive, bot module code) still land here.
    void OnPlayerResurrect(Player* player, float /*restore_percent*/, bool& /*applySickness*/) override
    {
        if (!sChallengeModes->challengeEnabledForPlayer(SETTING_HARDCORE, player))
        {
            return;
        }
        player->UpdatePlayerSetting(SETTING_SOURCE, HARDCORE_DEAD, 1);
        player->KillPlayer();
        KickDead(player);
    }
};

class ChallengeMode_SemiHardcore : public ChallengeMode
{
public:
    ChallengeMode_SemiHardcore() : ChallengeMode("ChallengeMode_SemiHardcore", SETTING_SEMI_HARDCORE) {}

    void OnPlayerKilledByCreature(Creature* /*killer*/, Player* player) override
    {
        if (!sChallengeModes->challengeEnabledForPlayer(SETTING_SEMI_HARDCORE, player))
        {
            return;
        }
        for (uint8 i = 0; i < EQUIPMENT_SLOT_END; ++i)
        {
            if (Item* pItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            {
                if (pItem->GetTemplate() && !pItem->IsEquipped())
                    continue;
                uint8 slot = pItem->GetSlot();
                SendSys(player, "You have lost your [" + pItem->GetTemplate()->Name1 + "].");
                player->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
            }
        }
        player->SetMoney(0);
    }
};

// Self Crafted and Green Quality share one equip gate so both rules are checked once and in the same way.
class ChallengeMode_Equipment : public ChallengeMode
{
public:
    ChallengeMode_Equipment(const char* scriptName, ChallengeModeSettings setting) : ChallengeMode(scriptName, setting) {}

    bool OnPlayerCanEquipItem(Player* player, uint8 /*slot*/, uint16& /*dest*/, Item* pItem, bool /*swap*/, bool /*not_loading*/) override
    {
        if (!sChallengeModes->challengeEnabledForPlayer(settingName, player))
        {
            return true;
        }
        return sChallengeModes->isEquipAllowed(player, pItem);
    }

    // Safety net for code that equips without asking CanEquipItem first (playerbots autogear and bot factory gear).
    void OnPlayerEquip(Player* player, Item* item, uint8 /*bag*/, uint8 /*slot*/, bool /*update*/) override
    {
        if (!sChallengeModes->enforceEquipmentEnabled || !sChallengeModes->challengeEnabledForPlayer(settingName, player))
        {
            return;
        }
        if (!sChallengeModes->isEquipAllowed(player, item))
        {
            sChallengeModes->scheduleEquipmentCheck(player);
        }
    }
};

class ChallengeMode_SelfCrafted : public ChallengeMode_Equipment
{
public:
    ChallengeMode_SelfCrafted() : ChallengeMode_Equipment("ChallengeMode_SelfCrafted", SETTING_SELF_CRAFTED) {}
};

class ChallengeMode_ItemQualityLevel : public ChallengeMode_Equipment
{
public:
    ChallengeMode_ItemQualityLevel() : ChallengeMode_Equipment("ChallengeMode_ItemQualityLevel", SETTING_ITEM_QUALITY_LEVEL) {}
};

class ChallengeMode_SlowXpGain : public ChallengeMode
{
public:
    ChallengeMode_SlowXpGain() : ChallengeMode("ChallengeMode_SlowXpGain", SETTING_SLOW_XP_GAIN) {}
};

class ChallengeMode_VerySlowXpGain : public ChallengeMode
{
public:
    ChallengeMode_VerySlowXpGain() : ChallengeMode("ChallengeMode_VerySlowXpGain", SETTING_VERY_SLOW_XP_GAIN) {}
};

// One Rate: x1 XP. The rate itself needs no code here (the server rates are 1); mod-individual-xp reads this setting and locks .xp while it is active.
class ChallengeMode_OneRate : public ChallengeMode
{
public:
    ChallengeMode_OneRate() : ChallengeMode("ChallengeMode_OneRate", SETTING_ONE_RATE) {}
};

// Progression: the setting is what mod-individual-progression reads. It never ends at a level (DisableLevel 0), but its level rewards still pay out.
class ChallengeMode_Progression : public ChallengeMode
{
public:
    ChallengeMode_Progression() : ChallengeMode("ChallengeMode_Progression", SETTING_PROGRESSION) {}
};

class ChallengeMode_QuestXpOnly : public ChallengeMode
{
public:
    ChallengeMode_QuestXpOnly() : ChallengeMode("ChallengeMode_QuestXpOnly", SETTING_QUEST_XP_ONLY) {}

    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* /*victim*/, uint8 xpSource) override
    {
        if (!sChallengeModes->challengeEnabledForPlayer(SETTING_QUEST_XP_ONLY, player))
        {
            return;
        }
        if (xpSource == XPSOURCE_KILL)
        {
            // The pet keeps its kill XP. The core pays the pet from the same variable after this hook, so pay it here before zeroing (same 50% split in a group as the core).
            if (Pet* pet = player->GetPet())
                pet->GivePetXP(player->GetGroup() ? amount / 2 : amount);
            amount = 0;
            return;
        }
        // Quests, exploration, battlegrounds, and dungeon finder rewards: x2, or x1 when One Rate is also active
        if (sChallengeModes->challengeEnabledForPlayer(SETTING_ONE_RATE, player))
        {
            return;
        }
        amount *= sChallengeModes->getXpBonusForChallenge(SETTING_QUEST_XP_ONLY);
    }
};

class ChallengeMode_IronMan : public ChallengeMode
{
public:
    ChallengeMode_IronMan() : ChallengeMode("ChallengeMode_IronMan", SETTING_IRON_MAN) {}

    void OnPlayerResurrect(Player* player, float /*restore_percent*/, bool& /*applySickness*/) override
    {
        if (!sChallengeModes->challengeEnabledForPlayer(SETTING_IRON_MAN, player))
        {
            return;
        }
        player->KillPlayer();
    }

    void OnPlayerLevelChanged(Player* player, uint8 oldlevel) override
    {
        if (!sChallengeModes->challengeEnabledForPlayer(SETTING_IRON_MAN, player))
        {
            return;
        }
        player->SetFreeTalentPoints(0); // Remove all talent points
        ChallengeMode::OnPlayerLevelChanged(player, oldlevel);
    }

    void OnPlayerTalentsReset(Player* player, bool /*noCost*/) override
    {
        if (!sChallengeModes->challengeEnabledForPlayer(SETTING_IRON_MAN, player))
        {
            return;
        }
        player->SetFreeTalentPoints(0); // Remove all talent points
    }

    bool OnPlayerCanEquipItem(Player* player, uint8 /*slot*/, uint16& /*dest*/, Item* pItem, bool /*swap*/, bool /*not_loading*/) override
    {
        if (!sChallengeModes->challengeEnabledForPlayer(SETTING_IRON_MAN, player))
        {
            return true;
        }
        return pItem->GetTemplate()->Quality <= ITEM_QUALITY_NORMAL;
    }

    bool OnPlayerCanApplyEnchantment(Player* player, Item* /*item*/, EnchantmentSlot /*slot*/, bool /*apply*/, bool /*apply_dur*/, bool /*ignore_condition*/) override
    {
        if (!sChallengeModes->challengeEnabledForPlayer(SETTING_IRON_MAN, player))
        {
            return true;
        }
        return false;
    }

    void OnPlayerLearnSpell(Player* player, uint32 spellID) override
    {
        if (!sChallengeModes->challengeEnabledForPlayer(SETTING_IRON_MAN, player))
        {
            return;
        }
        // These professions are class skills so they are always acceptable
        switch (spellID)
        {
            case RUNEFORGING:
            case POISONS:
            case BEAST_TRAINING:
                return;
            default:
                break;
        }
        // Do not allow learning any trade skills
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellID);
        if (!spellInfo)
            return;
        bool shouldForget = false;
        for (uint8 i = 0; i < 3; i++)
        {
            if (spellInfo->Effects[i].Effect == SPELL_EFFECT_TRADE_SKILL)
            {
                shouldForget = true;
            }
        }
        if (shouldForget)
        {
            player->removeSpell(spellID, SPEC_MASK_ALL, false);
        }
    }

    bool OnPlayerCanUseItem(Player* player, ItemTemplate const* proto, InventoryResult& /*result*/) override
    {
        if (!sChallengeModes->challengeEnabledForPlayer(SETTING_IRON_MAN, player))
        {
            return true;
        }
        // Do not allow using elixir, potion, or flask
        if (proto->Class == ITEM_CLASS_CONSUMABLE &&
                (proto->SubClass == ITEM_SUBCLASS_POTION ||
                proto->SubClass == ITEM_SUBCLASS_ELIXIR ||
                proto->SubClass == ITEM_SUBCLASS_FLASK))
        {
            return false;
        }
        // Do not allow food that gives food buffs
        if (proto->Class == ITEM_CLASS_CONSUMABLE && proto->SubClass == ITEM_SUBCLASS_FOOD)
        {
            for (const auto & Spell : proto->Spells)
            {
                SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(Spell.SpellId);
                if (!spellInfo)
                    continue;

                for (uint8 i = 0; i < 3; i++)
                {
                    if (spellInfo->Effects[i].ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL)
                    {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    bool OnPlayerCanGroupInvite(Player* player, std::string& /*membername*/) override
    {
        return !sChallengeModes->challengeEnabledForPlayer(SETTING_IRON_MAN, player);
    }

    bool OnPlayerCanGroupAccept(Player* player, Group* /*group*/) override
    {
        return !sChallengeModes->challengeEnabledForPlayer(SETTING_IRON_MAN, player);
    }
};

// Login line, grouping rule, and dungeon finder rule (Tofu Worldview)
class ChallengeModes_PlayerRules : public PlayerScript
{
public:
    ChallengeModes_PlayerRules() : PlayerScript("ChallengeModes_PlayerRules") {}

    void OnPlayerLogin(Player* player) override
    {
        if (!sChallengeModes->enabled())
            return;
        std::string const list = sChallengeModes->activeChallengeList(player);
        if (!list.empty())
            SendSys(player, "Active challenges: " + list + ".");
        if (sChallengeModes->enforceEquipmentEnabled && (sChallengeModes->challengeEnabledForPlayer(SETTING_SELF_CRAFTED, player) || sChallengeModes->challengeEnabledForPlayer(SETTING_ITEM_QUALITY_LEVEL, player)))
            sChallengeModes->scheduleEquipmentCheck(player);
    }

    bool OnPlayerCanGroupInvite(Player* player, std::string& membername) override
    {
        if (!sChallengeModes->enabled() || !sChallengeModes->groupRestriction)
            return true;
        Player* invitee = ObjectAccessor::FindPlayerByName(membername, false);
        if (!invitee)
            return true; // the core reports the missing player itself
        if (sChallengeModes->hasRestrictingChallenge(player) == sChallengeModes->hasRestrictingChallenge(invitee))
            return true;
        SendSys(player, NO_GROUP_MESSAGE);
        return false;
    }

    bool OnPlayerCanGroupAccept(Player* player, Group* group) override
    {
        if (!sChallengeModes->enabled() || !sChallengeModes->groupRestriction || !group || group->isBGGroup() || group->isBFGroup())
            return true;
        bool const restricted = sChallengeModes->hasRestrictingChallenge(player);
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == player)
                continue;
            if (sChallengeModes->hasRestrictingChallenge(member) != restricted)
            {
                SendSys(player, NO_GROUP_MESSAGE);
                return false;
            }
        }
        return true;
    }

    bool OnPlayerCanJoinLfg(Player* player, uint8 /*roles*/, std::set<uint32>& /*dungeons*/, std::string const& /*comment*/) override
    {
        if (!sChallengeModes->enabled() || !sChallengeModes->groupRestriction)
            return true;
        Group* group = player->GetGroup();
        // A group the dungeon finder already formed is continuing its run: it was checked when it formed
        if (group && group->isLFGGroup())
            return true;
        bool anyRestricted = sChallengeModes->hasRestrictingChallenge(player);
        if (group)
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource())
                    anyRestricted = anyRestricted || sChallengeModes->hasRestrictingChallenge(member);
        }
        if (!anyRestricted)
            return true;
        if (group && group->GetMembersCount() >= MAXGROUPSIZE)
            return true; // a full party of challenge characters (the grouping rule guarantees every member is one)
        SendSys(player, "Characters on a challenge (other than Progression) can only use the Dungeon Finder as a full party of five challenge characters, because the Dungeon Finder fills empty slots with other players and bots.");
        return false;
    }
};

// Catches group joins that skip the invite handlers (playerbots commands, random bot invites, scripts) and removes the member who breaks the grouping rule.
class ChallengeModes_GroupScript : public GroupScript
{
public:
    ChallengeModes_GroupScript() : GroupScript("ChallengeModes_GroupScript") {}

    void OnAddMember(Group* group, ObjectGuid guid) override
    {
        if (!sChallengeModes->enabled() || !sChallengeModes->groupRestriction || !group || group->isBGGroup() || group->isBFGroup())
            return;
        Player* added = ObjectAccessor::FindConnectedPlayer(guid);
        if (!added)
            return;
        bool const restricted = sChallengeModes->hasRestrictingChallenge(added);
        bool mismatch = false;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == added)
                continue;
            if (sChallengeModes->hasRestrictingChallenge(member) != restricted)
            {
                mismatch = true;
                break;
            }
        }
        if (!mismatch)
            return;

        // Removing a member from inside AddMember is unsafe, so it happens on the next update of the added player.
        ObjectGuid const leaderGuid = group->GetLeaderGUID();
        added->m_Events.AddEventAtOffset([guid, leaderGuid]()
        {
            Player* target = ObjectAccessor::FindConnectedPlayer(guid);
            if (!target)
                return;
            Group* current = target->GetGroup();
            if (!current || current->isBGGroup() || current->isBFGroup())
                return;
            current->RemoveMember(guid, GROUP_REMOVEMETHOD_DEFAULT);
            SendSys(target, NO_GROUP_MESSAGE);
            if (leaderGuid != guid)
                if (Player* leader = ObjectAccessor::FindConnectedPlayer(leaderGuid))
                    SendSys(leader, target->GetName() + " was removed from your group. " + NO_GROUP_MESSAGE);
        }, 100ms);
    }
};

// Hardcore spell rules: nobody can resurrect a Hardcore character, and a Hardcore character cannot cast spells that kill the caster (Divine Intervention by default).
class ChallengeModes_SpellScript : public AllSpellScript
{
public:
    ChallengeModes_SpellScript() : AllSpellScript("ChallengeModes_SpellScript") {}

    static Player* ResurrectTarget(Spell* spell)
    {
        if (Unit* unit = spell->m_targets.GetUnitTarget())
            if (Player* player = unit->ToPlayer())
                return player;
        if (Corpse* corpse = spell->m_targets.GetCorpseTarget())
            return ObjectAccessor::FindPlayer(corpse->GetOwnerGUID());
        return nullptr;
    }

    void OnSpellCheckCast(Spell* spell, bool /*strict*/, SpellCastResult& res) override
    {
        if (res != SPELL_CAST_OK || !sChallengeModes->enabled() || !spell)
            return;
        SpellInfo const* info = spell->GetSpellInfo();
        if (!info)
            return;
        Player* caster = spell->GetCaster() ? spell->GetCaster()->ToPlayer() : nullptr;

        if (caster && sChallengeModes->challengeEnabledForPlayer(SETTING_HARDCORE, caster))
        {
            if (sChallengeModes->hardcoreBlockedSpells.count(info->Id))
            {
                SendSys(caster, "Hardcore characters cannot cast a spell that would kill themselves.");
                res = SPELL_FAILED_DONT_REPORT;
                return;
            }
            if (info->HasEffect(SPELL_EFFECT_SELF_RESURRECT))
            {
                res = SPELL_FAILED_DONT_REPORT;
                return;
            }
        }

        if (info->HasEffect(SPELL_EFFECT_RESURRECT) || info->HasEffect(SPELL_EFFECT_RESURRECT_NEW))
        {
            Player* target = ResurrectTarget(spell);
            if (target && sChallengeModes->challengeEnabledForPlayer(SETTING_HARDCORE, target))
            {
                if (caster)
                    SendSys(caster, target->GetName() + " is a Hardcore character and cannot be resurrected.");
                res = SPELL_FAILED_TARGET_CANNOT_BE_RESURRECTED;
            }
        }
    }
};

class gobject_challenge_modes : public GameObjectScript
{
private:
    static bool playerSettingEnabled(Player* player, uint8 settingIndex)
    {
        return player->GetPlayerSetting(SETTING_SOURCE, settingIndex).value;
    }

    static std::string Describe(ChallengeModeSettings setting)
    {
        switch (setting)
        {
            case SETTING_HARDCORE:
                return "Hardcore: if you die, this character is dead for good. Ends at level 80.";
            case SETTING_SELF_CRAFTED:
                return "Self Crafted: you can only equip items you crafted yourself. Exempt: items needed for a quest, starting weapons and gear, tools such as fishing poles, shirts, tabards, and bags. Ends at level 80.";
            case SETTING_ITEM_QUALITY_LEVEL:
                return "Green Quality: you can only equip items of green (Uncommon) quality or lower. Ends at level 80.";
            case SETTING_ONE_RATE:
                return "One Rate: you level at x1 XP, and your personal XP rate (.xp) is locked. Ends at level 80.";
            case SETTING_QUEST_XP_ONLY:
                return "Quest Only: kills give you no XP (your pet still gets its kill XP). Quests, exploration, and other XP are doubled, or x1 if One Rate is also on. Your personal XP rate (.xp) is locked. Ends at level 80.";
            case SETTING_PROGRESSION:
                return "Progression: the world opens up raid tier by raid tier, starting in Classic, and your level cap rises as you clear each tier. Rewards at level 80. Ends when you clear the final raid tier.";
            case SETTING_SEMI_HARDCORE:
                return "Semi-Hardcore: dying destroys your worn gear and gold.";
            case SETTING_SLOW_XP_GAIN:
                return "Slow XP: you receive reduced XP.";
            case SETTING_VERY_SLOW_XP_GAIN:
                return "Very Slow XP: you receive greatly reduced XP.";
            case SETTING_IRON_MAN:
                return "Iron Man: white gear only, no talents, professions, or grouping.";
            default:
                return "";
        }
    }

    static bool Offerable(Player* player, ChallengeModeSettings setting)
    {
        if (!sChallengeModes->challengeEnabled(setting) || playerSettingEnabled(player, setting))
            return false;
        // Combinations the original module forbids
        if ((setting == SETTING_HARDCORE || setting == SETTING_SEMI_HARDCORE) && (playerSettingEnabled(player, SETTING_HARDCORE) || playerSettingEnabled(player, SETTING_SEMI_HARDCORE)))
            return false;
        if ((setting == SETTING_SLOW_XP_GAIN || setting == SETTING_VERY_SLOW_XP_GAIN) && (playerSettingEnabled(player, SETTING_SLOW_XP_GAIN) || playerSettingEnabled(player, SETTING_VERY_SLOW_XP_GAIN)))
            return false;
        if ((setting == SETTING_SELF_CRAFTED && playerSettingEnabled(player, SETTING_IRON_MAN)) || (setting == SETTING_IRON_MAN && playerSettingEnabled(player, SETTING_SELF_CRAFTED)))
            return false;
        return true;
    }

public:
    gobject_challenge_modes() : GameObjectScript("gobject_challenge_modes") { }

    struct gobject_challenge_modesAI: GameObjectAI
    {
        explicit gobject_challenge_modesAI(GameObject* object) : GameObjectAI(object) { };

        bool CanBeSeen(Player const* player) override
        {
            if ((player->GetLevel() > 1 && player->getClass() != CLASS_DEATH_KNIGHT) || (player->GetLevel() > 55))
            {
                return false;
            }
            return sChallengeModes->enabled();
        }
    };

    bool OnGossipHello(Player* player, GameObject* go) override
    {
        std::string const list = sChallengeModes->activeChallengeList(player);
        SendSys(player, list.empty() ? "You have no challenges active." : "Active challenges: " + list + ".");
        for (ChallengeModeSettings setting : ALL_CHALLENGES)
        {
            if (!Offerable(player, setting))
                continue;
            std::string popup = Describe(setting) + "\n\nThis cannot be undone.";
            if (setting != SETTING_PROGRESSION)
                popup += " While it is active you can only group with other challenge characters.";
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Enable " + sChallengeModes->getDisplayName(setting), 0, setting, popup, 0, false);
        }
        SendGossipMenuFor(player, 12669, go->GetGUID());
        return true;
    }

    bool OnGossipSelect(Player* player, GameObject* /*go*/, uint32 /*sender*/, uint32 action) override
    {
        CloseGossipMenuFor(player);
        if (!IsChallengeIndex(uint8(action)) || !Offerable(player, ChallengeModeSettings(action)))
            return true;
        ChallengeModeSettings const setting = ChallengeModeSettings(action);

        // Taking a restricting challenge while grouped with unrestricted characters would break the grouping rule
        if (setting != SETTING_PROGRESSION && sChallengeModes->groupRestriction && !sChallengeModes->hasRestrictingChallenge(player))
        {
            if (Group* group = player->GetGroup())
            {
                if (!group->isBGGroup() && !group->isBFGroup() && group->GetMembersCount() > 1)
                {
                    SendSys(player, "Leave your group before taking this challenge: challenge characters can only group with other challenge characters.");
                    return true;
                }
            }
        }

        if (setting == SETTING_PROGRESSION)
        {
#ifdef TOFU_HAS_INDIVIDUAL_PROGRESSION
            sIndividualProgression->SetProgressionMode(player, true);
#else
            player->UpdatePlayerSetting(SETTING_SOURCE, SETTING_PROGRESSION, 1);
#endif
        }
        else
        {
            player->UpdatePlayerSetting(SETTING_SOURCE, setting, 1);
        }

        SendSys(player, "Challenge enabled: " + sChallengeModes->getDisplayName(setting) + ".");
        if ((setting == SETTING_SELF_CRAFTED || setting == SETTING_ITEM_QUALITY_LEVEL) && sChallengeModes->enforceEquipmentEnabled)
            sChallengeModes->scheduleEquipmentCheck(player);
        return true;
    }

    GameObjectAI* GetAI(GameObject* object) const override
    {
        return new gobject_challenge_modesAI(object);
    }
};

// Add all scripts in one
void AddSC_mod_challenge_modes()
{
    new ChallengeModes_WorldScript();
    new gobject_challenge_modes();
    new ChallengeMode_Hardcore();
    new ChallengeMode_SemiHardcore();
    new ChallengeMode_SelfCrafted();
    new ChallengeMode_ItemQualityLevel();
    new ChallengeMode_SlowXpGain();
    new ChallengeMode_VerySlowXpGain();
    new ChallengeMode_QuestXpOnly();
    new ChallengeMode_IronMan();
    new ChallengeMode_OneRate();
    new ChallengeMode_Progression();
    new ChallengeModes_PlayerRules();
    new ChallengeModes_GroupScript();
    new ChallengeModes_SpellScript();
}
