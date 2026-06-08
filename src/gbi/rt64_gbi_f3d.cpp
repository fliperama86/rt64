//
// RT64
//

#include "rt64_gbi_f3d.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

#include "../include/rt64_extended_gbi.h"


#include "rt64_f3d.h"
#include "rt64_gbi_extended.h"
#include "rt64_gbi_rdp.h"

#ifndef LOD_ENABLE_RENDER_ADDR_TRACE
#define LOD_ENABLE_RENDER_ADDR_TRACE 0
#endif

#ifndef LOD_ENABLE_RENDER_GEOM_TRACE
#define LOD_ENABLE_RENDER_GEOM_TRACE 0
#endif

namespace RT64 {
    namespace GBI_F3D {
#if LOD_ENABLE_RENDER_GEOM_TRACE
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

        static void lodTraceRunDl(State *state, const DisplayList *caller, uint32_t segmentedAddress, uint32_t rdramAddress, const DisplayList *target, bool invalid, bool empty, uint8_t firstOpcode) {
            static uint32_t runDlTraceCount = 0;
            const bool overlay = lodTraceIsOverlayAddress(segmentedAddress) || lodTraceIsOverlayAddress(rdramAddress);
            if ((invalid || overlay) && (runDlTraceCount < 512)) {
                const uint32_t callerOffset = lodTraceDisplayListPointerOffset(state, caller);
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
            state->rsp->setVertex((*dl)->w1, (*dl)->p0(20, 4) + 1, (*dl)->p0(16, 4));
        }

        void runDl(State *state, DisplayList **dl) {
            const uint32_t rdramAddress = state->rsp->fromSegmentedMasked((*dl)->w1);

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
            }

            DisplayList *target = reinterpret_cast<DisplayList *>(state->fromRDRAM(rdramAddress));

            // Guard: skip if target doesn't look like valid DL data.
            // Valid GBI opcodes for F3DEX2 are in specific ranges (0x01-0x0B, 0xB6-0xFF).
            // MIPS instructions (which start with opcodes like 0x24, 0x27, 0x8C, 0xA4, etc.)
            // are NOT valid GBI commands. Check the first command's opcode.
            {
                uint8_t firstOpcode = (target->w0 >> 24) & 0xFF;
                bool likelyGBI = (firstOpcode <= 0x0B) || (firstOpcode >= 0xB4);
                bool isEmpty = (target->w0 == 0 && target->w1 == 0);
#if LOD_ENABLE_RENDER_GEOM_TRACE
                lodTraceRunDl(state, *dl, (*dl)->w1, rdramAddress, target, isEmpty || !likelyGBI, isEmpty, firstOpcode);
#endif
                if (isEmpty || !likelyGBI) {
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



            if ((*dl)->p0(16, 1) == 0) {
                state->pushReturnAddress(*dl);
            }

            *dl = target - 1;
        }

        void endDl(State *state, DisplayList **dl) {
            *dl = state->popReturnAddress();
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