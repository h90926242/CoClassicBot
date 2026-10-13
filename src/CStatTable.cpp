#include "CStatTable.h"
#include "game.h"

int CStatTable::GetValue(int statType) const
{
    auto fn = GameCall::CStatTable_GetValue();
    return fn ? fn(this, statType) : 0;
}
