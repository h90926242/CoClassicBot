#pragma once
#include "base.h"
#include <map>
#include <memory>
#include <string>

// =====================================================================
// CUIWindow — base of the client's own (Dear ImGui) windows, v1079
//
// The client registers every window once in a manager at
// GameAddr::UIWindowMgr: a std::map<int, std::shared_ptr<CUIWindow>> keyed by
// window id, followed by a std::deque<int> of the ids currently open.
// Verified live with Cheat Engine on 2026-09-29.
// =====================================================================

// Window ids (the map keys / CUIWindow::m_id).
namespace GameUIWindow {
    constexpr int NpcDialog        = 2;   // NPC task dialog (text, options) — TreasureBank uses it
    constexpr int Inventory        = 3;
    constexpr int Vip              = 19;  // VIP info and city teleports
    constexpr int Warehouse        = 22;  // Warehouseman
    constexpr int ComposeWarehouse = 28;  // ComposeBank
}

class CUIWindow
{
public:
    int  GetId() const { return m_id; }
    bool IsOpen() const { return m_bOpen != 0; }

    // What the title-bar X does: the window's virtual Close (slot 14, +0x70)
    // clears m_bOpen, drops the id from the open list and plays the close
    // sound. Client-side only, nothing is sent to the server.
    void Close()
    {
        using CloseFn = void(*)(CUIWindow*);
        constexpr size_t kCloseSlot = 14;
        auto* vtable = *reinterpret_cast<CloseFn* const*>(this);
        if (vtable && vtable[kCloseSlot])
            vtable[kCloseSlot](this);
    }

    void*   m_vtable;   // +0x00
    int     m_id;       // +0x08
    uint8_t m_bOpen;    // +0x0C
};

static_assert(offsetof(CUIWindow, m_id)    == 0x08, "CUIWindow::m_id");
static_assert(offsetof(CUIWindow, m_bOpen) == 0x0C, "CUIWindow::m_bOpen");

// VIP window (GameUIWindow::Vip). The client keeps no VIP flag on the hero; the
// server's reply to the VIP info request (MsgVip 0x1B5C mode 5, which the window
// sends when opened) fills this window's fields.
struct CUIVipWindow
{
    uint8_t     _pad00[0x50];   // CUIWindow + fields
    std::string m_strExpires;   // +0x50 "YYYY-MM-DD hh:mm"; "0000-00-00 00:00" until the reply arrived
};

static_assert(offsetof(CUIVipWindow, m_strExpires) == 0x50, "CUIVipWindow::m_strExpires");

struct CUIWindowMgr
{
    std::map<int, std::shared_ptr<CUIWindow>> m_windows;  // +0x00
    // +0x10 std::deque<int> ids of the open windows, in the order they opened
};

static_assert(sizeof(std::map<int, std::shared_ptr<CUIWindow>>) == 0x10, "CUIWindowMgr::m_windows");
