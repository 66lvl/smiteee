#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace IconFetch
{
    bool Initialize();
    void Shutdown();

    bool TryLoadCachedAbility(const std::string& godCode,
                              int slot1to4,
                              std::vector<uint8_t>& outBytes);
    bool TryLoadCachedRelic(const std::string& relicCode,
                            std::vector<uint8_t>& outBytes);

    void RequestAbilityIcon(const std::string& godCode,
                              int slot1to4,
                              const std::string& cacheKey);
    void RequestRelicIcon(const std::string& relicCode,
                          const std::string& cacheKey);

    void DeleteCachedRelic(const std::string& relicCode);

    bool TryTakeCompleted(const std::string& cacheKey,
                          std::vector<uint8_t>& outBytes);
}
