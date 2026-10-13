#include "gfx.h"
#include "log.h"
#include "offsets.h"

static uintptr_t g_moduleBase = 0;

typedef void (*FillRectFn)(int x1, int y1, int x2, int y2, uint32_t color);

void Gfx::Init(uintptr_t moduleBase)
{
    g_moduleBase = moduleBase;
    spdlog::info("[gfx] Init (base=0x{:X})", moduleBase);
}

void Gfx::FillRect(int x1, int y1, int x2, int y2, uint32_t colorARGB)
{
    const uintptr_t slot = GameOffsets::Get(GameAddr::FillRectPtr);
    if (!g_moduleBase || !slot) return;
    uintptr_t funcAddr = *(uintptr_t*)slot;
    if (!funcAddr) return;
    auto fn = (FillRectFn)funcAddr;
    fn(x1, y1, x2, y2, colorARGB);
}

void Gfx::DrawCross(int cx, int cy, int size, int thickness, uint32_t colorARGB)
{
    // Horizontal bar
    FillRect(cx - size, cy - thickness, cx + size, cy + thickness, colorARGB);
    // Vertical bar
    FillRect(cx - thickness, cy - size, cx + thickness, cy + size, colorARGB);
}
