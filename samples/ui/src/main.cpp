#include <SDL3/SDL_main.h>
#include <SDL3/SDL.h>
#include <RmlUi/Core.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <random>
#include <string>
#include <vector>
#include "command_line.h"
#include "engine.h"
#include "file_paths.h"
#include "logger.h"

namespace
{
    constexpr float kPi = 3.14159265358979f;
    // Static game data

    enum ItemId
    {
        Item_Blaster,
        Item_Rifle,
        Item_Medkit,
        Item_Stim,
        Item_Cell,
        Item_Scrap,
        Item_Chip,
        Item_Keycard,
        Item_Grenade,
        Item_ShieldCap,
        Item_Credits,
        Item_Count,
    };

    struct ItemDef
    {
        const char* id; // for the /give chat command
        const char* name;
        const char* icon; // content-root path: RmlUi's drag clone has no document URL, so relative paths fail
        const char* kind; // weapon | consumable | material | misc (inventory filters)
        const char* rarity; // common | rare | epic | legendary
        const char* stat;
        const char* desc;
        float weight;
        int maxStack;
        bool usable;
    };

    const ItemDef kItems[Item_Count] = {
        { "blaster", "M-7 Blaster", "/ui/icons/blaster.bmp", "weapon", "rare", "DMG 24  ·  RPM 300  ·  MAG 12", "Standard issue sidearm. Reliable, loud, and slightly scorched.", 2.5f, 1, false },
        { "rifle", "Arc Pulse Rifle", "/ui/icons/rifle.bmp", "weapon", "epic", "DMG 41  ·  RPM 540  ·  MAG 30", "Fires bolts of ionized plasma. Do not lick the barrel.", 6.0f, 1, false },
        { "medkit", "Medkit", "/ui/icons/medkit.bmp", "consumable", "common", "Restores 40 health", "Bandages, gel and a strongly worded leaflet about safety.", 0.8f, 5, true },
        { "stim", "Adrenal Stim", "/ui/icons/stim.bmp", "consumable", "rare", "Restores 20 health and 30 shield", "Tastes like copper and bad decisions.", 0.2f, 5, true },
        { "cell", "Energy Cell", "/ui/icons/battery.bmp", "consumable", "common", "+24 blaster ammo", "A rechargeable cell for M-series weapons.", 0.4f, 10, true },
        { "scrap", "Scrap Metal", "/ui/icons/scrap.bmp", "material", "common", "Crafting material", "Gears, bolts and the occasional spoon.", 0.5f, 50, false },
        { "chip", "Encrypted Chip", "/ui/icons/chip.bmp", "material", "rare", "Data: 4.2 TB (locked)", "Someone really wanted to keep this secret.", 0.1f, 10, false },
        { "keycard", "Vault Keycard", "/ui/icons/keycard.bmp", "misc", "legendary", "Opens: Relay Vault K-7", "The last keycard of its kind. Probably.", 0.1f, 1, false },
        { "grenade", "Plasma Grenade", "/ui/icons/grenade.bmp", "consumable", "rare", "Area damage 120", "Pull pin. Throw. Do not hold.", 0.6f, 4, true },
        { "shield", "Shield Capacitor", "/ui/icons/shield.bmp", "misc", "epic", "Passive: +25% shield regeneration", "Hums quietly when you're not looking.", 1.2f, 1, false },
        { "credits", "Credit Chip", "/ui/icons/credits.bmp", "misc", "common", "Worth 100 credits", "Use it to add the credits to your wallet.", 0.0f, 20, true },
    };

    enum ObjectiveId
    {
        Obj_Talk,
        Obj_Inventory,
        Obj_Fire,
        Obj_Reload,
        Obj_UseItem,
        Obj_Chat,
        Obj_Settings,
        Obj_Count,
    };

    const char* const kObjectiveText[Obj_Count] = {
        "Talk to Vex (E)",
        "Open your inventory (I)",
        "Fire your blaster (F / LMB)",
        "Reload (R)",
        "Use a medkit or a stim",
        "Send a chat message (T)",
        "Change a setting and Apply",
    };
    constexpr int kShotsForObjective = 10;

    // Dialogue tree. {name} is replaced with the player's callsign.
    enum DialogueEffect
    {
        Effect_None,
        Effect_GiveSupplies,
        Effect_AcceptQuest,
    };

    struct DialogueChoice
    {
        const char* text;
        int next; // -1 ends the conversation
    };

    struct DialogueNode
    {
        const char* text;
        DialogueEffect effect;
        std::vector<DialogueChoice> choices;
    };

    const std::vector<DialogueNode> kDialogue = {
        /* 0 */ { "{name}? About time. Base lost contact with the relay three hours ago, and you're the only pilot who answered.",
            Effect_None,
            { { "What happened to the relay?", 1 }, { "What's in it for me?", 2 }, { "I need supplies first.", 3 }, { "Later, Vex.", -1 } } },
        /* 1 */ { "Static, then nothing. Could be a storm. Could be raiders. Either way, somebody has to walk out there and look.", Effect_None, { { "I'll check it out.", 4 }, { "Sounds like a trap.", 5 }, { "Let me ask something else.", 0 } } },
        /* 2 */ { "Two hundred credits and my undying gratitude. The credits are real, at least.", Effect_None, { { "Make it three hundred.", 6 }, { "Deal.", 4 } } },
        /* 3 */ { "Take these. Two medkits and some energy cells. Try not to get them shot.", Effect_GiveSupplies, { { "Thanks, Vex.", 0 } } },
        /* 4 */ { "Good. Head north past the hangar. And {name}... keep your shields up.", Effect_AcceptQuest, { { "On my way.", -1 } } },
        /* 5 */ { "Everything out here is a trap. That's why I pay well.", Effect_None, { { "Fine. I'll go.", 4 }, { "Let me think about it.", 0 } } },
        /* 6 */ { "Two fifty, and I won't tell the captain you haggled with a robot.", Effect_None, { { "Deal.", 4 } } },
    };

    const char* const kLoadingTips[] = {
        "Every screen in this sample is an .rml file. Edit one and press F7 to reload it live.",
        "Press F8 in debug builds to open the RmlUi element inspector.",
        "The HUD health bar has a 'ghost' bar: it is just a CSS transition with a delay.",
        "Drag items in the inventory to swap or stack them.",
        "Type /help in the chat for console commands.",
        "The username field is bound two-way: rename yourself and every screen follows.",
    };

    const char* const kLoadingStages[] = { "Streaming terrain", "Compiling shaders", "Waking up Vex", "Calibrating HUD" };

    // Data model types (registered with RmlUi below)--

    struct Objective
    {
        Rml::String text;
        Rml::String progress;
        bool done = false;
    };

    struct Toast
    {
        Rml::String text;
        Rml::String kind; // info | good | warn | bad | loot (css class)
        Rml::String icon;
        float alpha = 1.0f;
        float age = 0.0f;
    };

    struct ChatLine
    {
        Rml::String who;
        Rml::String text;
        Rml::String kind; // player | npc | system | error | help (css class)
    };

    struct CompassMarker
    {
        Rml::String label;
        float bearing = 0.0f;
        float x = 0.0f; // percent of the compass width
        bool major = false;
        bool visible = false;
    };

    struct Slot
    {
        int item = -1;
        int count = 0;
        // Display copies of the item definition, so markup can read them.
        Rml::String name, icon, kind, rarity;
    };

    struct ItemDetail
    {
        bool has = false;
        int slot = -1;
        int count = 0;
        bool usable = false;
        Rml::String name, icon, kind, rarity, stat, desc;
    };

    enum class Screen
    {
        MainMenu,
        Loading,
        Playing,
    };

    enum class Overlay
    {
        None,
        Pause,
        Settings,
        Inventory,
        Dialogue,
        Chat,
    };

    constexpr int kSlotCount = 24;
    constexpr int kMagazineSize = 12;
    constexpr int kMaxHealth = 100;
    constexpr int kMaxShield = 100;
    constexpr int kXpPerLevel = 300;
    constexpr int kTrashTarget = 100; // drop_target value of the discard zone

    // Video settings as last applied, to show "not applied yet" in the settings.
    struct AppliedVideo
    {
        bool fullscreen = false;
        bool vsync = true;
        Rml::String fpsLimit = "0";
        int resScale = 100;
        int uiScale = 100;
    };

    struct Showcase
    {
        EngineApp* app = nullptr;
        Rml::DataModelHandle model;
        std::mt19937 rng{ 1234u };

        Screen screen = Screen::MainMenu;
        Overlay overlay = Overlay::None;
        Overlay settingsReturn = Overlay::None;
        bool transitionPending = false;
        Screen nextScreen = Screen::MainMenu;
        Overlay nextOverlay = Overlay::None;

        //  bound variables (names as in the documents)
        Rml::String player_name = "Nova";
        bool has_started = false;
        bool credits_open = false;
        Rml::String renderer_name;

        int load_pct = 0;
        Rml::String load_stage;
        Rml::String load_tip;

        int health = kMaxHealth;
        int shield = kMaxShield;
        int health_pct = 100;
        int shield_pct = 100;
        int xp_pct = 0;
        int level = 1;
        int score = 0;
        Rml::String difficulty_label;

        std::vector<CompassMarker> compass;
        Rml::String heading;
        std::vector<Objective> objectives;
        int objectives_done = 0;
        std::vector<Toast> toasts;
        float banner_time = 0.0f;
        float hurt_flash = 0.0f;
        float hit_flash = 0.0f;

        int ammo = kMagazineSize;
        int reserve = 48;
        bool reloading = false;
        int reload_pct = 0;

        std::vector<ChatLine> chat;
        bool chat_open = false;

        int fps = 0;
        Rml::String position;
        int shots_fired = 0;
        Rml::String play_time = "00:00";

        std::vector<Slot> slots;
        int selected_slot = -1;
        int drop_target = -1;
        Rml::String inv_filter = "all";
        ItemDetail detail;
        int credits = 250;
        Rml::String weight;
        int weight_max = 40;
        int weight_pct = 0;

        Rml::String dlg_speaker = "VEX";
        Rml::String dlg_text;
        bool dlg_typing = false;
        std::vector<Rml::String> dlg_choices;

        bool set_fullscreen = false;
        bool set_vsync = true;
        Rml::String set_fps_limit = "0";
        int set_res_scale = 100;
        int set_ui_scale = 100;
        int set_master = 80;
        int set_music = 60;
        int set_sfx = 90;
        bool set_subtitles = true;
        Rml::String set_difficulty = "normal";
        Rml::String set_crosshair = "#4ef2ff";
        int set_hud_opacity = 100;
        bool set_show_fps = true;
        bool settings_dirty = false;

        // ---- internal state ----
        AppliedVideo applied;
        float displayScale = 0.0f;
        int hoveredSlot = -1;
        int dialogueNode = 0;
        std::string dialogueFull;
        float dialogueReveal = 0.0f;
        bool suppliesGiven = false;
        bool questAccepted = false;
        float loadTime = 0.0f;
        float reloadTime = 0.0f;
        float sinceDamage = 10.0f;
        float shieldRegenCarry = 0.0f;
        float lootTimer = 8.0f;
        float emptyWarnCooldown = 0.0f;
        double playSeconds = 0.0;
        int pendingChatFocus = 0;
        int pendingChatScroll = 0;
        int fpsFrames = 0;
        float fpsTime = 0.0f;
        float positionTime = 0.0f;
        int lastHeading = -1;
        bool tour = false;
        int tourStep = -1;
        float tourTime = 0.0f;

        // Sets a bound scalar and marks it dirty only when it really changed.
        template<typename T>
        void Set(T& field, const T& value, const char* name)
        {
            if (field == value)
                return;
            field = value;
            model.DirtyVariable(name);
        }
        void Dirty(const char* name) { model.DirtyVariable(name); }
    };

    // Helpers
    UiSystem& Ui(Showcase& s)
    {
        return EngineGetUi(*s.app);
    }

    // Documents are looked up by their <body id> every time, so F7 (reload
    // from disk) never leaves us holding a dangling pointer.
    Rml::ElementDocument* Doc(Showcase& s, const char* id)
    {
        Rml::Context* context = Ui(s).GetContext();
        return context ? context->GetDocument(id) : nullptr;
    }

    std::string ReplaceAll(std::string text, const std::string& from, const std::string& to)
    {
        for (size_t pos = text.find(from); pos != std::string::npos; pos = text.find(from, pos + to.size()))
            text.replace(pos, from.size(), to);
        return text;
    }

    std::string Lower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c)
            { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    std::string Trim(const std::string& text)
    {
        const size_t begin = text.find_first_not_of(" \t");
        if (begin == std::string::npos)
            return {};
        const size_t end = text.find_last_not_of(" \t");
        return text.substr(begin, end - begin + 1);
    }

    std::string CallsignOrDefault(const Showcase& s)
    {
        const std::string name = Trim(s.player_name);
        return name.empty() ? std::string("Pilot") : name;
    }

    void RequestScreen(Showcase& s, Screen screen, Overlay overlay = Overlay::None)
    {
        s.transitionPending = true;
        s.nextScreen = screen;
        s.nextOverlay = overlay;
    }

    void RequestOverlay(Showcase& s, Overlay overlay)
    {
        RequestScreen(s, s.transitionPending ? s.nextScreen : s.screen, overlay);
    }

    // HUD feed: toasts, chat, objectives, score

    void AddToast(Showcase& s, const std::string& text, const char* kind = "info", const char* icon = "")
    {
        if (s.toasts.size() >= 5)
            s.toasts.erase(s.toasts.begin());
        s.toasts.push_back({ text, kind, icon, 1.0f, 0.0f });
        s.Dirty("toasts");
    }

    void AddChat(Showcase& s, const std::string& who, const std::string& text, const char* kind)
    {
        if (s.chat.size() >= 60)
            s.chat.erase(s.chat.begin());
        s.chat.push_back({ who, text, kind });
        s.Dirty("chat");
        s.pendingChatScroll = 3; // scroll to the bottom once the new line is laid out
    }

    void AddScore(Showcase& s, int amount)
    {
        const int oldLevel = s.level;
        s.Set(s.score, std::max(0, s.score + amount), "score");
        s.Set(s.level, 1 + s.score / kXpPerLevel, "level");
        s.Set(s.xp_pct, (s.score % kXpPerLevel) * 100 / kXpPerLevel, "xp_pct");
        if (s.level > oldLevel)
            AddToast(s, "Level up! You are now level " + std::to_string(s.level), "good");
    }

    void CompleteObjective(Showcase& s, ObjectiveId id)
    {
        Objective& objective = s.objectives[id];
        if (objective.done)
            return;
        objective.done = true;
        s.Dirty("objectives");
        s.Set(s.objectives_done, s.objectives_done + 1, "objectives_done");
        AddToast(s, "Objective complete: " + objective.text, "good");
        AddScore(s, 100);
        if (s.objectives_done == Obj_Count)
        {
            s.Set(s.banner_time, 4.0f, "banner_time");
            AddScore(s, 500);
            AddChat(s, "", "Mission complete. Thanks for playing the UI showcase!", "system");
        }
    }

    void ResetObjectives(Showcase& s)
    {
        s.objectives.clear();
        for (const char* text : kObjectiveText)
            s.objectives.push_back({ text, "", false });
        s.objectives[Obj_Fire].progress = "(0/" + std::to_string(kShotsForObjective) + ")";
        s.objectives_done = 0;
        s.Dirty("objectives");
        s.Dirty("objectives_done");
    }

    void SetHealth(Showcase& s, int health, int shield)
    {
        s.Set(s.health, std::clamp(health, 0, kMaxHealth), "health");
        s.Set(s.shield, std::clamp(shield, 0, kMaxShield), "shield");
        s.Set(s.health_pct, s.health * 100 / kMaxHealth, "health_pct");
        s.Set(s.shield_pct, s.shield * 100 / kMaxShield, "shield_pct");
    }

    void TakeDamage(Showcase& s, int amount)
    {
        const int absorbed = std::min(s.shield, amount);
        SetHealth(s, s.health - (amount - absorbed), s.shield - absorbed);
        s.Set(s.hurt_flash, 1.0f, "hurt_flash");
        s.sinceDamage = 0.0f;
        if (s.health <= 0)
        {
            SetHealth(s, kMaxHealth, kMaxShield);
            AddScore(s, -100);
            AddToast(s, "You were downed. Vex patched you up (-100 XP)", "bad");
        }
    }

    // Inventory

    void RefreshSlot(Slot& slot)
    {
        if (slot.item < 0 || slot.count <= 0)
        {
            slot = Slot{};
            return;
        }
        const ItemDef& def = kItems[slot.item];
        slot.name = def.name;
        slot.icon = def.icon;
        slot.kind = def.kind;
        slot.rarity = def.rarity;
    }

    void UpdateDetail(Showcase& s)
    {
        const int index = s.hoveredSlot >= 0 ? s.hoveredSlot : s.selected_slot;
        ItemDetail detail;
        if (index >= 0 && index < kSlotCount && s.slots[index].item >= 0)
        {
            const Slot& slot = s.slots[index];
            const ItemDef& def = kItems[slot.item];
            detail = { true, index, slot.count, def.usable, def.name, def.icon, def.kind, def.rarity, def.stat, def.desc };
        }
        s.detail = detail;
        s.Dirty("detail");
    }

    void RefreshInventory(Showcase& s)
    {
        float weight = 0.0f;
        for (Slot& slot : s.slots)
        {
            RefreshSlot(slot);
            if (slot.item >= 0)
                weight += kItems[slot.item].weight * static_cast<float>(slot.count);
        }
        char text[32];
        std::snprintf(text, sizeof(text), "%.1f", weight);
        s.Set(s.weight, Rml::String(text), "weight");
        s.Set(s.weight_pct, std::min(100, static_cast<int>(weight * 100.0f / static_cast<float>(s.weight_max))), "weight_pct");
        if (s.selected_slot >= 0 && s.slots[s.selected_slot].item < 0)
            s.Set(s.selected_slot, -1, "selected_slot");
        s.Dirty("slots");
        UpdateDetail(s);
    }

    // Returns how many could not be added.
    int AddItem(Showcase& s, ItemId item, int count)
    {
        const int maxStack = kItems[item].maxStack;
        for (Slot& slot : s.slots)
        {
            if (count <= 0)
                break;
            if (slot.item == item && slot.count < maxStack)
            {
                const int moved = std::min(count, maxStack - slot.count);
                slot.count += moved;
                count -= moved;
            }
        }
        for (Slot& slot : s.slots)
        {
            if (count <= 0)
                break;
            if (slot.item < 0)
            {
                slot.item = item;
                slot.count = std::min(count, maxStack);
                count -= slot.count;
            }
        }
        RefreshInventory(s);
        return count;
    }

    void GiveItem(Showcase& s, ItemId item, int count, bool announce)
    {
        const int left = AddItem(s, item, count);
        const int added = count - left;
        if (announce && added > 0)
        {
            AddToast(s, "+" + std::to_string(added) + "  " + kItems[item].name, "loot", kItems[item].icon);
            AddChat(s, "", "Picked up " + std::to_string(added) + "x " + kItems[item].name, "system");
        }
        if (left > 0)
            AddToast(s, "Inventory full", "warn");
    }

    void DiscardSlot(Showcase& s, int index)
    {
        if (index < 0 || index >= kSlotCount || s.slots[index].item < 0)
            return;
        AddToast(s, std::string("Discarded ") + kItems[s.slots[index].item].name, "warn", kItems[s.slots[index].item].icon);
        s.slots[index] = Slot{};
        RefreshInventory(s);
    }

    void UseSlot(Showcase& s, int index)
    {
        if (index < 0 || index >= kSlotCount || s.slots[index].item < 0)
            return;
        Slot& slot = s.slots[index];
        const ItemDef& def = kItems[slot.item];
        if (!def.usable)
        {
            AddToast(s, std::string(def.name) + " can't be used", "warn");
            return;
        }

        switch (slot.item)
        {
        case Item_Medkit:
            if (s.health >= kMaxHealth)
            {
                AddToast(s, "Already at full health", "warn");
                return;
            }
            SetHealth(s, s.health + 40, s.shield);
            CompleteObjective(s, Obj_UseItem);
            break;
        case Item_Stim:
            SetHealth(s, s.health + 20, s.shield + 30);
            CompleteObjective(s, Obj_UseItem);
            break;
        case Item_Cell:
            s.Set(s.reserve, s.reserve + 24, "reserve");
            break;
        case Item_Grenade:
            AddScore(s, 50);
            AddChat(s, "", "*BOOM* (+50 XP)", "system");
            break;
        case Item_Credits:
            s.Set(s.credits, s.credits + 100, "credits");
            break;
        default:
            break;
        }

        AddToast(s, std::string("Used ") + def.name, "good", def.icon);
        if (--slot.count <= 0)
            slot = Slot{};
        RefreshInventory(s);
    }

    void MoveSlot(Showcase& s, int from, int to)
    {
        if (from == to || from < 0 || to < 0 || from >= kSlotCount || to >= kSlotCount || s.slots[from].item < 0)
            return;
        Slot& a = s.slots[from];
        Slot& b = s.slots[to];
        if (a.item == b.item && b.count < kItems[b.item].maxStack)
        {
            // Same item: merge into the target stack.
            const int moved = std::min(a.count, kItems[b.item].maxStack - b.count);
            b.count += moved;
            a.count -= moved;
        }
        else
        {
            std::swap(a, b);
        }
        if (s.selected_slot == from)
            s.Set(s.selected_slot, to, "selected_slot");
        RefreshInventory(s);
    }

    void SortInventory(Showcase& s)
    {
        auto kindOrder = [](const char* kind)
        {
            const char* const order[] = { "weapon", "consumable", "material", "misc" };
            for (int i = 0; i < 4; ++i)
                if (std::string(kind) == order[i])
                    return i;
            return 4;
        };
        auto rarityOrder = [](const char* rarity)
        {
            const char* const order[] = { "legendary", "epic", "rare", "common" };
            for (int i = 0; i < 4; ++i)
                if (std::string(rarity) == order[i])
                    return i;
            return 4;
        };
        std::stable_sort(s.slots.begin(), s.slots.end(), [&](const Slot& a, const Slot& b)
            {
            if ((a.item < 0) != (b.item < 0))
                return b.item < 0;
            if (a.item < 0)
                return false;
            const ItemDef &da = kItems[a.item];
            const ItemDef &db = kItems[b.item];
            if (kindOrder(da.kind) != kindOrder(db.kind))
                return kindOrder(da.kind) < kindOrder(db.kind);
            if (rarityOrder(da.rarity) != rarityOrder(db.rarity))
                return rarityOrder(da.rarity) < rarityOrder(db.rarity);
            return std::string(da.name) < std::string(db.name); });
        s.Set(s.selected_slot, -1, "selected_slot");
        RefreshInventory(s);
    }

    void ResetInventory(Showcase& s)
    {
        s.slots.assign(kSlotCount, Slot{});
        const std::pair<ItemId, int> start[] = {
            { Item_Blaster, 1 },
            { Item_Rifle, 1 },
            { Item_Medkit, 2 },
            { Item_Stim, 1 },
            { Item_Cell, 4 },
            { Item_Scrap, 23 },
            { Item_Chip, 2 },
            { Item_Keycard, 1 },
            { Item_Grenade, 3 },
            { Item_ShieldCap, 1 },
            { Item_Credits, 2 },
        };
        // Spread them out a little so the grid has gaps to drag into.
        const int positions[] = { 0, 1, 6, 7, 8, 12, 13, 5, 2, 11, 18 };
        for (size_t i = 0; i < std::size(start); ++i)
            s.slots[positions[i]] = { start[i].first, start[i].second };
        RefreshInventory(s);
    }

    // Dialogue
    void ShowDialogueNode(Showcase& s, int node)
    {
        s.dialogueNode = node;
        const DialogueNode& n = kDialogue[node];
        s.dialogueFull = ReplaceAll(n.text, "{name}", CallsignOrDefault(s));
        s.dialogueReveal = 0.0f;
        s.Set(s.dlg_text, Rml::String(), "dlg_text");
        s.Set(s.dlg_typing, true, "dlg_typing");
        s.dlg_choices.clear();
        for (const DialogueChoice& choice : n.choices)
            s.dlg_choices.push_back(choice.text);
        s.Dirty("dlg_choices");

        if (n.effect == Effect_GiveSupplies && !s.suppliesGiven)
        {
            s.suppliesGiven = true;
            GiveItem(s, Item_Medkit, 2, true);
            GiveItem(s, Item_Cell, 3, true);
        }
        if (n.effect == Effect_AcceptQuest && !s.questAccepted)
        {
            s.questAccepted = true;
            AddToast(s, "Quest accepted: The Silent Relay", "good");
            AddChat(s, "Vex", "Relay coordinates sent to your HUD. Good luck, " + CallsignOrDefault(s) + ".", "npc");
        }
    }

    void SkipTypewriter(Showcase& s)
    {
        if (!s.dlg_typing)
            return;
        s.dialogueReveal = static_cast<float>(s.dialogueFull.size());
        s.Set(s.dlg_text, Rml::String(s.dialogueFull), "dlg_text");
        s.Set(s.dlg_typing, false, "dlg_typing");
    }

    void ChooseDialogue(Showcase& s, int index)
    {
        if (s.dlg_typing || s.overlay != Overlay::Dialogue)
            return;
        const DialogueNode& node = kDialogue[s.dialogueNode];
        if (index < 0 || index >= static_cast<int>(node.choices.size()))
            return;
        const int next = node.choices[index].next;
        if (next < 0)
            RequestOverlay(s, Overlay::None);
        else
            ShowDialogueNode(s, next);
    }

    // Chat / console
    void RunChatCommand(Showcase& s, const std::string& line)
    {
        std::vector<std::string> words;
        size_t pos = 0;
        while (pos < line.size())
        {
            const size_t end = line.find(' ', pos);
            const std::string word = line.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            if (!word.empty())
                words.push_back(word);
            if (end == std::string::npos)
                break;
            pos = end + 1;
        }
        const std::string command = Lower(words[0]);
        auto intArg = [&](size_t index, int fallback)
        {
            if (index >= words.size())
                return fallback;
            try
            {
                return std::stoi(words[index]);
            }
            catch (...)
            {
                return fallback;
            }
        };

        if (command == "/help")
        {
            AddChat(s, "", "/heal [n]  /hurt [n]  /give <item> [n]  /score <n>  /name <username>  /fps  /clear", "help");
            std::string items = "items:";
            for (const ItemDef& def : kItems)
                items += std::string(" ") + def.id;
            AddChat(s, "", items, "help");
        }
        else if (command == "/heal")
        {
            SetHealth(s, s.health + intArg(1, 100), s.shield + intArg(1, 100));
            AddChat(s, "", "Healed.", "system");
        }
        else if (command == "/hurt")
        {
            TakeDamage(s, intArg(1, 25));
        }
        else if (command == "/give" && words.size() >= 2)
        {
            const std::string id = Lower(words[1]);
            for (int i = 0; i < Item_Count; ++i)
            {
                if (id == kItems[i].id)
                {
                    GiveItem(s, static_cast<ItemId>(i), std::max(1, intArg(2, 1)), true);
                    return;
                }
            }
            AddChat(s, "", "Unknown item '" + words[1] + "'. Try /help", "error");
        }
        else if (command == "/score")
        {
            AddScore(s, intArg(1, 100));
        }
        else if (command == "/name" && words.size() >= 2)
        {
            s.Set(s.player_name, Rml::String(line.substr(line.find(' ') + 1).substr(0, 16)), "player_name");
            AddChat(s, "", "Username changed to " + s.player_name, "system");
        }
        else if (command == "/fps")
        {
            s.Set(s.set_show_fps, !s.set_show_fps, "set_show_fps");
        }
        else if (command == "/clear")
        {
            s.chat.clear();
            s.Dirty("chat");
        }
        else
        {
            AddChat(s, "", "Unknown command '" + words[0] + "'. Try /help", "error");
        }
    }

    void SubmitChat(Showcase& s, const std::string& raw)
    {
        const std::string text = Trim(raw);
        if (!text.empty())
        {
            if (text[0] == '/')
            {
                RunChatCommand(s, text);
            }
            else
            {
                AddChat(s, CallsignOrDefault(s), text, "player");
                const std::string lower = Lower(text);
                if (lower.find("vex") != std::string::npos)
                    AddChat(s, "Vex", "I heard my name. Less talking, more walking.", "npc");
                else if (lower.find("hello") != std::string::npos || lower.find("hi") == 0)
                    AddChat(s, "Vex", "Hello yourself. The relay won't fix itself.", "npc");
            }
            CompleteObjective(s, Obj_Chat);
        }
        RequestOverlay(s, Overlay::None);
    }

    Rml::Element* ChatInput(Showcase& s)
    {
        Rml::ElementDocument* hud = Doc(s, "hud");
        return hud ? hud->GetElementById("chat-input") : nullptr;
    }

    // Settings

    const char* DifficultyLabel(const Rml::String& difficulty)
    {
        if (difficulty == "story")
            return "STORY";
        if (difficulty == "hard")
            return "HARD";
        return "NORMAL";
    }

    int DamagePerHit(const Showcase& s)
    {
        if (s.set_difficulty == "story")
            return 8;
        if (s.set_difficulty == "hard")
            return 30;
        return 18;
    }

    void ApplyUiScale(Showcase& s)
    {
        s.displayScale = SDL_GetWindowDisplayScale(s.app->window.handle);
        if (s.displayScale <= 0.0f)
            s.displayScale = 1.0f;
        Ui(s).SetDensityScale(s.displayScale * static_cast<float>(s.set_ui_scale) / 100.0f);
    }

    void LoadSettingsIntoModel(Showcase& s)
    {
        GameSettings& settings = EngineGetGameSettings(*s.app);
        s.set_fullscreen = settings.GetFullscreenMode() != WindowMode::Windowed;
        s.set_vsync = settings.IsVSyncEnabled();
        s.set_fps_limit = std::to_string(static_cast<int>(settings.GetFrameRateLimit()));
        s.set_res_scale = static_cast<int>(settings.GetResolutionScaleValue() + 0.5f);
        s.set_ui_scale = settings.GetCustomInt("Showcase.UiScale", 100);
        s.set_master = settings.GetCustomInt("Showcase.MasterVolume", 80);
        s.set_music = settings.GetCustomInt("Showcase.MusicVolume", 60);
        s.set_sfx = settings.GetCustomInt("Showcase.EffectsVolume", 90);
        s.set_subtitles = settings.GetCustomBool("Showcase.Subtitles", true);
        s.set_difficulty = settings.GetCustomString("Showcase.Difficulty", "normal");
        s.set_crosshair = settings.GetCustomString("Showcase.Crosshair", "#4ef2ff");
        s.set_hud_opacity = settings.GetCustomInt("Showcase.HudOpacity", 100);
        s.set_show_fps = settings.GetCustomBool("Showcase.ShowFps", true);
        s.player_name = settings.GetCustomString("Showcase.Callsign", "Nova");
        s.applied = { s.set_fullscreen, s.set_vsync, s.set_fps_limit, s.set_res_scale, s.set_ui_scale };
    }

    void ApplySettings(Showcase& s)
    {
        EngineApp& app = *s.app;
        GameSettings& settings = EngineGetGameSettings(app);

        settings.SetVSyncEnabled(s.set_vsync);
        int fpsLimit = 0;
        try
        {
            fpsLimit = std::stoi(s.set_fps_limit);
        }
        catch (...)
        {}
        settings.SetFrameRateLimit(static_cast<float>(fpsLimit));
        settings.SetResolutionScaleValue(static_cast<float>(s.set_res_scale));
        settings.ApplyNonResolutionSettings();

        settings.SetCustomInt("Showcase.UiScale", s.set_ui_scale);
        settings.SetCustomInt("Showcase.MasterVolume", s.set_master);
        settings.SetCustomInt("Showcase.MusicVolume", s.set_music);
        settings.SetCustomInt("Showcase.EffectsVolume", s.set_sfx);
        settings.SetCustomBool("Showcase.Subtitles", s.set_subtitles);
        settings.SetCustomString("Showcase.Difficulty", s.set_difficulty);
        settings.SetCustomString("Showcase.Crosshair", s.set_crosshair);
        settings.SetCustomInt("Showcase.HudOpacity", s.set_hud_opacity);
        settings.SetCustomBool("Showcase.ShowFps", s.set_show_fps);
        settings.SetCustomString("Showcase.Callsign", s.player_name);

        // Toggling saves the settings itself.
        const bool isFullscreen = settings.GetFullscreenMode() != WindowMode::Windowed;
        if (s.set_fullscreen != isFullscreen)
            EngineToggleFullscreen(app);
        settings.SaveSettings();

        ApplyUiScale(s);
        s.applied = { s.set_fullscreen, s.set_vsync, s.set_fps_limit, s.set_res_scale, s.set_ui_scale };
        AddToast(s, "Settings applied", "good");
        CompleteObjective(s, Obj_Settings);
    }

    void ResetSettingsToDefaults(Showcase& s)
    {
        s.set_fullscreen = false;
        s.set_vsync = true;
        s.set_fps_limit = "0";
        s.set_res_scale = 100;
        s.set_ui_scale = 100;
        s.set_master = 80;
        s.set_music = 60;
        s.set_sfx = 90;
        s.set_subtitles = true;
        s.set_difficulty = "normal";
        s.set_crosshair = "#4ef2ff";
        s.set_hud_opacity = 100;
        s.set_show_fps = true;
        s.model.DirtyAllVariables();
    }

    // Screens and overlays

    void SetDocVisible(Showcase& s, const char* id, bool visible)
    {
        Rml::ElementDocument* doc = Doc(s, id);
        if (!doc || doc->IsVisible() == visible)
            return;
        if (visible)
            doc->Show();
        else
            doc->Hide();
    }

    void EnterOverlay(Showcase& s, Overlay overlay)
    {
        switch (overlay)
        {
        case Overlay::Inventory:
            s.hoveredSlot = -1;
            s.Set(s.drop_target, -1, "drop_target");
            UpdateDetail(s);
            CompleteObjective(s, Obj_Inventory);
            break;
        case Overlay::Dialogue:
            ShowDialogueNode(s, 0);
            CompleteObjective(s, Obj_Talk);
            break;
        case Overlay::Chat:
            s.pendingChatFocus = 5;
            break;
        default:
            break;
        }
    }

    void LeaveOverlay(Showcase& s, Overlay overlay)
    {
        if (overlay == Overlay::Chat)
        {
            if (Rml::Element* input = ChatInput(s))
            {
                static_cast<Rml::ElementFormControlInput*>(input)->SetValue("");
                input->Blur();
            }
            s.pendingChatFocus = 0;
        }
    }

    void ApplyTransition(Showcase& s)
    {
        if (!s.transitionPending)
            return;
        s.transitionPending = false;

        const Screen oldScreen = s.screen;
        const Overlay oldOverlay = s.overlay;
        s.screen = s.nextScreen;
        s.overlay = s.nextOverlay;

        if (oldOverlay != s.overlay)
            LeaveOverlay(s, oldOverlay);

        if (s.screen == Screen::Loading && oldScreen != Screen::Loading)
        {
            s.loadTime = 0.0f;
            s.Set(s.load_pct, 0, "load_pct");
            s.Set(s.load_tip, Rml::String(kLoadingTips[s.rng() % std::size(kLoadingTips)]), "load_tip");
        }
        if (s.screen == Screen::Playing && oldScreen != Screen::Playing && !s.has_started)
        {
            s.Set(s.has_started, true, "has_started");
            AddToast(s, "Welcome to base, " + CallsignOrDefault(s), "info");
            AddChat(s, "", "Connected to base. Type /help for commands.", "system");
            AddChat(s, "Vex", "Over here, " + CallsignOrDefault(s) + ". Come talk to me.", "npc");
        }
        if (s.screen == Screen::MainMenu)
            s.Set(s.credits_open, false, "credits_open");

        SetDocVisible(s, "main_menu", s.screen == Screen::MainMenu);
        SetDocVisible(s, "loading", s.screen == Screen::Loading);
        SetDocVisible(s, "hud", s.screen == Screen::Playing);
        SetDocVisible(s, "pause", s.overlay == Overlay::Pause);
        SetDocVisible(s, "settings", s.overlay == Overlay::Settings);
        SetDocVisible(s, "inventory", s.overlay == Overlay::Inventory);
        SetDocVisible(s, "dialogue", s.overlay == Overlay::Dialogue);
        s.Set(s.chat_open, s.overlay == Overlay::Chat, "chat_open");

        if (oldOverlay != s.overlay)
            EnterOverlay(s, s.overlay);

        const bool gameplay = s.screen == Screen::Playing && s.overlay == Overlay::None;
        EngineSetInputMode(*s.app, gameplay ? InputMode::GameOnly : InputMode::UIOnly);
    }

    void OpenSettings(Showcase& s)
    {
        s.settingsReturn = s.overlay == Overlay::Settings ? s.settingsReturn : s.overlay;
        LoadSettingsIntoModel(s);
        s.model.DirtyAllVariables();
        RequestOverlay(s, Overlay::Settings);
    }

    // Esc / "Back": closes the top-most thing.
    void Back(Showcase& s)
    {
        switch (s.overlay)
        {
        case Overlay::Settings:
            RequestOverlay(s, s.settingsReturn);
            break;
        case Overlay::None:
            if (s.screen == Screen::Playing)
                RequestOverlay(s, Overlay::Pause);
            else if (s.screen == Screen::MainMenu && s.credits_open)
                s.Set(s.credits_open, false, "credits_open");
            break;
        default:
            RequestOverlay(s, Overlay::None);
            break;
        }
    }

    // Setup: data model, documents, actions
    bool CreateDataModel(Showcase& s)
    {
        Rml::Context* context = Ui(s).GetContext();
        Rml::DataModelConstructor m = context->CreateDataModel("game");
        if (!m)
            return false;

        if (auto h = m.RegisterStruct<Objective>())
        {
            h.RegisterMember("text", &Objective::text);
            h.RegisterMember("progress", &Objective::progress);
            h.RegisterMember("done", &Objective::done);
        }
        m.RegisterArray<std::vector<Objective>>();
        if (auto h = m.RegisterStruct<Toast>())
        {
            h.RegisterMember("text", &Toast::text);
            h.RegisterMember("kind", &Toast::kind);
            h.RegisterMember("icon", &Toast::icon);
            h.RegisterMember("alpha", &Toast::alpha);
        }
        m.RegisterArray<std::vector<Toast>>();
        if (auto h = m.RegisterStruct<ChatLine>())
        {
            h.RegisterMember("who", &ChatLine::who);
            h.RegisterMember("text", &ChatLine::text);
            h.RegisterMember("kind", &ChatLine::kind);
        }
        m.RegisterArray<std::vector<ChatLine>>();
        if (auto h = m.RegisterStruct<CompassMarker>())
        {
            h.RegisterMember("label", &CompassMarker::label);
            h.RegisterMember("x", &CompassMarker::x);
            h.RegisterMember("major", &CompassMarker::major);
            h.RegisterMember("visible", &CompassMarker::visible);
        }
        m.RegisterArray<std::vector<CompassMarker>>();
        if (auto h = m.RegisterStruct<Slot>())
        {
            h.RegisterMember("item", &Slot::item);
            h.RegisterMember("count", &Slot::count);
            h.RegisterMember("name", &Slot::name);
            h.RegisterMember("icon", &Slot::icon);
            h.RegisterMember("kind", &Slot::kind);
            h.RegisterMember("rarity", &Slot::rarity);
        }
        m.RegisterArray<std::vector<Slot>>();
        if (auto h = m.RegisterStruct<ItemDetail>())
        {
            h.RegisterMember("has", &ItemDetail::has);
            h.RegisterMember("slot", &ItemDetail::slot);
            h.RegisterMember("count", &ItemDetail::count);
            h.RegisterMember("usable", &ItemDetail::usable);
            h.RegisterMember("name", &ItemDetail::name);
            h.RegisterMember("icon", &ItemDetail::icon);
            h.RegisterMember("kind", &ItemDetail::kind);
            h.RegisterMember("rarity", &ItemDetail::rarity);
            h.RegisterMember("stat", &ItemDetail::stat);
            h.RegisterMember("desc", &ItemDetail::desc);
        }
        m.RegisterArray<std::vector<Rml::String>>();

#define KES_BIND(name) m.Bind(#name, &s.name)
        KES_BIND(player_name);
        KES_BIND(has_started);
        KES_BIND(credits_open);
        KES_BIND(renderer_name);
        KES_BIND(load_pct);
        KES_BIND(load_stage);
        KES_BIND(load_tip);
        KES_BIND(health);
        KES_BIND(shield);
        KES_BIND(health_pct);
        KES_BIND(shield_pct);
        KES_BIND(xp_pct);
        KES_BIND(level);
        KES_BIND(score);
        KES_BIND(difficulty_label);
        KES_BIND(compass);
        KES_BIND(heading);
        KES_BIND(objectives);
        KES_BIND(objectives_done);
        KES_BIND(toasts);
        KES_BIND(banner_time);
        KES_BIND(hurt_flash);
        KES_BIND(hit_flash);
        KES_BIND(ammo);
        KES_BIND(reserve);
        KES_BIND(reloading);
        KES_BIND(reload_pct);
        KES_BIND(chat);
        KES_BIND(chat_open);
        KES_BIND(fps);
        KES_BIND(position);
        KES_BIND(shots_fired);
        KES_BIND(play_time);
        KES_BIND(slots);
        KES_BIND(selected_slot);
        KES_BIND(drop_target);
        KES_BIND(inv_filter);
        KES_BIND(detail);
        KES_BIND(credits);
        KES_BIND(weight);
        KES_BIND(weight_max);
        KES_BIND(weight_pct);
        KES_BIND(dlg_speaker);
        KES_BIND(dlg_text);
        KES_BIND(dlg_typing);
        KES_BIND(dlg_choices);
        KES_BIND(set_fullscreen);
        KES_BIND(set_vsync);
        KES_BIND(set_fps_limit);
        KES_BIND(set_res_scale);
        KES_BIND(set_ui_scale);
        KES_BIND(set_master);
        KES_BIND(set_music);
        KES_BIND(set_sfx);
        KES_BIND(set_subtitles);
        KES_BIND(set_difficulty);
        KES_BIND(set_crosshair);
        KES_BIND(set_hud_opacity);
        KES_BIND(set_show_fps);
        KES_BIND(settings_dirty);
#undef KES_BIND

        // Data events: data-event-<event>="name(args...)" in the documents.
        auto argInt = [](const Rml::VariantList& args, int fallback = -1)
        { return args.empty() ? fallback : args[0].Get<int>(fallback); };
        auto dragSource = [](Rml::Event& event)
        {
            auto* drag = static_cast<Rml::Element*>(event.GetParameter<void*>("drag_element", nullptr));
            return drag ? drag->GetAttribute<int>("slot", -1) : -1;
        };
        Showcase* sp = &s;

        m.BindEventCallback("inv_hover", [sp, argInt](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args)
            {
            sp->hoveredSlot = argInt(args);
            UpdateDetail(*sp); });
        m.BindEventCallback("inv_select", [sp, argInt](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args)
            {
            const int index = argInt(args);
            sp->Set(sp->selected_slot, (index >= 0 && sp->slots[index].item >= 0) ? index : -1, "selected_slot");
            UpdateDetail(*sp); });
        m.BindEventCallback("inv_use", [sp, argInt](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args)
            { UseSlot(*sp, argInt(args)); });
        m.BindEventCallback("inv_discard", [sp, argInt](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args)
            { DiscardSlot(*sp, argInt(args)); });
        m.BindEventCallback("inv_drop", [sp, argInt, dragSource](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList& args)
            {
            MoveSlot(*sp, dragSource(event), argInt(args));
            sp->Set(sp->drop_target, -1, "drop_target"); });
        m.BindEventCallback("inv_discard_drag", [sp, dragSource](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList&)
            {
            DiscardSlot(*sp, dragSource(event));
            sp->Set(sp->drop_target, -1, "drop_target"); });
        m.BindEventCallback("inv_sort", [sp](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
            { SortInventory(*sp); });

        m.BindEventCallback("dlg_skip", [sp](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
            { SkipTypewriter(*sp); });
        m.BindEventCallback("dlg_choose", [sp, argInt](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args)
            { ChooseDialogue(*sp, argInt(args)); });

        // Enter in a single-line text field sends "change" with linebreak = true.
        m.BindEventCallback("chat_change", [sp](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList&)
            {
            if (sp->overlay == Overlay::Chat && event.GetParameter<bool>("linebreak", false))
                SubmitChat(*sp, event.GetParameter<Rml::String>("value", "")); });
        // The text field has focus, so Escape goes to the UI, not to game input.
        m.BindEventCallback("chat_key", [sp](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList&)
            {
            if (event.GetParameter<int>("key_identifier", 0) == Rml::Input::KI_ESCAPE)
                RequestOverlay(*sp, Overlay::None); });

        s.model = m.GetModelHandle();
        return true;
    }

    void RegisterActions(Showcase& s)
    {
        UiSystem& ui = Ui(s);
        Showcase* sp = &s;
        // onclick="name" in the documents.
        ui.RegisterAction("menu_play", [sp](Rml::Event&, const std::string&)
            { RequestScreen(*sp, sp->has_started ? Screen::Playing : Screen::Loading); });
        ui.RegisterAction("open_settings", [sp](Rml::Event&, const std::string&)
            { OpenSettings(*sp); });
        ui.RegisterAction("toggle_credits", [sp](Rml::Event&, const std::string&)
            { sp->Set(sp->credits_open, !sp->credits_open, "credits_open"); });
        ui.RegisterAction("resume", [sp](Rml::Event&, const std::string&)
            { RequestOverlay(*sp, Overlay::None); });
        ui.RegisterAction("close_overlay", [sp](Rml::Event&, const std::string&)
            { Back(*sp); });
        ui.RegisterAction("to_main_menu", [sp](Rml::Event&, const std::string&)
            { RequestScreen(*sp, Screen::MainMenu); });
        ui.RegisterAction("settings_apply", [sp](Rml::Event&, const std::string&)
            { ApplySettings(*sp); });
        ui.RegisterAction("settings_defaults", [sp](Rml::Event&, const std::string&)
            { ResetSettingsToDefaults(*sp); });
        ui.RegisterAction("quit", [sp](Rml::Event&, const std::string&)
            { sp->app->running = false; });
    }

    bool InitShowcase(EngineApp& app, Showcase& s)
    {
        s.app = &app;
        UiSystem& ui = EngineGetUi(app);
        if (!ui.IsInitialized())
            return false;

        EngineSetWindowTitle(app, "KES UI showcase");
        s.renderer_name = app.renderer && app.renderer->GetBackend() == RendererBackend::D3D11 ? "Direct3D 11" : "Vulkan";

        LoadSettingsIntoModel(s);
        s.difficulty_label = DifficultyLabel(s.set_difficulty);
        const char* const labels[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
        for (int i = 0; i < 8; ++i)
            s.compass.push_back({ labels[i], i * 45.0f, 0.0f, i % 2 == 0, false });
        for (int i = 0; i < 24; ++i) // minor ticks every 15 degrees
            if (i % 3 != 0)
                s.compass.push_back({ "|", i * 15.0f, 0.0f, false, false });

        // The model must exist before documents that use it are loaded.
        if (!CreateDataModel(s))
            return false;
        ResetObjectives(s);
        ResetInventory(s);
        RegisterActions(s);

        // Loaded hidden, shown by ApplyTransition. Later documents draw on top.
        const char* const documents[] = { "ui/hud.rml", "ui/main_menu.rml", "ui/loading.rml", "ui/inventory.rml",
            "ui/dialogue.rml", "ui/pause.rml", "ui/settings.rml" };
        for (const char* path : documents)
            if (!ui.LoadDocument(path, false))
                EngineLogf("[Showcase] Failed to load %s", path);

        ApplyUiScale(s);
        s.tour = CommandLineHasFlag("tour");
        RequestScreen(s, Screen::MainMenu);
        ApplyTransition(s);
        return true;
    }

    // Per-frame update

    void UpdateLoading(Showcase& s, float dt)
    {
        constexpr float kDuration = 2.6f;
        s.loadTime += dt;
        // Uneven progress, like a real loader: fast, stall, fast.
        const float t = std::min(1.0f, s.loadTime / kDuration);
        const float eased = t < 0.5f ? t * 1.3f : 0.65f + (t - 0.5f) * 0.7f;
        s.Set(s.load_pct, std::min(100, static_cast<int>(eased * 100.0f)), "load_pct");
        const size_t stage = std::min<size_t>(std::size(kLoadingStages) - 1, static_cast<size_t>(t * std::size(kLoadingStages)));
        s.Set(s.load_stage, Rml::String(kLoadingStages[stage]), "load_stage");
        if (s.loadTime >= kDuration + 0.25f)
            RequestScreen(s, Screen::Playing);
    }

    void UpdateCompass(Showcase& s)
    {
        float heading = std::fmod(s.app->camera.yaw * 180.0f / kPi, 360.0f);
        if (heading < 0.0f)
            heading += 360.0f;
        const int rounded = static_cast<int>(heading + 0.5f) % 360;
        if (rounded == s.lastHeading)
            return;
        s.lastHeading = rounded;

        constexpr float kHalfFov = 80.0f; // degrees visible on each side
        for (CompassMarker& marker : s.compass)
        {
            float delta = std::fmod(marker.bearing - heading + 540.0f, 360.0f) - 180.0f;
            marker.visible = std::fabs(delta) < kHalfFov;
            marker.x = 50.0f + delta / kHalfFov * 50.0f;
        }
        s.Dirty("compass");
        char text[16];
        std::snprintf(text, sizeof(text), "%03d", rounded);
        s.Set(s.heading, Rml::String(text), "heading");
    }

    void UpdateGameplay(Showcase& s, float dt)
    {
        InputState& input = s.app->input;
        const bool gameplay = s.overlay == Overlay::None;

        if (gameplay)
        {
            const bool mouseFire = input.mouseButtons[SDL_BUTTON_LEFT] && !input.previousMouseButtons[SDL_BUTTON_LEFT];
            if (InputKeyPressed(input, SDL_SCANCODE_F) || mouseFire)
            {
                if (s.reloading)
                {
                }
                else if (s.ammo > 0)
                {
                    s.Set(s.ammo, s.ammo - 1, "ammo");
                    s.Set(s.shots_fired, s.shots_fired + 1, "shots_fired");
                    s.Set(s.hit_flash, 1.0f, "hit_flash");
                    AddScore(s, 10);
                    Objective& fire = s.objectives[Obj_Fire];
                    if (!fire.done)
                    {
                        fire.progress = "(" + std::to_string(std::min(s.shots_fired, kShotsForObjective)) + "/" + std::to_string(kShotsForObjective) + ")";
                        s.Dirty("objectives");
                        if (s.shots_fired >= kShotsForObjective)
                            CompleteObjective(s, Obj_Fire);
                    }
                }
                else if (s.emptyWarnCooldown <= 0.0f)
                {
                    AddToast(s, s.reserve > 0 ? "Magazine empty - press R" : "Out of ammo - use an Energy Cell", "warn");
                    s.emptyWarnCooldown = 1.5f;
                }
            }
            if (InputKeyPressed(input, SDL_SCANCODE_R) && !s.reloading && s.ammo < kMagazineSize && s.reserve > 0)
            {
                s.Set(s.reloading, true, "reloading");
                s.reloadTime = 0.0f;
            }
            if (InputKeyPressed(input, SDL_SCANCODE_H))
                TakeDamage(s, DamagePerHit(s));
            if (InputKeyPressed(input, SDL_SCANCODE_E))
                RequestOverlay(s, Overlay::Dialogue);
            if (InputKeyPressed(input, SDL_SCANCODE_I))
                RequestOverlay(s, Overlay::Inventory);
            if (InputKeyPressed(input, SDL_SCANCODE_T) || InputKeyPressed(input, SDL_SCANCODE_RETURN))
                RequestOverlay(s, Overlay::Chat);
        }

        // Timers keep running behind every overlay except the pause menu.
        if (s.overlay == Overlay::Pause || s.overlay == Overlay::Settings)
            return;

        s.playSeconds += dt;
        const int seconds = static_cast<int>(s.playSeconds);
        char timeText[16];
        std::snprintf(timeText, sizeof(timeText), "%02d:%02d", seconds / 60, seconds % 60);
        s.Set(s.play_time, Rml::String(timeText), "play_time");
        s.emptyWarnCooldown -= dt;

        if (s.reloading)
        {
            constexpr float kReloadDuration = 1.4f;
            s.reloadTime += dt;
            s.Set(s.reload_pct, std::min(100, static_cast<int>(s.reloadTime / kReloadDuration * 100.0f)), "reload_pct");
            if (s.reloadTime >= kReloadDuration)
            {
                const int moved = std::min(kMagazineSize - s.ammo, s.reserve);
                s.Set(s.ammo, s.ammo + moved, "ammo");
                s.Set(s.reserve, s.reserve - moved, "reserve");
                s.Set(s.reloading, false, "reloading");
                s.Set(s.reload_pct, 0, "reload_pct");
                CompleteObjective(s, Obj_Reload);
            }
        }

        // Shield regenerates 3 seconds after the last hit.
        s.sinceDamage += dt;
        if (s.sinceDamage > 3.0f && s.shield < kMaxShield)
        {
            s.shieldRegenCarry += dt * 15.0f;
            const int gained = static_cast<int>(s.shieldRegenCarry);
            s.shieldRegenCarry -= static_cast<float>(gained);
            SetHealth(s, s.health, s.shield + gained);
        }

        // Random loot every few seconds.
        s.lootTimer -= dt;
        if (s.lootTimer <= 0.0f)
        {
            s.lootTimer = 12.0f + static_cast<float>(s.rng() % 8);
            const std::pair<ItemId, int> loot[] = { { Item_Scrap, 3 }, { Item_Cell, 1 }, { Item_Medkit, 1 }, { Item_Chip, 1 }, { Item_Credits, 1 }, { Item_Grenade, 1 } };
            const auto& pick = loot[s.rng() % std::size(loot)];
            GiveItem(s, pick.first, pick.second, true);
        }

        // Fading effects.
        if (s.hurt_flash > 0.0f)
            s.Set(s.hurt_flash, std::max(0.0f, s.hurt_flash - dt * 2.5f), "hurt_flash");
        if (s.hit_flash > 0.0f)
            s.Set(s.hit_flash, std::max(0.0f, s.hit_flash - dt * 6.0f), "hit_flash");
        if (s.banner_time > 0.0f)
            s.Set(s.banner_time, std::max(0.0f, s.banner_time - dt), "banner_time");

        bool toastsChanged = false;
        for (Toast& toast : s.toasts)
        {
            toast.age += dt;
            const float alpha = std::clamp((5.0f - toast.age) / 0.6f, 0.0f, 1.0f);
            if (alpha != toast.alpha)
            {
                toast.alpha = alpha;
                toastsChanged = true;
            }
        }
        const size_t before = s.toasts.size();
        s.toasts.erase(std::remove_if(s.toasts.begin(), s.toasts.end(), [](const Toast& t)
                           { return t.alpha <= 0.0f; }),
            s.toasts.end());
        if (toastsChanged || s.toasts.size() != before)
            s.Dirty("toasts");

        UpdateCompass(s);
    }

    void UpdateDialogue(Showcase& s, float dt)
    {
        InputState& input = s.app->input;
        if (s.dlg_typing)
        {
            s.dialogueReveal += dt * 45.0f; // characters per second
            const size_t shown = std::min(s.dialogueFull.size(), static_cast<size_t>(s.dialogueReveal));
            s.Set(s.dlg_text, Rml::String(s.dialogueFull.substr(0, shown)), "dlg_text");
            if (shown >= s.dialogueFull.size())
                s.Set(s.dlg_typing, false, "dlg_typing");
            if (InputKeyPressed(input, SDL_SCANCODE_SPACE))
                SkipTypewriter(s);
            return;
        }
        for (int i = 0; i < 4; ++i)
            if (InputKeyPressed(input, static_cast<SDL_Scancode>(SDL_SCANCODE_1 + i)))
                ChooseDialogue(s, i);
    }

    void UpdateChat(Showcase& s)
    {
        if (s.pendingChatFocus > 0)
        {
            // The input row only becomes visible after the UI's next layout
            // pass, so focusing may take a frame or two.
            --s.pendingChatFocus;
            if (Rml::Element* input = ChatInput(s))
                if (input->Focus())
                    s.pendingChatFocus = 0;
        }
        if (s.pendingChatScroll > 0)
        {
            --s.pendingChatScroll;
            if (Rml::ElementDocument* hud = Doc(s, "hud"))
                if (Rml::Element* log = hud->GetElementById("chat-log"))
                    log->SetScrollTop(log->GetScrollHeight());
        }
    }

    void UpdateStats(Showcase& s, float dt)
    {
        ++s.fpsFrames;
        s.fpsTime += dt;
        if (s.fpsTime >= 0.5f)
        {
            s.Set(s.fps, static_cast<int>(s.fpsFrames / s.fpsTime + 0.5f), "fps");
            s.fpsFrames = 0;
            s.fpsTime = 0.0f;
        }
        s.positionTime += dt;
        if (s.positionTime >= 0.1f)
        {
            s.positionTime = 0.0f;
            const Camera& camera = s.app->camera;
            char text[64];
            std::snprintf(text, sizeof(text), "X %.1f  Y %.1f  Z %.1f", camera.x, camera.y, camera.z);
            s.Set(s.position, Rml::String(text), "position");
        }

        s.Set(s.difficulty_label, Rml::String(DifficultyLabel(s.set_difficulty)), "difficulty_label");
        const bool dirty = s.set_fullscreen != s.applied.fullscreen || s.set_vsync != s.applied.vsync ||
            s.set_fps_limit != s.applied.fpsLimit || s.set_res_scale != s.applied.resScale ||
            s.set_ui_scale != s.applied.uiScale;
        s.Set(s.settings_dirty, dirty, "settings_dirty");

        // Alt+Enter changes the window mode behind our back; keep the checkbox honest.
        const bool fullscreen = EngineGetGameSettings(*s.app).GetFullscreenMode() != WindowMode::Windowed;
        if (fullscreen != s.applied.fullscreen)
        {
            s.applied.fullscreen = fullscreen;
            s.Set(s.set_fullscreen, fullscreen, "set_fullscreen");
        }
        // The engine resets the UI scale when the window moves to another display.
        if (SDL_GetWindowDisplayScale(s.app->window.handle) != s.displayScale)
            ApplyUiScale(s);
    }

    // -tour: walks through every screen on a timer, like an attract mode.
    void UpdateTour(Showcase& s, float dt)
    {
        constexpr float kStepSeconds[] = { 3.0f, 4.0f, 3.0f, 3.5f, 4.0f, 3.0f, 3.0f, 3.0f, 3.0f, 3.0f, 3.0f, 3.0f };
        s.tourTime += dt;
        if (s.tourStep >= 0 && s.tourTime < kStepSeconds[s.tourStep])
            return;
        s.tourTime = 0.0f;
        s.tourStep = (s.tourStep + 1) % static_cast<int>(std::size(kStepSeconds));

        auto settingsTab = [&](int tab)
        {
            if (Rml::ElementDocument* doc = Doc(s, "settings"))
            {
                Rml::ElementList tabsets;
                doc->GetElementsByTagName(tabsets, "tabset");
                if (!tabsets.empty())
                    static_cast<Rml::ElementTabSet*>(tabsets[0])->SetActiveTab(tab);
            }
        };

        switch (s.tourStep)
        {
        case 0: RequestScreen(s, Screen::MainMenu); break;
        case 1: RequestScreen(s, s.has_started ? Screen::Playing : Screen::Loading); break;
        case 2:
            RequestScreen(s, Screen::Playing);
            s.Set(s.ammo, 3, "ammo");
            TakeDamage(s, 45);
            break;
        case 3:
            RequestOverlay(s, Overlay::Inventory);
            s.Set(s.selected_slot, 6, "selected_slot");
            UpdateDetail(s);
            break;
        case 4: RequestOverlay(s, Overlay::Dialogue); break;
        case 5: ChooseDialogue(s, 2); break;
        case 6:
            RequestOverlay(s, Overlay::Chat);
            SubmitChat(s, "/give grenade 2");
            SubmitChat(s, "hello vex");
            RequestOverlay(s, Overlay::Chat);
            break;
        case 7: RequestOverlay(s, Overlay::Pause); break;
        case 8:
            OpenSettings(s);
            settingsTab(0);
            break;
        case 9:  settingsTab(2); break;
        case 10: settingsTab(3); break;
        case 11:
            RequestOverlay(s, Overlay::None);
            TakeDamage(s, std::max(1, s.health + s.shield - 15)); // leaves 15 health: low health vignette
            break;
        }
    }

    void UpdateShowcase(Showcase& s, float dt)
    {
        if (s.tour)
            UpdateTour(s, dt);

        ApplyTransition(s);

        InputState& input = s.app->input;
        // Escape: game actions read as idle in UIOnly mode, so check the key too.
        if (InputActionPressed(input, "InputAction_Pause") || InputKeyPressed(input, SDL_SCANCODE_ESCAPE))
            Back(s);
        if (s.overlay == Overlay::Inventory && InputKeyPressed(input, SDL_SCANCODE_I))
            RequestOverlay(s, Overlay::None);

        switch (s.screen)
        {
        case Screen::MainMenu:
            break;
        case Screen::Loading:
            UpdateLoading(s, dt);
            break;
        case Screen::Playing:
            UpdateGameplay(s, dt);
            if (s.overlay == Overlay::Dialogue)
                UpdateDialogue(s, dt);
            UpdateChat(s);
            break;
        }
        UpdateStats(s, dt);
    }
}

int main(int argc, char* argv[])
{
    CollectPositionalCommandLineArguments(argc, argv);
    EngineSetCommandLine(argc, argv);

    EngineApp app = {};
    if (!EngineStart(app))
    {
        EngineShutdown(app);
        return 1;
    }

    Showcase showcase;
    if (!InitShowcase(app, showcase))
    {
        showcase.app = nullptr;
        EngineLog("[Showcase] UI system unavailable; running without the showcase UI.");
    }

    Uint64 last = SDL_GetPerformanceCounter();
    while (EngineTick(app))
    {
        const Uint64 now = SDL_GetPerformanceCounter();
        const float dt = std::min(0.1f, static_cast<float>(static_cast<double>(now - last) / static_cast<double>(SDL_GetPerformanceFrequency())));
        last = now;
        if (showcase.app)
            UpdateShowcase(showcase, dt);
    }

    EngineShutdown(app);
    return 0;
}
