//
// RT64 LoD NI display-list resolver
//

#include "lod_ni_dl_resolver.h"

#include <cstdint>
#include <cstdio>

#include "rt64_interpreter.h"
#include "rt64_state.h"
#include "gbi/rt64_gbi.h"

#ifndef LOD_ENABLE_RENDER_ADDR_TRACE
#define LOD_ENABLE_RENDER_ADDR_TRACE 0
#endif

#ifndef LOD_FIX_RUN_DL_USE_GBI_MAP
#define LOD_FIX_RUN_DL_USE_GBI_MAP 0
#endif

#ifndef LOD_FIX_RUN_DL_NI_BOUNDS
#define LOD_FIX_RUN_DL_NI_BOUNDS 0
#endif

#ifndef LOD_FIX_RUN_DL_STALE_NI_FALLBACK
#define LOD_FIX_RUN_DL_STALE_NI_FALLBACK 0
#endif

#if LOD_FIX_RUN_DL_NI_BOUNDS
extern "C" uint32_t ni_overlay_loaded_span(uint32_t vram);
#endif

#if LOD_FIX_RUN_DL_STALE_NI_FALLBACK
extern "C" const void* lod_ni_stale_dl_candidate(uint8_t* rdram, uint32_t segmented_address,
                                                  uint32_t min_size, uint32_t attempt,
                                                  int* pair_out, uint32_t* source_out);
extern "C" int lod_ni_overlay_loaded_0f_pair();
extern "C" int lod_ni_overlay_loaded_0e_pair();
#endif

namespace RT64 {
    static bool lodNiDlDecodeResolvedAddress(uint32_t rdramAddress, uint32_t &vramBase,
                                             uint32_t &offset, uint32_t &span) {
        const uint32_t hi = (rdramAddress >> 24) & 0xFFU;
        if (hi == 0x8E) {
            vramBase = 0x0E000000U;
            offset = rdramAddress - 0x8E000000U;
        }
        else if (hi == 0x8F) {
            vramBase = 0x0F000000U;
            offset = rdramAddress - 0x8F000000U;
        }
        else {
            vramBase = 0;
            offset = 0;
            span = 0;
            return false;
        }

#if LOD_FIX_RUN_DL_NI_BOUNDS
        span = ni_overlay_loaded_span(vramBase);
#else
        span = 0xFFFFFFFFU;
#endif
        return true;
    }

#if LOD_FIX_RUN_DL_STALE_NI_FALLBACK
    static int lodNiDlCurrentPair(uint32_t vramBase) {
        if (vramBase == 0x0F000000U) {
            return lod_ni_overlay_loaded_0f_pair();
        }
        if (vramBase == 0x0E000000U) {
            return lod_ni_overlay_loaded_0e_pair();
        }
        return -1;
    }
#endif

    static bool lodNiDlOpcodeLikelyGBI(State *state, uint8_t opcode) {
#if LOD_FIX_RUN_DL_USE_GBI_MAP
        const GBI *activeGBI = (state->ext.interpreter != nullptr) ? state->ext.interpreter->hleGBI : nullptr;
        return (activeGBI != nullptr) && (activeGBI->map[opcode] != nullptr);
#else
        return (opcode <= 0x0B) || (opcode >= 0xB4);
#endif
    }

    static bool lodNiDlWordLooksLikeMips(uint32_t word, bool &strong) {
        strong = false;
        if (word == 0) {
            return false;
        }

        const uint32_t op = word >> 26;
        if (op == 0x00) {
            const uint32_t funct = word & 0x3FU;
            switch (funct) {
            case 0x00: // SLL
            case 0x02: // SRL
            case 0x03: // SRA
            case 0x04: // SLLV
            case 0x06: // SRLV
            case 0x07: // SRAV
            case 0x08: // JR
            case 0x09: // JALR
            case 0x20: // ADD
            case 0x21: // ADDU
            case 0x22: // SUB
            case 0x23: // SUBU
            case 0x24: // AND
            case 0x25: // OR
            case 0x26: // XOR
            case 0x27: // NOR
            case 0x2A: // SLT
            case 0x2B: // SLTU
                strong = (funct != 0x00);
                return true;
            default:
                return false;
            }
        }

        switch (op) {
        case 0x02: // J
        case 0x03: // JAL
        case 0x04: // BEQ
        case 0x05: // BNE
        case 0x06: // BLEZ
        case 0x07: // BGTZ
        case 0x08: // ADDI
        case 0x09: // ADDIU
        case 0x0A: // SLTI
        case 0x0B: // SLTIU
        case 0x0C: // ANDI
        case 0x0D: // ORI
        case 0x0E: // XORI
        case 0x0F: // LUI
        case 0x10: // COP0
        case 0x11: // COP1
            return true;
        case 0x20: // LB
        case 0x21: // LH
        case 0x22: // LWL
        case 0x23: // LW
        case 0x24: // LBU
        case 0x25: // LHU
        case 0x26: // LWR
        case 0x28: // SB
        case 0x29: // SH
        case 0x2A: // SWL
        case 0x2B: // SW
        case 0x2E: // SWR
            strong = true;
            return true;
        default:
            return false;
        }
    }

    static bool lodNiDlTargetLooksLikeMipsCode(const DisplayList *target) {
        if (target == nullptr) {
            return false;
        }

        uint32_t mipsLikeWords = 0;
        uint32_t strongMipsWords = 0;
        for (uint32_t i = 0; i < 8; i++) {
            const uint32_t words[2] = { target[i].w0, target[i].w1 };
            for (uint32_t j = 0; j < 2; j++) {
                bool strong = false;
                if (lodNiDlWordLooksLikeMips(words[j], strong)) {
                    mipsLikeWords++;
                    if (strong) {
                        strongMipsWords++;
                    }
                }
            }
        }

        return (mipsLikeWords >= 8) && (strongMipsWords >= 4);
    }

    static bool lodNiDlTargetHasValidStart(State *state, const DisplayList *target) {
        if (target == nullptr) {
            return false;
        }

        const uint8_t opcode = (target->w0 >> 24) & 0xFFU;
        if ((target->w0 == 0 && target->w1 == 0) || opcode == 0x00) {
            return false;
        }
        if (!lodNiDlOpcodeLikelyGBI(state, opcode)) {
            return false;
        }
        return !lodNiDlTargetLooksLikeMipsCode(target);
    }

    static bool lodNiDlTargetStructurallyValid(State *state, const DisplayList *target) {
        if (!lodNiDlTargetHasValidStart(state, target)) {
            return false;
        }

        for (uint32_t i = 0; i < 64; i++) {
            const uint8_t opcode = (target[i].w0 >> 24) & 0xFFU;
            const bool empty = (target[i].w0 == 0 && target[i].w1 == 0);
            if (empty) {
                return false;
            }
            if (!lodNiDlOpcodeLikelyGBI(state, opcode)) {
                return false;
            }
            if (opcode == 0xDF) {
                return true;
            }
        }

        return false;
    }

    static const char *lodNiDlSourceName(LodNiDlResolveSource source) {
        switch (source) {
        case LodNiDlResolveSource::LiveCurrentPair:
            return "live";
        case LodNiDlResolveSource::StalePairSnapshot:
            return "snapshot";
        case LodNiDlResolveSource::PristineRom:
            return "rom";
        default:
            return "none";
        }
    }

    static LodNiDlResolveResult lodNiDlResolved(LodNiDlResolveSource source, DisplayList *target,
                                                uint32_t segmentedAddress, uint32_t rdramAddress,
                                                uint32_t vramBase, uint32_t offset, uint32_t span,
                                                int pair, const char *reason) {
        LodNiDlResolveResult result;
        result.status = LodNiDlResolveStatus::Resolved;
        result.source = source;
        result.target = target;
        result.segmentedAddress = segmentedAddress;
        result.rdramAddress = rdramAddress;
        result.vramBase = vramBase;
        result.offset = offset;
        result.span = span;
        result.pair = pair;
        result.reason = reason;
        return result;
    }

    static LodNiDlResolveResult lodNiDlRejected(uint32_t segmentedAddress, uint32_t rdramAddress,
                                                uint32_t vramBase, uint32_t offset, uint32_t span,
                                                int pair, const char *reason) {
        LodNiDlResolveResult result;
        result.status = LodNiDlResolveStatus::Rejected;
        result.segmentedAddress = segmentedAddress;
        result.rdramAddress = rdramAddress;
        result.vramBase = vramBase;
        result.offset = offset;
        result.span = span;
        result.pair = pair;
        result.reason = reason;
        return result;
    }

    LodNiDlResolveResult lodResolveNiDisplayListTarget(State *state, uint32_t segmentedAddress,
                                                       uint32_t rdramAddress, const char *context) {
        uint32_t vramBase = 0;
        uint32_t offset = 0;
        uint32_t span = 0;
        if (rdramAddress < 0x20000000U) {
            return {};
        }
        if (!lodNiDlDecodeResolvedAddress(rdramAddress, vramBase, offset, span)) {
            return lodNiDlRejected(segmentedAddress, rdramAddress, 0, 0, 0, -1, "non-ni");
        }

        int livePair = -1;
#if LOD_FIX_RUN_DL_STALE_NI_FALLBACK
        livePair = lodNiDlCurrentPair(vramBase);
#endif

        if (span >= sizeof(DisplayList) && offset <= (span - sizeof(DisplayList))) {
            DisplayList *liveTarget = reinterpret_cast<DisplayList *>(state->fromRDRAM(rdramAddress));
            if (lodNiDlTargetHasValidStart(state, liveTarget)) {
                return lodNiDlResolved(LodNiDlResolveSource::LiveCurrentPair, liveTarget,
                    segmentedAddress, rdramAddress, vramBase, offset, span, livePair, "live");
            }
        }

#if LOD_FIX_RUN_DL_STALE_NI_FALLBACK
        for (uint32_t attempt = 0; attempt < 16; attempt++) {
            int pair = -1;
            uint32_t source = 0;
            const void *candidate = lod_ni_stale_dl_candidate(state->RDRAM, segmentedAddress,
                sizeof(DisplayList), attempt, &pair, &source);
            if (candidate == nullptr) {
                break;
            }

            DisplayList *target = (DisplayList *)candidate;
            if (!lodNiDlTargetStructurallyValid(state, target)) {
                continue;
            }

            const LodNiDlResolveSource resultSource = (source == 1)
                ? LodNiDlResolveSource::StalePairSnapshot
                : LodNiDlResolveSource::PristineRom;

#if LOD_ENABLE_RENDER_ADDR_TRACE
            static uint32_t resolveTraceCount = 0;
            resolveTraceCount++;
            if ((resolveTraceCount <= 96) || ((resolveTraceCount % 500) == 0)) {
                fprintf(stderr,
                    "[LOD_NI_DL] #%u context=%s src=0x%08X phys=0x%08X live_pair=%d pair=%d source=%s off=0x%X w0=0x%08X w1=0x%08X\n",
                    resolveTraceCount,
                    context != nullptr ? context : "?",
                    segmentedAddress,
                    rdramAddress,
                    livePair,
                    pair,
                    lodNiDlSourceName(resultSource),
                    offset,
                    target->w0,
                    target->w1);
            }
#else
            (void)context;
#endif

            return lodNiDlResolved(resultSource, target, segmentedAddress, rdramAddress,
                vramBase, offset, span, pair, "stale-owner");
        }
#else
        (void)context;
#endif

        return lodNiDlRejected(segmentedAddress, rdramAddress, vramBase, offset, span,
            livePair, "no-valid-owner");
    }
}
