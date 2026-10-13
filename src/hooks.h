#pragma once
#include "base.h"
#include <functional>
#include <string>

void InitHooks();
void CleanupHooks();

// Whisper callback: called on the game thread when a whisper is received.
// Parameters: sender name, message text.
using WhisperCallback = std::function<void(const std::string& sender, const std::string& message)>;
void SetWhisperCallback(WhisperCallback cb);

// ── Loot drop records (from system messages) ─────────────────────────────────
struct LootDropRecord {
    Position    pos;
    DWORD       tick = 0;
    std::string itemName;   // from "A <name>(+<plus>) has dropped at x,y"; empty if not parsed
    int         plus = -1;  // -1 when the message had no "(+N)"
};

const std::vector<LootDropRecord>& GetLootDropRecords();
void PruneLootDropRecords(DWORD maxAgeMs = 60000);

bool HasIncomingTradeRequest(DWORD maxAgeMs = 15000);
OBJID GetIncomingTradeRequesterId();
const char* GetIncomingTradeRequesterName();
uint64_t GetLastTradeRequestRawState();
void ConsumeIncomingTradeRequest();
