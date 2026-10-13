#include "CHero.h"
#include "CGameMap.h"
#include "config.h"
#include "game.h"
#include "gateway.h"
#include "packets.h"
#include "hunt_settings.h"
#include "plugins/plugin_mgr.h"
#include "plugins/travel_plugin.h"
#include "log.h"
#include <cstring>

namespace {
constexpr uint16_t kMsgActionPacketType = 0x03F2;
constexpr uint16_t kMsgMapItemPacketType = 0x044D;
constexpr uint32_t kMsgActionModeRevive = 25;
constexpr uint32_t kMsgMapItemModePickup = 3;
constexpr uint16_t kMsgVipPacketType = 0x1B5C;
constexpr uint16_t kMsgTradePacketType = 0x0420;
constexpr uint32_t kMsgVipModeTeleport = 1;
constexpr uint32_t kMsgVipModeInfo = 5;      // what the VIP window sends when opened
constexpr uint32_t kVipDestinationTwinCity = 1;
constexpr uint32_t kVipDestinationPhoenixCastle = 2;
constexpr uint32_t kVipDestinationApeMountain = 3;
constexpr uint32_t kVipDestinationDesertCity = 4;
constexpr uint32_t kVipDestinationBirdIsland = 5;
constexpr uint32_t kTradeModeStart = 1;
constexpr uint32_t kTradeModeCancel = 2;
constexpr uint32_t kTradeModeAddItem = 6;
constexpr uint32_t kTradeModeAccept = 10;

struct SilverCacheState {
    OBJID heroId = 0;
    uint32_t value = 0;
    bool hasValue = false;
    bool isTrusted = false;
};

SilverCacheState g_silverCache;

void ResetSilverCacheForHero(OBJID heroId)
{
    if (g_silverCache.heroId == heroId)
        return;

    g_silverCache = {};
    g_silverCache.heroId = heroId;
}

void StoreSilverCache(OBJID heroId, uint32_t value, bool trusted)
{
    ResetSilverCacheForHero(heroId);
    if (g_silverCache.isTrusted && !trusted)
        return;

    g_silverCache.value = value;
    g_silverCache.hasValue = true;
    g_silverCache.isTrusted = trusted;
}

uint64_t GetGameTimestampMs()
{
    auto fn = GameCall::GetTimestamp();
    if (!fn)
        return 0;

    long long timestampNs = 0;
    fn(&timestampNs);
    return static_cast<uint64_t>(timestampNs / 1000000);
}

uint32_t GetVipDestinationForMap(OBJID mapId)
{
    switch (mapId) {
        case MAP_TWIN_CITY:      return kVipDestinationTwinCity;
        case MAP_PHOENIX_CASTLE: return kVipDestinationPhoenixCastle;
        case MAP_APE_MOUNTAIN:   return kVipDestinationApeMountain;
        case MAP_DESERT_CITY:    return kVipDestinationDesertCity;
        case MAP_BIRD_ISLAND:    return kVipDestinationBirdIsland;
        default:                 return 0;
    }
}
}

const char* GetEquipSlotName(int slot)
{
    switch (slot) {
        case EquipSlot::HEAD:     return "Head";
        case EquipSlot::NECKLACE: return "Necklace";
        case EquipSlot::ARMOR:    return "Armor";
        case EquipSlot::RWEAPON:  return "R.Hand";
        case EquipSlot::LWEAPON:  return "L.Hand";
        case EquipSlot::RING:     return "Ring";
        case EquipSlot::GARMENT:  return "Garment";
        case EquipSlot::BOOTS:    return "Boots";
        default:                  return "Unknown";
    }
}

CHero* CHero::GetSingletonPtr()
{
    // v1079: the game keeps a heap std::shared_ptr<CHero> behind a global pointer.
    auto** slot = Game::Resolve<Ref<CHero>**>(GameAddr::HeroPtr);
    if (!slot || !*slot) return nullptr;
    return (*slot)->get();
}

static int WriteVarint(uint8_t* buf, uint32_t value);

void CHero::Jump(int nX, int nY)
{
    const AutoHuntSettings& ah = GetAutoHuntSettings();
    const TravelSettings& ts = GetTravelSettings();

    // Check if either hunt or travel wants packet jump
    const bool travelActive = [&]() {
        if (auto* tp = PluginManager::Get().GetPlugin<TravelPlugin>())
            return tp->IsTraveling();
        return false;
    }();
    const bool wantPacketJump = ah.usePacketJump || (travelActive && ts.usePacketJump);

    if (!wantPacketJump) {
        if (auto jump = GameCall::CHero_Jump())
            jump(this, nX, nY);
        return;
    }

    // Suppress packet jump when other players are nearby — fall back to
    // the native jump so the game plays a normal animation.
    if (AreOtherPlayersNearby(GetID(), ah.playerWhitelist)) {
        if (auto jump = GameCall::CHero_Jump())
            jump(this, nX, nY);
    } else {
        // SetCommand tells the game a jump is happening so it doesn't
        // reset m_posMap on the next frame. ApplyLocalJumpPrediction
        // (inside SendJumpPacket) then overwrites the command status
        // to ACCOMPLISH and snaps the position instantly.
        // Pass the command as already accomplished so the game registers
        // the jump (preventing m_posMap resets) without starting its
        // animation engine which would fight with prediction.
        CCommand cmd = {};
        cmd.iType = _COMMAND_JUMP;
        cmd.iStatus = _CMDSTATUS_ACCOMPLISH;
        cmd.posTarget = Position(nX, nY);

        const uint64_t nowMs = GetGameTimestampMs();
        cmd.dwTimestamp = static_cast<DWORD>(nowMs);
        cmd.bDiagnal = m_posMap.DistanceTo(cmd.posTarget) > 18.0f ? TRUE : FALSE;

        SetCommand(&cmd);
        m_qwRuntimeA30 = nowMs > 5000 ? (nowMs - 5000) : 0;

        SendJumpPacket(GetID(), nX, nY, 0);
    }
}

void CHero::Walk(int nX, int nY)
{
    if (auto walk = GameCall::CHero_Walk())
        walk(this, nX, nY);
}

void CHero::Attack(OBJID idTarget)
{
    CRoleMgr* mgr = Game::GetRoleMgr();
    const RoleList roles = mgr ? mgr->Roles() : RoleList{};
    if (!mgr)
        return;

    for (const auto& roleRef : roles) {
        if (roleRef && roleRef->GetID() == idTarget) {
            AttackTarget(idTarget, roleRef->m_posMap);
            return;
        }
    }
}

void CHero::AttackTarget(OBJID idTarget, const Position& posTarget)
{
    uint8_t buf[48] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, GetID());

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, idTarget);

    buf[off++] = 0x18;
    off += WriteVarint(buf + off, (uint32_t)posTarget.x);

    buf[off++] = 0x20;
    off += WriteVarint(buf + off, (uint32_t)posTarget.y);

    buf[off++] = 0x30;
    off += WriteVarint(buf + off, 2); // physical attack interaction

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = 0x03FE;
    SendPacket(buf, off);
}

void CHero::ShootTarget(OBJID idTarget)
{
    // Get equipped weapon (right hand) ID for the shoot packet
    CItem* weapon = GetEquip(EquipSlot::RWEAPON);
    if (!weapon)
        return;

    uint8_t buf[48] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, GetID());

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, idTarget);

    buf[off++] = 0x28; // field 5: equipped weapon item ID
    off += WriteVarint(buf + off, weapon->GetID());

    buf[off++] = 0x30;
    off += WriteVarint(buf + off, 25); // archer/shoot interaction

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = 0x03FE;
    SendPacket(buf, off);
}

void CHero::PickupItem(OBJID idItem, const Position& pos)
{
    if (CGameMap* map = Game::GetMap()) {
        for (const auto& itemRef : map->m_vecItems) {
            if (!itemRef || itemRef->m_id != idItem)
                continue;
            if (itemRef->m_pos.x != pos.x || itemRef->m_pos.y != pos.y)
                continue;
            PickupItem(*itemRef);
            return;
        }
    }

    CCommand cmd = {};
    cmd.iType = _COMMAND_PICKUP;
    cmd.iStatus = _CMDSTATUS_BEGIN;
    cmd.idTarget = idItem;
    cmd.posTarget = pos;
    SetCommand(&cmd);
}

static int WriteVarint(uint8_t* buf, uint32_t value)
{
    int n = 0;
    while (value > 0x7F) {
        buf[n++] = (uint8_t)(value & 0x7F) | 0x80;
        value >>= 7;
    }
    buf[n++] = (uint8_t)value;
    return n;
}

static int WriteFixed32(uint8_t* buf, uint32_t value)
{
    memcpy(buf, &value, sizeof(value));
    return sizeof(value);
}

static bool SendPickupItemPacket(const CMapItem& item)
{
    uint8_t buf[48] = {};
    int off = 4;

    buf[off++] = 0x0D;
    off += WriteFixed32(buf + off, item.m_id);

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, item.m_idType);

    buf[off++] = 0x18;
    off += WriteVarint(buf + off, static_cast<uint32_t>(item.m_pos.x));

    buf[off++] = 0x20;
    off += WriteVarint(buf + off, static_cast<uint32_t>(item.m_pos.y));

    buf[off++] = 0x28;
    off += WriteVarint(buf + off, static_cast<uint32_t>(item.GetPlus()));

    buf[off++] = 0x30;
    off += WriteVarint(buf + off, kMsgMapItemModePickup);

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = kMsgMapItemPacketType;
    return SendPacket(buf, off);
}

void CHero::PickupItem(const CMapItem& item)
{
    if (!SendPickupItemPacket(item)) {
        CCommand cmd = {};
        cmd.iType = _COMMAND_PICKUP;
        cmd.iStatus = _CMDSTATUS_BEGIN;
        cmd.idTarget = item.m_id;
        cmd.posTarget = item.m_pos;
        SetCommand(&cmd);
    }
}

static bool SendMsgItem(OBJID idItem, uint32_t action, int slot = 0)
{
    uint8_t buf[32] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, idItem);

    if (slot > 0) {
        buf[off++] = 0x10;
        off += WriteVarint(buf + off, (uint32_t)slot);
    }

    buf[off++] = 0x28;
    off += WriteVarint(buf + off, action);

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = 0x03F1;
    return SendPacket(buf, off);
}

static bool SendWarehousePacket(OBJID idNpc, uint32_t action, uint32_t packageType, OBJID idItem = 0)
{
    uint8_t buf[32] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, idNpc);

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, action);

    buf[off++] = 0x18;
    off += WriteVarint(buf + off, packageType);

    if (idItem != 0) {
        buf[off++] = 0x20;
        off += WriteVarint(buf + off, idItem);
    }

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = 0x044E;
    return SendPacket(buf, off);
}

static bool SendSimplePacket(uint16_t type, uint8_t fieldTag, uint32_t value)
{
    uint8_t buf[16] = {};
    int off = 4;

    buf[off++] = fieldTag;
    off += WriteVarint(buf + off, value);

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = type;
    return SendPacket(buf, off);
}

static bool SendBuyItemPacket(OBJID idNpc, uint32_t typeId)
{
    uint8_t buf[32] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, idNpc);

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, typeId);

    buf[off++] = 0x28;
    off += WriteVarint(buf + off, 1);

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = 0x03F1;
    return SendPacket(buf, off);
}

static bool SendSellItemPacket(OBJID idNpc, OBJID idItem)
{
    uint8_t buf[32] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, idNpc);

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, idItem);

    buf[off++] = 0x28;
    off += WriteVarint(buf + off, 2);

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = 0x03F1;
    return SendPacket(buf, off);
}

static bool SendTradePacket(uint32_t mode, uint32_t value)
{
    uint8_t buf[32] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, mode);

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, value);

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = kMsgTradePacketType;
    return SendPacket(buf, off);
}

static bool SendDropItemPacket(OBJID idItem, const Position& pos)
{
    uint8_t buf[32] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, idItem);

    buf[off++] = 0x18;
    off += WriteVarint(buf + off, (uint32_t)pos.x);

    buf[off++] = 0x20;
    off += WriteVarint(buf + off, (uint32_t)pos.y);

    buf[off++] = 0x28;
    off += WriteVarint(buf + off, 3);

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = 0x03F1;
    return SendPacket(buf, off);
}

void CHero::MagicAttack(OBJID idMagic, OBJID idTarget, const Position& posTarget)
{
    uint8_t buf[48] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, GetID());

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, idTarget);

    buf[off++] = 0x18;
    off += WriteVarint(buf + off, (uint32_t)posTarget.x);

    buf[off++] = 0x20;
    off += WriteVarint(buf + off, (uint32_t)posTarget.y);

    buf[off++] = 0x30;
    off += WriteVarint(buf + off, 21); // magic attack interaction

    buf[off++] = 0x40;
    off += WriteVarint(buf + off, idMagic);

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = 0x03FE;
    SendPacket(buf, off);
}

void CHero::MagicAttack(OBJID idMagic, const Position& posTarget)
{
    MagicAttack(idMagic, 0, posTarget);
}

void CHero::StartMining()
{
    if (auto startMining = GameCall::CHero_StartMining()) {
        startMining(this);
        return;
    }

    // v1079 virtualized the client's StartMining, so do what it does: push the
    // mining command, then send MsgAction "mine" (captured from a manual start:
    // mode 36, hero id, timestamp, facing).
    constexpr uint32_t kMsgActionModeMine = 36;
    const uint64_t nowMs = GetGameTimestampMs();
    const DWORD timestamp = nowMs != 0 ? static_cast<DWORD>(nowMs) : GetTickCount();
    const int facing = m_cmdAction.nDir & 7;

    CCommand cmd = {};
    cmd.iType = _COMMAND_MINE_1079;
    cmd.iStatus = _CMDSTATUS_BEGIN;
    cmd.nDir = facing;
    cmd.dwTimestamp = timestamp;
    SetCommand(&cmd);

    uint8_t buf[32] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, kMsgActionModeMine);

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, GetID());

    buf[off++] = 0x18;
    off += WriteVarint(buf + off, timestamp);

    buf[off++] = 0x30;
    off += WriteVarint(buf + off, static_cast<uint32_t>(facing));

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = 0x03F2;
    SendPacket(buf, off);
}

void CHero::UseItem(OBJID idItem)
{
    SendMsgItem(idItem, 4);
}

void CHero::DropItem(OBJID idItem, const Position& pos)
{
    SendDropItemPacket(idItem, pos);
}

void CHero::EquipItem(OBJID idItem, int slot)
{
    SendMsgItem(idItem, 4, slot + 1);
}

void CHero::UnequipItem(OBJID idItem, int slot)
{
    SendMsgItem(idItem, 6, slot + 1);
}

void CHero::RepairItem(OBJID idItem)
{
    SendMsgItem(idItem, 14);
}

void CHero::OpenWarehouse(OBJID idNpc)
{
    ActivateNpc(idNpc);
    SendMsgItem(idNpc, 9);
    SendWarehousePacket(idNpc, 1, 10);
}

void CHero::DepositWarehouseItem(OBJID idNpc, OBJID idItem)
{
    SendWarehousePacket(idNpc, 2, 10, idItem);
}

void CHero::DepositWarehouseSilver(OBJID idNpc, uint32_t amount)
{
    if (amount == 0) return;

    // Packet type 0x03F1: field1=npcId, field2=amount, field5=10 (deposit silver)
    {
        uint8_t buf[32] = {};
        int off = 4;
        buf[off++] = 0x08;
        off += WriteVarint(buf + off, idNpc);
        buf[off++] = 0x10;
        off += WriteVarint(buf + off, amount);
        buf[off++] = 0x28;
        off += WriteVarint(buf + off, 10);
        *(uint16_t*)buf = (uint16_t)off;
        *(uint16_t*)(buf + 2) = 0x03F1;
        SendPacket(buf, off);
    }

    // Confirm packet: field1=npcId, field5=9
    {
        uint8_t buf[32] = {};
        int off = 4;
        buf[off++] = 0x08;
        off += WriteVarint(buf + off, idNpc);
        buf[off++] = 0x28;
        off += WriteVarint(buf + off, 9);
        *(uint16_t*)buf = (uint16_t)off;
        *(uint16_t*)(buf + 2) = 0x03F1;
        SendPacket(buf, off);
    }

    spdlog::info("[npc] DepositWarehouseSilver npc={} amount={}", idNpc, amount);
}

void CHero::WithdrawWarehouseItem(OBJID idNpc, OBJID idItem)
{
    SendWarehousePacket(idNpc, 3, 10, idItem);
}

void CHero::OpenTreasureBank(OBJID idNpc)
{
    ActivateNpc(idNpc);
}

void CHero::DepositTreasureBankMeteors(OBJID idNpc)
{
    ActivateNpc(idNpc);
    AnswerNpcEx(0, 101);
}

void CHero::DepositTreasureBankDragonBalls(OBJID idNpc)
{
    ActivateNpc(idNpc);
    AnswerNpcEx(1, 101);
}

void CHero::OpenComposeBank(OBJID idNpc)
{
    ActivateNpc(idNpc);
}

void CHero::DepositComposeBankAll()
{
    SendSimplePacket(0x1B67, 0x08, 2);
}

void CHero::CancelFly()
{
    constexpr uint32_t kMsgActionModeCancelFly = 53;
    uint8_t buf[32] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, kMsgActionModeCancelFly);

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, GetID());

    buf[off++] = 0x18;
    off += WriteVarint(buf + off, GetTickCount());

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = kMsgActionPacketType;
    SendPacket(buf, off);
}

void CHero::Sit()
{
    constexpr uint32_t kMsgActionModeSit = 3;
    uint8_t buf[32] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, kMsgActionModeSit);

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, GetID());

    buf[off++] = 0x18;
    off += WriteVarint(buf + off, GetTickCount());

    buf[off++] = 0x20;
    off += WriteVarint(buf + off, static_cast<uint32_t>(m_posMap.x));

    buf[off++] = 0x28;
    off += WriteVarint(buf + off, static_cast<uint32_t>(m_posMap.y));

    buf[off++] = 0x40;
    off += WriteVarint(buf + off, 250);

    *(uint16_t*)buf = static_cast<uint16_t>(off);
    *(uint16_t*)(buf + 2) = kMsgActionPacketType;
    SendPacket(buf, off);
}

void CHero::ReviveInTown()
{
    uint8_t buf[32] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, kMsgActionModeRevive);

    buf[off++] = 0x10;
    off += WriteVarint(buf + off, GetID());

    buf[off++] = 0x18;
    off += WriteVarint(buf + off, GetTickCount());

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = kMsgActionPacketType;
    SendPacket(buf, off);
}

bool CHero::VipTeleport(OBJID mapId)
{
    const uint32_t destination = GetVipDestinationForMap(mapId);
    if (destination == 0)
        return false;

    uint8_t buf[16] = {};
    int off = 4;

    buf[off++] = 0x08;
    off += WriteVarint(buf + off, kMsgVipModeTeleport);

    buf[off++] = 0x30;
    off += WriteVarint(buf + off, destination);

    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = kMsgVipPacketType;
    SendPacket(buf, off);
    return true;
}

void CHero::VipTeleportTwinCity()
{
    VipTeleport(MAP_TWIN_CITY);
}

void CHero::BuyItem(OBJID idNpc, uint32_t typeId)
{
    SendBuyItemPacket(idNpc, typeId);
}

void CHero::SellItem(OBJID idNpc, OBJID idItem)
{
    SendSellItemPacket(idNpc, idItem);
}

void CHero::StartTrade(OBJID idPlayer)
{
    SendTradePacket(kTradeModeStart, idPlayer);
}

void CHero::OfferTradeItem(OBJID idItem)
{
    SendTradePacket(kTradeModeAddItem, idItem);
}

void CHero::AcceptTrade(OBJID idPlayer)
{
    SendTradePacket(kTradeModeAccept, idPlayer);
}

void CHero::CancelTrade(OBJID idPlayer)
{
    SendTradePacket(kTradeModeCancel, idPlayer);
}

// =====================================================================
// NPC interaction — packet-based
// for our binary, so we send raw protobuf packets via SendMsg instead)
//
// Activate NPC packet (type 0x07EF):
//   protobuf field 1 (varint) = NPC entity ID
//   protobuf field 5 (varint) = 1
//
// Answer NPC packet (type 0x07F0):
//   protobuf field 5 (varint) = dialog option index
//   protobuf field 6 (varint) = NPC task/dialog ID
// =====================================================================
void CHero::ActivateNpc(OBJID idNpc)
{
    // [u16 size][u16 type=0x07EF][field1=idNpc][field5=1]
    uint8_t buf[32];
    int off = 4; // skip header, fill size+type later

    // field 1 (tag=0x08), varint = NPC entity ID
    buf[off++] = 0x08;
    off += WriteVarint(buf + off, idNpc);

    // field 5 (tag=0x28), varint = 1
    buf[off++] = 0x28;
    buf[off++] = 0x01;

    // Fill header
    *(uint16_t*)buf = (uint16_t)off;       // size
    *(uint16_t*)(buf + 2) = 0x07EF;        // type

    spdlog::debug("[npc] ActivateNpc entity={}, pkt size={}", idNpc, off);
    SendPacket(buf, off);
}

void CHero::AnswerNpc(int answer)
{
    AnswerNpcEx(answer, 101); // default task ID for Conductress
}

void CHero::AnswerNpcEx(int answer, int taskId)
{
    // [u16 size][u16 type=0x07F0][field5=answer][field6=taskId]
    // Proto3: field5=0 is omitted (default value not serialized)
    uint8_t buf[32];
    int off = 4;

    // field 5 (tag=0x28), varint = answer option (skip if 0)
    if (answer != 0) {
        buf[off++] = 0x28;
        off += WriteVarint(buf + off, (uint32_t)answer);
    }

    // field 6 (tag=0x30), varint = task/dialog ID
    buf[off++] = 0x30;
    off += WriteVarint(buf + off, (uint32_t)taskId);

    // Fill header
    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = 0x07F0;

    spdlog::debug("[npc] AnswerNpc option={} taskId={}, pkt size={}", answer, taskId, off);
    SendPacket(buf, off);
}

bool CHero::IsNpcActive() const
{
    return m_bNpcActive != FALSE;
}

OBJID CHero::GetActiveNpc() const
{
    return m_idActiveNpc;
}

int CHero::GetCurrentHp() const
{
    // v1079: the stat table's values are obfuscated; the HUD reads current HP
    // through this getter with stat 1.
    if (auto getStat = GameCall::CHero_GetStat())
        return static_cast<int>(getStat(this, 1));
    return m_pStatTable ? m_pStatTable->GetValue(1) : 0;
}

int CHero::GetMaxHp() const
{
    if (m_nMaxHp > 0)
        return m_nMaxHp;

    auto fn = GameCall::CHero_GetMaxHp();
    return fn ? fn(this) : 0;
}

int CHero::GetCurrentMana() const
{
    // v1079: current mana is an encrypted 16-byte block at +0x554 with its key
    // at +0x4A4. The game's getter (vtable slot +0x258) also feeds the
    // anti-tamper call hash, so decrypt the block ourselves and combine the
    // result the same way it does.
    if (auto decrypt = GameCall::StatBlock_Decrypt()) {
        constexpr size_t kManaKeyOffset = 0x4A4;
        constexpr size_t kManaBlockOffset = 0x554;
        const auto* self = reinterpret_cast<const uint8_t*>(this);
        uint32_t out[4] = {};
        decrypt(self + kManaKeyOffset, self + kManaBlockOffset, out);
        return static_cast<int>(out[0]
            + (((out[1] & 0xFF) << 8) | ((out[2] & 0xFF) << 16) | (out[3] << 24)));
    }
    auto fn = GameCall::CHero_GetCurrentMana();
    return fn ? fn(this) : 0;
}

int CHero::GetMaxMana() const
{
    // v1079: the game's own getter returns this cache while the flag is set
    // (0 on heroes without mana). Its fallback path feeds the anti-tamper call
    // hash, so it isn't called from here.
    if (m_bMaxManaValid)
        return m_nMaxMana;

    auto fn = GameCall::CHero_GetMaxMana();
    return fn ? fn(this) : 0;
}

void CHero::RefreshSilverCache(bool trusted) const
{
    StoreSilverCache(GetID(), m_dwSilver, trusted);
}

void CHero::SetTrustedSilver(uint32_t value) const
{
    StoreSilverCache(GetID(), value, true);
}

bool CHero::HasTrustedSilverCache() const
{
    ResetSilverCacheForHero(GetID());
    return g_silverCache.hasValue && g_silverCache.isTrusted;
}

uint32_t CHero::GetSilver() const
{
    RefreshSilverCache(false);
    ResetSilverCacheForHero(GetID());
    return g_silverCache.hasValue ? g_silverCache.value : m_dwSilver;
}

CMagic* CHero::FindMagicByName(const char* name) const
{
    if (!name || !name[0])
        return nullptr;

    for (const auto& magicRef : m_vecMagic) {
        if (!magicRef)
            continue;
        if (_stricmp(magicRef->GetName(), name) == 0)
            return magicRef.get();
    }

    return nullptr;
}

CMagic* CHero::FindMagicById(OBJID idMagic) const
{
    for (const auto& magicRef : m_vecMagic) {
        if (magicRef && magicRef->GetMagicType() == idMagic)
            return magicRef.get();
    }
    return nullptr;
}

// v1079 keeps no VIP flag on the hero. The bot asks the server for the VIP info
// once per hero, like opening the VIP window does, and the reply fills the VIP
// window's expiry text, also while the window is closed.
namespace {
OBJID g_vipInfoHeroId = 0;
DWORD g_vipInfoHeroSeenTick = 0;
DWORD g_vipInfoRequestTick = 0;          // 0 = not requested for this hero yet
constexpr DWORD kVipInfoRequestDelayMs = 3000;   // let the login settle first
constexpr DWORD kVipInfoReplyMs = 1500;          // until then the window may hold the previous hero's data
}

void CHero::UpdateVipInfo()
{
    const DWORD now = GetTickCount();
    if (g_vipInfoHeroId != GetID()) {
        g_vipInfoHeroId = GetID();
        g_vipInfoHeroSeenTick = now;
        g_vipInfoRequestTick = 0;
    }
    if (g_vipInfoRequestTick != 0 || now - g_vipInfoHeroSeenTick < kVipInfoRequestDelayMs)
        return;

    uint8_t buf[8] = {};
    int off = 4;
    buf[off++] = 0x08;
    off += WriteVarint(buf + off, kMsgVipModeInfo);
    *(uint16_t*)buf = (uint16_t)off;
    *(uint16_t*)(buf + 2) = kMsgVipPacketType;
    if (SendPacket(buf, off)) {
        g_vipInfoRequestTick = now;
        spdlog::info("[vip] Requested VIP info for {}", GetName());
    }
}

bool CHero::IsVip() const
{
    if (g_vipInfoHeroId != GetID() || g_vipInfoRequestTick == 0
        || GetTickCount() - g_vipInfoRequestTick < kVipInfoReplyMs)
        return false;

    const auto* window = reinterpret_cast<const CUIVipWindow*>(Game::GetUIWindow(GameUIWindow::Vip));
    if (!window)
        return false;

    // Fixed-width "YYYY-MM-DD hh:mm", so comparing the text compares the time;
    // "0000-00-00 00:00" (no VIP / no reply yet) is always in the past.
    const std::string& expires = window->m_strExpires;
    if (expires.size() != 16)
        return false;
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char now[20];
    snprintf(now, sizeof(now), "%04u-%02u-%02u %02u:%02u",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return expires > now;
}
