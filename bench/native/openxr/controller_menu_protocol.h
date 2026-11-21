#pragma once
#include <cstdint>
#include <cstddef>

// Shared with ControllerMenu.cs. Protected by <mapping-name>.Lock. The renderer
// never waits for WPF: a busy mutex simply keeps the last released menu texture.
namespace vrxmenu
{
constexpr uint32_t Magic = 0x58524356, Version = 7;
constexpr int Width = 1920, Height = 1536, EventCount = 64;
constexpr int MaxSnapshots = 6, SnapshotPathChars = 512, SnapshotMetaBytes = 1056;
constexpr int AtlasColumns = 2, AtlasWidth = Width * AtlasColumns;
constexpr int AtlasHeight = Height * ((MaxSnapshots + 1 + AtlasColumns - 1) / AtlasColumns);
constexpr int TitleLeft = 96, TitleTop = 96, TitleHeight = 112, TitleRight = Width-96;
inline bool TitleHit(float u, float v)
{ return u*Width >= TitleLeft+128 && u*Width < TitleRight-176 && v*Height >= TitleTop && v*Height <= TitleTop+TitleHeight; }
enum EventKind : int32_t { Move, Down, Up, Scroll, Cancel, RightDown, RightUp, HoverLeave };
enum PanelKind : uint32_t { SnapshotPanel, BrowserPanel, KeyboardPanel, UbuntuPanel, VrexBubblePanel, VrexChoicePanel, WebcamPanel, VrexChatPanel };
inline bool VrexCaption(uint32_t kind) { return kind == VrexBubblePanel || kind == VrexChoicePanel; }
inline bool PanelAcceptsPointer(uint32_t kind) { return kind != VrexBubblePanel; }
struct Event { int32_t kind; float x, y, value; };
struct Header
{
    uint32_t magic, version, enabled, visible, frame;
    float pointerX, pointerY;
    uint32_t write, read, controlScreen;
    uint64_t heartbeat;
    uint32_t snapshotRequest, snapshotAck, snapshotError, arrange;
};
struct Snapshot
{
    uint32_t id, frame, active, ready;
    uint32_t kind, reserved[3];
    char16_t path[SnapshotPathChars];
    uint8_t pixels[Width * Height * 4];
};
// Low byte: 0 UTF-16, 1 virtual key. Bits 8..11: Ctrl, Alt, Shift, Win.
// Each event presses and releases its modifiers, so focus loss cannot leave one held.
struct KeyboardEvent { uint32_t type, code; };
struct KeyboardState
{
    uint32_t write, read, focusSerial, flags;
    uint64_t targetWindow;
    KeyboardEvent events[256];
};
struct VrexState { uint32_t enabled,state; int32_t panelSlot; uint32_t reserved; float pointerX,pointerY; uint32_t avatar,shellRgb; };
struct PanelPlacement { uint32_t state; float rotation[4], position[3], size[2]; };
struct AudioPosition { uint32_t valid; float x,y,z; };
struct Shared { Header header; Event events[EventCount]; uint8_t pixels[Width * Height * 4]; Snapshot snapshots[MaxSnapshots]; KeyboardState keyboard; VrexState vrex; PanelPlacement placements[MaxSnapshots+1]; AudioPosition audio[MaxSnapshots+1]; uint32_t smoothEdges, reserved; };
static_assert(sizeof(Header) == 64 && sizeof(Event) == 16 && offsetof(Shared, pixels) == 1088);
static_assert(offsetof(Snapshot,pixels) == SnapshotMetaBytes);
static_assert(sizeof(KeyboardState) == 24+256*8);
static_assert(offsetof(Shared,keyboard) == 1088+Width*Height*4+MaxSnapshots*(SnapshotMetaBytes+Width*Height*4));
static_assert(sizeof(VrexState)==32 && offsetof(Shared,vrex)==offsetof(Shared,keyboard)+sizeof(KeyboardState));
static_assert(sizeof(PanelPlacement)==40 && offsetof(Shared,placements)==offsetof(Shared,vrex)+32);
static_assert(sizeof(AudioPosition)==16 && offsetof(Shared,audio)==offsetof(Shared,placements)+(MaxSnapshots+1)*40);
static_assert(offsetof(Shared,smoothEdges)==offsetof(Shared,audio)+(MaxSnapshots+1)*16);

inline void Push(Shared& s, Event e)
{
    // Drop the oldest events on overflow. WPF notices the gap and cancels any drag.
    s.events[s.header.write % EventCount] = e;
    ++s.header.write;
}
}
