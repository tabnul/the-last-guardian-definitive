// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: the in-game menu's Fun tab (tlg_overlay.cpp): jump to a start point and extra Tricos.
// The commands are Lua, run once by the settings tick (tlg_lua.cpp, within 15 frames).
#pragma once
#include <cstdint>
#include <string>

namespace TlgFun {
// Extra Tricos at most: the physics engine (Havok) runs out of constraint room at 10 Tricos
// in total on the terrace (s04 _090_GardenTerrace_001, 2026-11-04); 9 in total stayed stable.
inline constexpr int MaxTricos = 8;

void Jump(const char* stage, const char* area);
void AddTrico();
void AddEnemyTrico(); // the armoured one
void RemoveTricos(); // all extra Tricos, enemy ones too
// Trico actions (the Trico Tricks bar): the game's control commands, ControlCommand_C_<command>,
// asked of the story's Trico as the game's AI does (Ai2.Utils.ReqCtrlCmd). Checked one by one live
// 2026-11-15 (s04 _110_WatersideGarden_001, all 60): Act gets him into the pose the move needs, sends
// it in the same frame and watches his animation and his animation layers for the key. When the move
// does not start within ~1.5 s, Act resets him (ControlCommand_C_Restart, instant, back to standing)
// and tries once more; never while a story event, a jump or a climb has him (control mode EVENT,
// JUMP, COLUMN). 11 moves of the 60 only worked after that reset.
// "May not work": never seen working in that check; still in the bar, in their own group.
struct Action {
    const char* command; // nullptr: a group heading
    const char* name;
    const char* pose = nullptr; // nullptr: sent once (a small move, no animation to check); "": as he is;
                                // "stand", "sit", "lie", "photo" (photo pose loop), "excite" (excited
                                // stand), "sitplay" (sitting and playing): that pose first
    const char* key = nullptr;  // part of his animation name that shows the move; nullptr: the command
    float hold = 0;             // seconds to send the command every frame (the game does so for some)
};
// Group headings (the bar's group tabs, same order); the icon script and the bar key on these names.
inline constexpr const char* MayNotWork = "May not work";
inline constexpr Action Actions[] = {
    {nullptr, "Sounds and moods"},
    {"A_sneeze", "Sneeze", "", "sneeze"},
    {"A_yawn", "Yawn", "", "yawn"},
    {"roar", "Roar", "stand"},
    {"howling", "Howl", "stand"},
    {"howling_calm", "Howl softly", "stand", "howling"},
    {"A_groanA", "Groan", "", "groana"},
    {"A_face_rejoice", "Happy face", "", "rejoice"},
    {"A_inclines_neck", "Tilt head", "", "inclines"},
    {"A_scolded", "Scolded", "", "scolded"},
    {"A_refuse", "Refuse", "", "refuse"},
    {"threat_posture", "Threaten", "stand", "threat"},
    {nullptr, "Body"},
    {"flap_wings", "Flap wings", "stand"},
    {"A_wing_stretchA", "Stretch wings", "", "wing_stretcha"},
    {"A_wing_stretchB", "Stretch wings 2", "", "wing_stretchb"},
    {"stretch_front", "Stretch forward", "stand"},
    {"stretch_back", "Stretch back", "stand"},
    {"stretch_up", "Stretch up", "stand"},
    {"stand_shake_high", "Shake off", "stand", "shake_high"},
    {"stand_shake_drainsWater", "Shake off water", "stand", "drainswater"},
    {"excite_shake_head", "Excited head shake", "excite", "excite_shake_head", 3.0f},
    {"scratch_left", "Scratch left", "sit"},
    {"scratch_right", "Scratch right", "sit"},
    {"grooming_left", "Groom", "sit", "grooming"},
    {"grooming_wing_left", "Groom wing", "sit", "wing"},
    {"kneading", "Knead", "stand", "knead"},
    {"P_eye_wink_left", "Wink", "", "wink"},
    {"P_ear_shake_left", "Shake ear", "", "ear_shake"},
    {"P_fur_bristle", "Fluff fur", "", "fur_bristle"},
    {nullptr, "Play"},
    {"roll_on_ground", "Roll on the ground (needs room)", "sit"},
    {"sit_plays", "Sit and play", "sit", "sit_play"},
    {"sit_plays_pecks_nose", "Sit and peck", "sitplay", "pecks_nose", 3.0f},
    {"joy_jump", "Happy jump (needs room)", "stand"},
    {"mud_playing", "Play in the mud", "stand"},
    {"cat_toy", "Play like a cat", "stand", "cat_toy"},
    {"roll_in_nose", "Sniff and roll", "stand", "roll_in_nose"},
    {"smell", "Sniff", "stand", "smell"},
    {"touch_boy_right", "Touch the boy", "stand", "touch_right"},
    {"touch_left_side_soft", "Paw", "stand", "touch_left"}, // second half of the sealed Play act (Mods > Trico plays)
    {"toilet", "Toilet", "stand"},
    {nullptr, "Tummy"},
    {"tlg_gag", "Gag"},              // our Lua, not a control command (tlg_lua.cpp Act)
    {"tlg_throw_up", "Throw up"},    // the developers' cut throw-up, brought back
    {nullptr, "Rest"},
    {"sit", "Sit", "stand", "sit_"},
    {"sit_shake", "Sit and shake", "sit"},
    {"lie_down", "Lie down", "sit"},
    {"rise", "Get up", "stand"},
    {"stand", "Stand", ""},
    {nullptr, "Marketing / E3 poses"}, // the poses of the game's press shots and E3 demos
    {"EX_photo", "Photo pose", "stand", "photo"},
    {"EX_photo_claws", "Claws", "photo", "claws"},
    {"A_groanD", "Lion roar", "", "groand"},
    {nullptr, MayNotWork},
    {"A_excite", "Get excited", "stand", "excite"},
    {"A_hungry", "Hungry", "stand", "hungry"},
    {"puts_sand", "Dig sand", "stand", "sand"},
    {"smellA", "Sniff around", "stand", "smella"},
    {"TC_reactionA_to_boy", "React to the boy", "stand", "reaction"},
    {"lie_down_relax", "Lie down relaxed", "lie", "relax", 5.0f},
    {"liedown_smell", "Sniff lying down", "lie", "smell"},
    {"liedown_to_sleep", "Fall asleep", "lie", "sleep"},
    {"sleep", "Sleep", "lie", "sleep"},
    {"sleep_to_liedown", "Wake up", "lie", "sleep_to"},
};
void Act(const char* command);

// Trico's drool (setting trico_drool, tlg_lua.cpp Drool): the developers built it, then switched it off
// before release. The Fun tab and both launchers show this text.
inline constexpr const char* DroolHelp =
    "A feature the developers built and then switched off just before release: Trico drools. "
    "Always: a string of drool hangs from his beak the whole time. As the developers planned: it slowly "
    "grows while Trico smells something he wants, a smell-pot cart (s04_FirstAntenna and s10_Pit), and "
    "fades away when he loses interest (the boy's smell bottle was cut, so the carts are the only trigger). "
    "Stays on through jumps, new stages and extra Tricos, and is kept for the next start.";
void CountTricos();
int Tricos(); // the extra Tricos the game reported last; -1 before the first report
// Trico's disco eyes: the game's own random eye colours (EyeColorRandom, normally only in a few
// story spots), driven by our own tick. A jump turns them off.
void Disco(bool on);
bool DiscoOn();
// Trico's eye colour: every mood's colour (Default, Excite, Pulse, ...) gets this red/green/blue,
// each keeps its own brightness. EyeNormal puts the game's own colours back. A jump ends it.
void EyeColour(int r, int g, int b);
void EyeNormal();
// Trico's size: the story Trico's scale, 1 = normal. A jump or new stage brings him back to normal.
inline constexpr float SizeMin = 0.5f, SizeMax = 2.0f;
void Size(float scale);
float SizeNow();
void FreeCamera(bool on);
bool FreeCameraOn();
// Photo mode: our own fly camera (on the developers' editor camera), with the world frozen so
// the pad flies only the camera and the boy stays where he is. Two ways to freeze, because it is
// not yet tested which one keeps the fly camera moving:
//  Pause     - the game's own pause (luaCommand "trico pause", as the pause menu does).
//  StopTime  - the game's slow-motion speed set to almost zero (MS.SetGameSpeed); not exactly 0,
//              the game's scripts divide by the speed. The scene's own speed comes back after.
// Leaving puts back the normal camera and the game's speed or pause. A jump turns it off.
enum class PhotoFreeze { Pause = 0, StopTime = 1 };
inline constexpr const char* PhotoFreezeNames[] = {"Pause the game", "Stop time"};
// Photo mode button rows (Controls tab), in tlg_overlay.cpp PhotoSlots order.
inline constexpr const char* PhotoButtonNames[] = {"Screenshot", "Fly up", "Fly down", "Fast (hold)", "Slow (hold)",
                                                   "Hide / show the button help", "Freeze world on / off",
                                                   "Sticks: camera / boy"};
void PhotoMode(bool on);
bool PhotoModeOn();
/// Freeze world on / off: lets the world run under the photo camera, or freezes it again.
void PhotoWorldToggle();
bool PhotoWorldFrozen();
/// Sticks: camera or boy (the switch button, camera first). On the camera, the pad thread keeps
/// the sticks for the camera (PhotoTakesPad) and the game gets a still controller, so the boy
/// stands still; the camera tick reads them every frame through the "--TLGPAD" chunk. On the boy,
/// the game gets the controller and the camera holds still.
void PhotoSticksToggle();
bool PhotoSticksBoy();
/// Pad thread, every read: keeps the raw sticks (0..255, 128 = centre); true = hide the
/// controller from the game (photo mode, sticks on the camera, game pause menu closed).
bool PhotoTakesPad(uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry);
/// The "--TLGPAD" chunk: "return lx,ly,rx,ry" (-1..1, up = +), zeros when the camera may not move.
std::string PhotoPadChunk();
/// The photo buttons held now and the invert setting, for the camera tick (the overlay sends
/// them when they change; does nothing outside photo mode).
void PhotoPad(bool up, bool down, bool fast, bool slow, bool invert_y);
void PhotoModeFreeze(PhotoFreeze how); // takes effect at the next start of photo mode
PhotoFreeze PhotoModeFreeze();
// Tricopilot: the developers' self-driving Trico, left in the retail game
// (game/scripts/ai2/condor/condordebugautotrico.lua). Trico walks the scene by himself and the
// camera follows him. The launcher's benchmark can keep it on for a whole run (TLG_TRICOPILOT=1).
void Tricopilot(bool on);
bool TricopilotOn();
// Which camera watches him. The first three are the developers' own presets for this tool
// (TricoAutoRunTest_near / _middle / _far); Game is the normal camera, which keeps following the
// boy. Middle is what autoTrico() picks itself, Near frames Trico best in open scenes. Changing it
// while he drives takes effect at once. The launcher passes it as TLG_TRICOPILOT_CAM=near|middle|
// far|game.
enum class PilotCam { Near = 0, Middle = 1, Far = 2, Game = 3 };
void TricopilotCamera(PilotCam cam);
PilotCam TricopilotCamera();
inline constexpr const char* PilotCamNames[] = {"Near", "Middle", "Far", "Game camera"};
inline constexpr const char* PilotCamKeys[] = {"near", "middle", "far", "game"};
inline constexpr const char* TricopilotCamHelp =
    "Near keeps Trico big in the picture and is the best of the four in open scenes. Middle is the "
    "one the developers' tool picks itself: a classic top-down view, but in a narrow or roofed "
    "place the camera can end up inside the rock. Far looks down on the whole area, which is lovely "
    "in a valley and useless indoors, because Trico becomes a dot. Game camera leaves your normal "
    "camera alone: it keeps following the boy while Trico wanders off on his own.";
// The Fun tab, both launchers and the docs show this text.
inline constexpr const char* TricopilotHelp =
    "Trico drives himself: he wanders the scene, looks for edges to climb or jump, and the camera "
    "follows him. It is the developers' own debug tool, still in the game. They used it to leave the "
    "game running by itself for hours: their script can even walk him on to the next scene and knows "
    "every stage's start points, their own version of a soak test. Three of its pieces say how old it "
    "is: a line to restart the game over a network socket from a PC called D:\\trico, a camera preset "
    "named TricoAutoRunTest_middle, and helper volumes that are not even on the disc any more (the "
    "game quietly skips those, which is why he sometimes picks an odd route). Turning it off gives "
    "you the camera and Trico back. For fun only: he ignores the story, he can wander into water or "
    "into a load trigger, and the story keeps running around him.";
bool EyeCustom();
} // namespace TlgFun

// tlg: quitting from the keyboard (Ctrl+X, window close) and the controller (Controls tab combo, held
// 2 s). Runs the game's own luaCommand("quit") once no load is running, so its exit callbacks save
// the config and perpetual data; the game then ends in sceSystemServiceLoadExec (src/runtime_tlg.c),
// which exits instead of restarting. Closes anyway when the game has not finished after a while.
namespace TlgQuit {
void Request(const char* why);
bool Requested();
}
// tlg: the QA menu combo of Controls (when it is not the default L1+L3, which the game's pad check handles).
namespace TlgFun {
void ToggleQaMenu();
}
