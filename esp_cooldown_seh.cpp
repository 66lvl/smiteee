#include "esp_cooldown_seh.h"
#include "../core/sdk_helpers.h"
#include <Windows.h>
#include <cstring>
#include <cctype>

namespace EspCooldownSeh
{
    struct SehCdEntry
    {
        float remaining = 0.f;
        float duration  = 0.f;
        int   slot      = 0;
    };

    static bool SafeRead(uintptr_t addr, void* out, size_t size)
    {
        __try {
            if (!Helpers::IsValidPtr(reinterpret_cast<const void*>(addr)))
                return false;
            memcpy(out, reinterpret_cast<const void*>(addr), size);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool IsValidPtr(uintptr_t ptr)
    {
        return ptr != 0 && Helpers::IsValidPtr(reinterpret_cast<const void*>(ptr));
    }

    static void ToUpperAscii(char* s)
    {
        if (!s) return;
        for (; *s; ++s)
            *s = (char)std::toupper((unsigned char)*s);
    }

    static bool IsExcludedItemCooldownName(const char* name)
    {
        if (!name || !name[0]) return true;

        static const char* kExclude[] = {
            "ITEM_", "ITEMS_", "STARTER_", "POTION", "CHALICE", "WARD",
            "BATTLECRY", "TELEPORTER", "BARONSBREW", "NAGABUFF", "NAGA_",
            "CONSUMABLE", "MEDITATION", "FAMILIAR", "HEALTHPOTION",
            "MANAPOTION", "COOLDOWNREDUCTION", "ULTCOOLDOWN", "NONULTCOOLDOWN",
            "COOLDOWNS", "INSTANTCOOLDOWN", nullptr
        };

        for (int i = 0; kExclude[i]; ++i)
        {
            if (strstr(name, kExclude[i]) != nullptr)
                return true;
        }
        return false;
    }

    static int ParseAbilitySlotFromName(const char* name)
    {
        if (!name) return 0;
        for (int slot = 1; slot <= 4; ++slot)
        {
            char pattern[8];
            snprintf(pattern, sizeof(pattern), "_A0%d_", slot);
            if (strstr(name, pattern) != nullptr)
                return slot;

            char suffix[8];
            snprintf(suffix, sizeof(suffix), "_A0%d", slot);
            size_t len = strlen(name);
            size_t slen = strlen(suffix);
            if (len >= slen && strcmp(name + len - slen, suffix) == 0)
                return slot;
        }
        return 0;
    }

    static bool FNameToUtf8(const SDK::FName& fname, char* out, size_t outLen)
    {
        out[0] = '\0';
        __try {
            if (fname.IsNone()) return false;

            wchar_t wbuf[128] = {};
            UC::FString fstr(wbuf, 0, 128);
            if (!SDK::FName::AppendString)
                SDK::FName::InitInternal();

            auto appendFn = reinterpret_cast<void(*)(const SDK::FName*, UC::FString&)>(
                SDK::FName::AppendString);
            SDK::InSDKUtils::CallGameFunction(appendFn, &fname, fstr);

            const wchar_t* w = fstr.CStr();
            if (!w || !*w) return false;
            if (WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)outLen, nullptr, nullptr) <= 0)
                return false;
            return out[0] != '\0';
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool CopyObjectName(uintptr_t objectPtr, char* out, size_t outLen)
    {
        out[0] = '\0';
        __try {
            if (!IsValidPtr(objectPtr))
                return false;

            SDK::FName fname{};
            if (!SafeRead(objectPtr + 0x18, &fname.ComparisonIndex, 4))
                return false;
            SafeRead(objectPtr + 0x1C, &fname.Number, 4);
            return FNameToUtf8(fname, out, outLen);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool IsAbilityCooldownName(const char* name)
    {
        if (!name || !name[0])
            return false;

        char upper[128]{};
        strncpy_s(upper, sizeof(upper), name, _TRUNCATE);
        ToUpperAscii(upper);

        if (strstr(upper, "COOLDOWN") == nullptr)
            return false;
        return !IsExcludedItemCooldownName(upper);
    }

    static bool IsAbilityCooldownEffect(uintptr_t classPtr)
    {
        char name[128]{};
        if (!CopyObjectName(classPtr, name, sizeof(name)))
            return false;
        ToUpperAscii(name);
        return IsAbilityCooldownName(name);
    }

#if ESP_CD_MAX_RELIC_SLOTS > 0

    static bool UpperContainsKnownRelicMarker(const char* upper)
    {
        if (!upper || !upper[0])
            return false;

        static const char* kMarkers[] = {
            "AEGISOFACCELERATION",
            "TIMELOCKAEGIS",
            "BLINKRUNE",
            "SCORCHED",
            "BLESSEDBARRIER",
            "BRACEROFRADIANCE",
            "BRACEROFBRILLIANCE",
            "CLOAKOFMEDITATION",
            "CLOAKOFTHEASCETIC",
            "PROPHETICCLOAK",
            "MAGISCLOAK",
            "DIVINEBARRIER",
            "EMBLEMOFINCREASING",
            "HORRIFICEMBLEM",
            "HEAVENLYWINGS",
            "ENTANGLINGWINGS",
            "MAGICSHELL",
            "SHELLOFREBUKE",
            "SHIELDOFTHORNS",
            "TEMPORALBEADS",
            "TELEPORTFRAGMENT",
            "PERSISTENTTELEPORT",
            "SUNDERINGARC",
            "SUNDERINGBLAST",
            "SCORCHINGBLINKRUNE",
            "SCORCHINGBLINK",
            "BLINKINGABYSS",
            "BLESSEDBARRIER",
            "PHOENIXAMULET",
            "BRACEROFRADIANCE",
            "BRACEROFBRILLIANCE",
            "CLOAKOFMEDITATION",
            "CLOAKOFTHEASCETIC",
            "PROPHETICCLOAK",
            "MAGISCLOAK",
            "DIVINEBARRIER",
            "TIMELOCKAEGIS",
            "EMBLEMOFINCREASING",
            "HORRIFICEMBLEM",
            "HEAVENLYWINGS",
            "HUSSARSWINGS",
            "ENTANGLINGWINGS",
            "MAGICSHELL",
            "PHANTOMSHELL",
            "SHELLOFREBUKE",
            "SHIELDOFTHORNS",
            "PURIFICATIONBEADS",
            "TALISMANOFPURIFICATION",
            "TEMPORALBEADS",
            "TELEPORTFRAGMENT",
            "PERSISTENTTELEPORT",
            "SUNDERINGARC",
            "SUNDERINGBLAST",
            "SUNDERINGECHO",
            "AGILITYRELIC",
            "AGILITYGREAVES",
            "WINDSHARD",
            "VISIONSHARD",
            "STURDYSHARD",
            "VIBRANTSHARD",
            nullptr
        };

        for (int i = 0; kMarkers[i]; ++i)
        {
            if (strstr(upper, kMarkers[i]) != nullptr)
                return true;
        }
        return false;
    }

    static bool UpperIsShardOrAgilityRelic(const char* upper)
    {
        if (!upper || !upper[0])
            return false;

        if (strstr(upper, "AGILITYRELIC") != nullptr ||
            strstr(upper, "AGILITYGREAVES") != nullptr)
            return true;
        if (strstr(upper, "AGILITY") != nullptr && strstr(upper, "RELIC") != nullptr)
            return true;
        if (strstr(upper, "WINDSHARD") != nullptr ||
            strstr(upper, "VISIONSHARD") != nullptr ||
            strstr(upper, "STURDYSHARD") != nullptr ||
            strstr(upper, "VIBRANTSHARD") != nullptr)
            return true;
        if (strstr(upper, "SHARDRELIC") != nullptr)
            return true;
        return false;
    }

    static bool NameMatchesKnownRelic(const char* name)
    {
        if (!name || !name[0])
            return false;

        char upper[128]{};
        strncpy_s(upper, sizeof(upper), name, _TRUNCATE);
        ToUpperAscii(upper);

        if (strstr(upper, "GRANTEDABILITIES") != nullptr)
            return false;

        if (UpperIsShardOrAgilityRelic(upper))
            return true;

        if (strstr(upper, "RELIC") != nullptr)
            return true;

        return UpperContainsKnownRelicMarker(upper);
    }

    static bool IsRelicCooldownName(const char* name)
    {
        if (!name || !name[0])
            return false;

        char upper[128]{};
        strncpy_s(upper, sizeof(upper), name, _TRUNCATE);
        ToUpperAscii(upper);

        if (strstr(upper, "COOLDOWN") == nullptr)
            return false;
        if (strstr(upper, "COOLDOWNREDUCTION") != nullptr) return false;
        if (strstr(upper, "ULTCOOLDOWN") != nullptr) return false;
        if (strstr(upper, "NONULTCOOLDOWN") != nullptr) return false;
        if (strstr(upper, "COOLDOWNS") != nullptr) return false;

        if (NameMatchesKnownRelic(name))
            return true;

        if (strstr(upper, "GE_ITEM_") != nullptr || strstr(upper, "GE_ITEMS_") != nullptr)
        {
            if (strstr(upper, "BUMBA") != nullptr) return false;
            if (strstr(upper, "JOTUNN") != nullptr) return false;
            if (strstr(upper, "SPEAROF") != nullptr) return false;
            if (strstr(upper, "CHRONOS") != nullptr) return false;
            return true;
        }

        return (strstr(upper, "GA_ITEMS_") != nullptr || strstr(upper, "GA_ITEM_") != nullptr) &&
               NameMatchesKnownRelic(name);
    }

    static bool RelicEffectNameMatches(const char* name)
    {
        if (!name || !name[0])
            return false;

        char upper[128]{};
        strncpy_s(upper, sizeof(upper), name, _TRUNCATE);
        ToUpperAscii(upper);

        if (strstr(upper, "COOLDOWN") == nullptr)
            return false;

        return IsRelicCooldownName(name);
    }

    static bool IsRelicCooldownEffectEntry(uintptr_t effPtr, uintptr_t classPtr)
    {
        __try {
            uintptr_t defPtr = 0;
            if (SafeRead(effPtr + 0x18, &defPtr, 8) && IsValidPtr(defPtr))
            {
                auto* def = reinterpret_cast<SDK::UGameplayEffect*>(defPtr);
                if (def && Helpers::IsValidPtr(def) &&
                    def->IsA(SDK::UHWGameplayEffect_Cooldown::StaticClass()))
                {
                    char name[128]{};
                    if (CopyObjectName(defPtr, name, sizeof(name)))
                        return IsRelicCooldownName(name);
                    return false;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        char name[128]{};
        if (!CopyObjectName(classPtr, name, sizeof(name)))
            return false;
        return IsRelicCooldownName(name);
    }

#endif // kMaxRelicSlots > 0

    static bool IsAbilityCooldownEffectEntry(uintptr_t effPtr, uintptr_t classPtr)
    {
        __try {
            uintptr_t defPtr = 0;
            if (SafeRead(effPtr + 0x18, &defPtr, 8) && IsValidPtr(defPtr))
            {
                auto* def = reinterpret_cast<SDK::UGameplayEffect*>(defPtr);
                if (def && Helpers::IsValidPtr(def) &&
                    def->IsA(SDK::UHWGameplayEffect_Cooldown::StaticClass()))
                {
                    char name[128]{};
                    if (CopyObjectName(defPtr, name, sizeof(name)))
                    {
                        ToUpperAscii(name);
                        return IsAbilityCooldownName(name);
                    }
                    return true;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        return IsAbilityCooldownEffect(classPtr);
    }

    static bool ReadEffectClassPtr(uintptr_t effPtr, uintptr_t* outClassPtr)
    {
        *outClassPtr = 0;

        __try {
            uintptr_t defPtr = 0;
            if (!SafeRead(effPtr + 0x18, &defPtr, 8)) return false;
            if (!IsValidPtr(defPtr)) return false;

            uintptr_t classPtr = 0;
            if (!SafeRead(defPtr + 0x10, &classPtr, 8)) return false;
            if (!IsValidPtr(classPtr)) return false;

            uintptr_t vtable = 0;
            if (!SafeRead(classPtr, &vtable, 8)) return false;
            if (vtable < 0x10000) return false;

            *outClassPtr = classPtr;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool ResolveAscPtr(SDK::APawn* pawn, uintptr_t* outAsc)
    {
        *outAsc = 0;

        __try {
            if (pawn && pawn->IsA(SDK::AHWCharacter_Base::StaticClass()))
            {
                auto* ch = static_cast<SDK::AHWCharacter_Base*>(pawn);
                auto* comp = ch->GetAbilitySystemComponent();
                if (comp && Helpers::IsValidPtr(comp))
                {
                    *outAsc = reinterpret_cast<uintptr_t>(comp);
                    return true;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        uintptr_t pawnBase = reinterpret_cast<uintptr_t>(pawn);
        uintptr_t asActor = 0;
        if (!SafeRead(pawnBase + 0x06B8, &asActor, 8)) return false;
        if (!IsValidPtr(asActor)) return false;

        uintptr_t asc = 0;
        if (!SafeRead(asActor + 0x0338, &asc, 8)) return false;
        if (!IsValidPtr(asc)) return false;

        *outAsc = asc;
        return true;
    }

    static bool ReadEnemyCooldowns_SEH(uintptr_t ascPtr,
                                       float worldTime,
                                       float serverTime,
                                       SehCdEntry* out,
                                       int maxOut,
                                       int& outCount,
                                       uintptr_t* effPtrBuf,
                                       uintptr_t* classPtrBuf,
                                       int32_t* nameIdxBuf,
                                       float maxDuration = 120.f)
    {
        outCount = 0;
        __try {
            uintptr_t effData = 0;
            int32_t effNum = 0;
            if (!SafeRead(ascPtr + 0x09A0, &effData, 8)) return false;
            if (!SafeRead(ascPtr + 0x09A8, &effNum, 4)) return false;
            if (!IsValidPtr(effData) || effNum <= 0 || effNum > 512) return false;

            for (int32_t e = 0; e < effNum && outCount < maxOut; ++e)
            {
                uintptr_t eff = effData + static_cast<uintptr_t>(e) * 0x3F8;
                float dur = 0.f, startWorld = 0.f, startServer = 0.f;
                if (!SafeRead(eff + 0x78, &dur, 4)) continue;
                if (dur <= 0.f || dur > maxDuration) continue;
                if (!SafeRead(eff + 0x2E8, &startWorld, 4)) continue;
                SafeRead(eff + 0x2E0, &startServer, 4);

                float rem = -1.f;
                if (startWorld > 0.f && worldTime > 0.f)
                {
                    float r = (startWorld + dur) - worldTime;
                    if (r > 0.f && r <= dur) rem = r;
                }
                if (rem <= 0.f && startServer > 0.f && serverTime > 0.f)
                {
                    float r = (startServer + dur) - serverTime;
                    if (r > 0.f && r <= dur) rem = r;
                }
                if (rem <= 0.f) continue;

                uintptr_t classPtr = 0;
                ReadEffectClassPtr(eff, &classPtr);

                int32_t nameIdx = 0;
                if (classPtr)
                    SafeRead(classPtr + 0x18, &nameIdx, 4);

                out[outCount].remaining = rem;
                out[outCount].duration  = dur;
                out[outCount].slot      = 0;

                uintptr_t defPtr = 0;
                SafeRead(eff + 0x18, &defPtr, 8);
                if (IsValidPtr(defPtr))
                {
                    char defName[128]{};
                    if (CopyObjectName(defPtr, defName, sizeof(defName)))
                        out[outCount].slot = ParseAbilitySlotFromName(defName);
                }
                if (out[outCount].slot == 0 && classPtr)
                {
                    char className[128]{};
                    if (CopyObjectName(classPtr, className, sizeof(className)))
                        out[outCount].slot = ParseAbilitySlotFromName(className);
                }

                effPtrBuf[outCount]   = eff;
                classPtrBuf[outCount] = classPtr;
                nameIdxBuf[outCount]  = nameIdx;
                ++outCount;
            }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static void ConsolidateSehBySlot(const SehCdEntry* src, int srcCount,
                                     SehCdEntry bySlot[kMaxCooldowns + 1])
    {
        for (int i = 0; i < srcCount; ++i)
        {
            if (src[i].slot < 1 || src[i].slot > kMaxCooldowns)
                continue;

            SehCdEntry& dst = bySlot[src[i].slot];
            if (dst.remaining < src[i].remaining)
                dst = src[i];
        }
    }

    static void InitAbilitySlots(EspAbilitySlotInfo out[kMaxCooldowns])
    {
        for (int i = 0; i < kMaxCooldowns; ++i)
        {
            out[i] = {};
            out[i].slot = i + 1;
            out[i].state = EspAbilitySlotState::Locked;
        }
    }

#if ESP_CD_MAX_RELIC_SLOTS > 0

    static void InitRelicSlots(EspAbilitySlotInfo out[kMaxRelicSlots])
    {
        for (int i = 0; i < kMaxRelicSlots; ++i)
        {
            out[i] = {};
            out[i].slot = i + 1;
            out[i].state = EspAbilitySlotState::Locked;
        }
    }

    static int RelicSlotFromInput(int input)
    {
        if (input >= 5 && input <= 8)
            return (input - 4 <= kMaxRelicSlots) ? (input - 4) : 0;
        if (input >= 9 && input <= 12)
            return (input - 8 <= kMaxRelicSlots) ? (input - 8) : 0;
        return 0;
    }

    static SDK::UAbilitySystemComponent* ResolveRelicAsc(SDK::AHWCharacter_Base* ch)
    {
        if (!ch || !Helpers::IsValidPtr(ch))
            return nullptr;

        __try {
            auto* hwAsc = ch->GetHWAbilitySystemComponent();
            if (hwAsc && Helpers::IsValidPtr(hwAsc))
                return hwAsc;

            auto* asc = ch->GetAbilitySystemComponent();
            if (asc && Helpers::IsValidPtr(asc))
                return asc;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        return nullptr;
    }

    static void StripRelicAssetSuffix(char* upper)
    {
        if (!upper || !upper[0])
            return;

        static const char* kStrip[] = {
            "GA_ITEMS_", "GA_ITEM_", "GE_ITEM_", "GE_ITEMS_", "ITEMS_", "ITEM_",
            "RELIC_", "T3_", "BP_", "ABP_", "WBP_", nullptr
        };
        for (int pass = 0; pass < 2; ++pass)
        {
            for (int i = 0; kStrip[i]; ++i)
            {
                char* hit = strstr(upper, kStrip[i]);
                if (hit == upper)
                {
                    const size_t len = strlen(kStrip[i]);
                    memmove(upper, upper + len, strlen(upper + len) + 1);
                    break;
                }
            }
        }

        char* cooldown = strstr(upper, "COOLDOWN");
        if (cooldown)
            *cooldown = '\0';

        size_t len = strlen(upper);
        while (len > 2 && upper[len - 2] == '_' && upper[len - 1] == 'C')
        {
            upper[len - 2] = '\0';
            len -= 2;
        }
        while (len > 0 && (upper[len - 1] == '_' ||
               (upper[len - 1] >= '0' && upper[len - 1] <= '9')))
        {
            upper[--len] = '\0';
        }
    }

    static void WriteRelicCodeHint(const char* rawName, char* out, size_t outLen)
    {
        if (!out || outLen == 0)
            return;
        out[0] = '\0';
        if (!rawName || !rawName[0])
            return;

        char upper[128]{};
        strncpy_s(upper, sizeof(upper), rawName, _TRUNCATE);
        ToUpperAscii(upper);
        StripRelicAssetSuffix(upper);

        if (!upper[0])
            return;

        if (strstr(upper, "AGILITY") != nullptr && strstr(upper, "RELIC") != nullptr)
            strncpy_s(out, outLen, "AGILITYRELIC", _TRUNCATE);
        else if (strstr(upper, "WIND") != nullptr && strstr(upper, "SHARD") != nullptr)
            strncpy_s(out, outLen, "AGILITYRELIC", _TRUNCATE);
        else if (strstr(upper, "AGILITY") != nullptr && strstr(upper, "GREAVES") != nullptr)
            strncpy_s(out, outLen, "AGILITYRELIC", _TRUNCATE);
        else if (strstr(upper, "BLINK") != nullptr && strstr(upper, "ABYSS") != nullptr)
            strncpy_s(out, outLen, "BLINKINGABYSS", _TRUNCATE);
        else if (strstr(upper, "BLINK") != nullptr)
            strncpy_s(out, outLen, "BLINKRUNE", _TRUNCATE);
        else if (strstr(upper, "TEMPORAL") != nullptr)
            strncpy_s(out, outLen, "TEMPORALBEADS", _TRUNCATE);
        else if (strstr(upper, "PURIFICATION") != nullptr || strstr(upper, "BEADS") != nullptr)
            strncpy_s(out, outLen, "PURIFICATIONBEADS", _TRUNCATE);
        else if (strstr(upper, "TALISMAN") != nullptr)
            strncpy_s(out, outLen, "TALISMANOFPURIFICATION", _TRUNCATE);
        else if (strstr(upper, "SUNDERINGECHO") != nullptr || strstr(upper, "SUNDERECHO") != nullptr)
            strncpy_s(out, outLen, "SUNDERINGECHO", _TRUNCATE);
        else if (strstr(upper, "SUNDERING") != nullptr || strstr(upper, "SUNDER") != nullptr)
            strncpy_s(out, outLen, "SUNDERINGARC", _TRUNCATE);
        else if (strstr(upper, "PHANTOM") != nullptr || strstr(upper, "MAGICSHELL") != nullptr)
            strncpy_s(out, outLen, "PHANTOMSHELL", _TRUNCATE);
        else if (strstr(upper, "REBUKE") != nullptr)
            strncpy_s(out, outLen, "SHELLOFREBUKE", _TRUNCATE);
        else if (strstr(upper, "THORN") != nullptr)
            strncpy_s(out, outLen, "SHIELDOFTHORNS", _TRUNCATE);
        else if (strstr(upper, "TELEPORT") != nullptr)
            strncpy_s(out, outLen, "TELEPORTFRAGMENT", _TRUNCATE);
        else if (strstr(upper, "VISION") != nullptr && strstr(upper, "SHARD") != nullptr)
            strncpy_s(out, outLen, "VISIONSHARD", _TRUNCATE);
        else if (strstr(upper, "STURDY") != nullptr && strstr(upper, "SHARD") != nullptr)
            strncpy_s(out, outLen, "STURDYSHARD", _TRUNCATE);
        else if (strstr(upper, "VIBRANT") != nullptr && strstr(upper, "SHARD") != nullptr)
            strncpy_s(out, outLen, "VIBRANTSHARD", _TRUNCATE);
        else if (strstr(upper, "HEAVENLY") != nullptr || strstr(upper, "ENTANGLING") != nullptr)
            strncpy_s(out, outLen, "HEAVENLYWINGS", _TRUNCATE);
        else if (strstr(upper, "BRACER") != nullptr)
            strncpy_s(out, outLen, "BRACEROFRADIANCE", _TRUNCATE);
        else if (strstr(upper, "CLOAK") != nullptr || strstr(upper, "ASCETIC") != nullptr)
            strncpy_s(out, outLen, "CLOAKOFMEDITATION", _TRUNCATE);
        else if (strstr(upper, "EMBLEM") != nullptr || strstr(upper, "HORRIFIC") != nullptr)
            strncpy_s(out, outLen, "EMBLEMOFINCREASINGPERIL", _TRUNCATE);
        else if (strstr(upper, "BLESSED") != nullptr && strstr(upper, "BARRIER") != nullptr)
            strncpy_s(out, outLen, "BLESSEDBARRIER", _TRUNCATE);
        else if (strstr(upper, "DIVINE") != nullptr && strstr(upper, "BARRIER") != nullptr)
            strncpy_s(out, outLen, "DIVINEBARRIER", _TRUNCATE);
        else if (strstr(upper, "PHOENIX") != nullptr)
            strncpy_s(out, outLen, "PHOENIXAMULET", _TRUNCATE);
        else if (strstr(upper, "TIMELOCK") != nullptr || strstr(upper, "TIMELOCKAEGIS") != nullptr)
            strncpy_s(out, outLen, "TIMELOCKAEGIS", _TRUNCATE);
        else if (strstr(upper, "AEGIS") != nullptr)
            strncpy_s(out, outLen, "AEGISOFACCELERATION", _TRUNCATE);
        else
            strncpy_s(out, outLen, upper, _TRUNCATE);
    }

    static bool FNameToAscii(const SDK::FName& fname, char* out, size_t outLen)
    {
        return FNameToUtf8(fname, out, outLen);
    }

    static bool InventorySlotIsRelic(const SDK::FHWInventorySlot& slot)
    {
        char buf[96]{};
        __try {
            if (!FNameToAscii(slot.Type.TagName, buf, sizeof(buf)))
                return false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        ToUpperAscii(buf);
        return strstr(buf, "RELIC") != nullptr || strstr(buf, "SHARD") != nullptr;
    }

    static bool IsGaItemsActiveAbilityName(const char* upper)
    {
        if (!upper || !upper[0])
            return false;
        if (strstr(upper, "GA_ITEMS_") == nullptr && strstr(upper, "GA_ITEM_") == nullptr)
            return false;
        if (strstr(upper, "AGILITYRELIC") != nullptr)
            return false;
        if (strstr(upper, "_LISTENER") != nullptr ||
            strstr(upper, "_PSV") != nullptr ||
            strstr(upper, "SPEEDBUFF") != nullptr ||
            strstr(upper, "SHIFTINGSTATS") != nullptr)
            return false;
        return true;
    }

    static bool IsRelicAbilityName(const char* name)
    {
        if (!name || !name[0])
            return false;

        if (NameMatchesKnownRelic(name))
            return true;

        char upper[128]{};
        strncpy_s(upper, sizeof(upper), name, _TRUNCATE);
        ToUpperAscii(upper);

        if (UpperIsShardOrAgilityRelic(upper))
            return true;

        if (strstr(upper, "GRANTEDABILITIES") != nullptr)
            return false;
        if (strstr(upper, "_LISTENER") != nullptr ||
            strstr(upper, "_PSV") != nullptr ||
            strstr(upper, "SHIFTINGSTATS") != nullptr ||
            strstr(upper, "SPEEDBUFF") != nullptr)
            return false;
        if (strstr(upper, "BUMBA") != nullptr ||
            strstr(upper, "JOTUNN") != nullptr ||
            strstr(upper, "SPEAROF") != nullptr ||
            strstr(upper, "CHRONOS") != nullptr ||
            strstr(upper, "HEALTHPOTION") != nullptr ||
            strstr(upper, "MANAPOTION") != nullptr ||
            strstr(upper, "POTION") != nullptr ||
            strstr(upper, "WARD") != nullptr)
            return false;

        if (strstr(upper, "GA_ITEMS_") != nullptr ||
            strstr(upper, "GA_ITEM_") != nullptr ||
            strstr(upper, "_ITEMS_") != nullptr)
        {
            return IsGaItemsActiveAbilityName(upper) ||
                   UpperContainsKnownRelicMarker(upper) ||
                   strstr(upper, "RELIC") != nullptr;
        }

        return false;
    }

    static bool IsRelicAbility(SDK::UGameplayAbility* ability,
                               const char* name,
                               int inputId,
                               bool inRelicInventorySlot)
    {
        if (!ability || !Helpers::IsValidPtr(ability))
            return false;

        if (!IsRelicAbilityName(name))
            return false;

        if (inRelicInventorySlot || RelicSlotFromInput(inputId) > 0)
            return true;

        return true;
    }

#endif // kMaxRelicSlots > 0

    static bool IsGodAbilityName(const char* name)
    {
        if (!name || !name[0])
            return false;

        char upper[128]{};
        strncpy_s(upper, sizeof(upper), name, _TRUNCATE);
        ToUpperAscii(upper);

        if (strstr(upper, "ITEM") != nullptr) return false;
        if (strstr(upper, "POTION") != nullptr) return false;
        if (strstr(upper, "CONSUMABLE") != nullptr) return false;
        if (strstr(upper, "RELIC") != nullptr) return false;
        if (strstr(upper, "WARD") != nullptr) return false;
        if (strstr(upper, "STARTER_") != nullptr) return false;
        return true;
    }

    static bool IsGodAbility(SDK::UGameplayAbility* ability)
    {
        if (!ability || !Helpers::IsValidPtr(ability))
            return false;

        char name[128]{};
        __try {
            if (!CopyObjectName(reinterpret_cast<uintptr_t>(ability), name, sizeof(name)))
                return false;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

        return IsGodAbilityName(name);
    }

    static float GetAbilityCooldownRemaining(SDK::UGameplayAbility* ability)
    {
        if (!ability || !Helpers::IsValidPtr(ability))
            return 0.f;

        __try {
            SDK::UFunction* fn = SDK::UGameplayAbility::StaticClass()->GetFunction(
                "GameplayAbility", "GetCooldownTimeRemaining");
            if (!fn)
                return 0.f;

            struct { float ReturnValue; } parms{};
            auto flgs = fn->FunctionFlags;
            fn->FunctionFlags |= 0x400;
            ability->ProcessEvent(fn, &parms);
            fn->FunctionFlags = flgs;
            return parms.ReturnValue;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return 0.f; }
    }

    static void ApplySlotCooldown(EspAbilitySlotInfo& slot, float rem, float dur)
    {
        if (rem <= 0.05f)
            return;

        slot.state = EspAbilitySlotState::OnCooldown;
        if (rem > slot.remaining)
        {
            slot.remaining = rem;
            slot.duration = (dur > 0.f ? dur : rem);
        }
    }

    static bool PopulateFromAbilitySpecs(SDK::APawn* pawn,
                                         EspAbilitySlotInfo out[kMaxCooldowns])
    {
        bool ascOk = false;
        __try {
            if (!pawn || !pawn->IsA(SDK::AHWCharacter_Base::StaticClass()))
                return false;

            auto* ch = static_cast<SDK::AHWCharacter_Base*>(pawn);
            auto* asc = ch->GetAbilitySystemComponent();
            if (!asc || !Helpers::IsValidPtr(asc))
                return false;

            ascOk = true;
            const float godLevel = Helpers::GetLevel(ch);
            const auto& items = asc->ActivatableAbilities.Items;
            const int specCount = items.Num();

            for (int s = 0; s < specCount; ++s)
            {
                if (!items.IsValidIndex(s))
                    continue;

                const SDK::FGameplayAbilitySpec& spec = items[s];
                const int input = spec.InputID;
                if (input < 1 || input > kMaxCooldowns)
                    continue;

                SDK::UGameplayAbility* ability = spec.Ability;
                if (!ability || !Helpers::IsValidPtr(ability) || !IsGodAbility(ability))
                    continue;

                if (spec.Level <= 0)
                    continue;

                EspAbilitySlotInfo& slot = out[input - 1];
                slot.state = EspAbilitySlotState::Ready;

                const float rem = GetAbilityCooldownRemaining(ability);
                ApplySlotCooldown(slot, rem, rem);
            }

            if (godLevel > 0.f && godLevel < 5.f &&
                out[3].state == EspAbilitySlotState::Ready)
            {
                out[3].state = EspAbilitySlotState::Locked;
                out[3].remaining = 0.f;
                out[3].duration = 0.f;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return ascOk;
    }

#if ESP_CD_MAX_RELIC_SLOTS > 0

    static void WriteRelicCodeFromEquipment(SDK::UHWEquipmentInstance* instance,
                                            SDK::UGameplayAbility* ability,
                                            char* relicCodeOut,
                                            size_t relicCodeLen)
    {
        if (!relicCodeOut || relicCodeLen == 0 || relicCodeOut[0])
            return;

        char name[128]{};
        __try {
            if (instance && Helpers::IsValidPtr(instance))
            {
                SDK::UHWEquipmentItem* item = instance->EquipmentItem;
                if (item && Helpers::IsValidPtr(item) &&
                    CopyObjectName(reinterpret_cast<uintptr_t>(item), name, sizeof(name)))
                {
                    WriteRelicCodeHint(name, relicCodeOut, relicCodeLen);
                    if (relicCodeOut[0])
                        return;
                }

                if (CopyObjectName(reinterpret_cast<uintptr_t>(instance), name, sizeof(name)))
                {
                    WriteRelicCodeHint(name, relicCodeOut, relicCodeLen);
                    if (relicCodeOut[0])
                        return;
                }
            }

            if (ability && Helpers::IsValidPtr(ability) &&
                CopyObjectName(reinterpret_cast<uintptr_t>(ability), name, sizeof(name)))
                WriteRelicCodeHint(name, relicCodeOut, relicCodeLen);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static bool EquipmentNameLooksLikeRelic(const char* name)
    {
        if (!name || !name[0])
            return false;
        if (NameMatchesKnownRelic(name))
            return true;

        char upper[128]{};
        strncpy_s(upper, sizeof(upper), name, _TRUNCATE);
        ToUpperAscii(upper);
        StripRelicAssetSuffix(upper);

        if (UpperIsShardOrAgilityRelic(upper))
            return true;

        return UpperContainsKnownRelicMarker(upper) || strstr(upper, "RELIC") != nullptr;
    }

    static bool ScanInventoryEntriesForRelic(SDK::AHWCharacter_Base* ch,
                                             EspAbilitySlotInfo out[kMaxRelicSlots],
                                             char* relicCodeOut,
                                             size_t relicCodeLen)
    {
        if (!ch)
            return false;

        auto* invMgr = ch->GetInventoryManager();
        if (!invMgr || !Helpers::IsValidPtr(invMgr))
            return false;

        bool found = false;
        __try {
            auto* hwAsc = ch->GetHWAbilitySystemComponent();
            const auto& entries = invMgr->Inventory.Entries;

            for (int i = 0; i < entries.Num(); ++i)
            {
                if (!entries.IsValidIndex(i))
                    continue;

                const SDK::FHWInventoryEntry& entry = entries[i];
                SDK::UHWEquipmentInstance* instance = entry.EquipmentInstance;
                if (!instance || !Helpers::IsValidPtr(instance))
                    continue;

                const bool relicSlot = InventorySlotIsRelic(entry.SlotId);
                char itemName[128]{};
                bool nameIsRelic = false;

                SDK::UHWEquipmentItem* item = instance->EquipmentItem;
                if (item && Helpers::IsValidPtr(item) &&
                    CopyObjectName(reinterpret_cast<uintptr_t>(item), itemName, sizeof(itemName)))
                {
                    nameIsRelic = EquipmentNameLooksLikeRelic(itemName);
                }

                if (!relicSlot && !nameIsRelic)
                {
                    if (CopyObjectName(reinterpret_cast<uintptr_t>(instance),
                                       itemName, sizeof(itemName)))
                        nameIsRelic = EquipmentNameLooksLikeRelic(itemName);
                    if (!nameIsRelic)
                        continue;
                }

                SDK::UGameplayAbility* ability = nullptr;
                if (hwAsc && Helpers::IsValidPtr(hwAsc))
                {
                    ability =
                        SDK::UHWLibrary_AbilitySystem::GetPrimaryAbilityInstanceBasedOnInventorySlot(
                            hwAsc, entry.SlotId);
                }

                EspAbilitySlotInfo& slot = out[0];
                if (slot.state != EspAbilitySlotState::OnCooldown)
                    slot.state = EspAbilitySlotState::Ready;

                if (ability && Helpers::IsValidPtr(ability))
                {
                    const float rem = GetAbilityCooldownRemaining(ability);
                    ApplySlotCooldown(slot, rem, rem);
                }

                WriteRelicCodeFromEquipment(instance, ability, relicCodeOut, relicCodeLen);
                found = true;
                break;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        return found;
    }

    static bool PopulateFromRelicSpecs(SDK::APawn* pawn,
                                       EspAbilitySlotInfo out[kMaxRelicSlots],
                                       char* relicCodeOut = nullptr,
                                       size_t relicCodeLen = 0)
    {
        bool ascOk = false;
        __try {
            if (!pawn || !pawn->IsA(SDK::AHWCharacter_Base::StaticClass()))
                return false;

            auto* ch = static_cast<SDK::AHWCharacter_Base*>(pawn);
            auto* asc = ResolveRelicAsc(ch);
            if (!asc || !Helpers::IsValidPtr(asc))
                asc = ch->GetAbilitySystemComponent();
            if (!asc || !Helpers::IsValidPtr(asc))
                return false;

            ascOk = true;
            const auto& items = asc->ActivatableAbilities.Items;
            const int specCount = items.Num();

            for (int s = 0; s < specCount; ++s)
            {
                if (!items.IsValidIndex(s))
                    continue;

                const SDK::FGameplayAbilitySpec& spec = items[s];
                SDK::UGameplayAbility* ability = spec.Ability;
                if (!ability || !Helpers::IsValidPtr(ability))
                    continue;

                char name[128]{};
                if (!CopyObjectName(reinterpret_cast<uintptr_t>(ability), name, sizeof(name)))
                    continue;

                bool inRelicSlot = false;
                __try {
                    auto* hwAsc = ch->GetHWAbilitySystemComponent();
                    if (hwAsc && Helpers::IsValidPtr(hwAsc))
                    {
                        const SDK::FHWInventorySlot invSlot =
                            SDK::UHWLibrary_EquipmentSystem::GetInventorySlotFromAbility(
                                spec.Handle, hwAsc);
                        inRelicSlot = InventorySlotIsRelic(invSlot);
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {}

                if (!IsRelicAbility(ability, name, spec.InputID, inRelicSlot))
                    continue;

                int relicSlot = RelicSlotFromInput(spec.InputID);
                if (relicSlot <= 0)
                    relicSlot = 1;

                if (relicSlot < 1 || relicSlot > kMaxRelicSlots)
                    continue;

                EspAbilitySlotInfo& slot = out[relicSlot - 1];
                if (slot.state != EspAbilitySlotState::OnCooldown)
                    slot.state = EspAbilitySlotState::Ready;

                const float rem = GetAbilityCooldownRemaining(ability);
                ApplySlotCooldown(slot, rem, rem);

                if (relicCodeOut && !relicCodeOut[0])
                    WriteRelicCodeHint(name, relicCodeOut, relicCodeLen);
            }

            auto* hwAsc = ch->GetHWAbilitySystemComponent();
            auto* invMgr = ch->GetInventoryManager();
            if (invMgr && Helpers::IsValidPtr(invMgr) && kMaxRelicSlots > 0)
            {
                static const char* kRelicTypeTags[] = {
                    "Equipment.Type.Relic",
                    "Equipment.Relic",
                    "Item.Type.Relic",
                    "Relic",
                    "Equipment.Type.Shard",
                    "Shard",
                    nullptr
                };

                for (int t = 0; kRelicTypeTags[t]; ++t)
                {
                    SDK::FGameplayTag relicType{};
                    bool tagOk = false;
                    __try {
                        wchar_t wtag[64]{};
                        MultiByteToWideChar(CP_UTF8, 0, kRelicTypeTags[t], -1, wtag, 64);
                        const SDK::FName tagName =
                            SDK::UKismetStringLibrary::Conv_StringToName(SDK::FString(wtag));
                        tagOk = SDK::UHWAIBlueprintLibrary::RequestGameplayTagFromName(
                            tagName, &relicType);
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER) {
                        tagOk = false;
                    }

                    if (!tagOk)
                        continue;

                    SDK::TArray<SDK::UHWEquipmentInstance*> equipmentList{};
                    __try {
                        equipmentList = invMgr->RetrieveEquipmentsBySlotType(relicType);
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER) {
                        continue;
                    }

                    for (int eq = 0; eq < equipmentList.Num(); ++eq)
                    {
                        if (!equipmentList.IsValidIndex(eq))
                            continue;

                        SDK::UHWEquipmentInstance* instance = equipmentList[eq];
                        if (!instance || !Helpers::IsValidPtr(instance))
                            continue;

                        SDK::FHWInventorySlot invSlot{};
                        __try {
                            invSlot = instance->GetInventorySlot();
                        }
                        __except (EXCEPTION_EXECUTE_HANDLER) {
                            continue;
                        }

                        SDK::UGameplayAbility* ability = nullptr;
                        if (hwAsc && Helpers::IsValidPtr(hwAsc))
                        {
                            ability =
                                SDK::UHWLibrary_AbilitySystem::GetPrimaryAbilityInstanceBasedOnInventorySlot(
                                    hwAsc, invSlot);
                        }

                        EspAbilitySlotInfo& slot = out[0];
                        if (slot.state != EspAbilitySlotState::OnCooldown)
                            slot.state = EspAbilitySlotState::Ready;

                        if (ability && Helpers::IsValidPtr(ability))
                        {
                            const float rem = GetAbilityCooldownRemaining(ability);
                            ApplySlotCooldown(slot, rem, rem);
                        }

                        WriteRelicCodeFromEquipment(instance, ability, relicCodeOut, relicCodeLen);
                        break;
                    }

                    if (out[0].state != EspAbilitySlotState::Locked)
                        break;
                }
            }

            ScanInventoryEntriesForRelic(ch, out, relicCodeOut, relicCodeLen);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return ascOk;
    }

    static void OverlaySehRelicCooldowns(uintptr_t ascPtr,
                                       float worldTime,
                                       float serverTime,
                                       EspAbilitySlotInfo out[kMaxRelicSlots],
                                       char relicCodeOut[48])
    {
        SehCdEntry tmp[32]{};
        uintptr_t effPtrs[32]{};
        uintptr_t classPtrs[32]{};
        int32_t nameIdxs[32]{};
        int rawCount = 0;
        ReadEnemyCooldowns_SEH(ascPtr, worldTime, serverTime, tmp, 32, rawCount,
                               effPtrs, classPtrs, nameIdxs, 300.f);

        float bestRem = 0.f;
        float bestDur = 0.f;
        char bestName[128]{};
        for (int i = 0; i < rawCount; ++i)
        {
            char effectName[128]{};
            uintptr_t defPtr = 0;
            if (SafeRead(effPtrs[i] + 0x18, &defPtr, 8) && IsValidPtr(defPtr))
                CopyObjectName(defPtr, effectName, sizeof(effectName));
            else if (classPtrs[i])
                CopyObjectName(classPtrs[i], effectName, sizeof(effectName));

            const bool relicFx = effectName[0] && RelicEffectNameMatches(effectName);
            const bool relicEntry = IsRelicCooldownEffectEntry(effPtrs[i], classPtrs[i]);
            if (!relicFx && !relicEntry)
                continue;

            if (effectName[0] && !bestName[0])
                strncpy_s(bestName, sizeof(bestName), effectName, _TRUNCATE);

            if (tmp[i].remaining >= bestRem)
            {
                bestRem = tmp[i].remaining;
                bestDur = tmp[i].duration;
                if (effectName[0])
                    strncpy_s(bestName, sizeof(bestName), effectName, _TRUNCATE);
            }
        }

        if (kMaxRelicSlots > 0)
        {
            if (out[0].state == EspAbilitySlotState::Locked && (bestRem > 0.f || bestName[0]))
                out[0].state = EspAbilitySlotState::Ready;
            if (bestRem > 0.f)
                ApplySlotCooldown(out[0], bestRem, bestDur);
        }

        if (relicCodeOut && bestName[0] && !relicCodeOut[0])
            WriteRelicCodeHint(bestName, relicCodeOut, 48);
    }

    static void MarkRelicFromAbilityName(const char* abilityName,
                                         SDK::UGameplayAbility* ability,
                                         EspAbilitySlotInfo out[kMaxRelicSlots],
                                         char relicCodeOut[48])
    {
        if (!IsRelicAbilityName(abilityName))
            return;

        EspAbilitySlotInfo& slot = out[0];
        if (slot.state != EspAbilitySlotState::OnCooldown)
            slot.state = EspAbilitySlotState::Ready;

        if (ability && Helpers::IsValidPtr(ability))
        {
            const float rem = GetAbilityCooldownRemaining(ability);
            ApplySlotCooldown(slot, rem, rem);
        }

        if (relicCodeOut && !relicCodeOut[0])
            WriteRelicCodeHint(abilityName, relicCodeOut, 48);
    }

    static void ScanActivatableRelicAbilities(SDK::UAbilitySystemComponent* asc,
                                              EspAbilitySlotInfo out[kMaxRelicSlots],
                                              char relicCodeOut[48])
    {
        if (!asc || !Helpers::IsValidPtr(asc))
            return;

        __try {
            const auto& items = asc->ActivatableAbilities.Items;
            for (int s = 0; s < items.Num(); ++s)
            {
                if (!items.IsValidIndex(s))
                    continue;

                SDK::UGameplayAbility* ability = items[s].Ability;
                if (!ability || !Helpers::IsValidPtr(ability))
                    continue;

                char name[128]{};
                if (!CopyObjectName(reinterpret_cast<uintptr_t>(ability), name, sizeof(name)))
                    continue;

                MarkRelicFromAbilityName(name, ability, out, relicCodeOut);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

#endif // kMaxRelicSlots > 0

    static void OverlaySehCooldowns(uintptr_t ascPtr,
                                    float worldTime,
                                    float serverTime,
                                    EspAbilitySlotInfo out[kMaxCooldowns])
    {
        SehCdEntry tmp[32]{};
        uintptr_t effPtrs[32]{};
        uintptr_t classPtrs[32]{};
        int32_t nameIdxs[32]{};
        int rawCount = 0;
        ReadEnemyCooldowns_SEH(ascPtr, worldTime, serverTime, tmp, 32, rawCount,
                               effPtrs, classPtrs, nameIdxs);

        SehCdEntry filtered[32]{};
        int filteredCount = 0;
        for (int i = 0; i < rawCount && filteredCount < 32; ++i)
        {
            if (IsAbilityCooldownEffectEntry(effPtrs[i], classPtrs[i]))
                filtered[filteredCount++] = tmp[i];
        }

        SehCdEntry bySlot[kMaxCooldowns + 1]{};
        ConsolidateSehBySlot(filtered, filteredCount, bySlot);

        bool activeThisFrame[kMaxCooldowns]{};
        for (int slot = 1; slot <= kMaxCooldowns; ++slot)
        {
            if (bySlot[slot].remaining <= 0.f)
                continue;
            activeThisFrame[slot - 1] = true;
            ApplySlotCooldown(out[slot - 1], bySlot[slot].remaining, bySlot[slot].duration);
        }

        for (int i = 0; i < kMaxCooldowns; ++i)
        {
            if (activeThisFrame[i])
                continue;
            if (out[i].state != EspAbilitySlotState::OnCooldown)
                continue;
            out[i].state = EspAbilitySlotState::Ready;
            out[i].remaining = 0.f;
            out[i].duration = 0.f;
        }
    }

    bool GetEnemyAbilityStates(SDK::APawn* pawn, float worldTime,
                               EspAbilitySlotInfo out[kMaxCooldowns])
    {
        InitAbilitySlots(out);
        if (!pawn || !out)
            return false;

        bool ascOk = false;
        __try {
            ascOk = PopulateFromAbilitySpecs(pawn, out);

            float clientTime = worldTime;
            if (clientTime <= 0.f)
                clientTime = Helpers::GetWorldTimeSeconds();
            const float serverTime = Helpers::GetServerWorldTimeSeconds();

            uintptr_t asc = 0;
            if ((clientTime > 0.f || serverTime > 0.f) && ResolveAscPtr(pawn, &asc))
                OverlaySehCooldowns(asc, clientTime, serverTime, out);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            InitAbilitySlots(out);
        }

        return ascOk;
    }

    bool GetEnemyRelicStates(SDK::APawn* pawn, float worldTime,
                             EspAbilitySlotInfo out[kMaxRelicSlots],
                             char relicCodeOut[48])
    {
        InitRelicSlots(out);
        if (relicCodeOut)
            relicCodeOut[0] = '\0';
        if (!pawn || !out)
            return false;

        bool ascOk = false;
        __try {
            ascOk = PopulateFromRelicSpecs(
                pawn, out, relicCodeOut, relicCodeOut ? 48u : 0u);

            if (Helpers::SafeIsA(pawn, SDK::AHWCharacter_Base::StaticClass()))
            {
                auto* ch = static_cast<SDK::AHWCharacter_Base*>(pawn);
                ScanInventoryEntriesForRelic(ch, out, relicCodeOut, relicCodeOut ? 48u : 0u);
                ScanActivatableRelicAbilities(ResolveRelicAsc(ch), out, relicCodeOut);
                if (out[0].state == EspAbilitySlotState::Locked)
                {
                    auto* baseAsc = ch->GetAbilitySystemComponent();
                    ScanActivatableRelicAbilities(baseAsc, out, relicCodeOut);
                }
            }

            float clientTime = worldTime;
            if (clientTime <= 0.f)
                clientTime = Helpers::GetWorldTimeSeconds();
            const float serverTime = Helpers::GetServerWorldTimeSeconds();

            uintptr_t baseAscPtr = 0;
            if (ResolveAscPtr(pawn, &baseAscPtr) &&
                baseAscPtr && (clientTime > 0.f || serverTime > 0.f))
            {
                OverlaySehRelicCooldowns(baseAscPtr, clientTime, serverTime, out, relicCodeOut);
            }

            if (Helpers::SafeIsA(pawn, SDK::AHWCharacter_Base::StaticClass()))
            {
                auto* ch = static_cast<SDK::AHWCharacter_Base*>(pawn);
                auto* hwAsc = ResolveRelicAsc(ch);
                const uintptr_t hwAscPtr = reinterpret_cast<uintptr_t>(hwAsc);
                if (hwAscPtr && hwAscPtr != baseAscPtr &&
                    (clientTime > 0.f || serverTime > 0.f))
                {
                    OverlaySehRelicCooldowns(hwAscPtr, clientTime, serverTime, out, relicCodeOut);
                }
            }

            if (out[0].state != EspAbilitySlotState::Locked && relicCodeOut && !relicCodeOut[0])
            {
                if (Helpers::SafeIsA(pawn, SDK::AHWCharacter_Base::StaticClass()))
                {
                    auto* ch = static_cast<SDK::AHWCharacter_Base*>(pawn);
                    auto* asc = ResolveRelicAsc(ch);
                    if (!asc)
                        asc = ch->GetAbilitySystemComponent();
                    ScanActivatableRelicAbilities(asc, out, relicCodeOut);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            InitRelicSlots(out);
            if (relicCodeOut)
                relicCodeOut[0] = '\0';
        }

        return ascOk || out[0].state != EspAbilitySlotState::Locked;
    }
}
