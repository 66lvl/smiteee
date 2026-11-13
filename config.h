#pragma once

#include "imgui/imgui.h"
#include <windows.h>
#include <Xinput.h>
#pragma comment(lib, "xinput9_1_0.lib")

#ifndef VK_XBUTTON1
#define VK_XBUTTON1 0x05
#endif
#ifndef VK_XBUTTON2
#define VK_XBUTTON2 0x06
#endif

ImVec4 hex(int hex, float alpha = 1.0);
ImVec4 operator "" _h(unsigned long long h);
#define AIMBOT
#define AIMBOT_PREDICTION
#define NORECOIL
#define DEFAULTFOV
//#define TRIGGERBOT
#define GLOW
#define THIRDPERSON
#define NAME_ESP
#define CFG_LEGACY_ESP
#define BONES
#define HP_NAME
#define HP_BAR
#define DISTANCE_ESP

extern bool hideConsole;

#ifdef AIMBOT
extern ImVec4 aimbotFovColor;
extern bool aimbotEnabled;
extern float aimbotSpeed;
extern float aimbotFov;
extern float customTimeDilation;
extern bool timeDilationEnabled;
extern int timeDilationKey;
extern int timeDilationKeyIndex;
extern bool timeDilationBinding;
extern bool timeDilationHold;
extern bool aimbotFovEnabled;
extern bool aimbotSmoothingEnabled;
extern bool aimbotLockWhenClose;
extern bool aimbotPitchEnabled;
extern bool aimbotLockStealthed;
extern bool aimbotLockDeployables;
extern bool visibilityCheckEnabled;

extern bool isHold;
extern bool invertHold;
extern int holdKey;
extern int holdKeyIndex;

#ifdef AIMBOT_PREDICTION
extern bool aimbotPredictionEnabled;
extern float aimbotPredictionSpeed;
#endif // AIMBOT_PREDICTION

// Bone aimlock settings
extern int aimbotBoneTarget;
extern const char* boneTargetNames[];
extern const int boneTargetCount;

#endif
#ifdef TRIGGERBOT
extern bool triggerbotEnabled;
#endif

#ifdef NORECOIL
extern bool noRecoilEnabled;
extern bool noSpreadEnabled;
extern bool kunaiNoSpreadEnabled;
extern bool spamreload;
extern float spamReloadMinTime;
#endif

// Anti-AFK settings
extern bool antiAfkEnabled;
extern int antiAfkLevel;

#ifdef DEFAULTFOV
extern bool defaultFovEnabled;
extern float defaultFovValue;
#endif

#ifdef THIRDPERSON
extern bool firstPersonEnabled;
#endif
#ifdef GLOW 
extern bool glowEnabled;
extern ImVec4 glowColor;
#endif

// Lobby IP/Port display settings
extern bool lobbyDisplayEnabled;
extern ImVec4 lobbyDisplayColor;
extern float lobbyDisplayFontSize;

// FPS counter settings
extern bool fpsCounterEnabled;
extern ImVec4 fpsCounterColor;
extern float fpsCounterFontSize;


// Overlay settings
extern bool vsyncEnabled;


#ifdef CFG_LEGACY_ESP
extern bool deployablesEspEnabled;
extern ImVec4 boxColor;
extern ImVec4 boxBehindWallColor;
extern ImVec4 targetColor;
extern bool boxEnabled;
extern ImVec4 boxFillColor;
extern bool boxFillEnabled;
extern int boxType; // 0=Normal, 1=Corner

#ifdef NAME_ESP
extern ImVec4 nameColor;
extern bool nameEnabled;
#endif // NAME_ESP
#ifdef BONES
extern ImVec4 boneColor;
extern bool bonesEnabled;
extern bool skeleton;
extern bool boneHead;
extern bool boneSpine;
extern bool boneArms;
extern bool boneLegs;
extern bool boneFingers;
extern bool boneDump;
#endif // BONES
#ifdef HP_NAME
extern ImVec4 hpnameColor;
extern bool hpnameEnabled;
#endif // HP_BAR
#ifdef HP_BAR
extern ImVec4 hpBarBgColor;
extern ImVec4 hpBarLowColor;
extern ImVec4 hpBarHighColor;
extern bool hpBarEnabled;
#endif // HP_BAR
extern ImVec4 tracersNearColor;
extern ImVec4 tracersFarColor;
extern bool tracersEnabled;
extern int tracersType;
extern bool useKernelMode;
extern bool useGameWindowTracking;

#ifdef DISTANCE_ESP
extern ImVec4 distanceColor;
extern bool distanceEnabled;
#endif // DISTANCE_ESP

// Ultimate status indicator
extern bool ultStatusEnabled;
extern ImVec4 ultStatusColor;
extern ImVec4 ultStatusReadyColor;

// Ultimate charge bar settings
extern bool ultChargeBarEnabled;
extern ImVec4 ultChargeBarBgColor;
extern ImVec4 ultChargeBarFillColor;
extern ImVec4 ultChargeBarReadyColor;
extern float ultChargeBarWidth;
extern float ultChargeBarHeight;

// Allies aimbot and ESP settings
extern bool alliesAimbotEnabled;
extern bool alliesVisibilityCheckEnabled;
extern bool alliesLockDeployables;
extern bool alliesDeployablesEspEnabled;
extern float alliesAimbotFov;
extern ImVec4 alliesAimbotFovColor;
extern bool alliesAimbotFovEnabled;
extern float alliesAimbotSpeed;
extern bool alliesAimbotSmoothingEnabled;
extern bool alliesAimbotPredictionEnabled;
extern float alliesAimbotPredictionSpeed;
extern int alliesAimbotBoneTarget;
extern bool alliesIsHold;
extern bool alliesInvertHold;
extern int alliesHoldKey;
extern int alliesHoldKeyIndex;
extern bool alliesEspEnabled;
extern bool alliesBoxEnabled;
extern ImVec4 alliesBoxColor;
extern ImVec4 alliesBoxFillColor;
extern bool alliesBoxFillEnabled;
extern bool alliesNameEnabled;
extern ImVec4 alliesNameColor;
extern bool alliesBonesEnabled;
extern ImVec4 alliesBoneColor;
extern bool alliesSkeleton;
extern bool alliesHpBarEnabled;
extern ImVec4 alliesHpBarBgColor;
extern ImVec4 alliesHpBarLowColor;
extern ImVec4 alliesHpBarHighColor;
extern bool alliesHpNameEnabled;
extern ImVec4 alliesHpNameColor;
extern bool alliesTracersEnabled;
extern ImVec4 alliesTracersNearColor;
extern ImVec4 alliesTracersFarColor;
extern int alliesTracersType;
#ifdef DISTANCE_ESP
extern bool alliesDistanceEnabled;
extern ImVec4 alliesDistanceColor;
#endif // DISTANCE_ESP

// Allies ultimate status indicator
extern bool alliesUltStatusEnabled;
extern ImVec4 alliesUltStatusColor;
extern ImVec4 alliesUltStatusReadyColor;

// Allies ultimate charge bar settings
extern bool alliesUltChargeBarEnabled;
extern ImVec4 alliesUltChargeBarBgColor;
extern ImVec4 alliesUltChargeBarFillColor;
extern ImVec4 alliesUltChargeBarReadyColor;
extern float alliesUltChargeBarWidth;
extern float alliesUltChargeBarHeight;
#endif // CFG_LEGACY_ESP

// Process waiting state
extern bool isWaitingForProcess;
extern bool processFound;
extern bool showWaitingUI;


static const char* holdKeys[] {
    "Left mouse button",
    "Right mouse button",
    "Middle mouse button",
    "Mouse X1",
    "Mouse X2",
    "Backspace",
    "Tab",
    "Clear",
    "Enter",
    "Shift",
    "Ctrl",
    "Alt",
    "Pause",
    "Caps Lock",
    "Escape",
    "Space",
    "Page Up",
    "Page Down",
    "End",
    "Home",
    "Left Arrow",
    "Up Arrow",
    "Right Arrow",
    "Down Arrow",
    "Print Screen",
    "Insert",
    "Delete",
    "Help",
    "0",
    "1",
    "2",
    "3",
    "4",
    "5",
    "6",
    "7",
    "8",
    "9",
    "A",
    "B",
    "C",
    "D",
    "E",
    "F",
    "G",
    "H",
    "I",
    "J",
    "K",
    "L",
    "M",
    "N",
    "O",
    "P",
    "Q",
    "R",
    "S",
    "T",
    "U",
    "V",
    "W",
    "X",
    "Y",
    "Z",
    "Numpad 0",
    "Numpad 1",
    "Numpad 2",
    "Numpad 3",
    "Numpad 4",
    "Numpad 5",
    "Numpad 6",
    "Numpad 7",
    "Numpad 8",
    "Numpad 9",
    "Numpad *",
    "Numpad +",
    "Numpad Separator",
    "Numpad -",
    "Numpad .",
    "Numpad /",
    "F1",
    "F2",
    "F3",
    "F4",
    "F5",
    "F6",
    "F7",
    "F8",
    "F9",
    "F10",
    "F11",
    "F12",
    "F13",
    "F14",
    "F15",
    "F16",
    "F17",
    "F18",
    "F19",
    "F20",
    "F21",
    "F22",
    "F23",
    "F24",
    "; (Semicolon)",
    "= (Equals)",
    ", (Comma)",
    "- (Minus)",
    ". (Period)",
    "/ (Slash)",
    "` (Grave)",
    "[ (Left Bracket)",
    "\\ (Backslash)",
    "] (Right Bracket)",
    "' (Quotes)",
    "Gamepad A",
    "Gamepad B",
    "Gamepad X",
    "Gamepad Y",
    "Gamepad LB",
    "Gamepad RB",
    "Gamepad Back",
    "Gamepad Start",
    "Gamepad Left Stick",
    "Gamepad Right Stick",
    "Gamepad DPad Up",
    "Gamepad DPad Down",
    "Gamepad DPad Left",
    "Gamepad DPad Right",
    "Gamepad Left Trigger",
    "Gamepad Right Trigger",
};

static const int holdKeysCodes[] {
    VK_LBUTTON,
    VK_RBUTTON,
    VK_MBUTTON,
    VK_XBUTTON1,
    VK_XBUTTON2,
    VK_BACK,
    VK_TAB,
    VK_CLEAR,
    VK_RETURN,
    VK_SHIFT,
    VK_CONTROL,
    VK_MENU,
    VK_PAUSE,
    VK_CAPITAL,
    VK_ESCAPE,
    VK_SPACE,
    VK_PRIOR,
    VK_NEXT,
    VK_END,
    VK_HOME,
    VK_LEFT,
    VK_UP,
    VK_RIGHT,
    VK_DOWN,
    VK_SNAPSHOT,
    VK_INSERT,
    VK_DELETE,
    VK_HELP,
    '0','1','2','3','4','5','6','7','8','9',
    'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    VK_NUMPAD0,VK_NUMPAD1,VK_NUMPAD2,VK_NUMPAD3,VK_NUMPAD4,VK_NUMPAD5,VK_NUMPAD6,VK_NUMPAD7,VK_NUMPAD8,VK_NUMPAD9,
    VK_MULTIPLY,VK_ADD,VK_SEPARATOR,VK_SUBTRACT,VK_DECIMAL,VK_DIVIDE,
    VK_F1,VK_F2,VK_F3,VK_F4,VK_F5,VK_F6,VK_F7,VK_F8,VK_F9,VK_F10,VK_F11,VK_F12,
    VK_F13,VK_F14,VK_F15,VK_F16,VK_F17,VK_F18,VK_F19,VK_F20,VK_F21,VK_F22,VK_F23,VK_F24,
    VK_OEM_1, VK_OEM_PLUS, VK_OEM_COMMA, VK_OEM_MINUS, VK_OEM_PERIOD, VK_OEM_2, VK_OEM_3, VK_OEM_4, VK_OEM_5, VK_OEM_6, VK_OEM_7,
#define GAMEPAD_CODE_MASK 0x10000
#define GAMEPAD_CODE(MASK) (GAMEPAD_CODE_MASK | (MASK))
    GAMEPAD_CODE(XINPUT_GAMEPAD_A),
    GAMEPAD_CODE(XINPUT_GAMEPAD_B),
    GAMEPAD_CODE(XINPUT_GAMEPAD_X),
    GAMEPAD_CODE(XINPUT_GAMEPAD_Y),
    GAMEPAD_CODE(XINPUT_GAMEPAD_LEFT_SHOULDER),
    GAMEPAD_CODE(XINPUT_GAMEPAD_RIGHT_SHOULDER),
    GAMEPAD_CODE(XINPUT_GAMEPAD_BACK),
    GAMEPAD_CODE(XINPUT_GAMEPAD_START),
    GAMEPAD_CODE(XINPUT_GAMEPAD_LEFT_THUMB),
    GAMEPAD_CODE(XINPUT_GAMEPAD_RIGHT_THUMB),
    GAMEPAD_CODE(XINPUT_GAMEPAD_DPAD_UP),
    GAMEPAD_CODE(XINPUT_GAMEPAD_DPAD_DOWN),
    GAMEPAD_CODE(XINPUT_GAMEPAD_DPAD_LEFT),
    GAMEPAD_CODE(XINPUT_GAMEPAD_DPAD_RIGHT),
    GAMEPAD_CODE(0x1000), 
    GAMEPAD_CODE(0x2000), 
};

static const int holdSize = (int)(sizeof(holdKeysCodes) / sizeof(holdKeysCodes[0]));

bool SaveConfig();
bool LoadConfig();

namespace Config
{
    bool Save();
    bool Load();
    enum class ResetTarget { Visuals, Cooldowns, Combat, Map, Camera, Automation, Overlay, All, Environment };
    void ResetInMemory(ResetTarget target); // Explicitly never saves to disk.
    void SaveDefault(); // Legacy API; not used by the menu. Still resets then saves.
}

// Real cooldown display (legacy)
extern float s_realCooldown[4];

// Silent Aim Experiment Toggle
extern bool silentAimExperimentEnabled;
extern int silentAimExperimentType; // 0 = Ranged A/B Trace, 1 = Other types in future
