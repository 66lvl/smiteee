#pragma once

#include "../core/smite_sdk.h"
#include <cstddef>
#include <cstdint>

namespace SkinSeh
{
    struct RawSkinEntry
    {
        SDK::UHWSkinItem* Skin = nullptr;
        char Name[128]{};
    };

    static constexpr int kMaxRawSkins = 256;
    static constexpr int kMaxCharItems = 4;
    static constexpr int kMaxLoadedSkins = 4096;

    bool CopyObjectName(SDK::UObject* obj, char* out, size_t outLen);
    void InspectSkinIcon(SDK::UHWSkinItem* skin, char* out, size_t outLen);
    void StartLoadedIconScan(SDK::UHWSkinItem* skin);
    void TickLoadedIconScan();
    void CancelLoadedIconScan();
    bool LoadedIconScanRunning();
    int LoadedIconScanIndex();
    int LoadedIconScanLimit();
    void FormatLoadedIconReport(char* out, size_t outLen);

    SDK::AHWCharacter_Base* GetLiveGodPawn();
    SDK::AHWPlayerState* GetLocalPlayerState();
    const SDK::UHWCharacterItem* ResolveCharacterItemPtr();
    int CollectCharacterItemCandidates(const SDK::UHWCharacterItem* outItems[kMaxCharItems]);

    bool IsValidSkin(SDK::UObject* obj);

    int ScanSkinsFromGodSubsystem(const SDK::UHWCharacterItem* charItem,
                                  const char* godKey,
                                  RawSkinEntry* out,
                                  int maxOut);
    int ScanSkinsFromCharacterItem(const SDK::UHWCharacterItem* charItem,
                                   RawSkinEntry* out,
                                   int maxOut);
    int ScanSkinsByAssociation(const SDK::UHWCharacterItem* charItem,
                               const char* godKey,
                               RawSkinEntry* out,
                               int maxOut,
                               int maxAdds);
    int ScanLoadedSkinAssets(const char* godFilterKey,
                             RawSkinEntry* out,
                             int maxOut,
                             int maxAdds);
    // Diagnostic build: application is disabled.
    const char* LastApplyStatus();
    bool ApplySkinToLoadout(SDK::UHWSkinItem* skin, const SDK::UHWCharacterItem* charItem);

    /* Diagnostic build: pending apply is disabled. */
    void TickPendingApply();
    void ClearPendingApply();
}
