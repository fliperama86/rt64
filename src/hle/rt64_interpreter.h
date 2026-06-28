//
// RT64
//

#pragma once

#include "rt64_state.h"

#include "gbi/rt64_f3d.h"
#include "gbi/rt64_gbi.h"

#ifndef LOD_ENABLE_DL_PATH_TRACE
#define LOD_ENABLE_DL_PATH_TRACE 0
#endif

namespace RT64 {
#if LOD_ENABLE_DL_PATH_TRACE
    void lodTraceDlPathRecord(State *state, const GBI *gbi, DisplayList *dl, uint32_t cmdCount);
    void lodTraceDlPathDump(State *state, const GBI *gbi, const char *reason, uint32_t dlStartAddress, DisplayList *dlStart, DisplayList *dl, uint32_t cmdCount, uint32_t aux0, uint32_t aux1);
#endif

    struct Interpreter {
        State *state;
        GBIManager gbiManager;
        GBI *hleGBI;
        uint8_t extendedOpCode = 0;
        GBIFunction extendedFunction = nullptr;

        struct {
            uint32_t textAddress = 0;
            uint32_t dataAddress = 0;
        } UCode;

        Interpreter();
        void setup(State *state);
        void loadUCodeGBI(uint32_t textAddress, uint32_t dataAddress, bool resetFromTask);
        void processRDPLists(uint32_t dlStartAdddress, DisplayList* dlStart, DisplayList* dlEnd);
        void processDisplayLists(uint32_t dlStartAdddress, DisplayList *dlStart);
    };
};
