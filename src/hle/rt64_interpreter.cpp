//
// RT64
//

#include "rt64_interpreter.h"

#include <cassert>
#include <cstdio>

//#define DUMP_DISPLAY_LISTS

#ifndef LOD_ENABLE_GBI_MISS_TRACE
#define LOD_ENABLE_GBI_MISS_TRACE 0
#endif

#ifndef LOD_FIX_PRESERVE_GBI_ON_LOAD_MISS
#define LOD_FIX_PRESERVE_GBI_ON_LOAD_MISS 0
#endif

namespace RT64 {
    static FILE *displayListFp = nullptr;

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
                }
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
