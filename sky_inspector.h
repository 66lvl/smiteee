#pragma once

// One-shot local world inspection. No object pointers escape into the UI/report.
namespace SkyInspector
{
    bool Pending();
    void ProcessOnWindowThread(); // Called only by WorldEnvironment's window dispatcher.
    void CancelPending();
    void DrawPanel();            // UI and request publication only; never reads the SDK.
}
