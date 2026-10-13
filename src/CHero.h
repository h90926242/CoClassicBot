#pragma once
#include "CRole.h"
#include "CItem.h"
#include "CMagic.h"
#include "CStatTable.h"

struct CMapItem;

// =====================================================================
// Equipment slot indices (0-based, 8 slots total)
// From FUN_140189950 — equipment update handler
// =====================================================================
namespace EquipSlot {
    constexpr int HEAD     = 0;
    constexpr int NECKLACE = 1;
    constexpr int ARMOR    = 2;
    constexpr int RWEAPON  = 3;  // right hand (main weapon)
    constexpr int LWEAPON  = 4;  // left hand (shield / dual wield)
    constexpr int RING     = 5;
    constexpr int GARMENT  = 6;
    constexpr int BOOTS    = 7;
    constexpr int COUNT    = 8;
}

const char* GetEquipSlotName(int slot);

// =====================================================================
// CHero — the player's controllable character
//
// Hierarchy: CMapObj -> CRole -> CHero
//
// The weak_ptr at CRole+0x08 (from enable_shared_from_this) was
// previously mistaken for a separate shared_ptr<CRole>; it actually
// points back to the object itself.
//
// Offsets verified via Ghidra + Cheat Engine.
// =====================================================================
#pragma pack(push, 1)
class CHero : public CRole
{
public:
    static CHero* GetSingletonPtr();

    static constexpr int MAX_BAG_ITEMS = 40;

    // v1079 layout (verified live 2026-09-27 on a logged-in hero unless marked
    // UNVERIFIED). CRole grew by 0x10, and 0x68 more bytes were inserted between
    // m_qwRuntimeA30 and the inventory, so later fields moved by +0x78.
private:
    BYTE _pad72C[0x978 - 0x72C]; // +0x72C  hero runtime fields

public:
    CStatTable* m_pStatTable;      // +0x978 verified 2026-09-29: read by the game's CHero_GetStat;
                                   //        its values are obfuscated (use CHero::GetCurrentHp)

private:
    BYTE _pad980[0xA40 - 0x980];   // +0x980 gap to runtime slot

public:
    uint64_t m_qwRuntimeA30;       // +0xA40 movement timestamp (was 0xA30; CHero_Jump writes it)

private:
    BYTE _padA48[0xAA8 - 0xA48];   // +0xA48

public:
    uint32_t m_dwSilver;           // +0xAA8 silver (old 0xA30 + 0x10 + 0x68; verified live 2026-09-28)

private:
    BYTE _padAAC[0xB98 - 0xAAC];   // +0xAAC gap to inventory deque

public:
    std::deque<PItem> m_deqItem;   // +0xB98  inventory items (was 0xB20)

private:
    BYTE _padDeq[0xC00 - 0xB98 - sizeof(std::deque<PItem>)]; // gap between deque end and equipment

public:
    PItem m_equipment[EquipSlot::COUNT]; // +0xC00  equipped items (was 0xB88)

private:
    BYTE _padC80[0xD20 - 0xC80];   // +0xC80 gap to max mana cache

public:
    int32_t m_nMaxMana;            // +0xD20 cached max MP (verified 2026-09-29: 4900 on a 3820/4900 hero)
    uint8_t m_bMaxManaValid;       // +0xD24 cache-valid byte; the game's max-MP getter returns
                                   //        m_nMaxMana while it is set (0 on heroes without mana)

private:
    BYTE _padD25[0x1078 - 0xD25];  // +0xD25

public:
    BOOL m_bNpcActive;             // +0x1078 UNVERIFIED active NPC dialog flag (was 0x1000)

private:
    BYTE _pad107C[0x1990 - 0x107C]; // +0x107C gap to magic vector

public:
    std::vector<PMagic> m_vecMagic; // +0x1990  learned skills (was 0x1918)

private:
    BYTE _pad19A8[0x379C - 0x19A8]; // +0x19A8 gap to active NPC UID

public:
    OBJID m_idActiveNpc;            // +0x379C UNVERIFIED NPC in dialog (was 0x3724)
    // The old VIP flag slot (+0x37B8) is not a VIP flag in v1079 (290 on a VIP
    // hero, 24 on a non-VIP one); see IsVip().

    // ── Helpers ──
    bool IsBagFull() const { return m_deqItem.size() >= MAX_BAG_ITEMS; }
    CItem* GetEquip(int slot) const {
        if (slot < 0 || slot >= EquipSlot::COUNT) return nullptr;
        return m_equipment[slot].get();
    }

    void Jump(int nX, int nY);
    void JumpPacket(int nX, int nY);
    void Walk(int nX, int nY);
    void Attack(OBJID idTarget);
    void AttackTarget(OBJID idTarget, const Position& posTarget);
    void ShootTarget(OBJID idTarget);
    void MagicAttack(OBJID idMagic, const Position& posTarget);
    void PickupItem(OBJID idItem, const Position& pos);
    void PickupItem(const CMapItem& item);
    void MagicAttack(OBJID idMagic, OBJID idTarget, const Position& posTarget);
    void StartMining();
    void UseItem(OBJID idItem);
    void DropItem(OBJID idItem, const Position& pos);
    void EquipItem(OBJID idItem, int slot);
    void UnequipItem(OBJID idItem, int slot);
    void RepairItem(OBJID idItem);
    void OpenWarehouse(OBJID idNpc);
    void DepositWarehouseItem(OBJID idNpc, OBJID idItem);
    void DepositWarehouseSilver(OBJID idNpc, uint32_t amount);
    void WithdrawWarehouseItem(OBJID idNpc, OBJID idItem);
    void OpenTreasureBank(OBJID idNpc);
    void DepositTreasureBankMeteors(OBJID idNpc);
    void DepositTreasureBankDragonBalls(OBJID idNpc);
    void OpenComposeBank(OBJID idNpc);
    void DepositComposeBankAll();
    void CancelFly();
    void Sit();
    void ReviveInTown();
    bool VipTeleport(OBJID mapId);
    void VipTeleportTwinCity();
    void BuyItem(OBJID idNpc, uint32_t typeId);
    void SellItem(OBJID idNpc, OBJID idItem);
    void StartTrade(OBJID idPlayer);
    void OfferTradeItem(OBJID idItem);
    void AcceptTrade(OBJID idPlayer);
    void CancelTrade(OBJID idPlayer);

    // ── NPC interaction (packet-based) ──
    void ActivateNpc(OBJID idNpc);
    void AnswerNpc(int answer);                  // uses default taskId=101
    void AnswerNpcEx(int answer, int taskId);    // explicit task ID
    bool IsNpcActive() const;
    OBJID GetActiveNpc() const;

    // ── State queries ──
    int GetCurrentHp() const;
    int GetMaxHp() const;
    int GetCurrentMana() const;
    int GetMaxMana() const;
    void RefreshSilverCache(bool trusted = false) const;
    void SetTrustedSilver(uint32_t value) const;
    bool HasTrustedSilverCache() const;
    uint32_t GetSilverRuntimeValue() const { return m_dwSilver; }
    uint32_t GetSilver() const;
    CMagic* FindMagicByName(const char* name) const;
    CMagic* FindMagicById(OBJID idMagic) const;
    // VIP status from the VIP window's expiry text (false until the VIP info
    // reply for this hero arrived). UpdateVipInfo requests that info once per hero.
    bool IsVip() const;
    void UpdateVipInfo();
};
#pragma pack(pop)

static_assert(offsetof(CHero, m_pStatTable) == 0x978, "CHero::m_pStatTable");
static_assert(offsetof(CHero, m_qwRuntimeA30) == 0xA40, "CHero::m_qwRuntimeA30");
static_assert(offsetof(CHero, m_dwSilver) == 0xAA8, "CHero::m_dwSilver");
static_assert(offsetof(CHero, m_nMaxMana) == 0xD20, "CHero::m_nMaxMana");
static_assert(offsetof(CHero, m_bMaxManaValid) == 0xD24, "CHero::m_bMaxManaValid");
static_assert(offsetof(CHero, m_bNpcActive) == 0x1078, "CHero::m_bNpcActive");
static_assert(offsetof(CHero, m_deqItem) == 0xB98, "CHero::m_deqItem");
static_assert(offsetof(CHero, m_equipment) == 0xC00, "CHero::m_equipment");
static_assert(offsetof(CHero, m_vecMagic) == 0x1990, "CHero::m_vecMagic");
static_assert(offsetof(CHero, m_idActiveNpc) == 0x379C, "CHero::m_idActiveNpc");

#define g_objHero (*CHero::GetSingletonPtr())
