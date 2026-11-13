#include "ability_icons.h"
#include "icon_fetch.h"
#include "../core/renderer.h"
#include "../core/sdk_helpers.h"
#include "../resources/resource.h"
#include <Windows.h>
#include <wincodec.h>
#include <cstring>
#include <cctype>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

namespace AbilityIcons
{
    struct EmbeddedEntry
    {
        const unsigned char* data = nullptr;
        unsigned int         size = 0;
    };

    static std::unordered_map<std::string, EmbeddedEntry> g_EmbeddedIndex;
    static std::unordered_map<std::string, std::string> g_CompactGodLookup;
    static std::unordered_map<std::string, IconView> g_TextureCache;
    static std::mutex g_Mutex;
    static bool g_Ready = false;

    static std::string MakeKey(const std::string& godCode, int slot)
    {
        return godCode + "|" + std::to_string(slot);
    }

    static std::string MakeRelicKey(const std::string& relicCode)
    {
        return "RELIC|" + relicCode;
    }

    static std::string ToUpperAscii(std::string s)
    {
        for (char& c : s)
            c = (char)std::toupper((unsigned char)c);
        return s;
    }

    static std::string CompactGodKey(std::string s)
    {
        s = ToUpperAscii(std::move(s));
        std::string out;
        out.reserve(s.size());
        for (char c : s)
        {
            if (c == '_' || c == ' ' || c == '-' || c == '.')
                continue;
            out.push_back(c);
        }
        return out;
    }

    static const char* MapGodAlias(const std::string& compact)
    {
        static const struct { const char* from; const char* to; } kAliases[] = {
            { "AHAMURA", "AMC" },
            { "AHMURA", "AMC" },
            { "AHMURAC", "AMC" },
            { "KUKULAN", "KUKULKAN" },
            { "KUKULKAN", "KUKULKAN" },
            { "SUNWUKONG", "SUNWUKONG" },
            { "WUKONG", "SUNWUKONG" },
            { "KINGARTHUR", "KINGARTHUR_A" },
            { "KINGARTHUR_A", "KINGARTHUR_A" },
            { "KINGARTHUR_B", "KINGARTHUR_B" },
            { "ARTHUR", "KINGARTHUR_A" },
            { "MERLINARCANE", "MERLIN_SETA" },
            { "MERLIN_ARCANE", "MERLIN_SETA" },
            { "MERLIN_SETA", "MERLIN_SETA" },
            { "MERLIN", "MERLIN_SETA" },
            { "MERLINFIRE", "MERLIN_SETB" },
            { "MERLIN_FIRE", "MERLIN_SETB" },
            { "MERLIN_SETB", "MERLIN_SETB" },
            { "MERLINICE", "MERLIN_SETC" },
            { "MERLIN_ICE", "MERLIN_SETC" },
            { "MERLIN_SETC", "MERLIN_SETC" },
            { "CHANGE", "CHANGE" },
            { "CHANG", "CHANGE" },
            { "BACCHUS", "BACHUS" },
            { "BACHUS", "BACHUS" },
            { "BARI", "PRINCESSBARI" },
            { "PRINCESSBARI", "PRINCESSBARI" },
        };

        for (const auto& alias : kAliases)
        {
            if (compact == alias.from)
                return alias.to;
        }
        return nullptr;
    }

    static void StripGodAssetSuffix(std::string& name)
    {
        static const char* kPrefixes[] = {
            "HWCHARACTERITEM_", "CHARACTERITEM_", "CI_", "BP_GOD_", "ABP_GOD_",
            "BP_", "ABP_", "WBP_", "SK_", nullptr
        };
        for (int i = 0; kPrefixes[i]; ++i)
        {
            const size_t len = strlen(kPrefixes[i]);
            if (name.size() >= len && _strnicmp(name.c_str(), kPrefixes[i], (int)len) == 0)
            {
                name = name.substr(len);
                break;
            }
        }

        for (int pass = 0; pass < 4; ++pass)
        {
            while (!name.empty() && (name.back() == '_' ||
                   (name.back() >= '0' && name.back() <= '9')))
                name.pop_back();

            if (name.size() > 2 && name.compare(name.size() - 2, 2, "_C") == 0)
                name.resize(name.size() - 2);
            else
                break;
        }
    }

    static std::string CanonicalizeGodCode(std::string code)
    {
        code = ToUpperAscii(std::move(code));
        StripGodAssetSuffix(code);
        if (code.empty())
            return code;

        const std::string compact = CompactGodKey(code);
        if (const char* alias = MapGodAlias(compact))
        {
            if (alias[0])
                return std::string(alias);
            return {};
        }

        auto compactIt = g_CompactGodLookup.find(compact);
        if (compactIt != g_CompactGodLookup.end())
            return compactIt->second;

        return code;
    }

    static bool CopyObjectName(SDK::UObject* obj, char* out, size_t outLen)
    {
        out[0] = '\0';
        __try {
            if (!obj || !Helpers::IsValidPtr(obj))
                return false;

            SDK::FName fname = obj->Name;
            if (fname.IsNone())
                return false;

            wchar_t wbuf[160] = {};
            UC::FString fstr(wbuf, 0, 160);
            if (!SDK::FName::AppendString)
                SDK::FName::InitInternal();

            auto appendFn = reinterpret_cast<void(*)(const SDK::FName*, UC::FString&)>(
                SDK::FName::AppendString);
            SDK::InSDKUtils::CallGameFunction(appendFn, &fname, fstr);

            const wchar_t* w = fstr.CStr();
            if (!w || !*w)
                return false;
            if (WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)outLen, nullptr, nullptr) <= 0)
                return false;

            char* slash = strrchr(out, '/');
            if (slash && slash[1])
                memmove(out, slash + 1, strlen(slash + 1) + 1);
            return out[0] != '\0';
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return false;
    }

    static const SDK::UHWCharacterItem* TryCharacterItem(const SDK::UHWCharacterItem* item)
    {
        if (item && Helpers::IsValidPtr(item))
            return item;
        return nullptr;
    }

    static const SDK::UHWCharacterItem* ResolveCharacterItem(SDK::AHWCharacter_Base* ch)
    {
        __try {
            if (!ch || !Helpers::IsValidPtr(ch))
                return nullptr;

            if (auto* asa = ch->GetCharacterAbilitySystemActor())
            {
                if (Helpers::IsValidPtr(asa))
                {
                    if (const SDK::UHWCharacterItem* item = TryCharacterItem(asa->GetCurrentCharacterItem()))
                        return item;

                    if (auto* loadout = asa->GetLoadoutComponent())
                    {
                        if (Helpers::IsValidPtr(loadout))
                        {
                            if (const SDK::UHWCharacterItem* item =
                                    TryCharacterItem(loadout->GetCurrentCharacterItem()))
                                return item;
                        }
                    }
                }
            }

            if (auto* ps = ch->GetHWPlayerState())
            {
                if (Helpers::IsValidPtr(ps))
                {
                    if (const SDK::UHWCharacterItem* item =
                            TryCharacterItem(ps->GetDraftCharacterChoice()))
                        return item;

                    if (auto* psAsa = ps->GetCharacterAbilitySystemActor())
                    {
                        if (Helpers::IsValidPtr(psAsa))
                        {
                            if (const SDK::UHWCharacterItem* item =
                                    TryCharacterItem(psAsa->GetCurrentCharacterItem()))
                                return item;
                        }
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return nullptr;
    }

    static std::string GodCodeFromRawName(std::string raw)
    {
        if (raw.empty())
            return {};

        raw = Helpers::StripBlueprintActorName(std::move(raw));
        return CanonicalizeGodCode(raw);
    }

    static std::string LookupGodCodeFuzzy(const std::string& hint)
    {
        if (hint.empty() || g_CompactGodLookup.empty())
            return {};

        const std::string compact = CompactGodKey(hint);
        if (compact.empty())
            return {};

        if (const char* alias = MapGodAlias(compact))
        {
            if (alias[0])
                return std::string(alias);
        }

        auto exact = g_CompactGodLookup.find(compact);
        if (exact != g_CompactGodLookup.end())
            return exact->second;

        std::string bestGod;
        size_t bestScore = 0;
        for (const auto& pair : g_CompactGodLookup)
        {
            if (pair.first.size() < 3)
                continue;

            if (compact.find(pair.first) != std::string::npos ||
                pair.first.find(compact) != std::string::npos)
            {
                if (pair.first.size() > bestScore)
                {
                    bestScore = pair.first.size();
                    bestGod = pair.second;
                }
            }
        }

        return bestGod;
    }

    static HMODULE GetSelfModule()
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&GetSelfModule),
            &self);
        if (!self)
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               L"Trainer.dll", &self);
        return self;
    }

#pragma pack(push, 1)
    struct PackHeader
    {
        char     magic[4];
        uint32_t version;
        uint32_t entryCount;
    };

    struct PackEntry
    {
        char     key[64];
        uint32_t offset;
        uint32_t size;
    };
#pragma pack(pop)

    static bool LoadEmbeddedIndex()
    {
        g_EmbeddedIndex.clear();
        g_CompactGodLookup.clear();

        HMODULE module = GetSelfModule();
        if (!module)
            return false;

        HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(IDR_ABILITY_ICONS), RT_RCDATA);
        if (!res)
            return false;

        HGLOBAL loaded = LoadResource(module, res);
        if (!loaded)
            return false;

        const auto* blob = static_cast<const unsigned char*>(LockResource(loaded));
        const DWORD blobSize = SizeofResource(module, res);
        if (!blob || blobSize < sizeof(PackHeader))
            return false;

        const auto* header = reinterpret_cast<const PackHeader*>(blob);
        if (memcmp(header->magic, "HWAI", 4) != 0 || header->version != 1u)
            return false;

        if (header->entryCount == 0)
            return false;

        const size_t indexBytes = sizeof(PackHeader) + header->entryCount * sizeof(PackEntry);
        if (indexBytes > blobSize)
            return false;

        const auto* entries = reinterpret_cast<const PackEntry*>(blob + sizeof(PackHeader));
        for (uint32_t i = 0; i < header->entryCount; ++i)
        {
            const PackEntry& e = entries[i];
            if (e.size == 0)
                continue;
            if (e.offset > blobSize || e.size > blobSize - e.offset)
                continue;

            std::string key(e.key, strnlen(e.key, sizeof(e.key)));
            if (key.empty())
                continue;

            EmbeddedEntry view{};
            view.data = blob + e.offset;
            view.size = e.size;
            g_EmbeddedIndex[key] = view;

            const size_t sep = key.find('|');
            if (sep != std::string::npos)
            {
                const std::string god = key.substr(0, sep);
                g_CompactGodLookup[CompactGodKey(god)] = god;
            }
        }

        return !g_EmbeddedIndex.empty();
    }

    static bool IsMerlinBaseCode(const std::string& code)
    {
        const std::string compact = CompactGodKey(code);
        if (compact.empty() || compact.find("MERLIN") == std::string::npos)
            return false;

        if (compact == "MERLIN")
            return true;

        return compact.find("SETA") == std::string::npos &&
               compact.find("SETB") == std::string::npos &&
               compact.find("SETC") == std::string::npos &&
               compact.find("FIRE") == std::string::npos &&
               compact.find("ICE") == std::string::npos &&
               compact.find("ARCANE") == std::string::npos;
    }

    struct MerlinStanceScores
    {
        int fire = 0;
        int ice = 0;
        int arcane = 0;
    };

    static bool ScoreMerlinStanceSeh(SDK::AHWCharacter_Base* ch, MerlinStanceScores* out)
    {
        if (!ch || !out)
            return false;

        out->fire = 0;
        out->ice = 0;
        out->arcane = 0;

        __try {
            SDK::UAbilitySystemComponent* asc = ch->GetAbilitySystemComponent();
            if (!asc || !Helpers::IsValidPtr(asc))
                return false;

            const auto& items = asc->ActivatableAbilities.Items;
            for (int s = 0; s < items.Num(); ++s)
            {
                if (!items.IsValidIndex(s))
                    continue;

                const SDK::FGameplayAbilitySpec& spec = items[s];
                if (spec.InputID < 1 || spec.InputID > 4)
                    continue;

                SDK::UGameplayAbility* ability = spec.Ability;
                if (!ability || !Helpers::IsValidPtr(ability))
                    continue;

                char name[128]{};
                if (!CopyObjectName(ability, name, sizeof(name)))
                    continue;

                for (char* p = name; *p; ++p)
                    *p = (char)std::toupper((unsigned char)*p);

                if (strstr(name, "SETB") != nullptr ||
                    strstr(name, "FIRE") != nullptr ||
                    strstr(name, "INFERNO") != nullptr ||
                    strstr(name, "DRAGON") != nullptr ||
                    strstr(name, "FLAME") != nullptr)
                {
                    out->fire++;
                    continue;
                }

                if (strstr(name, "SETC") != nullptr ||
                    strstr(name, "ICE") != nullptr ||
                    strstr(name, "FROST") != nullptr ||
                    strstr(name, "BLIZZ") != nullptr ||
                    strstr(name, "GLACIAL") != nullptr)
                {
                    out->ice++;
                    continue;
                }

                if (strstr(name, "SETA") != nullptr ||
                    strstr(name, "ARCANE") != nullptr ||
                    strstr(name, "RADIATE") != nullptr ||
                    strstr(name, "ORACLE") != nullptr ||
                    strstr(name, "PULSE") != nullptr)
                {
                    out->arcane++;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }

        return true;
    }

    static std::string ResolveMerlinStanceCode(SDK::AHWCharacter_Base* ch)
    {
        if (!ch)
            return "MERLIN_SETA";

        MerlinStanceScores scores{};
        if (!ScoreMerlinStanceSeh(ch, &scores))
            return "MERLIN_SETA";

        if (scores.fire > scores.ice && scores.fire >= scores.arcane && scores.fire > 0)
            return "MERLIN_SETB";
        if (scores.ice > scores.fire && scores.ice >= scores.arcane && scores.ice > 0)
            return "MERLIN_SETC";
        if (scores.arcane > 0)
            return "MERLIN_SETA";
        return "MERLIN_SETA";
    }

    static std::string FinalizeGodCode(SDK::APawn* pawn, std::string code)
    {
        if (code.empty())
            return code;

        if (IsMerlinBaseCode(code))
        {
            if (pawn && Helpers::SafeIsA(pawn, SDK::AHWCharacter_Base::StaticClass()))
            {
                return ResolveMerlinStanceCode(
                    static_cast<SDK::AHWCharacter_Base*>(pawn));
            }
            return "MERLIN_SETA";
        }

        return CanonicalizeGodCode(std::move(code));
    }

    static const EmbeddedEntry* TryMerlinEmbeddedEntry(
        int slot1to4,
        const std::function<const EmbeddedEntry*(const std::string&)>& tryKey)
    {
        static const char* kSets[] = {
            "MERLIN_SETA", "MERLIN_SETB", "MERLIN_SETC",
            "MERLIN_ARCANE", "MERLIN_FIRE", "MERLIN_ICE", nullptr
        };

        for (int i = 0; kSets[i]; ++i)
        {
            if (const EmbeddedEntry* hit = tryKey(kSets[i]))
                return hit;
        }

        (void)slot1to4;
        return nullptr;
    }

    static const EmbeddedEntry* FindEmbeddedEntry(const std::string& godCode, int slot1to4)
    {
        const std::string canonical = CanonicalizeGodCode(godCode);
        if (canonical.empty())
            return nullptr;

        auto tryKey = [&](const std::string& god) -> const EmbeddedEntry* {
            const std::string key = MakeKey(god, slot1to4);
            auto it = g_EmbeddedIndex.find(key);
            return it != g_EmbeddedIndex.end() ? &it->second : nullptr;
        };

        if (const EmbeddedEntry* hit = tryKey(canonical))
            return hit;

        const std::string compact = CompactGodKey(canonical);
        auto compactIt = g_CompactGodLookup.find(compact);
        if (compactIt != g_CompactGodLookup.end())
        {
            if (const EmbeddedEntry* hit = tryKey(compactIt->second))
                return hit;
        }

        for (const auto& pair : g_EmbeddedIndex)
        {
            const size_t sep = pair.first.find('|');
            if (sep == std::string::npos)
                continue;
            if (pair.first.substr(sep + 1) != std::to_string(slot1to4))
                continue;

            const std::string packGod = pair.first.substr(0, sep);
            if (CompactGodKey(packGod) == compact)
                return &pair.second;
        }

        if (compact.find("MERLIN") != std::string::npos)
        {
            if (const EmbeddedEntry* hit = TryMerlinEmbeddedEntry(slot1to4, tryKey))
                return hit;
        }

        return nullptr;
    }

    static bool LoadPngRgbaFromMemory(const unsigned char* pngData,
                                      unsigned int pngSize,
                                      std::vector<unsigned char>& outRgba,
                                      int& outW,
                                      int& outH);

    static IconView LoadIconTextureFromBytes(const std::string& cacheKey,
                                             const unsigned char* data,
                                             unsigned int size)
    {
        IconView empty{};
        std::vector<unsigned char> rgba;
        int w = 0, h = 0;
        if (!LoadPngRgbaFromMemory(data, size, rgba, w, h))
            return empty;

        Renderer::GpuTexture gpu = Renderer::CreateTextureRgba(rgba.data(), w, h);
        if (!gpu.valid)
            return empty;

        IconView view{};
        view.texId = gpu.id;
        view.width = gpu.width;
        view.height = gpu.height;
        view.valid = true;

        std::lock_guard<std::mutex> lock(g_Mutex);
        g_TextureCache[cacheKey] = view;
        return view;
    }

    static IconView LoadIconTexture(const std::string& cacheKey,
                                    const EmbeddedEntry& entry)
    {
        return LoadIconTextureFromBytes(cacheKey, entry.data, entry.size);
    }

    static IconView TryLoadRemoteIcon(const std::string& cacheKey,
                                      const std::function<void()>& requestFn,
                                      const std::function<bool(std::vector<uint8_t>&)>& cacheFn)
    {
        IconView empty{};

        std::vector<uint8_t> bytes;
        if (cacheFn(bytes))
        {
            IconView cached = LoadIconTextureFromBytes(cacheKey, bytes.data(), (unsigned int)bytes.size());
            if (cached.valid)
                return cached;
        }

        requestFn();

        if (IconFetch::TryTakeCompleted(cacheKey, bytes))
        {
            IconView completed = LoadIconTextureFromBytes(cacheKey, bytes.data(), (unsigned int)bytes.size());
            if (completed.valid)
                return completed;
        }

        return empty;
    }

    static bool LoadPngRgbaFromMemory(const unsigned char* pngData,
                                      unsigned int pngSize,
                                      std::vector<unsigned char>& outRgba,
                                      int& outW,
                                      int& outH)
    {
        outRgba.clear();
        outW = outH = 0;

        if (!pngData || pngSize == 0)
            return false;

        IWICImagingFactory* factory = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory))))
            return false;

        IWICStream* stream = nullptr;
        HRESULT hr = factory->CreateStream(&stream);
        if (FAILED(hr) || !stream)
        {
            factory->Release();
            return false;
        }

        hr = stream->InitializeFromMemory(const_cast<BYTE*>(pngData), pngSize);
        if (FAILED(hr))
        {
            stream->Release();
            factory->Release();
            return false;
        }

        IWICBitmapDecoder* decoder = nullptr;
        hr = factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
        stream->Release();
        if (FAILED(hr) || !decoder)
        {
            factory->Release();
            return false;
        }

        IWICBitmapFrameDecode* frame = nullptr;
        hr = decoder->GetFrame(0, &frame);
        if (FAILED(hr) || !frame)
        {
            decoder->Release();
            factory->Release();
            return false;
        }

        IWICFormatConverter* converter = nullptr;
        hr = factory->CreateFormatConverter(&converter);
        if (FAILED(hr) || !converter)
        {
            frame->Release();
            decoder->Release();
            factory->Release();
            return false;
        }

        hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                   WICBitmapDitherTypeNone, nullptr, 0.f,
                                   WICBitmapPaletteTypeCustom);
        if (FAILED(hr))
        {
            converter->Release();
            frame->Release();
            decoder->Release();
            factory->Release();
            return false;
        }

        UINT w = 0, h = 0;
        converter->GetSize(&w, &h);
        if (w == 0 || h == 0)
        {
            converter->Release();
            frame->Release();
            decoder->Release();
            factory->Release();
            return false;
        }

        const UINT stride = w * 4u;
        const UINT size = stride * h;
        outRgba.resize(size);
        hr = converter->CopyPixels(nullptr, stride, size, outRgba.data());

        converter->Release();
        frame->Release();
        decoder->Release();
        factory->Release();

        if (FAILED(hr))
        {
            outRgba.clear();
            return false;
        }

        outW = (int)w;
        outH = (int)h;
        return true;
    }

    bool Initialize()
    {
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            if (g_Ready)
                return true;

            if (!Renderer::IsReady())
                return false;

            CoInitializeEx(nullptr, COINIT_MULTITHREADED);

            LoadEmbeddedIndex();
            IconFetch::Initialize();
            g_Ready = true;

            if (!g_EmbeddedIndex.empty())
            {
                printf("[+] AbilityIcons: embedded pack loaded (%zu icons)\n",
                       g_EmbeddedIndex.size());
            }
            else
            {
                printf("[+] AbilityIcons: runtime wiki fetch enabled (no embedded pack)\n");
            }
        }

        if (g_Ready)
        {
            static const char* kWarmGods[] = { "ACHILLES", "KUKULKAN", "CHANGE", nullptr };
            for (int i = 0; kWarmGods[i]; ++i)
            {
                IconView warm = GetAbilityIcon(kWarmGods[i], 1);
                if (warm.valid)
                    printf("[+] AbilityIcons: warm-up OK (%s A1 %dx%d)\n",
                           kWarmGods[i], warm.width, warm.height);
            }
        }

        return g_Ready;
    }

    void Shutdown()
    {
        IconFetch::Shutdown();
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_TextureCache.clear();
        g_EmbeddedIndex.clear();
        g_CompactGodLookup.clear();
        g_Ready = false;
    }

    static bool TryCopyClassName(SDK::AHWCharacter_Base* ch, char* out, size_t outLen)
    {
        out[0] = '\0';
        __try {
            if (!ch || !Helpers::IsValidPtr(ch))
                return false;

            SDK::UClass* cls = ch->Class;
            if (!cls || !Helpers::IsValidPtr(cls))
                return false;

            return CopyObjectName(reinterpret_cast<SDK::UObject*>(cls), out, outLen);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return false;
    }

    std::string ResolveGodCode(SDK::APawn* pawn)
    {
        if (!pawn || !Helpers::SafeIsA(pawn, SDK::AHWCharacter_Base::StaticClass()))
            return {};

        auto* ch = static_cast<SDK::AHWCharacter_Base*>(pawn);

        char raw[128]{};
        if (const SDK::UHWCharacterItem* item = ResolveCharacterItem(ch))
        {
            if (CopyObjectName(const_cast<SDK::UHWCharacterItem*>(item), raw, sizeof(raw)))
            {
                const std::string code = GodCodeFromRawName(raw);
                if (!code.empty())
                    return FinalizeGodCode(pawn, code);
            }
        }

        if (CopyObjectName(static_cast<SDK::UObject*>(ch), raw, sizeof(raw)))
        {
            const std::string code = GodCodeFromRawName(raw);
            if (!code.empty())
                return FinalizeGodCode(pawn, code);
        }

        if (TryCopyClassName(ch, raw, sizeof(raw)))
        {
            const std::string code = GodCodeFromRawName(raw);
            if (!code.empty())
                return FinalizeGodCode(pawn, code);
        }

        const std::string display = Helpers::GetPawnDisplayName(pawn);
        if (!display.empty())
        {
            const std::string code = GodCodeFromRawName(display);
            if (!code.empty())
                return FinalizeGodCode(pawn, code);

            const std::string fuzzy = LookupGodCodeFuzzy(display);
            if (!fuzzy.empty())
                return FinalizeGodCode(pawn, fuzzy);
        }

        if (CopyObjectName(static_cast<SDK::UObject*>(pawn), raw, sizeof(raw)))
        {
            const std::string fuzzy = LookupGodCodeFuzzy(raw);
            if (!fuzzy.empty())
                return FinalizeGodCode(pawn, fuzzy);
        }

        return {};
    }

    IconView GetAbilityIcon(const std::string& godCode, int slot1to4)
    {
        IconView empty{};
        if (godCode.empty() || slot1to4 < 1 || slot1to4 > 4)
            return empty;

        if (!g_Ready && !Initialize())
            return empty;

        const std::string canonical = CanonicalizeGodCode(godCode);
        if (canonical.empty())
            return empty;

        const std::string cacheKey = MakeKey(canonical, slot1to4);

        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            auto it = g_TextureCache.find(cacheKey);
            if (it != g_TextureCache.end())
                return it->second;
        }

        std::vector<uint8_t> cachedBytes;
        if (IconFetch::TryLoadCachedAbility(canonical, slot1to4, cachedBytes))
            return LoadIconTextureFromBytes(cacheKey, cachedBytes.data(), (unsigned int)cachedBytes.size());

        const EmbeddedEntry* entry = FindEmbeddedEntry(canonical, slot1to4);
        if (entry)
            return LoadIconTexture(cacheKey, *entry);

        return TryLoadRemoteIcon(
            cacheKey,
            [&]() { IconFetch::RequestAbilityIcon(canonical, slot1to4, cacheKey); },
            [&](std::vector<uint8_t>& bytes) {
                return IconFetch::TryLoadCachedAbility(canonical, slot1to4, bytes);
            });
    }

    static const char* NormalizeAgilityRelicCompact(const std::string& compact)
    {
        if (compact.empty())
            return nullptr;

        if (compact == "AGILITYRELIC" || compact == "WINDSHARD")
            return "AGILITYRELIC";
        if (compact.find("AGILITYRELIC") != std::string::npos)
            return "AGILITYRELIC";
        if (compact.find("WINDSHARD") != std::string::npos)
            return "AGILITYRELIC";
        if (compact.find("AGILITYGREAVES") != std::string::npos)
            return "AGILITYRELIC";
        if (compact.find("AGILITY") != std::string::npos &&
            (compact.find("GREAVES") != std::string::npos ||
             compact.find("RELIC") != std::string::npos))
            return "AGILITYRELIC";
        if (compact.find("GENERICDASH") != std::string::npos &&
            compact.find("AGILITY") != std::string::npos)
            return "AGILITYRELIC";
        return nullptr;
    }

    static const char* MapRelicAlias(const std::string& compact)
    {
        if (const char* agility = NormalizeAgilityRelicCompact(compact))
            return agility;

        static const struct { const char* from; const char* to; } kAliases[] = {
            { "SCORCHINGBLINKRUNE", "SCORCHINGBLINKRUNE" },
            { "SCORCHINGBLINK", "SCORCHINGBLINKRUNE" },
            { "SCORCHED", "SCORCHINGBLINKRUNE" },
            { "BLINKRUNE", "SCORCHINGBLINKRUNE" },
            { "BLINKINGABYSS", "BLINKINGABYSS" },
            { "MAGICSHELL", "PHANTOMSHELL" },
            { "ENTANGLINGWINGS", "HEAVENLYWINGS" },
            { "HORRIFICEMBLEM", "EMBLEMOFINCREASINGPERIL" },
            { "EMBLEMOFINCREASING", "EMBLEMOFINCREASINGPERIL" },
            { "CLOAKOFTHEASCETIC", "CLOAKOFMEDITATION" },
            { "BRACEROFBRILLIANCE", "BRACEROFRADIANCE" },
            { "PHANTOMSHELL", "PHANTOMSHELL" },
            { "PHANTOM", "PHANTOM" },
            { "SHELL", "PHANTOMSHELL" },
            { "SHELLOFREBUKE", "SHELLOFREBUKE" },
            { "PURIFICATIONBEADS", "PURIFICATIONBEADS" },
            { "PURIFICATION", "PURIFICATIONBEADS" },
            { "TEMPORALBEADS", "TEMPORALBEADS" },
            { "TALISMANOFPURIFICATION", "TALISMANOFPURIFICATION" },
            { "SUNDERINGARC", "SUNDERINGBLAST" },
            { "SUNDERINGBLAST", "SUNDERINGBLAST" },
            { "SUNDERINGECHO", "SUNDERINGECHO" },
            { "SUNDERINGSPEER", "SUNDERINGSPEER" },
            { "SUNDERINGSIPHON", "SUNDERINGSIPHON" },
            { "SUNDER", "SUNDER" },
            { "AEGISOFACCELERATION", "AEGISOFACCELERATION" },
            { "TIMELOCKAEGIS", "TIMELOCKAEGIS" },
            { "SHIELDOFTHORNS", "SHIELDOFTHORNS" },
            { "THORNSOFOVERGROWTH", "THORNSOFOVERGROWTH" },
            { "THORNSOFSAPPINGSTRENGTH", "THORNSOFSAPPINGSTRENGTH" },
            { "THORNS", "SHIELDOFTHORNS" },
            { "TELEPORTGLYPH", "TELEPORTGLYPH" },
            { "TELEPORT", "TELEPORT" },
            { "TELEPORTFRAGMENT", "TELEPORTFRAGMENT" },
            { "PERSISTENTTELEPORT", "PERSISTENTTELEPORT" },
            { "SANCTUARY", "SANCTUARY" },
            { "SPRINT", "SPRINT" },
            { "SCOUT", "SCOUT" },
            { "WRATH", "WRATH" },
            { "AGILITYRELIC", "AGILITYRELIC" },
            { "AGILITYGREAVES", "AGILITYRELIC" },
            { "WINDSHARD", "AGILITYRELIC" },
            { "VISIONSHARD", "VISIONSHARD" },
            { "VIBRANTSHARD", "VIBRANTSHARD" },
            { "STURDYSHARD", "STURDYSHARD" },
        };

        for (const auto& alias : kAliases)
        {
            if (compact == alias.from)
                return alias.to;
        }
        return nullptr;
    }

    static bool RelicNameLooksPassive(const char* upper)
    {
        if (!upper || !upper[0])
            return true;
        if (strstr(upper, "GRANTEDABILITIES") != nullptr)
            return true;
        return false;
    }

    static bool RelicNameLooksActive(const char* upper)
    {
        if (!upper || !upper[0] || RelicNameLooksPassive(upper))
            return false;

        static const char* kMarkers[] = {
            "AEGISOFACCELERATION", "TIMELOCKAEGIS", "BLINKRUNE", "BLINKINGABYSS",
            "SCORCHINGBLINK", "SCORCHINGBLINKRUNE", "SCORCHED", "PURIFICATIONBEADS",
            "TEMPORALBEADS", "PURIFICATION", "TALISMANOFPURIFICATION", "MAGICSHELL",
            "PHANTOMSHELL", "PHANTOM", "SHELLOFREBUKE", "SUNDERINGARC", "SUNDERINGBLAST",
            "SUNDERINGECHO", "SUNDERINGSPEER", "SUNDERINGSIPHON", "SUNDER", "SANCTUARY",
            "SPRINT", "SCOUT", "WRATH", "SHIELDOFTHORNS", "THORNSOFOVERGROWTH",
            "THORNSOFSAPPINGSTRENGTH", "TELEPORTFRAGMENT", "PERSISTENTTELEPORT",
            "TELEPORTGLYPH", "TELEPORT", "AGILITYRELIC", "AGILITYGREAVES",
            "VISIONSHARD", "WINDSHARD", "VIBRANTSHARD",
            "STURDYSHARD", "HEAVENLYWINGS", "ENTANGLINGWINGS",
            "EMBLEMOFINCREASING", "HORRIFICEMBLEM", "DIVINEBARRIER", "MAGISCLOAK",
            "PROPHETICCLOAK", "CLOAKOFMEDITATION", "CLOAKOFTHEASCETIC",
            "BRACEROFRADIANCE", "BRACEROFBRILLIANCE", "BLESSEDBARRIER",
            "PHOENIXAMULET", "HUSSARSWINGS", nullptr
        };

        for (int i = 0; kMarkers[i]; ++i)
        {
            if (strstr(upper, kMarkers[i]) != nullptr)
                return true;
        }

        return (strstr(upper, "GA_ITEMS_") != nullptr || strstr(upper, "GA_ITEM_") != nullptr);
    }

    static bool RelicNameIdentifiesIcon(const char* upper)
    {
        if (!upper || !upper[0] || RelicNameLooksPassive(upper))
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

        return RelicNameLooksActive(upper);
    }

    static std::string RelicCodeFromRawName(const char* raw)
    {
        if (!raw || !raw[0])
            return {};

        std::string name = raw;
        name = ToUpperAscii(std::move(name));

        static const char* kStrip[] = {
            "GA_ITEMS_", "GA_ITEM_", "GE_ITEM_", "GE_ITEMS_", "ITEMS_", "ITEM_",
            "RELIC_", "T3_", "BP_", "ABP_", "WBP_", nullptr
        };
        for (int i = 0; kStrip[i]; ++i)
        {
            const size_t len = strlen(kStrip[i]);
            if (name.size() >= len && _strnicmp(name.c_str(), kStrip[i], (int)len) == 0)
            {
                name = name.substr(len);
                break;
            }
        }

        while (!name.empty() && (name.back() == '_' ||
               (name.back() >= '0' && name.back() <= '9')))
            name.pop_back();
        if (name.size() > 2 && name.compare(name.size() - 2, 2, "_C") == 0)
            name.resize(name.size() - 2);

        const std::string compact = CompactGodKey(name);
        if (compact.empty())
            return {};

        if (const char* alias = MapRelicAlias(compact))
            return std::string(alias);

        if (g_EmbeddedIndex.find(MakeRelicKey(compact)) != g_EmbeddedIndex.end())
            return compact;

        for (const auto& pair : g_EmbeddedIndex)
        {
            if (pair.first.rfind("RELIC|", 0) != 0)
                continue;
            const std::string packCode = pair.first.substr(6);
            if (compact.find(CompactGodKey(packCode)) != std::string::npos ||
                CompactGodKey(packCode).find(compact) != std::string::npos)
                return packCode;
        }

        return compact;
    }

    std::string CanonicalizeRelicCode(std::string code)
    {
        if (code.empty())
            return code;

        code = ToUpperAscii(std::move(code));
        const std::string compact = CompactGodKey(code);
        if (compact.empty())
            return {};

        if (const char* alias = MapRelicAlias(compact))
            return std::string(alias);

        if (g_EmbeddedIndex.find(MakeRelicKey(compact)) != g_EmbeddedIndex.end())
            return compact;

        return compact;
    }

    static const EmbeddedEntry* FindRelicEmbeddedEntry(const std::string& relicCode)
    {
        const std::string canonical = CanonicalizeRelicCode(relicCode);
        if (canonical.empty())
            return nullptr;

        auto it = g_EmbeddedIndex.find(MakeRelicKey(canonical));
        if (it != g_EmbeddedIndex.end())
            return &it->second;

        const std::string compact = CompactGodKey(canonical);
        for (const auto& pair : g_EmbeddedIndex)
        {
            if (pair.first.rfind("RELIC|", 0) != 0)
                continue;
            const std::string packCode = pair.first.substr(6);
            if (CompactGodKey(packCode) == compact)
                return &pair.second;
        }

        return nullptr;
    }

    static bool InventorySlotIsRelicTag(const SDK::FHWInventorySlot& slot)
    {
        char buf[96]{};
        __try {
            wchar_t wbuf[96]{};
            UC::FString fstr(wbuf, 0, 96);
            if (!SDK::FName::AppendString)
                SDK::FName::InitInternal();
            auto appendFn = reinterpret_cast<void(*)(const SDK::FName*, UC::FString&)>(
                SDK::FName::AppendString);
            SDK::InSDKUtils::CallGameFunction(appendFn, &slot.Type.TagName, fstr);
            const wchar_t* w = fstr.CStr();
            if (!w || !*w)
                return false;
            WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)sizeof(buf), nullptr, nullptr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }

        for (char* p = buf; *p; ++p)
            *p = (char)std::toupper((unsigned char)*p);
        return strstr(buf, "RELIC") != nullptr || strstr(buf, "SHARD") != nullptr;
    }

    static bool ResolveRelicRawName(SDK::APawn* pawn, char* outName, size_t outLen)
    {
        if (!pawn || !outName || outLen == 0)
            return false;
        outName[0] = '\0';

        if (!Helpers::SafeIsA(pawn, SDK::AHWCharacter_Base::StaticClass()))
            return false;

        auto* ch = static_cast<SDK::AHWCharacter_Base*>(pawn);
        bool found = false;

        __try {
            SDK::UAbilitySystemComponent* asc = ch->GetHWAbilitySystemComponent();
            SDK::UHWAbilitySystemComponent* hwAsc = ch->GetHWAbilitySystemComponent();
            if (!asc || !Helpers::IsValidPtr(asc))
                asc = ch->GetAbilitySystemComponent();
            if (!asc || !Helpers::IsValidPtr(asc))
                found = false;
            else
            {
                const auto& items = asc->ActivatableAbilities.Items;
                for (int s = 0; s < items.Num(); ++s)
                {
                    if (!items.IsValidIndex(s))
                        continue;

                    const SDK::FGameplayAbilitySpec& spec = items[s];
                    SDK::UGameplayAbility* ability = spec.Ability;
                    if (!ability || !Helpers::IsValidPtr(ability))
                        continue;

                    char name[128]{};
                    if (!CopyObjectName(ability, name, sizeof(name)))
                        continue;

                    char upper[128]{};
                    strncpy_s(upper, sizeof(upper), name, _TRUNCATE);
                    for (char* p = upper; *p; ++p)
                        *p = (char)std::toupper((unsigned char)*p);
                    if (!RelicNameIdentifiesIcon(upper))
                        continue;

                    bool inRelicSlot = false;
                    if (hwAsc && Helpers::IsValidPtr(hwAsc))
                    {
                        const SDK::FHWInventorySlot invSlot =
                            SDK::UHWLibrary_EquipmentSystem::GetInventorySlotFromAbility(
                                spec.Handle, hwAsc);
                        inRelicSlot = InventorySlotIsRelicTag(invSlot);
                    }

                    if (!inRelicSlot && spec.InputID < 5 &&
                        strstr(upper, "GA_ITEMS_") == nullptr &&
                        strstr(upper, "GA_ITEM_") == nullptr)
                        continue;

                    strncpy_s(outName, outLen, name, _TRUNCATE);
                    found = true;
                    break;
                }

                if (!found && hwAsc && Helpers::IsValidPtr(hwAsc))
                {
                    auto* invMgr = ch->GetInventoryManager();
                    if (invMgr && Helpers::IsValidPtr(invMgr))
                    {
                        static const char* kRelicTypeTags[] = {
                            "Equipment.Type.Relic", "Equipment.Relic",
                            "Item.Type.Relic", "Relic",
                            "Equipment.Type.Shard", "Shard", nullptr
                        };

                        for (int t = 0; kRelicTypeTags[t] && !found; ++t)
                        {
                            SDK::FGameplayTag relicType{};
                            wchar_t wtag[64]{};
                            MultiByteToWideChar(CP_UTF8, 0, kRelicTypeTags[t], -1, wtag, 64);
                            const SDK::FName tagName =
                                SDK::UKismetStringLibrary::Conv_StringToName(SDK::FString(wtag));
                            if (!SDK::UHWAIBlueprintLibrary::RequestGameplayTagFromName(
                                    tagName, &relicType))
                                continue;

                            SDK::TArray<SDK::UHWEquipmentInstance*> equipmentList =
                                invMgr->RetrieveEquipmentsBySlotType(relicType);

                            for (int eq = 0; eq < equipmentList.Num(); ++eq)
                            {
                                if (!equipmentList.IsValidIndex(eq))
                                    continue;

                                SDK::UHWEquipmentInstance* instance = equipmentList[eq];
                                if (!instance || !Helpers::IsValidPtr(instance))
                                    continue;

                                SDK::FHWInventorySlot invSlot = instance->GetInventorySlot();
                                SDK::UGameplayAbility* ability =
                                    SDK::UHWLibrary_AbilitySystem::GetPrimaryAbilityInstanceBasedOnInventorySlot(
                                        hwAsc, invSlot);
                                if (!ability || !Helpers::IsValidPtr(ability))
                                    continue;

                                char name[128]{};
                                if (!CopyObjectName(ability, name, sizeof(name)))
                                    continue;

                                char upper[128]{};
                                strncpy_s(upper, sizeof(upper), name, _TRUNCATE);
                                for (char* p = upper; *p; ++p)
                                    *p = (char)std::toupper((unsigned char)*p);
                                if (RelicNameLooksPassive(upper))
                                    continue;
                                if (!RelicNameIdentifiesIcon(upper) &&
                                    !RelicNameLooksActive(upper))
                                    continue;

                                strncpy_s(outName, outLen, name, _TRUNCATE);
                                found = true;
                                break;
                            }
                        }

                        if (!found)
                        {
                            const auto& entries = invMgr->Inventory.Entries;
                            for (int eq = 0; eq < entries.Num() && !found; ++eq)
                            {
                                if (!entries.IsValidIndex(eq))
                                    continue;

                                SDK::UHWEquipmentInstance* instance = entries[eq].EquipmentInstance;
                                if (!instance || !Helpers::IsValidPtr(instance))
                                    continue;

                                char name[128]{};
                                SDK::UHWEquipmentItem* item = instance->EquipmentItem;
                                if (item && Helpers::IsValidPtr(item) &&
                                    CopyObjectName(item, name, sizeof(name)))
                                {
                                    char upper[128]{};
                                    strncpy_s(upper, sizeof(upper), name, _TRUNCATE);
                                    for (char* p = upper; *p; ++p)
                                        *p = (char)std::toupper((unsigned char)*p);
                                    if (RelicNameLooksPassive(upper))
                                        continue;
                                    if (!RelicNameIdentifiesIcon(upper) &&
                                        !RelicNameLooksActive(upper))
                                        continue;
                                    strncpy_s(outName, outLen, name, _TRUNCATE);
                                    found = true;
                                }
                            }
                        }
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            found = false;
        }

        return found && outName[0] != '\0';
    }

    std::string ResolveRelicCode(SDK::APawn* pawn)
    {
        char rawName[128]{};
        if (!ResolveRelicRawName(pawn, rawName, sizeof(rawName)))
            return {};
        return RelicCodeFromRawName(rawName);
    }

    IconView GetRelicIcon(const std::string& relicCode)
    {
        IconView empty{};
        if (relicCode.empty())
            return empty;

        if (!g_Ready && !Initialize())
            return empty;

        const std::string canonical = CanonicalizeRelicCode(relicCode);
        if (canonical.empty())
            return empty;

        const std::string cacheKey = MakeRelicKey(canonical);

        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            auto it = g_TextureCache.find(cacheKey);
            if (it != g_TextureCache.end())
                return it->second;
        }

        std::vector<uint8_t> cachedBytes;
        if (IconFetch::TryLoadCachedRelic(canonical, cachedBytes))
        {
            IconView cached = LoadIconTextureFromBytes(
                cacheKey, cachedBytes.data(), (unsigned int)cachedBytes.size());
            if (cached.valid)
                return cached;
            IconFetch::DeleteCachedRelic(canonical);
        }

        if (IconFetch::TryTakeCompleted(cacheKey, cachedBytes))
        {
            IconView completed = LoadIconTextureFromBytes(
                cacheKey, cachedBytes.data(), (unsigned int)cachedBytes.size());
            if (completed.valid)
                return completed;
            IconFetch::DeleteCachedRelic(canonical);
        }

        const EmbeddedEntry* entry = FindRelicEmbeddedEntry(canonical);
        if (!entry && canonical == "AGILITYRELIC")
            entry = FindRelicEmbeddedEntry("WINDSHARD");
        if (entry)
        {
            IconView embedded = LoadIconTexture(cacheKey, *entry);
            if (embedded.valid)
                return embedded;
        }

        IconView remote = TryLoadRemoteIcon(
            cacheKey,
            [&]() { IconFetch::RequestRelicIcon(canonical, cacheKey); },
            [&](std::vector<uint8_t>& bytes) {
                return IconFetch::TryLoadCachedRelic(canonical, bytes);
            });
        return remote;
    }
}
