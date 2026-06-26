//
// RT64 LoD NI display-list resolver
//

#pragma once

#include <cstdint>

namespace RT64 {
    struct DisplayList;
    struct State;

    enum class LodNiDlResolveStatus : uint32_t {
        NotNi,
        Resolved,
        Rejected
    };

    enum class LodNiDlResolveSource : uint32_t {
        None,
        LiveCurrentPair,
        StalePairSnapshot,
        PristineRom
    };

    struct LodNiDlResolveResult {
        LodNiDlResolveStatus status = LodNiDlResolveStatus::NotNi;
        LodNiDlResolveSource source = LodNiDlResolveSource::None;
        DisplayList *target = nullptr;
        uint32_t segmentedAddress = 0;
        uint32_t rdramAddress = 0;
        uint32_t vramBase = 0;
        uint32_t offset = 0;
        uint32_t span = 0;
        int pair = -1;
        const char *reason = nullptr;
    };

    LodNiDlResolveResult lodResolveNiDisplayListTarget(State *state, uint32_t segmentedAddress,
                                                       uint32_t rdramAddress, const char *context);
}
