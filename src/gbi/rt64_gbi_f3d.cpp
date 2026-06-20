//
// RT64
//

#include "rt64_gbi_f3d.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cstdio>

#include "../include/rt64_extended_gbi.h"


#include "rt64_f3d.h"
#include "rt64_gbi_extended.h"
#include "rt64_gbi_rdp.h"
#include "hle/rt64_interpreter.h"

#ifndef LOD_ENABLE_RENDER_ADDR_TRACE
#define LOD_ENABLE_RENDER_ADDR_TRACE 0
#endif

#ifndef LOD_ENABLE_RENDER_GEOM_TRACE
#define LOD_ENABLE_RENDER_GEOM_TRACE 0
#endif

#ifndef LOD_ENABLE_GBI_MISS_TRACE
#define LOD_ENABLE_GBI_MISS_TRACE 0
#endif

#ifndef LOD_ENABLE_RUN_DL_SUSPICIOUS_TRACE
#define LOD_ENABLE_RUN_DL_SUSPICIOUS_TRACE 0
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
#endif

namespace RT64 {
    namespace GBI_F3D {
#if LOD_ENABLE_RENDER_GEOM_TRACE
        extern "C" {
            uint32_t g_lod_render_dl_root_segmented = 0;
            uint32_t g_lod_render_dl_root_physical = 0;
            uint32_t g_lod_render_dl_root_early_scene = 0;
        }

        struct LodTraceDlRootContext {
            uint32_t segmentedAddress;
            uint32_t rdramAddress;
            uint32_t earlyScene;
        };

        static LodTraceDlRootContext lodTraceDlRootStack[64] = {};
        static uint32_t lodTraceDlRootStackDepth = 0;

        static bool lodTraceIsEarlySceneRoot(uint32_t segmentedAddress) {
            return (segmentedAddress == 0x0F002230U) || (segmentedAddress == 0x0F0022A8U);
        }

        static void lodTraceSetEarlyDlRoot(uint32_t segmentedAddress, uint32_t rdramAddress) {
            if (lodTraceIsEarlySceneRoot(segmentedAddress)) {
                g_lod_render_dl_root_segmented = segmentedAddress;
                g_lod_render_dl_root_physical = rdramAddress;
                g_lod_render_dl_root_early_scene = 1;
            }
        }

        static void lodTracePushDlRoot(uint32_t segmentedAddress, uint32_t rdramAddress) {
            if (lodTraceDlRootStackDepth < 64) {
                lodTraceDlRootStack[lodTraceDlRootStackDepth++] = {
                    g_lod_render_dl_root_segmented,
                    g_lod_render_dl_root_physical,
                    g_lod_render_dl_root_early_scene
                };
            }

            lodTraceSetEarlyDlRoot(segmentedAddress, rdramAddress);
        }

        static void lodTracePopDlRoot() {
            if (lodTraceDlRootStackDepth > 0) {
                const LodTraceDlRootContext context = lodTraceDlRootStack[--lodTraceDlRootStackDepth];
                g_lod_render_dl_root_segmented = context.segmentedAddress;
                g_lod_render_dl_root_physical = context.rdramAddress;
                g_lod_render_dl_root_early_scene = context.earlyScene;
            }
            else {
                g_lod_render_dl_root_segmented = 0;
                g_lod_render_dl_root_physical = 0;
                g_lod_render_dl_root_early_scene = 0;
            }
        }

        static bool lodTraceIsOverlayAddress(uint32_t address) {
            const uint32_t hi = (address >> 24) & 0xFFU;
            return (hi == 0x0E) || (hi == 0x0F) || (hi == 0x8E) || (hi == 0x8F);
        }

        static uint32_t lodTraceDisplayListPointerOffset(State *state, const DisplayList *ptr) {
            const uintptr_t base = reinterpret_cast<uintptr_t>(state->RDRAM);
            const uintptr_t cur = reinterpret_cast<uintptr_t>(ptr);
            if (cur >= base) {
                const uintptr_t diff = cur - base;
                if (diff <= 0xFFFFFFFFULL) {
                    return static_cast<uint32_t>(diff);
                }
            }
            return 0xFFFFFFFFU;
        }

        struct LodTraceDlFingerprint {
            uint32_t caller;
            uint32_t segmentedAddress;
            uint32_t rdramAddress;
            uint32_t w0;
            uint32_t w1;
        };

        static bool lodTraceSameDlFingerprint(const LodTraceDlFingerprint &a, const LodTraceDlFingerprint &b) {
            return (a.caller == b.caller) && (a.segmentedAddress == b.segmentedAddress) && (a.rdramAddress == b.rdramAddress) &&
                (a.w0 == b.w0) && (a.w1 == b.w1);
        }

        static bool lodTraceRememberValidOverlayDl(const LodTraceDlFingerprint &fingerprint) {
            static LodTraceDlFingerprint seen[64] = {};
            static uint32_t seenCount = 0;
            for (uint32_t i = 0; i < seenCount; i++) {
                if (lodTraceSameDlFingerprint(seen[i], fingerprint)) {
                    return false;
                }
            }

            if (seenCount < 64) {
                seen[seenCount++] = fingerprint;
                return true;
            }

            return false;
        }

        static void lodTraceReadVertexWords(const void *ptr, uint32_t &w0, uint32_t &w1, uint32_t &w2, uint32_t &w3) {
            std::memcpy(&w0, static_cast<const uint8_t *>(ptr) + 0, sizeof(uint32_t));
            std::memcpy(&w1, static_cast<const uint8_t *>(ptr) + 4, sizeof(uint32_t));
            std::memcpy(&w2, static_cast<const uint8_t *>(ptr) + 8, sizeof(uint32_t));
            std::memcpy(&w3, static_cast<const uint8_t *>(ptr) + 12, sizeof(uint32_t));
        }

        void lodTraceVertexCommand(State *state, DisplayList *dl, const char *decoder, uint32_t address, uint32_t vtxCount, uint32_t dstIndex, bool physicalDataFormat) {
            static uint32_t vertexCommandTraceCount = 0;
            const uint32_t rdramAddress = physicalDataFormat ? state->rsp->fromSegmentedMaskedPD(address) : state->rsp->fromSegmentedMasked(address);
            const uint8_t *vertexBytes = state->fromRDRAM(rdramAddress);
            const RSP::Vertex *vertices = reinterpret_cast<const RSP::Vertex *>(vertexBytes);
            const RSP::VertexPD *pdVertices = reinterpret_cast<const RSP::VertexPD *>(vertexBytes);
            bool suspicious = (vtxCount == 0) || (vtxCount > 64);
            int16_t minX = 0, minY = 0, minZ = 0, maxX = 0, maxY = 0, maxZ = 0;
            uint32_t minXIndex = 0, minYIndex = 0, minZIndex = 0, maxXIndex = 0, maxYIndex = 0, maxZIndex = 0;
            if (vtxCount > 0) {
                if (physicalDataFormat) {
                    minX = maxX = pdVertices[0].x;
                    minY = maxY = pdVertices[0].y;
                    minZ = maxZ = pdVertices[0].z;
                    for (uint32_t i = 0; i < vtxCount; i++) {
                        const RSP::VertexPD &v = pdVertices[i];
                        if (v.x < minX) { minX = v.x; minXIndex = i; }
                        if (v.x > maxX) { maxX = v.x; maxXIndex = i; }
                        if (v.y < minY) { minY = v.y; minYIndex = i; }
                        if (v.y > maxY) { maxY = v.y; maxYIndex = i; }
                        if (v.z < minZ) { minZ = v.z; minZIndex = i; }
                        if (v.z > maxZ) { maxZ = v.z; maxZIndex = i; }
                        if ((std::abs(int(v.x)) > 20000) || (std::abs(int(v.y)) > 20000) || (std::abs(int(v.z)) > 20000)) {
                            suspicious = true;
                        }
                    }
                }
                else {
                    minX = maxX = vertices[0].x;
                    minY = maxY = vertices[0].y;
                    minZ = maxZ = vertices[0].z;
                    for (uint32_t i = 0; i < vtxCount; i++) {
                        const RSP::Vertex &v = vertices[i];
                        if (v.x < minX) { minX = v.x; minXIndex = i; }
                        if (v.x > maxX) { maxX = v.x; maxXIndex = i; }
                        if (v.y < minY) { minY = v.y; minYIndex = i; }
                        if (v.y > maxY) { maxY = v.y; maxYIndex = i; }
                        if (v.z < minZ) { minZ = v.z; minZIndex = i; }
                        if (v.z > maxZ) { maxZ = v.z; maxZIndex = i; }
                        if ((std::abs(int(v.x)) > 20000) || (std::abs(int(v.y)) > 20000) || (std::abs(int(v.z)) > 20000)) {
                            suspicious = true;
                        }
                    }
                }
            }

            if (!suspicious) {
                return;
            }

            if (vertexCommandTraceCount < 256) {
                const uint32_t callerOffset = lodTraceDisplayListPointerOffset(state, dl);
                const uint32_t f3dCount = dl->p0(20, 4) + 1;
                const uint32_t f3dDst = dl->p0(16, 4);
                const uint32_t f3dexCount = dl->p0(10, 6);
                const uint32_t f3dexDst = dl->p0(17, 7);
                const uint32_t f3dex2Count = dl->p0(12, 8);
                const uint32_t f3dex2Dst = dl->p0(1, 7) - f3dex2Count;
                const uint32_t waveCount = dl->p0(9, 7);
                const uint32_t waveDst = dl->p0(16, 8) / 5;
                fprintf(stderr,
                    "[RT64-GEOM][VTXCMD] #%u decoder=%s caller=0x%08X w0=0x%08X w1=0x%08X seg=0x%08X phys=0x%08X count=%u dst=%u min=(%d@+%u,%d@+%u,%d@+%u) max=(%d@+%u,%d@+%u,%d@+%u) alt_f3d=(%u,%u) alt_f3dex=(%u,%u) alt_f3dex2=(%u,%u) alt_wave=(%u,%u)%s\n",
                    vertexCommandTraceCount + 1, decoder, callerOffset, dl->w0, dl->w1, address, rdramAddress, vtxCount, dstIndex,
                    minX, minXIndex, minY, minYIndex, minZ, minZIndex,
                    maxX, maxXIndex, maxY, maxYIndex, maxZ, maxZIndex,
                    f3dCount, f3dDst, f3dexCount, f3dexDst, f3dex2Count, f3dex2Dst, waveCount, waveDst,
                    physicalDataFormat ? " PD" : "");
                const uint32_t dumpCount = std::min<uint32_t>(vtxCount, 4);
                for (uint32_t i = 0; i < dumpCount; i++) {
                    if (physicalDataFormat) {
                        const RSP::VertexPD &v = pdVertices[i];
                        uint32_t rw0 = 0, rw1 = 0, rw2 = 0;
                        std::memcpy(&rw0, reinterpret_cast<const uint8_t *>(&pdVertices[i]) + 0, sizeof(uint32_t));
                        std::memcpy(&rw1, reinterpret_cast<const uint8_t *>(&pdVertices[i]) + 4, sizeof(uint32_t));
                        std::memcpy(&rw2, reinterpret_cast<const uint8_t *>(&pdVertices[i]) + 8, sizeof(uint32_t));
                        fprintf(stderr,
                            "[RT64-GEOM][VTXRAW] #%u +%u raw_pd=(0x%08X,0x%08X,0x%08X) xyz=(%d,%d,%d) st=(%d,%d) ci=%u\n",
                            vertexCommandTraceCount + 1, i, rw0, rw1, rw2, v.x, v.y, v.z, v.s, v.t, v.ci);
                    }
                    else {
                        const RSP::Vertex &v = vertices[i];
                        uint32_t rw0 = 0, rw1 = 0, rw2 = 0, rw3 = 0;
                        lodTraceReadVertexWords(&vertices[i], rw0, rw1, rw2, rw3);
                        fprintf(stderr,
                            "[RT64-GEOM][VTXRAW] #%u +%u raw=(0x%08X,0x%08X,0x%08X,0x%08X) xyz=(%d,%d,%d) st=(%d,%d) rgba=(%u,%u,%u,%u) normal=(%d,%d,%d,%d)\n",
                            vertexCommandTraceCount + 1, i, rw0, rw1, rw2, rw3, v.x, v.y, v.z, v.s, v.t,
                            v.color.r, v.color.g, v.color.b, v.color.a, v.normal.x, v.normal.y, v.normal.z, v.normal.a);
                    }
                }

                const uint32_t extremeIndices[6] = { minXIndex, minYIndex, minZIndex, maxXIndex, maxYIndex, maxZIndex };
                for (uint32_t e = 0; e < 6; e++) {
                    const uint32_t i = extremeIndices[e];
                    bool alreadyDumped = (i < dumpCount);
                    for (uint32_t p = 0; p < e; p++) {
                        if (extremeIndices[p] == i) {
                            alreadyDumped = true;
                            break;
                        }
                    }

                    if (!alreadyDumped && (i < vtxCount)) {
                        if (physicalDataFormat) {
                            const RSP::VertexPD &v = pdVertices[i];
                            uint32_t rw0 = 0, rw1 = 0, rw2 = 0;
                            std::memcpy(&rw0, reinterpret_cast<const uint8_t *>(&pdVertices[i]) + 0, sizeof(uint32_t));
                            std::memcpy(&rw1, reinterpret_cast<const uint8_t *>(&pdVertices[i]) + 4, sizeof(uint32_t));
                            std::memcpy(&rw2, reinterpret_cast<const uint8_t *>(&pdVertices[i]) + 8, sizeof(uint32_t));
                            fprintf(stderr,
                                "[RT64-GEOM][VTXRAW-EXTREME] #%u +%u slot=%u raw_pd=(0x%08X,0x%08X,0x%08X) xyz=(%d,%d,%d) st=(%d,%d) ci=%u%s\n",
                                vertexCommandTraceCount + 1, i, dstIndex + i, rw0, rw1, rw2, v.x, v.y, v.z, v.s, v.t, v.ci,
                                ((std::abs(int(v.x)) > 20000) || (std::abs(int(v.y)) > 20000) || (std::abs(int(v.z)) > 20000)) ? " SUSPICIOUS" : "");
                        }
                        else {
                            const RSP::Vertex &v = vertices[i];
                            uint32_t rw0 = 0, rw1 = 0, rw2 = 0, rw3 = 0;
                            lodTraceReadVertexWords(&vertices[i], rw0, rw1, rw2, rw3);
                            fprintf(stderr,
                                "[RT64-GEOM][VTXRAW-EXTREME] #%u +%u slot=%u raw=(0x%08X,0x%08X,0x%08X,0x%08X) xyz=(%d,%d,%d) st=(%d,%d) rgba=(%u,%u,%u,%u) normal=(%d,%d,%d,%d)%s\n",
                                vertexCommandTraceCount + 1, i, dstIndex + i, rw0, rw1, rw2, rw3, v.x, v.y, v.z, v.s, v.t,
                                v.color.r, v.color.g, v.color.b, v.color.a, v.normal.x, v.normal.y, v.normal.z, v.normal.a,
                                ((std::abs(int(v.x)) > 20000) || (std::abs(int(v.y)) > 20000) || (std::abs(int(v.z)) > 20000)) ? " SUSPICIOUS" : "");
                        }
                    }
                }
            }
            else if (vertexCommandTraceCount == 256) {
                fprintf(stderr, "[RT64-GEOM][VTXCMD] trace limit reached; suppressing further suspicious vertex command logs\n");
            }
            vertexCommandTraceCount++;
        }

        static void lodTraceRunDl(State *state, const DisplayList *caller, uint32_t segmentedAddress, uint32_t rdramAddress, const DisplayList *target, bool invalid, bool empty, uint8_t firstOpcode) {
            static uint32_t runDlTraceCount = 0;
            const bool overlay = lodTraceIsOverlayAddress(segmentedAddress) || lodTraceIsOverlayAddress(rdramAddress);
            const uint32_t callerOffset = lodTraceDisplayListPointerOffset(state, caller);
            const LodTraceDlFingerprint fingerprint = { callerOffset, segmentedAddress, rdramAddress, target->w0, target->w1 };
            const bool shouldLog = invalid || (overlay && lodTraceRememberValidOverlayDl(fingerprint));
            if (shouldLog) {
                if (runDlTraceCount < 512) {
                    fprintf(stderr,
                        "[RT64-GEOM][DL] #%u caller=0x%08X src=0x%08X phys=0x%08X op=0x%02X w0=0x%08X w1=0x%08X empty=%u invalid=%u\n",
                        runDlTraceCount + 1, callerOffset, segmentedAddress, rdramAddress, firstOpcode,
                        target->w0, target->w1, empty ? 1U : 0U, invalid ? 1U : 0U);
                    if (invalid && !empty) {
                        for (uint32_t i = 0; i < 8; i++) {
                            fprintf(stderr, "[RT64-GEOM][DL-DUMP] #%u +%02u w0=0x%08X w1=0x%08X\n",
                                runDlTraceCount + 1, i, target[i].w0, target[i].w1);
                        }
                    }
                }
                else if (runDlTraceCount == 512) {
                    fprintf(stderr, "[RT64-GEOM][DL] trace limit reached; suppressing further display-list logs\n");
                }
                runDlTraceCount++;
            }
        }
#endif

#if LOD_ENABLE_RUN_DL_SUSPICIOUS_TRACE && !LOD_ENABLE_RENDER_GEOM_TRACE
        static uint32_t lodTraceDisplayListPointerOffset(State *state, const DisplayList *ptr) {
            const uintptr_t base = reinterpret_cast<uintptr_t>(state->RDRAM);
            const uintptr_t cur = reinterpret_cast<uintptr_t>(ptr);
            if (cur >= base) {
                const uintptr_t diff = cur - base;
                if (diff <= 0xFFFFFFFFULL) {
                    return static_cast<uint32_t>(diff);
                }
            }
            return 0xFFFFFFFFU;
        }
#endif

#if LOD_ENABLE_RUN_DL_SUSPICIOUS_TRACE
        static bool lodTraceIsSuspiciousNiDlTarget(uint32_t segmentedAddress, uint32_t rdramAddress, uint8_t firstOpcode) {
            const uint32_t segHi = (segmentedAddress >> 24) & 0xFFU;
            const uint32_t physHi = (rdramAddress >> 24) & 0xFFU;
            const bool niAddress = (segHi == 0x0E) || (segHi == 0x0F) || (segHi == 0x8E) || (segHi == 0x8F) ||
                (physHi == 0x0E) || (physHi == 0x0F) || (physHi == 0x8E) || (physHi == 0x8F);
            if (!niAddress) {
                return false;
            }

            const uint32_t off = rdramAddress & 0x00FFFFFFU;
            return (firstOpcode == 0xDD) ||
                (off >= 0x0000E780U && off < 0x0000E880U) ||
                (off >= 0x00013300U && off < 0x00013480U);
        }

        static void lodTraceSuspiciousRunDl(State *state, const DisplayList *caller, uint32_t segmentedAddress, uint32_t rdramAddress, const DisplayList *target, uint8_t firstOpcode) {
            if (!lodTraceIsSuspiciousNiDlTarget(segmentedAddress, rdramAddress, firstOpcode)) {
                return;
            }

            static uint32_t suspiciousRunDlTraceCount = 0;
            suspiciousRunDlTraceCount++;
            if (suspiciousRunDlTraceCount <= 96 || (suspiciousRunDlTraceCount % 100) == 0) {
                const uint32_t callerOffset = lodTraceDisplayListPointerOffset(state, caller);
                fprintf(stderr,
                    "[GBI_RUN_DL_SUSPICIOUS] #%u dl=%llu start=0x%08X caller=0x%08X caller_w0=0x%08X caller_w1=0x%08X caller_op=0x%02X no_push_bit=%u src=0x%08X phys=0x%08X target_op=0x%02X target_w0=0x%08X target_w1=0x%08X current_ucode=%u\n",
                    suspiciousRunDlTraceCount,
                    static_cast<unsigned long long>(state->displayListCounter),
                    state->displayListAddress,
                    callerOffset,
                    caller->w0,
                    caller->w1,
                    (caller->w0 >> 24) & 0xFFU,
                    caller->p0(16, 1) ? 1U : 0U,
                    segmentedAddress,
                    rdramAddress,
                    firstOpcode,
                    target->w0,
                    target->w1,
                    state->ext.interpreter->hleGBI != nullptr ? static_cast<unsigned>(state->ext.interpreter->hleGBI->ucode) : 0U);
                for (uint32_t i = 0; i < 8; i++) {
                    fprintf(stderr,
                        "[GBI_RUN_DL_SUSPICIOUS_DUMP] #%u +%02u w0=0x%08X w1=0x%08X\n",
                        suspiciousRunDlTraceCount, i, target[i].w0, target[i].w1);
                }
            }
        }
#endif

        void matrix(State *state, DisplayList **dl) {
            state->rsp->matrix((*dl)->w1, (*dl)->p0(16, 8));
        }

        void popMatrix(State *state, DisplayList **dl) {
            if ((*dl)->w1 == 0) {
                state->rsp->popMatrix(1);
            }
        }
        
        void moveMem(State *state, DisplayList **dl) {
            switch ((*dl)->p0(16, 8)) {
            case F3D_G_MV_VIEWPORT:
                state->rsp->setViewport((*dl)->w1);
                break;
            case F3D_G_MV_MATRIX_1:
                state->rsp->forceMatrix((*dl)->w1);
                *dl = *dl + 3;
                break;
            case F3D_G_MV_L0:
                state->rsp->setLight(0, (*dl)->w1);
                break;
            case F3D_G_MV_L1:
                state->rsp->setLight(1, (*dl)->w1);
                break;
            case F3D_G_MV_L2:
                state->rsp->setLight(2, (*dl)->w1);
                break;
            case F3D_G_MV_L3:
                state->rsp->setLight(3, (*dl)->w1);
                break;
            case F3D_G_MV_L4:
                state->rsp->setLight(4, (*dl)->w1);
                break;
            case F3D_G_MV_L5:
                state->rsp->setLight(5, (*dl)->w1);
                break;
            case F3D_G_MV_L6:
                state->rsp->setLight(6, (*dl)->w1);
                break;
            case F3D_G_MV_L7:
                state->rsp->setLight(7, (*dl)->w1);
                break;
            case F3D_G_MV_LOOKATX:
                state->rsp->setLookAt(0, (*dl)->w1);
                break;
            case F3D_G_MV_LOOKATY:
                state->rsp->setLookAt(1, (*dl)->w1);
                break;
            default:
                assert(false && "Unimplemented move mem.");
                break;
            }
        }
        
        void vertex(State *state, DisplayList **dl) {
#if LOD_ENABLE_RENDER_GEOM_TRACE
            lodTraceVertexCommand(state, *dl, "F3D", (*dl)->w1, (*dl)->p0(20, 4) + 1, (*dl)->p0(16, 4));
#endif
            state->rsp->setVertex((*dl)->w1, (*dl)->p0(20, 4) + 1, (*dl)->p0(16, 4));
        }


#if LOD_FIX_RUN_DL_STALE_NI_FALLBACK
        static bool lodRunDlOpcodeLikelyGBI(State *state, uint8_t opcode) {
#if LOD_FIX_RUN_DL_USE_GBI_MAP
            const GBI *activeGBI = (state->ext.interpreter != nullptr) ? state->ext.interpreter->hleGBI : nullptr;
            return (activeGBI != nullptr) && (activeGBI->map[opcode] != nullptr);
#else
            return (opcode <= 0x0B) || (opcode >= 0xB4);
#endif
        }

        static bool lodRunDlStaleCandidateLooksValid(State *state, const DisplayList *target) {
            if (target == nullptr) {
                return false;
            }

            for (uint32_t i = 0; i < 64; i++) {
                const uint8_t opcode = (target[i].w0 >> 24) & 0xFF;
                const bool empty = (target[i].w0 == 0 && target[i].w1 == 0);
                if (empty) {
                    return false;
                }
                if ((i == 0) && (opcode == 0x00)) {
                    return false;
                }
                if (!lodRunDlOpcodeLikelyGBI(state, opcode)) {
                    return false;
                }
                if (opcode == 0xDF) {
                    return true;
                }
            }

            return false;
        }

        static DisplayList *lodRunDlTryStaleNiFallback(State *state, uint32_t segmentedAddress,
                                                       const char *reason) {
            for (uint32_t attempt = 0; attempt < 16; attempt++) {
                int pair = -1;
                uint32_t source = 0;
                const void *candidate = lod_ni_stale_dl_candidate(state->RDRAM, segmentedAddress,
                    sizeof(DisplayList), attempt, &pair, &source);
                if (candidate == nullptr) {
                    return nullptr;
                }

                DisplayList *target = (DisplayList *)candidate;
                if (!lodRunDlStaleCandidateLooksValid(state, target)) {
                    continue;
                }

#if LOD_ENABLE_RENDER_ADDR_TRACE
                static uint32_t staleNiFallbackTraceCount = 0;
                staleNiFallbackTraceCount++;
                if ((staleNiFallbackTraceCount <= 96) || ((staleNiFallbackTraceCount % 500) == 0)) {
                    fprintf(stderr,
                        "[RUN_DL_STALE_NI] #%u reason=%s src=0x%08X pair=%d source=%u w0=0x%08X w1=0x%08X\n",
                        staleNiFallbackTraceCount,
                        reason != nullptr ? reason : "?",
                        segmentedAddress,
                        pair,
                        source,
                        target->w0,
                        target->w1);
                }
#else
                (void)reason;
#endif
                return target;
            }

            return nullptr;
        }
#endif

#if LOD_FIX_RUN_DL_NI_BOUNDS
        static bool lodNiExtendedDlTargetInLoadedSpan(uint32_t rdramAddress, uint32_t &vramBase, uint32_t &offset, uint32_t &span) {
            const uint32_t hi = (rdramAddress >> 24) & 0xFF;
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

            span = ni_overlay_loaded_span(vramBase);
            return (span >= sizeof(DisplayList)) && (offset <= (span - sizeof(DisplayList)));
        }
#endif

        void runDl(State *state, DisplayList **dl) {
            const uint32_t rdramAddress = state->rsp->fromSegmentedMasked((*dl)->w1);
#if LOD_FIX_RUN_DL_STALE_NI_FALLBACK
            DisplayList *lodStaleNiFallbackTarget = nullptr;
#endif

            // Guard: skip sub-DL if address is outside RDRAM.
            // Allow 0x8E/0x8F region (LoD NI overlay data via MEM_W).
            if (rdramAddress >= 0x20000000) {
                uint32_t hi = (rdramAddress >> 24) & 0xFF;
                if (hi != 0x8E && hi != 0x8F) {
                    static int skip_count = 0;
                    if (++skip_count <= 5) {
                        fprintf(stderr, "[RT64-DL] Skipping sub-DL at phys 0x%08X (out of RDRAM)\n", rdramAddress);
                    }
                    return;
                }
#if LOD_FIX_RUN_DL_NI_BOUNDS
                uint32_t niVramBase = 0;
                uint32_t niOffset = 0;
                uint32_t niSpan = 0;
                if (!lodNiExtendedDlTargetInLoadedSpan(rdramAddress, niVramBase, niOffset, niSpan)) {
#if LOD_FIX_RUN_DL_STALE_NI_FALLBACK
                    lodStaleNiFallbackTarget = lodRunDlTryStaleNiFallback(state, (*dl)->w1, "bounds");
                    if (lodStaleNiFallbackTarget != nullptr) {
                        goto lod_run_dl_have_target;
                    }
#endif
#if LOD_ENABLE_RENDER_ADDR_TRACE
                    static uint32_t niBoundsSkipCount = 0;
                    niBoundsSkipCount++;
                    if ((niBoundsSkipCount <= 16) || ((niBoundsSkipCount % 100) == 0)) {
                        fprintf(stderr,
                            "[RT64-DL][NI_BOUNDS] skip #%u src=0x%08X phys=0x%08X vram=0x%08X off=0x%X span=0x%X\n",
                            niBoundsSkipCount,
                            (*dl)->w1,
                            rdramAddress,
                            niVramBase,
                            niOffset,
                            niSpan);
                    }
#endif
                    return;
                }
#endif
            }

#if LOD_FIX_RUN_DL_STALE_NI_FALLBACK
lod_run_dl_have_target:
            DisplayList *target = (lodStaleNiFallbackTarget != nullptr)
                ? lodStaleNiFallbackTarget
                : reinterpret_cast<DisplayList *>(state->fromRDRAM(rdramAddress));
#else
            DisplayList *target = reinterpret_cast<DisplayList *>(state->fromRDRAM(rdramAddress));
#endif

            // Guard: skip if target doesn't look like valid DL data.
            // Valid GBI opcodes for F3DEX2 are in specific ranges (0x01-0x0B, 0xB6-0xFF).
            // MIPS instructions (which start with opcodes like 0x24, 0x27, 0x8C, 0xA4, etc.)
            // are NOT valid GBI commands. Check the first command's opcode.
            {
                uint8_t firstOpcode = (target->w0 >> 24) & 0xFF;
#if LOD_FIX_RUN_DL_USE_GBI_MAP
                const GBI *activeGBI = (state->ext.interpreter != nullptr) ? state->ext.interpreter->hleGBI : nullptr;
                bool likelyGBI = (activeGBI != nullptr) && (activeGBI->map[firstOpcode] != nullptr);
#else
                bool likelyGBI = (firstOpcode <= 0x0B) || (firstOpcode >= 0xB4);
#endif
                bool isEmpty = (target->w0 == 0 && target->w1 == 0);
#if LOD_ENABLE_RUN_DL_SUSPICIOUS_TRACE
                lodTraceSuspiciousRunDl(state, *dl, (*dl)->w1, rdramAddress, target, firstOpcode);
#endif
#if LOD_ENABLE_RENDER_GEOM_TRACE
                lodTraceRunDl(state, *dl, (*dl)->w1, rdramAddress, target, isEmpty || !likelyGBI, isEmpty, firstOpcode);
#endif
                if (isEmpty || !likelyGBI) {
#if LOD_FIX_RUN_DL_STALE_NI_FALLBACK
                    DisplayList *guardFallback = lodRunDlTryStaleNiFallback(state, (*dl)->w1, "guard");
                    if (guardFallback != nullptr) {
                        target = guardFallback;
                        firstOpcode = (target->w0 >> 24) & 0xFF;
                        likelyGBI = lodRunDlOpcodeLikelyGBI(state, firstOpcode);
                        isEmpty = (target->w0 == 0 && target->w1 == 0);
                    }
                    if (isEmpty || !likelyGBI)
#endif
                    {
#if LOD_ENABLE_RENDER_ADDR_TRACE
                    static uint32_t invalidDlSkipCount = 0;
                    if (invalidDlSkipCount < 64) {
                        fprintf(stderr, "[RT64-DL][GUARD] skip src=0x%08X phys=0x%08X op=0x%02X w0=0x%08X w1=0x%08X empty=%u\n",
                            (*dl)->w1, rdramAddress, firstOpcode, target->w0, target->w1, isEmpty ? 1U : 0U);
                    }
                    else if (invalidDlSkipCount == 64) {
                        fprintf(stderr, "[RT64-DL][GUARD] trace limit reached; suppressing further invalid-sub-DL logs\n");
                    }
                    invalidDlSkipCount++;
#endif
                        return;
                    }
                }
            }



            if ((*dl)->p0(16, 1) == 0) {
#if LOD_ENABLE_RENDER_GEOM_TRACE
                lodTracePushDlRoot((*dl)->w1, rdramAddress);
#endif
                state->pushReturnAddress(*dl);
            }
#if LOD_ENABLE_RENDER_GEOM_TRACE
            else {
                // Branch-style G_DL replaces the current list and does not push a return address.
                // It still needs to establish root context for diagnostics/tinting.
                lodTraceSetEarlyDlRoot((*dl)->w1, rdramAddress);
            }
#endif

            *dl = target - 1;
        }

        void endDl(State *state, DisplayList **dl) {
            *dl = state->popReturnAddress();
#if LOD_ENABLE_RENDER_GEOM_TRACE
            lodTracePopDlRoot();
#endif
        }

        void sprite2DBase(State *state, DisplayList **dl) {
            // TODO
        }

        void tri1(State *state, DisplayList **dl) {
            state->rsp->drawIndexedTri((*dl)->p1(16, 8) / 10, (*dl)->p1(8, 8) / 10, (*dl)->p1(0, 8) / 10);
        }
        
        void quad(State *state, DisplayList **dl) {
            const uint8_t v0 = (*dl)->p1(24, 8) / 10;
            const uint8_t v1 = (*dl)->p1(16, 8) / 10;
            const uint8_t v2 = (*dl)->p1(8, 8) / 10;
            const uint8_t v3 = (*dl)->p1(0, 8) / 10;
            state->rsp->drawIndexedTri(v0, v1, v2);
            state->rsp->drawIndexedTri(v0, v2, v3);
        }

        void cullDl(State *state, DisplayList **dl) {
            // TODO
        }

        void moveWord(State *state, DisplayList **dl) {
            uint8_t type = (*dl)->p0(0, 8);
            switch (type) {
            case G_MW_MATRIX:
                assert(false);
                // TODO
                break;
            case G_MW_NUMLIGHT:
                state->rsp->setLightCount((((*dl)->w1 - 0x80000000) >> 5) - 1);
                break;
            case G_MW_CLIP:
                state->rsp->setClipRatioEdge(((*dl)->p0(8, 16) - G_MWO_CLIP_RNX) / 8, int16_t((*dl)->w1 & 0xFFFFU));
                break;
            case G_MW_SEGMENT:
                state->rsp->setSegment((*dl)->p0(10, 4), (*dl)->w1);
                break;
            case G_MW_FOG:
                state->rsp->setFog((int16_t)((*dl)->p1(16, 16)), (int16_t)((*dl)->p1(0, 16)));
                break;
            case G_MW_LIGHTCOL:
                state->rsp->setLightColor((*dl)->p0(8, 16) / 32, (*dl)->w1);
                break;
            case F3D_G_MW_POINTS: 
                state->rsp->modifyVertex((*dl)->p0(8, 16) / 40, (*dl)->p0(8, 16) % 40, (*dl)->w1);
                break;
            case G_MW_PERSPNORM:
                // TODO
                break;
            default:
                break;
            }
        }

        void texture(State *state, DisplayList **dl) {
            uint8_t tile = (*dl)->p0(8, 3);
            uint8_t level = (*dl)->p0(11, 3);
            uint8_t on = (*dl)->p0(0, 8);
            uint16_t sc = (*dl)->p1(16, 16);
            uint16_t tc = (*dl)->p1(0, 16);
            state->rsp->setTexture(tile, level, on, sc, tc);
        }

        void setOtherModeH(State *state, DisplayList **dl) {
            state->rsp->setOtherModeH((*dl)->p0(0, 8), (*dl)->p0(8, 8), (*dl)->w1);
        }

        void setOtherModeL(State *state, DisplayList **dl) {
            state->rsp->setOtherModeL((*dl)->p0(0, 8), (*dl)->p0(8, 8), (*dl)->w1);
        }

        void setGeometryMode(State *state, DisplayList **dl) {
            state->rsp->setGeometryMode((*dl)->w1);
        }

        void clearGeometryMode(State *state, DisplayList **dl) {
            state->rsp->clearGeometryMode((*dl)->w1);
        }

        void rdpHalf1(State *state, DisplayList **dl) {
            state->microcode.half1 = (*dl)->w1;
        }

        void rdpHalf2(State *state, DisplayList **dl) {
            state->microcode.half2 = (*dl)->w1;
        }

        void setColorImage(State *state, DisplayList **dl) {
            const uint8_t fmt = (*dl)->p0(21, 3);
            const uint8_t siz = (*dl)->p0(19, 2);
            const uint16_t width = (*dl)->p0(0, 12) + 1;
            const uint32_t address = (*dl)->w1;
            state->rsp->setColorImage(fmt, siz, width, address);
        }

        void setDepthImage(State *state, DisplayList **dl) {
            const uint32_t address = (*dl)->w1;
            state->rsp->setDepthImage(address);
        }

        void setTextureImage(State *state, DisplayList **dl) {
            const uint8_t fmt = (*dl)->p0(21, 3);
            const uint8_t siz = (*dl)->p0(19, 2);
            const uint16_t width = (*dl)->p0(0, 12) + 1;
            const uint32_t address = (*dl)->w1;
            state->rsp->setTextureImage(fmt, siz, width, address);
        }

        void reset(State *state) {
            state->rsp->setLookAtVectors(hlslpp::float3(0.0f, 1.0f, 0.0f), hlslpp::float3(1.0f, 0.0f, 0.0f));
            state->rsp->setFog(0x0100, 0x0000);
        }

        void setup(GBI *gbi) {
            gbi->constants = {
                { F3DENUM::G_MTX_MODELVIEW, 0x00 },
                { F3DENUM::G_MTX_PROJECTION, 0x01 },
                { F3DENUM::G_MTX_MUL, 0x00 },
                { F3DENUM::G_MTX_LOAD, 0x02 },
                { F3DENUM::G_MTX_NOPUSH, 0x00 },
                { F3DENUM::G_MTX_PUSH, 0x04 },
                { F3DENUM::G_TEXTURE_ENABLE, 0x00000002 },
                { F3DENUM::G_SHADING_SMOOTH, 0x00000200 },
                { F3DENUM::G_CULL_FRONT, 0x00001000 },
                { F3DENUM::G_CULL_BACK, 0x00002000 },
                { F3DENUM::G_CULL_BOTH, 0x00003000 }
            };
            
            gbi->map[F3D_G_SPNOOP] = &GBI_EXTENDED::noOpHook;
            gbi->map[F3D_G_MTX] = &matrix;
            gbi->map[F3D_G_MOVEMEM] = &moveMem;
            gbi->map[F3D_G_VTX] = &vertex;
            gbi->map[F3D_G_DL] = &runDl;
            gbi->map[F3D_G_ENDDL] = &endDl;
            gbi->map[F3D_G_SPRITE2D_BASE] = &sprite2DBase;
            gbi->map[F3D_G_TRI1] = &tri1;
            gbi->map[F3D_G_QUAD] = &quad;
            gbi->map[F3D_G_CULLDL] = &cullDl;
            gbi->map[F3D_G_POPMTX] = &popMatrix;
            gbi->map[F3D_G_MOVEWORD] = &moveWord;
            gbi->map[F3D_G_TEXTURE] = &texture;
            gbi->map[F3D_G_SETOTHERMODE_H] = &setOtherModeH;
            gbi->map[F3D_G_SETOTHERMODE_L] = &setOtherModeL;
            gbi->map[F3D_G_SETGEOMETRYMODE] = &setGeometryMode;
            gbi->map[F3D_G_CLEARGEOMETRYMODE] = &clearGeometryMode;
            gbi->map[F3D_G_RDPHALF_1] = &rdpHalf1;
            gbi->map[F3D_G_RDPHALF_2] = &rdpHalf2;
            gbi->map[G_SETCIMG] = &setColorImage;
            gbi->map[G_SETZIMG] = &setDepthImage;
            gbi->map[G_SETTIMG] = &setTextureImage;
            gbi->map[G_RDPNOOP] = &GBI_RDP::noOp;

            gbi->resetFromTask = &reset;
        }
    }
};
