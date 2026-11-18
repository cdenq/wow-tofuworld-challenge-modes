#ifndef AZEROTHCORE_CHALLENGEMODES_H
#define AZEROTHCORE_CHALLENGEMODES_H

#include "ScriptMgr.h"
#include "Player.h"
#include "Config.h"
#include "Chat.h"
#include "ScriptedCreature.h"
#include "ScriptedGossip.h"
#include "SpellMgr.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "GameObjectAI.h"
#include "Pet.h"
#include <array>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

// Player setting indexes under the "mod-challenge-modes" source. Never renumber: the values are stored per character in character_settings.
enum ChallengeModeSettings
{
    SETTING_HARDCORE           = 0,
    SETTING_SEMI_HARDCORE      = 1, // retired on Tofu Worldview (SemiHardcore.Enable = 0)
    SETTING_SELF_CRAFTED       = 2,
    SETTING_ITEM_QUALITY_LEVEL = 3, // shown in game as "Green Quality"
    SETTING_SLOW_XP_GAIN       = 4, // retired on Tofu Worldview
    SETTING_VERY_SLOW_XP_GAIN  = 5, // retired on Tofu Worldview
    SETTING_QUEST_XP_ONLY      = 6, // shown in game as "Quest Only"
    SETTING_IRON_MAN           = 7, // retired on Tofu Worldview
    HARDCORE_DEAD              = 8, // state flag, not a challenge
    SETTING_ONE_RATE           = 9, // Tofu Worldview
    SETTING_PROGRESSION        = 10, // Tofu Worldview: read by mod-individual-progression (IndividualProgression.Tofu.ModeSettingIndex)
    SETTING_COMPLETED          = 11  // state flag: bitmask (1 << challenge index) of challenges finished at their DisableLevel
};

constexpr uint8 CHALLENGE_MODE_COUNT = 11; // indexes 0 to 10; HARDCORE_DEAD (8) is skipped wherever modes are iterated

enum AllowedProfessions
{
    RUNEFORGING    = 53428,
    POISONS        = 2842,
    BEAST_TRAINING = 5149
};

struct ChallengeItemReward
{
    uint32 itemId;
    uint32 count;
};

// Everything the conf file says about one challenge. Keys are "<ConfName>.<Option>", for example "Hardcore.TitleRewards".
struct ChallengeDefinition
{
    std::string confName;
    std::string displayName;
    bool enable = false;
    uint32 disableLevel = 0;
    float xpMultiplier = 1.0f;
    uint32 itemRewardAmount = 1;
    std::map<uint8, std::vector<uint32>> titleRewards;
    std::map<uint8, uint32> talentRewards;
    std::map<uint8, std::vector<ChallengeItemReward>> itemRewards;
    std::map<uint8, uint32> goldRewards;
    std::map<uint8, std::vector<uint32>> achievementRewards;
    std::map<uint8, std::vector<std::string>> tcgCodeRewards; // mod-tcg-vendors reward_group keys, one code each
    std::string rewardMailSubject; // placeholders {MODE} and {LEVEL}
    std::string rewardMailBody; // placeholders {MODE}, {LEVEL}, and {CODES}
};

class ChallengeModes
{
public:
    static ChallengeModes* instance();

    bool challengesEnabled = false;
    bool groupRestriction = true;
    bool enforceEquipmentEnabled = true;
    uint32 itemQualityMax = ITEM_QUALITY_UNCOMMON;
    bool tcgCodesConfigured = false; // ChallengeModes.TcgCodes.Enable
    bool tcgCodesAvailable = false; // the codes table was found at startup
    std::string tcgCodesTable = "account_tcg_codes";
    std::array<ChallengeDefinition, CHALLENGE_MODE_COUNT> definitions;
    std::unordered_set<uint32> hardcoreBlockedSpells;
    std::unordered_set<uint32> selfCraftedExtraExemptItems;
    std::unordered_set<uint32> questNeededItems; // filled at startup from quest_template
    std::unordered_set<uint32> startingGearItems; // filled at startup from CharStartOutfit.dbc and playercreateinfo_item
    std::unordered_set<uint32> professionToolItems; // filled at startup from the Totem fields of every spell

    [[nodiscard]] bool enabled() const { return challengesEnabled; }
    [[nodiscard]] bool challengeEnabled(ChallengeModeSettings setting) const;
    [[nodiscard]] uint32 getDisableLevel(ChallengeModeSettings setting) const;
    [[nodiscard]] float getXpBonusForChallenge(ChallengeModeSettings setting) const;
    bool challengeEnabledForPlayer(ChallengeModeSettings setting, Player* player) const;
    [[nodiscard]] ChallengeDefinition const* getDefinition(ChallengeModeSettings setting) const;
    [[nodiscard]] std::string const& getDisplayName(ChallengeModeSettings setting) const;

    // Tofu Worldview rules shared by several scripts
    bool hasRestrictingChallenge(Player* player) const; // any active challenge other than Progression
    bool hasPersonalXpLock(Player* player) const; // One Rate or Quest Only active
    std::string activeChallengeList(Player* player) const;
    bool isSelfCraftedExempt(ItemTemplate const* proto) const;
    bool isEquipAllowed(Player* player, Item* item, std::string* reason = nullptr) const;
    void enforceEquipment(Player* player) const;
    void scheduleEquipmentCheck(Player* player) const;
    bool isCompleted(Player* player, ChallengeModeSettings setting) const;
    void markCompleted(Player* player, ChallengeModeSettings setting) const;
    void grantLevelRewards(Player* player, ChallengeModeSettings setting, uint8 level) const;
    void buildStartupData();
    [[nodiscard]] bool tcgCodesActive() const { return tcgCodesConfigured && tcgCodesAvailable; }
    void checkTcgCodesTable(); // startup only: looks for the codes table in the characters database
    void validateTcgCodeRewards() const; // startup and config reload: logs malformed or unknown reward_group keys
};

#define sChallengeModes ChallengeModes::instance()

#endif //AZEROTHCORE_CHALLENGEMODES_H
