#include "icon_fetch.h"
#include <Windows.h>
#include <ShlObj.h>
#include <winhttp.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "winhttp.lib")

namespace IconFetch
{
    namespace
    {
        constexpr wchar_t kUserAgent[] = L"HemingwayTrainer/1.0 (runtime icon fetch)";

        struct Job
        {
            std::string cacheKey;
            bool        isRelic = false;
            std::string godCode;
            int         slot = 0;
            std::string relicCode;
        };

        enum class JobState { Pending, Ready, Failed };

        std::mutex g_Mutex;
        std::condition_variable g_Cv;
        std::queue<Job> g_Queue;
        std::unordered_map<std::string, JobState> g_State;
        std::unordered_map<std::string, std::vector<uint8_t>> g_Results;
        std::unordered_map<std::string, DWORD> g_LastRetryTick;
        std::thread g_Worker;
        std::atomic<bool> g_Running{ false };
        std::atomic<bool> g_Started{ false };
        std::atomic<bool> g_RelicCatalogReady{ false };
        std::string g_CacheRoot;
        std::mutex g_CatalogMutex;
        std::unordered_map<std::string, std::string> g_RelicCatalog;

        std::wstring ToWide(const std::string& s);
        bool HttpGetBytes(const std::wstring& host,
                          const std::wstring& path,
                          std::vector<uint8_t>& out);
        bool IsValidImageBytes(const std::vector<uint8_t>& data);
        bool DeleteFileIfExists(const std::string& path);
        std::string UrlEncode(const std::string& value);
        bool DownloadWikiFile(const std::wstring& apiHost,
                              const std::string& fileTitle,
                              std::vector<uint8_t>& out);

        std::string CompactRelicKey(const std::string& value)
        {
            std::string out;
            out.reserve(value.size());
            for (unsigned char c : value)
            {
                if (std::isalnum(c))
                    out.push_back((char)std::toupper(c));
            }
            return out;
        }

        bool JsonExtractTitles(const std::string& json, std::vector<std::string>& titles)
        {
            const std::string key = "\"title\":\"";
            size_t pos = 0;
            bool found = false;
            while ((pos = json.find(key, pos)) != std::string::npos)
            {
                pos += key.size();
                size_t end = pos;
                while (end < json.size() && json[end] != '"')
                {
                    if (json[end] == '\\' && end + 1 < json.size())
                        end += 2;
                    else
                        ++end;
                }
                if (end > pos)
                {
                    titles.push_back(json.substr(pos, end - pos));
                    found = true;
                }
                pos = end + 1;
            }
            return found;
        }

        bool JsonExtractContinueToken(const std::string& json,
                                      const char* key,
                                      std::string& outToken)
        {
            const std::string needle = std::string("\"") + key + "\":\"";
            size_t pos = json.find(needle);
            if (pos == std::string::npos)
                return false;
            pos += needle.size();
            size_t end = json.find('"', pos);
            if (end == std::string::npos)
                return false;
            outToken = json.substr(pos, end - pos);
            return true;
        }

        bool WikiApiQuery(const std::wstring& host,
                          const std::string& query,
                          std::string& outJson)
        {
            std::vector<uint8_t> body;
            if (!HttpGetBytes(host, ToWide(query), body))
                return false;
            outJson.assign(body.begin(), body.end());
            return !outJson.empty();
        }

        bool ListWikiCategoryFiles(const wchar_t* host,
                                   const char* category,
                                   std::vector<std::string>& titles)
        {
            std::string cmContinue;
            for (int page = 0; page < 32; ++page)
            {
                std::string query =
                    "/api.php?action=query&format=json&list=categorymembers&cmtype=file&cmlimit=500&cmtitle=" +
                    UrlEncode(category);
                if (!cmContinue.empty())
                    query += "&cmcontinue=" + UrlEncode(cmContinue);

                std::string json;
                if (!WikiApiQuery(host, query, json))
                    return false;

                JsonExtractTitles(json, titles);

                std::string next;
                if (!JsonExtractContinueToken(json, "cmcontinue", next))
                    break;
                cmContinue = std::move(next);
            }
            return true;
        }

        bool ParseRelicWikiTitle(const std::string& fileTitle,
                                 std::string& wikiBase,
                                 std::string& compactOut)
        {
            std::string name = fileTitle;
            if (name.rfind("File:", 0) == 0)
                name = name.substr(5);

            const struct { const char* prefix; size_t len; } kPrefixes[] = {
                { "Relic ", 6 },
                { "T3 ", 3 },
            };

            for (const auto& prefix : kPrefixes)
            {
                if (name.size() >= prefix.len &&
                    _strnicmp(name.c_str(), prefix.prefix, (int)prefix.len) == 0)
                {
                    name = name.substr(prefix.len);
                    break;
                }
            }

            static const char* kSuffixes[] = {
                " UpgradedRelic Old.png",
                " UpgradedRelic.png",
                " Relic S9 Old.png",
                " Relic S9.png",
                " Relic Old.png",
                " Relic.png",
                ".png",
            };

            for (const char* suffix : kSuffixes)
            {
                const size_t len = strlen(suffix);
                if (name.size() >= len &&
                    _strnicmp(name.c_str() + name.size() - len, suffix, (int)len) == 0)
                {
                    name = name.substr(0, name.size() - len);
                    break;
                }
            }

            wikiBase = name;
            compactOut = CompactRelicKey(wikiBase);
            if (compactOut.empty())
                return false;

            // SMITE 2 wiki uses Relic_Agility_Relic.png style assets keyed as Agility Relic.
            if (compactOut == "AGILITYRELIC")
                wikiBase = "Relic_Agility";
            else if (compactOut == "BLINKRUNE" || compactOut == "SCORCHINGBLINKRUNE")
                wikiBase = "Relic_BlinkRune";
            else if (compactOut == "PHANTOMSHELL" || compactOut == "MAGICSHELL")
                wikiBase = "Relic_PhantomShell";
            else if (compactOut == "PURIFICATIONBEADS")
                wikiBase = "Relic_PurificationBeads";
            else if (compactOut == "SUNDERINGBLAST" || compactOut == "SUNDERINGARC")
                wikiBase = "Relic_SunderingArc";
            else if (compactOut == "AEGISOFACCELERATION")
                wikiBase = "Relic_AegisofAcceleration";

            if (compactOut == "EMPTY" || compactOut == "LOCKED" ||
                compactOut == "SHARDRELIC" || compactOut == "SHARDRELICEMPTY")
                return false;

            return true;
        }

        void IngestRelicCatalogHost(const wchar_t* host, bool overwriteExisting)
        {
            std::vector<std::string> titles;
            if (!ListWikiCategoryFiles(host, "Category:Relic_icons", titles))
                return;

            std::lock_guard<std::mutex> lock(g_CatalogMutex);
            for (const std::string& title : titles)
            {
                std::string base;
                std::string compact;
                if (!ParseRelicWikiTitle(title, base, compact))
                    continue;

                if (!overwriteExisting &&
                    g_RelicCatalog.find(compact) != g_RelicCatalog.end())
                    continue;

                g_RelicCatalog[compact] = base;
            }
        }

        void EnsureRelicCatalog()
        {
            if (g_RelicCatalogReady.load())
                return;

            IngestRelicCatalogHost(L"wiki.smite2.com", true);
            IngestRelicCatalogHost(L"smite.fandom.com", false);

            g_RelicCatalogReady = true;
            printf("[+] IconFetch: relic catalog loaded (%zu entries)\n",
                   g_RelicCatalog.size());
        }

        void AddUniqueCandidate(std::vector<std::string>& out,
                                std::unordered_set<std::string>& seen,
                                const std::string& value)
        {
            if (value.empty() || !seen.insert(value).second)
                return;
            out.push_back(value);
        }

        const char* RelicWikiBaseName(const std::string& code);

        void AddRelicWikiCandidates(const std::string& code,
                                    std::vector<std::string>& out,
                                    std::unordered_set<std::string>& seen)
        {
            static const struct
            {
                const char* code;
                const char* bases[6];
            } kMultiMap[] = {
                { "AGILITYRELIC", { "Relic_Agility", "WindShard", nullptr } },
                { "WINDSHARD", { "Relic_Agility", "WindShard", nullptr } },
                { "SCORCHINGBLINKRUNE", { "ScorchingBlinkRune", "Relic_BlinkRune", "Relic_Blink_Rune", nullptr } },
                { "BLINKRUNE", { "ScorchingBlinkRune", "Relic_BlinkRune", "Relic_Blink_Rune", nullptr } },
                { "BLINKINGABYSS", { "T3_Blinking_Abyss", "Relic_BlinkRune", "Relic_Blink_Rune", nullptr } },
                { "PHANTOMSHELL", { "PhantomShell", "Relic_PhantomShell", "Relic_Phantom_Shell", nullptr } },
                { "MAGICSHELL", { "PhantomShell", "Relic_PhantomShell", "Relic_Phantom_Shell", nullptr } },
                { "PHANTOM", { "Phantom", "Relic_PhantomShell", "Relic_Phantom_Shell", nullptr } },
                { "SHELLOFREBUKE", { "ShellOfRebuke", "T3_Shell_of_Rebuke", "Relic_PhantomShell", nullptr } },
                { "PURIFICATIONBEADS", { "PurificationBeads", "Relic_PurificationBeads", "Relic_Purification_Beads", nullptr } },
                { "TEMPORALBEADS", { "TemporalBeads", "Relic_PurificationBeads", "Relic_Purification_Beads", nullptr } },
                { "TALISMANOFPURIFICATION", { "TalismanOfPurification", "T3_Talisman_of_Purification", "Relic_PurificationBeads", nullptr } },
                { "SUNDERINGBLAST", { "SunderingBlast", "Relic_SunderingArc", "Relic_Sundering_Arc", nullptr } },
                { "SUNDERINGARC", { "SunderingBlast", "Relic_SunderingArc", "Relic_Sundering_Arc", nullptr } },
                { "SUNDERINGECHO", { "SunderingEcho", "T3_Sundering_Echo", "Relic_SunderingArc", nullptr } },
                { "SUNDERINGSPEER", { "SunderingSpear", "Relic_SunderingArc", nullptr } },
                { "SUNDERINGSIPHON", { "SunderingSiphon", "Sunder", nullptr } },
                { "SUNDER", { "Sunder", "Relic_SunderingArc", nullptr } },
                { "AEGISOFACCELERATION", { "AegisofAcceleration", "Relic_AegisofAcceleration", "Relic_Aegis_of_Acceleration", nullptr } },
                { "TIMELOCKAEGIS", { "TimelockAegis", "T3_Time-lock_Aegis", "Relic_AegisofAcceleration", nullptr } },
                { "SHIELDOFTHORNS", { "ShieldofThorns", "ThornsofOvergrowth", nullptr } },
                { "THORNSOFOVERGROWTH", { "ThornsofOvergrowth", "ShieldofThorns", nullptr } },
                { "THORNSOFSAPPINGSTRENGTH", { "ThornsofSappingStrength", "ShieldofThorns", nullptr } },
                { "THORNS", { "ShieldofThorns", "ThornsofSappingStrength", nullptr } },
                { "TELEPORTFRAGMENT", { "TeleportFragment", "Relic_Teleport", nullptr } },
                { "PERSISTENTTELEPORT", { "PersistentTeleport", "TeleportFragment", nullptr } },
                { "TELEPORTGLYPH", { "TeleportGlyph", "Teleport", nullptr } },
                { "TELEPORT", { "Teleport", "TeleportGlyph", nullptr } },
                { "VISIONSHARD", { "VisionShard", nullptr } },
                { "STURDYSHARD", { "SturdyShard", nullptr } },
                { "VIBRANTSHARD", { "VibrantShard", nullptr } },
                { "HEAVENLYWINGS", { "HeavenlyWings", "EntanglingWings", nullptr } },
                { "ENTANGLINGWINGS", { "EntanglingWings", "HeavenlyWings", nullptr } },
                { "BRACEROFRADIANCE", { "BracerofRadiance", "BracerOfRadiance", nullptr } },
                { "BRACEROFBRILLIANCE", { "BracerofBrilliance", "BracerofRadiance", nullptr } },
                { "CLOAKOFMEDITATION", { "CloakofMeditation", "CloakOfMeditation", nullptr } },
                { "CLOAKOFTHEASCETIC", { "CloakoftheAscetic", "CloakofMeditation", nullptr } },
                { "EMBLEMOFINCREASINGPERIL", { "EmblemofIncreasingPeril", "HorrificEmblem", nullptr } },
                { "EMBLEMOFINCREASING", { "EmblemofIncreasingPeril", "HorrificEmblem", nullptr } },
                { "HORRIFICEMBLEM", { "HorrificEmblem", "EmblemofIncreasingPeril", nullptr } },
                { "BLESSEDBARRIER", { "BlessedBarrier", nullptr } },
                { "DIVINEBARRIER", { "DivineBarrier", nullptr } },
                { "PHOENIXAMULET", { "PhoenixAmulet", nullptr } },
            };

            for (const auto& entry : kMultiMap)
            {
                if (_stricmp(code.c_str(), entry.code) != 0)
                    continue;

                for (int i = 0; entry.bases[i]; ++i)
                    AddUniqueCandidate(out, seen, entry.bases[i]);
                return;
            }

            if (const char* mapped = RelicWikiBaseName(code))
                AddUniqueCandidate(out, seen, mapped);
        }

        const char* RelicDisplayTitle(const std::string& code)
        {
            static const struct { const char* code; const char* title; } kTitles[] = {
                { "AGILITYRELIC", "Agility Relic" },
                { "WINDSHARD", "Agility Relic" },
                { "SCORCHINGBLINKRUNE", "Blink Rune" },
                { "BLINKRUNE", "Blink Rune" },
                { "BLINKINGABYSS", "Blinking Abyss" },
                { "PHANTOMSHELL", "Phantom Shell" },
                { "MAGICSHELL", "Phantom Shell" },
                { "SHELLOFREBUKE", "Shell of Rebuke" },
                { "PURIFICATIONBEADS", "Purification Beads" },
                { "TEMPORALBEADS", "Temporal Beads" },
                { "TALISMANOFPURIFICATION", "Talisman of Purification" },
                { "SUNDERINGBLAST", "Sundering Arc" },
                { "SUNDERINGARC", "Sundering Arc" },
                { "SUNDERINGECHO", "Sundering Echo" },
                { "AEGISOFACCELERATION", "Aegis of Acceleration" },
                { "TIMELOCKAEGIS", "Time-lock Aegis" },
                { "TELEPORTFRAGMENT", "Teleport Fragment" },
                { "PERSISTENTTELEPORT", "Persistent Teleport" },
                { "HEAVENLYWINGS", "Heavenly Wings" },
                { "ENTANGLINGWINGS", "Heavenly Wings" },
                { "BRACEROFRADIANCE", "Bracer of Radiance" },
                { "BRACEROFBRILLIANCE", "Bracer of Radiance" },
                { "CLOAKOFMEDITATION", "Cloak of Meditation" },
                { "CLOAKOFTHEASCETIC", "Cloak of Meditation" },
                { "EMBLEMOFINCREASINGPERIL", "Emblem of Increasing Peril" },
                { "EMBLEMOFINCREASING", "Emblem of Increasing Peril" },
                { "HORRIFICEMBLEM", "Emblem of Increasing Peril" },
                { "BLESSEDBARRIER", "Blessed Barrier" },
                { "DIVINEBARRIER", "Divine Barrier" },
            };

            for (const auto& entry : kTitles)
            {
                if (_stricmp(code.c_str(), entry.code) == 0)
                    return entry.title;
            }
            return nullptr;
        }

        std::vector<std::string> BuildRelicWikiCandidates(const std::string& relicCode)
        {
            EnsureRelicCatalog();

            std::vector<std::string> candidates;
            std::unordered_set<std::string> seen;

            AddRelicWikiCandidates(relicCode, candidates, seen);

            {
                std::lock_guard<std::mutex> lock(g_CatalogMutex);
                const auto it = g_RelicCatalog.find(relicCode);
                if (it != g_RelicCatalog.end())
                    AddUniqueCandidate(candidates, seen, it->second);
            }

            // Heuristic title-case for compact codes not in the wiki catalog yet.
            if (relicCode.size() >= 4)
            {
                std::string title;
                title.push_back((char)std::toupper((unsigned char)relicCode[0]));
                for (size_t i = 1; i < relicCode.size(); ++i)
                    title.push_back((char)std::tolower((unsigned char)relicCode[i]));
                AddUniqueCandidate(candidates, seen, title);
            }

            return candidates;
        }

        bool TryDownloadRelicTitle(const std::wstring& host,
                                   const std::string& fileTitle,
                                   std::vector<uint8_t>& out)
        {
            return DownloadWikiFile(host, fileTitle, out);
        }

        bool TryDirectWikiImage(const std::wstring& host,
                                const std::string& baseName,
                                const char* fileSuffix,
                                std::vector<uint8_t>& out)
        {
            char path[256]{};
            snprintf(path, sizeof(path), "/images/%s%s", baseName.c_str(), fileSuffix);
            return HttpGetBytes(host, ToWide(path), out);
        }

        bool FetchRelicBytes(const std::string& relicCode, std::vector<uint8_t>& out)
        {
            const std::vector<std::string> candidates = BuildRelicWikiCandidates(relicCode);
            if (candidates.empty())
                return false;

            static const char* kDirectSuffixes[] = {
                "_Relic_S9.png",
                "_Relic_S11.png",
                "_Relic_S10.png",
                "_Relic.png",
                ".png",
                "_UpgradedRelic.png",
                "_Relic Old.png",
            };

            for (const std::string& base : candidates)
            {
                for (const char* suffix : kDirectSuffixes)
                {
                    if (TryDirectWikiImage(L"wiki.smite2.com", base, suffix, out))
                        return true;
                }
            }

            if (const char* displayTitle = RelicDisplayTitle(relicCode))
            {
                char titleBuf[192]{};
                snprintf(titleBuf, sizeof(titleBuf), "File:Relic %s.png", displayTitle);
                if (TryDownloadRelicTitle(L"wiki.smite2.com", titleBuf, out))
                    return true;
            }

            char titleBuf[192]{};
            const char* patterns[] = {
                "File:%s Relic S9.png",
                "File:%s Relic S11.png",
                "File:%s Relic S10.png",
                "File:%s Relic.png",
                "File:%s UpgradedRelic.png",
                "File:%s Relic Old.png",
                "File:Relic %s.png",
                "File:T3 %s.png",
            };

            static const wchar_t* kHosts[] = {
                L"wiki.smite2.com",
                L"smite.fandom.com",
            };

            for (const wchar_t* host : kHosts)
            {
                for (const std::string& base : candidates)
                {
                    for (const char* pattern : patterns)
                    {
                        snprintf(titleBuf, sizeof(titleBuf), pattern, base.c_str());
                        if (TryDownloadRelicTitle(host, titleBuf, out))
                            return true;
                    }
                }
            }
            return false;
        }

        bool FetchRelicBytesValidated(const std::string& relicCode, std::vector<uint8_t>& out)
        {
            if (!FetchRelicBytes(relicCode, out))
                return false;
            if (IsValidImageBytes(out))
                return true;
            out.clear();
            return false;
        }

        std::string GetCacheRoot()
        {
            char appData[MAX_PATH]{};
            SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appData);
            std::string dir = std::string(appData) + "\\HemingwayTrainer\\icon_cache\\";
            CreateDirectoryA((std::string(appData) + "\\HemingwayTrainer").c_str(), nullptr);
            CreateDirectoryA(dir.c_str(), nullptr);
            CreateDirectoryA((dir + "abilities").c_str(), nullptr);
            CreateDirectoryA((dir + "relics").c_str(), nullptr);
            return dir;
        }

        std::string UrlEncode(const std::string& value)
        {
            static const char* hex = "0123456789ABCDEF";
            std::string out;
            out.reserve(value.size() * 3);
            for (unsigned char c : value)
            {
                if ((c >= 'A' && c <= 'Z') ||
                    (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.' || c == '~')
                {
                    out.push_back((char)c);
                }
                else if (c == ' ')
                {
                    out.push_back('%');
                    out.push_back('2');
                    out.push_back('0');
                }
                else
                {
                    out.push_back('%');
                    out.push_back(hex[c >> 4]);
                    out.push_back(hex[c & 0xF]);
                }
            }
            return out;
        }

        std::wstring ToWide(const std::string& s)
        {
            if (s.empty())
                return {};
            const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
            std::wstring out(len, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len);
            return out;
        }

        bool HttpGetBytes(const std::wstring& host,
                          const std::wstring& path,
                          std::vector<uint8_t>& out)
        {
            out.clear();

            HINTERNET session = WinHttpOpen(
                kUserAgent,
                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                WINHTTP_NO_PROXY_NAME,
                WINHTTP_NO_PROXY_BYPASS,
                0);
            if (!session)
                return false;

            HINTERNET connect = WinHttpConnect(
                session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
            if (!connect)
            {
                WinHttpCloseHandle(session);
                return false;
            }

            HINTERNET request = WinHttpOpenRequest(
                connect, L"GET", path.c_str(), nullptr,
                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                WINHTTP_FLAG_SECURE);
            if (!request)
            {
                WinHttpCloseHandle(connect);
                WinHttpCloseHandle(session);
                return false;
            }

            bool ok = WinHttpSendRequest(
                          request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != FALSE &&
                      WinHttpReceiveResponse(request, nullptr) != FALSE;

            if (ok)
            {
                DWORD status = 0;
                DWORD statusSize = sizeof(status);
                WinHttpQueryHeaders(
                    request,
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX,
                    &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
                ok = (status >= 200 && status < 300);
            }

            if (ok)
            {
                for (;;)
                {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0)
                        break;

                    const size_t base = out.size();
                    out.resize(base + avail);
                    DWORD read = 0;
                    if (!WinHttpReadData(request, out.data() + base, avail, &read))
                    {
                        out.resize(base);
                        ok = false;
                        break;
                    }
                    out.resize(base + read);
                }
            }

            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return ok && !out.empty();
        }

        bool IsValidImageBytes(const std::vector<uint8_t>& data)
        {
            if (data.size() < 12)
                return false;
            if (data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G')
                return true;
            if (data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
                data[8] == 'W' && data[9] == 'E' && data[10] == 'B' && data[11] == 'P')
                return true;
            return false;
        }

        bool DeleteFileIfExists(const std::string& path)
        {
            return DeleteFileA(path.c_str()) != FALSE || GetLastError() == ERROR_FILE_NOT_FOUND;
        }

        bool JsonExtractUrl(const std::string& json, std::string& outUrl)
        {
            const std::string key = "\"url\":\"";
            size_t pos = json.find(key);
            if (pos == std::string::npos)
                return false;
            pos += key.size();
            size_t end = pos;
            while (end < json.size() && json[end] != '"')
            {
                if (json[end] == '\\' && end + 1 < json.size())
                    end += 2;
                else
                    ++end;
            }
            if (end >= json.size())
                return false;

            outUrl = json.substr(pos, end - pos);
            for (size_t i = 0; i < outUrl.size(); ++i)
            {
                if (outUrl[i] == '\\' && i + 1 < outUrl.size() && outUrl[i + 1] == '/')
                {
                    outUrl.erase(i, 1);
                }
            }
            return !outUrl.empty();
        }

        bool ParseUrl(const std::string& url, std::wstring& host, std::wstring& path)
        {
            if (url.rfind("https://", 0) != 0)
                return false;

            size_t slash = url.find('/', 8);
            if (slash == std::string::npos)
                return false;

            host = ToWide(url.substr(8, slash - 8));
            path = ToWide(url.substr(slash));
            return !host.empty() && !path.empty();
        }

        bool QueryWikiImageUrl(const std::wstring& apiHost,
                               const std::string& fileTitle,
                               std::string& outUrl)
        {
            const std::string query =
                "/api.php?action=query&format=json&prop=imageinfo&iiprop=url&titles=" +
                UrlEncode(fileTitle);

            std::vector<uint8_t> body;
            if (!HttpGetBytes(apiHost, ToWide(query), body))
                return false;

            const std::string json(body.begin(), body.end());
            return JsonExtractUrl(json, outUrl);
        }

        bool DownloadWikiFile(const std::wstring& apiHost,
                              const std::string& fileTitle,
                              std::vector<uint8_t>& out)
        {
            std::string imageUrl;
            if (!QueryWikiImageUrl(apiHost, fileTitle, imageUrl))
                return false;

            std::wstring host, path;
            if (!ParseUrl(imageUrl, host, path))
                return false;

            return HttpGetBytes(host, path, out);
        }

        std::string TitleCaseSegment(std::string word)
        {
            if (word.empty())
                return word;

            for (char& c : word)
                c = (char)std::toupper((unsigned char)c);

            if (word == "SETA") return "SetA";
            if (word == "SETB") return "SetB";
            if (word == "SETC") return "SetC";
            if (word == "BACHUS") return "Bacchus";
            if (word == "PRINCESSBARI") return "PrincessBari";
            if (word == "BARI") return "PrincessBari";

            std::string out;
            out.push_back((char)std::toupper((unsigned char)word[0]));
            for (size_t i = 1; i < word.size(); ++i)
                out.push_back((char)std::tolower((unsigned char)word[i]));
            return out;
        }

        std::string GodCodeToWikiSegment(std::string godCode)
        {
            for (char& c : godCode)
                c = (char)std::toupper((unsigned char)c);

            static const struct { const char* code; const char* wiki; } kWikiGods[] = {
                { "BARI", "PrincessBari" },
                { "PRINCESSBARI", "PrincessBari" },
            };
            for (const auto& entry : kWikiGods)
            {
                if (godCode == entry.code)
                    return entry.wiki;
            }

            std::string segment;
            size_t start = 0;
            while (start <= godCode.size())
            {
                size_t next = godCode.find('_', start);
                const std::string part = godCode.substr(
                    start, next == std::string::npos ? std::string::npos : next - start);
                if (!part.empty())
                {
                    if (!segment.empty())
                        segment += ' ';
                    segment += TitleCaseSegment(part);
                }
                if (next == std::string::npos)
                    break;
                start = next + 1;
            }
            return segment;
        }

        const char* RelicWikiBaseName(const std::string& code)
        {
            static const struct { const char* code; const char* wiki; } kMap[] = {
                { "SCORCHINGBLINKRUNE", "ScorchingBlinkRune" },
                { "PHANTOMSHELL", "PhantomShell" },
                { "PHANTOM", "Phantom" },
                { "SHELLOFREBUKE", "ShellOfRebuke" },
                { "PURIFICATIONBEADS", "PurificationBeads" },
                { "TALISMANOFPURIFICATION", "TalismanOfPurification" },
                { "SUNDERINGBLAST", "SunderingBlast" },
                { "SUNDERINGECHO", "SunderingEcho" },
                { "SUNDERINGSPEER", "SunderingSpear" },
                { "SUNDERINGSIPHON", "SunderingSiphon" },
                { "SUNDER", "Sunder" },
                { "AEGISOFACCELERATION", "AegisofAcceleration" },
                { "TIMELOCKAEGIS", "TimelockAegis" },
                { "SHIELDOFTHORNS", "ShieldofThorns" },
                { "THORNSOFOVERGROWTH", "ThornsofOvergrowth" },
                { "THORNSOFSAPPINGSTRENGTH", "ThornsofSappingStrength" },
                { "TELEPORTGLYPH", "TeleportGlyph" },
                { "TELEPORT", "Teleport" },
                { "TELEPORTFRAGMENT", "TeleportFragment" },
                { "PERSISTENTTELEPORT", "PersistentTeleport" },
                { "SANCTUARY", "Sanctuary" },
                { "SPRINT", "Sprint" },
                { "SCOUT", "Scout" },
                { "WRATH", "Wrath" },
                { "AGILITYRELIC", "Relic_Agility" },
                { "WINDSHARD", "WindShard" },
                { "VISIONSHARD", "VisionShard" },
                { "VIBRANTSHARD", "VibrantShard" },
                { "STURDYSHARD", "SturdyShard" },
                { "BLINKINGABYSS", "BlinkingAbyss" },
                { "MAGICSHELL", "MagicShell" },
                { "HEAVENLYWINGS", "HeavenlyWings" },
                { "ENTANGLINGWINGS", "EntanglingWings" },
                { "EMBLEMOFINCREASING", "EmblemofIncreasingPeril" },
                { "EMBLEMOFINCREASINGPERIL", "EmblemofIncreasingPeril" },
                { "HORRIFICEMBLEM", "HorrificEmblem" },
                { "DIVINEBARRIER", "DivineBarrier" },
                { "MAGISCLOAK", "MagicCloak" },
                { "PROPHETICCLOAK", "PropheticCloak" },
                { "CLOAKOFMEDITATION", "CloakofMeditation" },
                { "CLOAKOFTHEASCETIC", "CloakoftheAscetic" },
                { "BRACEROFRADIANCE", "BracerofRadiance" },
                { "BRACEROFBRILLIANCE", "BracerofBrilliance" },
                { "BLESSEDBARRIER", "BlessedBarrier" },
                { "PHOENIXAMULET", "PhoenixAmulet" },
                { "HUSSARSWINGS", "HussarsWings" },
                { "AEGISAMULET", "AegisAmulet" },
                { "TEMPORALBEADS", "TemporalBeads" },
                { "SHELL", "Shell" },
                { "PURIFICATION", "Purification" },
            };

            for (const auto& entry : kMap)
            {
                if (_stricmp(code.c_str(), entry.code) == 0)
                    return entry.wiki;
            }
            return nullptr;
        }

        bool FetchAbilityBytes(const std::string& godCode,
                               int slot,
                               std::vector<uint8_t>& out)
        {
            const std::string segment = GodCodeToWikiSegment(godCode);
            if (segment.empty() || slot < 1 || slot > 4)
                return false;

            char titleBuf[160]{};
            const char* patterns[] = {
                "File:Icons2 %s A0%d.png",
                "File:Icons2 %s A%d.png",
                "File:Icons %s A0%d.png",
                "File:Icons %s A%d.png",
            };

            static const wchar_t* kHosts[] = {
                L"wiki.smite2.com",
                L"smite.fandom.com",
            };

            for (const wchar_t* host : kHosts)
            {
                for (const char* pattern : patterns)
                {
                    snprintf(titleBuf, sizeof(titleBuf), pattern, segment.c_str(), slot);
                    if (DownloadWikiFile(host, titleBuf, out))
                        return true;
                }
            }
            return false;
        }

        std::string AbilityCachePath(const std::string& godCode, int slot)
        {
            char buf[260]{};
            snprintf(buf, sizeof(buf), "%sabilities\\%s_%d.png",
                     g_CacheRoot.c_str(), godCode.c_str(), slot);
            return buf;
        }

        std::string RelicCachePath(const std::string& relicCode)
        {
            char buf[260]{};
            snprintf(buf, sizeof(buf), "%srelics\\%s.png",
                     g_CacheRoot.c_str(), relicCode.c_str());
            return buf;
        }

        bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out)
        {
            out.clear();
            HANDLE file = CreateFileA(
                path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return false;

            const DWORD size = GetFileSize(file, nullptr);
            if (size == INVALID_FILE_SIZE || size < 24)
            {
                CloseHandle(file);
                return false;
            }

            out.resize(size);
            DWORD read = 0;
            const BOOL ok = ReadFile(file, out.data(), size, &read, nullptr);
            CloseHandle(file);
            if (!ok || read != size)
            {
                out.clear();
                return false;
            }
            return true;
        }

        bool WriteFileBytes(const std::string& path, const std::vector<uint8_t>& data)
        {
            HANDLE file = CreateFileA(
                path.c_str(), GENERIC_WRITE, 0, nullptr,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return false;

            DWORD written = 0;
            const BOOL ok = WriteFile(
                file, data.data(), (DWORD)data.size(), &written, nullptr);
            CloseHandle(file);
            return ok && written == data.size();
        }

        void WorkerMain()
        {
            EnsureRelicCatalog();

            for (;;)
            {
                Job job{};
                {
                    std::unique_lock<std::mutex> lock(g_Mutex);
                    g_Cv.wait(lock, [] { return !g_Running.load() || !g_Queue.empty(); });
                    if (!g_Running.load() && g_Queue.empty())
                        break;
                    job = std::move(g_Queue.front());
                    g_Queue.pop();
                }

                std::vector<uint8_t> bytes;
                bool ok = false;
                if (job.isRelic)
                {
                    ok = FetchRelicBytesValidated(job.relicCode, bytes);
                    if (!ok)
                        printf("[!] IconFetch: relic icon fetch failed (%s)\n", job.relicCode.c_str());
                }
                else
                    ok = FetchAbilityBytes(job.godCode, job.slot, bytes);

                if (ok)
                {
                    if (job.isRelic)
                        WriteFileBytes(RelicCachePath(job.relicCode), bytes);
                    else
                        WriteFileBytes(AbilityCachePath(job.godCode, job.slot), bytes);
                }

                {
                    std::lock_guard<std::mutex> lock(g_Mutex);
                    g_State[job.cacheKey] = ok ? JobState::Ready : JobState::Failed;
                    if (ok)
                        g_Results[job.cacheKey] = std::move(bytes);
                }
            }
        }

        void EnsureWorker()
        {
            if (g_Started.exchange(true))
                return;
            g_CacheRoot = GetCacheRoot();
            g_Running = true;
            g_Worker = std::thread(WorkerMain);
        }

        bool ShouldRetryFailedJob(const std::string& cacheKey)
        {
            const DWORD now = GetTickCount();
            const auto it = g_LastRetryTick.find(cacheKey);
            if (it != g_LastRetryTick.end() && (now - it->second) < 5000)
                return false;
            g_LastRetryTick[cacheKey] = now;
            return true;
        }

        void EnqueueJob(Job job)
        {
            EnsureWorker();
            std::lock_guard<std::mutex> lock(g_Mutex);
            const auto it = g_State.find(job.cacheKey);
            if (it != g_State.end())
            {
                if (it->second == JobState::Pending || it->second == JobState::Ready)
                    return;
                if (it->second == JobState::Failed && !ShouldRetryFailedJob(job.cacheKey))
                    return;
            }

            g_State[job.cacheKey] = JobState::Pending;
            g_Queue.push(std::move(job));
            g_Cv.notify_one();
        }
    }

    bool Initialize()
    {
        EnsureWorker();
        return true;
    }

    void Shutdown()
    {
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            if (!g_Started.load())
                return;
            g_Running = false;
        }
        g_Cv.notify_all();
        if (g_Worker.joinable())
            g_Worker.join();
        g_Started = false;

        std::lock_guard<std::mutex> lock(g_Mutex);
        while (!g_Queue.empty())
            g_Queue.pop();
        g_State.clear();
        g_Results.clear();
        g_RelicCatalog.clear();
        g_RelicCatalogReady = false;
    }

    bool TryLoadCachedAbility(const std::string& godCode,
                              int slot1to4,
                              std::vector<uint8_t>& outBytes)
    {
        if (g_CacheRoot.empty())
            g_CacheRoot = GetCacheRoot();
        return ReadFileBytes(AbilityCachePath(godCode, slot1to4), outBytes);
    }

    bool TryLoadCachedRelic(const std::string& relicCode,
                            std::vector<uint8_t>& outBytes)
    {
        if (g_CacheRoot.empty())
            g_CacheRoot = GetCacheRoot();
        return ReadFileBytes(RelicCachePath(relicCode), outBytes);
    }

    void DeleteCachedRelic(const std::string& relicCode)
    {
        if (g_CacheRoot.empty())
            g_CacheRoot = GetCacheRoot();
        DeleteFileIfExists(RelicCachePath(relicCode));
    }

    void RequestAbilityIcon(const std::string& godCode,
                            int slot1to4,
                            const std::string& cacheKey)
    {
        Job job{};
        job.cacheKey = cacheKey;
        job.godCode = godCode;
        job.slot = slot1to4;
        EnqueueJob(std::move(job));
    }

    void RequestRelicIcon(const std::string& relicCode,
                          const std::string& cacheKey)
    {
        Job job{};
        job.cacheKey = cacheKey;
        job.isRelic = true;
        job.relicCode = relicCode;
        EnqueueJob(std::move(job));
    }

    bool TryTakeCompleted(const std::string& cacheKey,
                          std::vector<uint8_t>& outBytes)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        const auto stateIt = g_State.find(cacheKey);
        if (stateIt == g_State.end() || stateIt->second != JobState::Ready)
            return false;

        const auto dataIt = g_Results.find(cacheKey);
        if (dataIt == g_Results.end())
            return false;

        outBytes = dataIt->second;
        return true;
    }
}
