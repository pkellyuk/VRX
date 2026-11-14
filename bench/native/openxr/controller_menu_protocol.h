#pragma once
#include <cstdint>
#include <cstddef>

// Shared with ControllerMenu.cs. Protected by <mapping-name>.Lock. The renderer
// never waits for WPF: a busy mutex simply keeps the last released menu texture.
namespace vrxmenu
{
constexpr uint32_t Magic = 0x58524356, Version = 1;
constexpr int Width = 960, Height = 768, EventCount = 64;
constexpr int TitleLeft = 24, TitleTop = 24, TitleHeight = 56, TitleRight = Width-24;
inline bool TitleHit(float u, float v)
{ return u*Width >= TitleLeft && u*Width <= TitleRight && v*Height >= TitleTop && v*Height <= TitleTop+TitleHeight; }
enum EventKind : int32_t { Move, Down, Up, Scroll, Cancel };
struct Event { int32_t kind; float x, y, value; };
struct Header
{
    uint32_t magic, version, enabled, visible, frame;
    float pointerX, pointerY;
    uint32_t write, read, controlScreen;
    uint64_t heartbeat;
    uint64_t padding[2];
};
struct Shared { Header header; Event events[EventCount]; uint8_t pixels[Width * Height * 4]; };
static_assert(sizeof(Header) == 64 && sizeof(Event) == 16 && offsetof(Shared, pixels) == 1088);

inline void Push(Shared& s, Event e)
{
    // Drop the oldest events on overflow. WPF notices the gap and cancels any drag.
    s.events[s.header.write % EventCount] = e;
    ++s.header.write;
}
}
