//
// RT64
//

#include "rt64_interpreter.h"

#include <cassert>
#include <cstdio>
#include <cinttypes>

//#define DUMP_DISPLAY_LISTS

#ifndef LOD_ENABLE_GBI_MISS_TRACE
#define LOD_ENABLE_GBI_MISS_TRACE 0
#endif

#ifndef LOD_FIX_PRESERVE_GBI_ON_LOAD_MISS
#define LOD_FIX_PRESERVE_GBI_ON_LOAD_MISS 0
#endif

#ifndef LOD_FIX_MALFORMED_DL_CLEAR_STACK
#define LOD_FIX_MALFORMED_DL_CLEAR_STACK 0
#endif

#ifndef LOD_ENABLE_RENDER_GEOM_TRACE
#define LOD_ENABLE_RENDER_GEOM_TRACE 0
#endif

#ifndef LOD_ENABLE_DL_PATH_TRACE
#define LOD_ENABLE_DL_PATH_TRACE 0
#endif

extern "C" {
    extern uint32_t g_tlb_segment_0e;
    extern uint32_t g_tlb_segment_0f;
}

namespace RT64 {
    static FILE *displayListFp = nullptr;

#if LOD_ENABLE_DL_PATH_TRACE
    struct LodDlPathEntry {
        uint64_t displayListCounter = 0;
        uint32_t rootAddress = 0;
        uint32_t cmdCount = 0;
        uint32_t hostOffset = 0xFFFFFFFFU;
        uint32_t returnHostOffset = 0xFFFFFFFFU;
        uint32_t w0 = 0;
        uint32_t w1 = 0;
        uint8_t opCode = 0;
        uint8_t ucode = 0xFFU;
        uint8_t known = 0;
        uint8_t stackDepth = 0;
    };

    struct LodDlPathRing {
        LodDlPathEntry entries[64];
        uint32_t serial = 0;
        uint32_t used = 0;
        uint32_t rootAddress = 0;
        uint32_t rootHostOffset = 0xFFFFFFFFU;
        uint64_t displayListCounter = 0;
    };

    static thread_local LodDlPathRing lodDlPathRing;

    static uint32_t lodDlPathPointerOffset(State *state, const DisplayList *ptr) {
        if ((state == nullptr) || (ptr == nullptr)) {
            return 0xFFFFFFFFU;
        }

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

    static bool lodDlPathOpcodeKnown(const GBI *gbi, uint8_t opCode) {
        return (gbi != nullptr) && (gbi->map[opCode] != nullptr);
    }

    static void lodTraceDlPathBegin(State *state, uint32_t dlStartAddress, DisplayList *dlStart) {
        lodDlPathRing = {};
        lodDlPathRing.rootAddress = dlStartAddress;
        lodDlPathRing.rootHostOffset = lodDlPathPointerOffset(state, dlStart);
        lodDlPathRing.displayListCounter = (state != nullptr) ? state->displayListCounter : 0;
    }

    void lodTraceDlPathRecord(State *state, const GBI *gbi, DisplayList *dl, uint32_t cmdCount) {
        if ((state == nullptr) || (dl == nullptr)) {
            return;
        }

        if (lodDlPathRing.displayListCounter != state->displayListCounter) {
            lodTraceDlPathBegin(state, state->displayListAddress, nullptr);
        }

        LodDlPathEntry &entry = lodDlPathRing.entries[lodDlPathRing.serial % 64];
        const uint8_t opCode = static_cast<uint8_t>(dl->w0 >> 24);
        entry.displayListCounter = state->displayListCounter;
        entry.rootAddress = state->displayListAddress;
        entry.cmdCount = cmdCount;
        entry.hostOffset = lodDlPathPointerOffset(state, dl);
        entry.returnHostOffset = state->returnAddressStack.empty()
            ? 0xFFFFFFFFU
            : lodDlPathPointerOffset(state, state->returnAddressStack.back());
        entry.w0 = dl->w0;
        entry.w1 = dl->w1;
        entry.opCode = opCode;
        entry.ucode = (gbi != nullptr) ? static_cast<uint8_t>(gbi->ucode) : 0xFFU;
        entry.known = lodDlPathOpcodeKnown(gbi, opCode) ? 1U : 0U;
        entry.stackDepth = (state->returnAddressStack.size() > 0xFFU) ? 0xFFU : static_cast<uint8_t>(state->returnAddressStack.size());

        lodDlPathRing.serial++;
        if (lodDlPathRing.used < 64) {
            lodDlPathRing.used++;
        }
    }

    void lodTraceDlPathDump(State *state, const GBI *gbi, const char *reason, uint32_t dlStartAddress, DisplayList *dlStart, DisplayList *dl, uint32_t cmdCount, uint32_t aux0, uint32_t aux1) {
        static uint32_t dumpCount = 0;
        dumpCount++;
        if (dumpCount > 16) {
            return;
        }

        const uint32_t startAddress = (dlStartAddress != 0) ? dlStartAddress : lodDlPathRing.rootAddress;
        const uint32_t startHostOffset = (dlStart != nullptr) ? lodDlPathPointerOffset(state, dlStart) : lodDlPathRing.rootHostOffset;
        const uint32_t currentHostOffset = lodDlPathPointerOffset(state, dl);
        const uint8_t currentOpcode = (dl != nullptr) ? static_cast<uint8_t>(dl->w0 >> 24) : 0xFFU;
        const uint32_t stackDepth = (state != nullptr) ? static_cast<uint32_t>(state->returnAddressStack.size()) : 0U;

        fprintf(stderr,
            "[DL_PATH][DUMP] #%u reason=%s dl=%" PRIu64 " start=0x%08X start_host=0x%08X cur_host=0x%08X cmd=%u ucode=%u op=0x%02X known=%u stack=%u aux0=0x%08X aux1=0x%08X w0=0x%08X w1=0x%08X\n",
            dumpCount,
            (reason != nullptr) ? reason : "unknown",
            (state != nullptr) ? state->displayListCounter : 0,
            startAddress,
            startHostOffset,
            currentHostOffset,
            cmdCount,
            (gbi != nullptr) ? static_cast<unsigned>(gbi->ucode) : 0xFFFFFFFFU,
            currentOpcode,
            lodDlPathOpcodeKnown(gbi, currentOpcode) ? 1U : 0U,
            stackDepth,
            aux0,
            aux1,
            (dl != nullptr) ? dl->w0 : 0,
            (dl != nullptr) ? dl->w1 : 0);

        const uint32_t used = lodDlPathRing.used;
        const uint32_t firstSerial = lodDlPathRing.serial - used;
        for (uint32_t i = 0; i < used; i++) {
            const uint32_t serial = firstSerial + i;
            const LodDlPathEntry &entry = lodDlPathRing.entries[serial % 64];
            fprintf(stderr,
                "[DL_PATH][ENTRY] dump=%u age=%02u serial=%u dl=%" PRIu64 " root=0x%08X cmd=%u host=0x%08X ret=0x%08X ucode=%u op=0x%02X known=%u stack=%u w0=0x%08X w1=0x%08X\n",
                dumpCount,
                used - i,
                serial,
                entry.displayListCounter,
                entry.rootAddress,
                entry.cmdCount,
                entry.hostOffset,
                entry.returnHostOffset,
                static_cast<unsigned>(entry.ucode),
                entry.opCode,
                entry.known,
                entry.stackDepth,
                entry.w0,
                entry.w1);
        }

        if ((state != nullptr) && (dlStart != nullptr) && (startHostOffset <= (0x00800000U - (8U * sizeof(DisplayList))))) {
            for (uint32_t i = 0; i < 8; i++) {
                fprintf(stderr,
                    "[DL_PATH][ROOT] dump=%u +%02u host=0x%08X w0=0x%08X w1=0x%08X\n",
                    dumpCount,
                    i,
                    startHostOffset + static_cast<uint32_t>(i * sizeof(DisplayList)),
                    dlStart[i].w0,
                    dlStart[i].w1);
            }
        }
    }
#endif

#if LOD_ENABLE_RENDER_GEOM_TRACE
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

    static bool lodTraceOpcodeKnown(const GBI *gbi, uint8_t opCode) {
        return (gbi != nullptr) && (gbi->map[opCode] != nullptr);
    }

    static void lodTraceDumpSegments(State *state, const char *tag, uint32_t serial) {
        fprintf(stderr,
            "[RT64-GEOM][%s-SEG] #%u s0=%08X s1=%08X s2=%08X s3=%08X s4=%08X s5=%08X s6=%08X s7=%08X tlb0e=%08X tlb0f=%08X\n",
            tag,
            serial,
            state->rsp->segments[0], state->rsp->segments[1], state->rsp->segments[2], state->rsp->segments[3],
            state->rsp->segments[4], state->rsp->segments[5], state->rsp->segments[6], state->rsp->segments[7],
            g_tlb_segment_0e, g_tlb_segment_0f);
        fprintf(stderr,
            "[RT64-GEOM][%s-SEG] #%u s8=%08X s9=%08X sA=%08X sB=%08X sC=%08X sD=%08X sE=%08X sF=%08X\n",
            tag,
            serial,
            state->rsp->segments[8], state->rsp->segments[9], state->rsp->segments[10], state->rsp->segments[11],
            state->rsp->segments[12], state->rsp->segments[13], state->rsp->segments[14], state->rsp->segments[15]);
    }

    static void lodTraceDisplayListRoot(State *state, const GBI *gbi, uint32_t dlStartAddress, DisplayList *dlStart) {
        static uint32_t rootTraceCount = 0;
        rootTraceCount++;

        const uint32_t hostOffset = lodTraceDisplayListPointerOffset(state, dlStart);
        const uint8_t firstOpcode = (dlStart->w0 >> 24) & 0xFF;
        const bool firstKnown = lodTraceOpcodeKnown(gbi, firstOpcode);
        const bool firstEmpty = (dlStart->w0 == 0) && (dlStart->w1 == 0);
        const bool suspicious = firstEmpty || !firstKnown || (dlStartAddress >= 0x00800000U) || (hostOffset >= 0x00800000U);
        const bool shouldLog = (rootTraceCount <= 96) || suspicious || ((rootTraceCount % 100) == 0);

        if (!shouldLog) {
            return;
        }

        fprintf(stderr,
            "[RT64-GEOM][DLROOT] #%u dl=%llu start=0x%08X host=0x%08X ucode=%u op=0x%02X known=%u empty=%u w0=0x%08X w1=0x%08X suspicious=%u\n",
            rootTraceCount,
            static_cast<unsigned long long>(state->displayListCounter),
            dlStartAddress,
            hostOffset,
            (gbi != nullptr) ? static_cast<unsigned>(gbi->ucode) : 0xFFFFFFFFU,
            firstOpcode,
            firstKnown ? 1U : 0U,
            firstEmpty ? 1U : 0U,
            dlStart->w0,
            dlStart->w1,
            suspicious ? 1U : 0U);

        lodTraceDumpSegments(state, "DLROOT", rootTraceCount);

        if (suspicious || (rootTraceCount <= 16)) {
            for (uint32_t i = 0; i < 8; i++) {
                fprintf(stderr, "[RT64-GEOM][DLROOT-DUMP] #%u +%02u w0=0x%08X w1=0x%08X\n",
                    rootTraceCount, i, dlStart[i].w0, dlStart[i].w1);
            }
        }
    }

    static void lodTraceDisplayListAbort(State *state, const GBI *gbi, uint32_t dlStartAddress, DisplayList *dlStart, DisplayList *dl, uint32_t cmdCount, int abortCount) {
        const uint32_t startHostOffset = lodTraceDisplayListPointerOffset(state, dlStart);
        const uint32_t currentHostOffset = lodTraceDisplayListPointerOffset(state, dl);
        const uint8_t currentOpcode = (dl->w0 >> 24) & 0xFF;
        fprintf(stderr,
            "[RT64-GEOM][DLABORT] #%d dl=%llu start=0x%08X start_host=0x%08X cur_host=0x%08X cmd=%u ucode=%u op=0x%02X known=%u w0=0x%08X w1=0x%08X\n",
            abortCount,
            static_cast<unsigned long long>(state->displayListCounter),
            dlStartAddress,
            startHostOffset,
            currentHostOffset,
            cmdCount,
            (gbi != nullptr) ? static_cast<unsigned>(gbi->ucode) : 0xFFFFFFFFU,
            currentOpcode,
            lodTraceOpcodeKnown(gbi, currentOpcode) ? 1U : 0U,
            dl->w0,
            dl->w1);
        lodTraceDumpSegments(state, "DLABORT", static_cast<uint32_t>(abortCount));
    }
#endif

    // Interpreter

    Interpreter::Interpreter() {
        state = nullptr;
        hleGBI = nullptr;
        extendedFunction = gbiManager.getExtendedFunction();
    }

    void Interpreter::setup(State *state) {
        this->state = state;
    }

    void Interpreter::loadUCodeGBI(uint32_t textAddress, uint32_t dataAddress, bool resetFromTask) {
        if (!resetFromTask) {
            state->flush();
        }

        const uint32_t AddressMask = 0xFFFFF8;
        const uint32_t maskedTextAddress = textAddress & AddressMask;
        const uint32_t maskedDataAddress = dataAddress & AddressMask;
        bool skipResetForPreservedMiss = false;
        if ((UCode.textAddress != maskedTextAddress) || (UCode.dataAddress != maskedDataAddress)) {
            GBI *matchedGBI = gbiManager.getGBIForUCode(state->RDRAM, maskedTextAddress, maskedDataAddress);
#if LOD_FIX_PRESERVE_GBI_ON_LOAD_MISS
            if ((matchedGBI == nullptr) && !resetFromTask && (hleGBI != nullptr)) {
                skipResetForPreservedMiss = true;
#if LOD_ENABLE_GBI_MISS_TRACE
                static uint32_t preservedLoadMissCount = 0;
                preservedLoadMissCount++;
                if ((preservedLoadMissCount <= 16) || ((preservedLoadMissCount % 100) == 0)) {
                    fprintf(stderr,
                        "[GBI_LOAD_UCODE_PRESERVE] #%u dl=%llu start=0x%08X text=0x%08X data=0x%08X masked_text=0x%08X masked_data=0x%08X prev_ucode=%u\n",
                        preservedLoadMissCount,
                        static_cast<unsigned long long>(state->displayListCounter),
                        state->displayListAddress,
                        textAddress,
                        dataAddress,
                        maskedTextAddress,
                        maskedDataAddress,
                        static_cast<unsigned>(hleGBI->ucode));
                }
#endif
            }
            else
#endif
            {
                hleGBI = matchedGBI;
                if (hleGBI != nullptr) {
                    state->rsp->setGBI(hleGBI);
                }

                UCode.textAddress = maskedTextAddress;
                UCode.dataAddress = maskedDataAddress;
            }
        }

        if ((hleGBI != nullptr) && !skipResetForPreservedMiss) {
            GBIReset resetFunction = resetFromTask ? hleGBI->resetFromTask : hleGBI->resetFromLoad;
            if (resetFunction != nullptr) {
                resetFunction(state);
            }
        }
    }

    void Interpreter::processRDPLists(uint32_t dlStartAdddress, DisplayList *dlStart, DisplayList *dlEnd) {
        state->dlCpuProfiler.start();

        // Update the state with the current display list address.
        state->displayListAddress = dlStartAdddress;
        state->displayListCounter++;

        // Check RDRAM if required.
        state->checkRDRAM();

        GBI *rdpGBI = state->rdp->gbi;
        constexpr unsigned int opCodeMask = 0x3F;

        // Run the command interpreter.
        assert(rdpGBI != nullptr);
        DisplayList *dl = dlStart;
        uint8_t opCode;
        GBIFunction func;
        uint32_t cmdLength;
        size_t pendingCommandRemainingBytes = state->rdp->pendingCommandRemainingBytes;

        if (dlStart >= dlEnd) {
            state->dlCpuProfiler.end();
            return;
        }

        if (pendingCommandRemainingBytes != 0) {
            // Copy the remaining command bytes from the current displaylist
            uint32_t toCopy = (uint32_t)std::min(pendingCommandRemainingBytes, (uintptr_t)dlEnd - (uintptr_t)dl);
            memcpy(state->rdp->pendingCommandBuffer.data() + state->rdp->pendingCommandCurrentBytes, dl, toCopy);

            // Modify start to skip the copied bytes
            dl = (DisplayList *)(toCopy + (uintptr_t)dl);

            // Check if we've copied all of the bytes of the command into the buffer
            if (pendingCommandRemainingBytes == toCopy) {
                // All bytes have been copied, so run the completed command
                DisplayList *pendingCommand = (DisplayList *)state->rdp->pendingCommandBuffer.data();
                opCode = (pendingCommand->w0 >> 24) & opCodeMask;
                func = rdpGBI->map[opCode];

                if (func != nullptr) {
                    func(state, &pendingCommand);
                }
                else {
                    RT64_LOG_PRINTF("DL Parser ran into an unknown RDP opCode: %u / 0x%X", opCode, opCode);
                }

                state->rdp->pendingCommandCurrentBytes = 0;
                state->rdp->pendingCommandRemainingBytes = 0;
            }
            // Not all of the bytes were copied, so adjust RDP state accordingly and exit.
            else {
                state->rdp->pendingCommandCurrentBytes += toCopy;
                state->rdp->pendingCommandRemainingBytes -= toCopy;
                state->dlCpuProfiler.end();
                return;
            }
        }

        // Create a dummy pointer and pass that, since displaylist pointer incrementing is handled differently in LLE.
        DisplayList *dummy;
        while ((dl != nullptr) && ((dlEnd == nullptr) || (dl < dlEnd))) {
            opCode = (dl->w0 >> 24) & opCodeMask;

            if ((extendedOpCode != 0) && (opCode == extendedOpCode)) {
                dummy = dl;
                extendedFunction(state, &dl);
                cmdLength = 1;
            }
            else {
                func = rdpGBI->map[opCode];
                cmdLength = state->rdp->commandWordLengths[opCode];

#       ifdef DUMP_DISPLAY_LISTS
                RT64_LOG_PRINTF("0x%08X 0x%08X", dl->w0, dl->w1);
#       endif

                // Check if this command is unfinished and store the partial contents if so.
                if (dl + cmdLength > dlEnd) {
                    uint32_t toCopy = (uint32_t)((uintptr_t)dlEnd - (uintptr_t)dl);
                    memcpy(state->rdp->pendingCommandBuffer.data(), dl, toCopy);
                    state->rdp->pendingCommandCurrentBytes = toCopy;
                    state->rdp->pendingCommandRemainingBytes = cmdLength * sizeof(DisplayList) - toCopy;
                    break;
                }

                if (func != nullptr) {
                    dummy = dl;
                    func(state, &dummy);
                }
                else {
                    RT64_LOG_PRINTF("DL Parser ran into an unknown RDP opCode: %u / 0x%X", opCode, opCode);
                }
            }

            if (dl != nullptr) {
                dl += cmdLength;
            }
        }

        state->dlCpuProfiler.end();
    }

    void Interpreter::processDisplayLists(uint32_t dlStartAdddress, DisplayList *dlStart) {
#if LOD_ENABLE_GBI_MISS_TRACE
        if (hleGBI == nullptr) {
            static uint32_t null_hle_skip_count = 0;
            null_hle_skip_count++;
            if (null_hle_skip_count <= 16 || (null_hle_skip_count % 100) == 0) {
                fprintf(stderr,
                        "[GBI_MISS_SKIP] #%u skipping HLE DL at 0x%08X because no matching GBI is loaded\n",
                        null_hle_skip_count, dlStartAdddress);
            }
            return;
        }
#else
        assert(hleGBI != nullptr);
#endif

        state->dlCpuProfiler.start();

        // Update the state with the current display list address.
        state->displayListAddress = dlStartAdddress;
        state->displayListCounter++;

#if LOD_FIX_MALFORMED_DL_CLEAR_STACK
        state->returnAddressStack.clear();
#endif
#if LOD_ENABLE_DL_PATH_TRACE
        lodTraceDlPathBegin(state, dlStartAdddress, dlStart);
#endif
#if LOD_ENABLE_RENDER_GEOM_TRACE
        lodTraceDisplayListRoot(state, hleGBI, dlStartAdddress, dlStart);
#endif

        // Check RDRAM if required.
        state->checkRDRAM();

        // Run the command interpreter.
        DisplayList *dl = dlStart;
        uint8_t opCode;
        GBIFunction func;
        uint32_t cmdCount = 0;
        constexpr uint32_t maxCmds = 100000;
        while (dl != nullptr) {
            if (++cmdCount > maxCmds) {
                static int abort_count = 0;
                abort_count++;
                if (abort_count <= 5) {
                    fprintf(stderr, "[RT64] DL exceeded %u commands — aborting (#%d). Last cmd: %08X %08X at %p\n",
                            maxCmds, abort_count, dl->w0, dl->w1, (void*)dl);
#if LOD_ENABLE_RENDER_GEOM_TRACE
                    lodTraceDisplayListAbort(state, hleGBI, dlStartAdddress, dlStart, dl, cmdCount, abort_count);
#endif
#if LOD_ENABLE_DL_PATH_TRACE
                    lodTraceDlPathDump(state, hleGBI, "max-cmds", dlStartAdddress, dlStart, dl, cmdCount, 0, 0);
#endif
                }
#if LOD_FIX_MALFORMED_DL_CLEAR_STACK
                state->returnAddressStack.clear();
#endif
                dl = nullptr; // Clean exit — don't break, let the while loop end naturally
                continue;
            }
#if LOD_ENABLE_GBI_MISS_TRACE
            if (hleGBI == nullptr) {
                static uint32_t mid_dl_null_hle_skip_count = 0;
                mid_dl_null_hle_skip_count++;
                if (mid_dl_null_hle_skip_count <= 16 || (mid_dl_null_hle_skip_count % 100) == 0) {
                    fprintf(stderr,
                            "[GBI_MISS_SKIP] mid-DL #%u at start=0x%08X cmd=%u because no matching GBI is loaded\n",
                            mid_dl_null_hle_skip_count, dlStartAdddress, cmdCount);
                }
                dl = nullptr;
                continue;
            }
#endif
            opCode = (dl->w0 >> 24);

#if LOD_ENABLE_DL_PATH_TRACE
            lodTraceDlPathRecord(state, hleGBI, dl, cmdCount);
#endif
            if ((extendedOpCode != 0) && (opCode == extendedOpCode)) {
                extendedFunction(state, &dl);
            }
            else {
                func = hleGBI->map[opCode];

#       ifdef DUMP_DISPLAY_LISTS
                RT64_LOG_PRINTF("0x%08X 0x%08X", dl->w0, dl->w1);
#       endif

                if (func != nullptr) {
                    func(state, &dl);
                }
                else {
                    RT64_LOG_PRINTF("DL Parser ran into an unknown opCode (GBI %u): %u / 0x%X", uint32_t(hleGBI->ucode), opCode, opCode);
                }
            }

            if (dl != nullptr) {
                dl++;
            }
        }

        state->dlCpuProfiler.end();
    }
};
