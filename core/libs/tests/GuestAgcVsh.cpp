#include "prx/libSceAgc/Acb/include/Control.hpp"
#include "prx/libSceAgc/Acb/include/Dispatch.hpp"
#include "prx/libSceAgc/Acb/include/Memory.hpp"
#include "prx/libSceAgc/Acb/include/Sync.hpp"
#include "prx/libSceAgc/Cb/include/Branch.hpp"
#include "prx/libSceAgc/Cb/include/Dispatch.hpp"
#include "prx/libSceAgc/Cb/include/Registers.hpp"
#include "prx/libSceAgc/Cb/include/Sync.hpp"
#include "prx/libSceAgc/DcbDraw/include/DrawIndexed.hpp"
#include "prx/libSceAgc/DcbDraw/include/IndexBuffer.hpp"
#include "prx/libSceAgc/DcbDraw/include/Instancing.hpp"
#include "prx/libSceAgc/DcbDraw/include/Stats.hpp"
#include "prx/libSceAgc/DcbFlow/include/Control.hpp"
#include "prx/libSceAgc/DcbFlow/include/Dispatch.hpp"
#include "prx/libSceAgc/DcbFlow/include/Memory.hpp"
#include "prx/libSceAgc/DcbFlow/include/Sync.hpp"
#include "prx/libSceAgc/DcbState/include/ContextState.hpp"
#include "prx/libSceAgc/DcbState/include/Display.hpp"
#include "prx/libSceAgc/DcbState/include/Predication.hpp"
#include "prx/libSceAgc/DcbState/include/Registers.hpp"
#include "prx/libSceAgc/Misc/include/PacketInfo.hpp"
#include "prx/libSceAgc/Patch/include/Branch.hpp"
#include "prx/libSceAgc/Patch/include/CondExec.hpp"
#include "prx/libSceAgc/Patch/include/Dma.hpp"
#include "prx/libSceAgc/Patch/include/IndirectRegisters.hpp"
#include "prx/libSceAgc/Patch/include/Jump.hpp"
#include "prx/libSceAgc/Patch/include/QueueEndOfPipeAction.hpp"
#include "prx/libSceAgc/Patch/include/Rewind.hpp"
#include "prx/libSceAgc/Patch/include/WaitRegMem.hpp"
#include "prx/libSceAgc/Predication/include/Predication.hpp"
#include "prx/libSceAgc/Shader/include/InterpolantMapping.hpp"
#include "prx/libSceAgc/Shader/include/LinkShaders.hpp"
#include "prx/libSceAgc/Shader/include/PrimState.hpp"
#include "SceTypes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

void* vshLibrary = nullptr;

void Require(bool condition, const std::string& message) {
    if (condition) return;
    std::fprintf(stderr, "AgcVsh: %s\n", message.c_str());
    std::abort();
}

template <typename Function>
Function Vsh(const char* name) {
    const std::string symbol = std::string(name) + "_nid_no_patch";
#ifdef _WIN32
    void* address = reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(vshLibrary), symbol.c_str()));
#else
    void* address = dlsym(vshLibrary, symbol.c_str());
#endif
    Require(address != nullptr, symbol + " is not exported by libSceAgcVsh");
    return reinterpret_cast<Function>(address);
}

#define VSH(name) Vsh<decltype(&name)>(#name)

using Words = std::vector<std::uint32_t>;

struct Outcome {
    bool threw = false;
    std::string message;
    std::uint64_t result = 0;
    Words words;
};

Outcome Capture(const Words& input, const std::function<std::uint64_t(std::uint32_t*)>& call) {
    Outcome outcome;
    outcome.words = input;
    try {
        outcome.result = call(outcome.words.data());
    } catch (const std::runtime_error& error) {
        outcome.threw = true;
        outcome.message = error.what();
    }
    return outcome;
}

void Compare(const std::string& name, const Outcome& agc, const Outcome& vsh) {
    Require(agc.threw == vsh.threw && agc.message == vsh.message,
            name + ": libSceAgc " + (agc.threw ? "threw '" + agc.message + "'" : "returned") + ", libSceAgcVsh " + (vsh.threw ? "threw '" + vsh.message + "'" : "returned"));
    Require(agc.result == vsh.result, name + ": the results differ");
    Require(agc.words == vsh.words, name + ": the command words differ");
}

#define SAME(label, input, name, ...)                                                                                                              \
    Compare(label, Capture(input, [&]([[maybe_unused]] std::uint32_t* cmd) { return static_cast<std::uint64_t>(name(__VA_ARGS__)); }), \
            Capture(input, [&]([[maybe_unused]] std::uint32_t* cmd) { return static_cast<std::uint64_t>(VSH(name)(__VA_ARGS__)); }))

constexpr std::size_t PacketWords = 64;
constexpr std::uint32_t Filler = 0xabcdef01u;

template <typename Writer>
Words Build(Writer write) {
    std::array<std::uint32_t, PacketWords> words{};
    words.fill(Filler);
    CommandBuffer buffer{words.data(), words.data() + words.size(), words.data(), words.data() + words.size(), nullptr, nullptr, 0};
    Require(write(&buffer) == words.data(), "a libSceAgc writer did not start at the buffer");
    return Words(words.begin(), words.end());
}

template <typename T>
T* At(std::uint64_t address) {
    return reinterpret_cast<T*>(static_cast<std::uintptr_t>(address));
}

std::vector<std::pair<std::string, Words>> Packets() {
    alignas(8) static std::uint64_t label = 0;
    static std::uint32_t condition = 1;
    alignas(8) static Label endOfPipe{};
    std::vector<std::pair<std::string, Words>> packets;
    packets.emplace_back("no packet", Words(PacketWords, Filler));
    packets.emplace_back("branch", Build([](CommandBuffer* buffer) {
        return sceAgcCbBranch(buffer, 2, 3, At<const volatile std::uint64_t>(0x0000123456789ab8ull), 0x1122334455667788ull, 0x99aabbccddeeff00ull, 1,
                              At<const volatile std::uint32_t>(0x0000002233445564ull), 0x123u, 2, At<const volatile std::uint32_t>(0x0000006677889900ull), 0x456u);
    }));
    packets.emplace_back("dcb cond exec", Build([](CommandBuffer* buffer) { return sceAgcDcbCondExec(buffer, &condition, 3); }));
    packets.emplace_back("acb cond exec", Build([](CommandBuffer* buffer) { return sceAgcAcbCondExec(buffer, &condition, 3); }));
    packets.emplace_back("dma data", Build([](CommandBuffer* buffer) { return sceAgcDcbDmaData(buffer, 1, 5, 2, 0x0000778899aabbccull, 10, 1, 0x0000112233445566ull, 0x1234u, 1, 1, 1); }));
    packets.emplace_back("rewind", Build([](CommandBuffer* buffer) { return sceAgcDcbRewind(buffer, 0); }));
    packets.emplace_back("jump", Build([](CommandBuffer* buffer) { return sceAgcDcbJump(buffer, 1, 2, At<const std::uint32_t>(0x1000ull), 0x40u); }));
    packets.emplace_back("release mem", Build([](CommandBuffer* buffer) {
        return sceAgcCbReleaseMem(buffer, 0x28, 0x0a5, 1, 2, &endOfPipe, 2, 0x1122334455667788ull, 0, 0, 0, 0);
    }));
    packets.emplace_back("index count", Build([](CommandBuffer* buffer) { return sceAgcDcbSetIndexCount(buffer, 7); }));
    packets.emplace_back("wait reg mem", Build([](CommandBuffer* buffer) { return sceAgcDcbWaitRegMem(buffer, 0, 3, 0, 0, &label, 1, 0xffffffffu, 0x40); }));
    packets.emplace_back("wait reg mem 64", Build([](CommandBuffer* buffer) { return sceAgcDcbWaitRegMem(buffer, 1, 3, 0, 0, &label, 1, ~0ull, 0x40); }));
    packets.emplace_back("cx registers indirect", Build([](CommandBuffer* buffer) {
        return sceAgcDcbSetCxRegistersIndirect(buffer, At<const volatile ShaderRegister>(0x0000000012345670ull), 3);
    }));
    packets.emplace_back("sh registers indirect", Build([](CommandBuffer* buffer) {
        return sceAgcDcbSetShRegistersIndirect(buffer, At<const volatile ShaderRegister>(0x0000000012345670ull), 3);
    }));
    packets.emplace_back("uc registers indirect", Build([](CommandBuffer* buffer) {
        return sceAgcDcbSetUcRegistersIndirect(buffer, At<const volatile ShaderRegister>(0x0000000012345670ull), 3);
    }));
    return packets;
}

void CheckSizes() {
    const Words none;
    SAME("sceAgcAcbAcquireMemGetSize", none, sceAgcAcbAcquireMemGetSize);
    SAME("sceAgcAcbAtomicGdsGetSize", none, sceAgcAcbAtomicGdsGetSize);
    SAME("sceAgcAcbAtomicMemGetSize", none, sceAgcAcbAtomicMemGetSize);
    SAME("sceAgcAcbCondExecGetSize", none, sceAgcAcbCondExecGetSize);
    SAME("sceAgcAcbCopyDataGetSize", none, sceAgcAcbCopyDataGetSize);
    SAME("sceAgcAcbDispatchIndirectGetSize", none, sceAgcAcbDispatchIndirectGetSize);
    SAME("sceAgcAcbDmaDataGetSize", none, sceAgcAcbDmaDataGetSize);
    SAME("sceAgcAcbJumpGetSize", none, sceAgcAcbJumpGetSize);
    SAME("sceAgcAcbPrimeUtcl2GetSize", none, sceAgcAcbPrimeUtcl2GetSize);
    SAME("sceAgcAcbQueueEndOfShaderActionGetSize", none, sceAgcAcbQueueEndOfShaderActionGetSize);
    SAME("sceAgcAcbRewindGetSize", none, sceAgcAcbRewindGetSize);
    SAME("sceAgcCbCondWriteGetSize", none, sceAgcCbCondWriteGetSize);
    SAME("sceAgcCbDispatchGetSize", none, sceAgcCbDispatchGetSize);
    SAME("sceAgcCbQueueEndOfPipeActionGetSize", none, sceAgcCbQueueEndOfPipeActionGetSize);
    SAME("sceAgcDcbAtomicGdsGetSize", none, sceAgcDcbAtomicGdsGetSize);
    SAME("sceAgcDcbAtomicMemGetSize", none, sceAgcDcbAtomicMemGetSize);
    SAME("sceAgcDcbCondExecGetSize", none, sceAgcDcbCondExecGetSize);
    SAME("sceAgcDcbCopyDataGetSize", none, sceAgcDcbCopyDataGetSize);
    SAME("sceAgcDcbDispatchIndirectGetSize", none, sceAgcDcbDispatchIndirectGetSize);
    SAME("sceAgcDcbDmaDataGetSize", none, sceAgcDcbDmaDataGetSize);
    SAME("sceAgcDcbDrawIndexGetSize", none, sceAgcDcbDrawIndexGetSize);
    SAME("sceAgcDcbDrawIndexIndirectGetSize", none, sceAgcDcbDrawIndexIndirectGetSize);
    SAME("sceAgcDcbDrawIndexOffsetGetSize", none, sceAgcDcbDrawIndexOffsetGetSize);
    SAME("sceAgcDcbEndOcclusionQueryGetSize", none, sceAgcDcbEndOcclusionQueryGetSize);
    SAME("sceAgcDcbGetLodStatsGetSize", none, sceAgcDcbGetLodStatsGetSize);
    SAME("sceAgcDcbJumpGetSize", none, sceAgcDcbJumpGetSize);
    SAME("sceAgcDcbPrimeUtcl2GetSize", none, sceAgcDcbPrimeUtcl2GetSize);
    SAME("sceAgcDcbQueueEndOfShaderActionGetSize", none, sceAgcDcbQueueEndOfShaderActionGetSize);
    SAME("sceAgcDcbSetBaseDrawIndirectArgsGetSize", none, sceAgcDcbSetBaseDrawIndirectArgsGetSize);
    SAME("sceAgcDcbSetBoolPredicationEnableGetSize", none, sceAgcDcbSetBoolPredicationEnableGetSize);
    SAME("sceAgcDcbSetCxRegisterDirectGetSize", none, sceAgcDcbSetCxRegisterDirectGetSize);
    SAME("sceAgcDcbSetIndexIndirectArgsGetSize", none, sceAgcDcbSetIndexIndirectArgsGetSize);
    SAME("sceAgcDcbSetNumInstancesGetSize", none, sceAgcDcbSetNumInstancesGetSize);
    SAME("sceAgcDcbSetPredicationDisableGetSize", none, sceAgcDcbSetPredicationDisableGetSize);
    SAME("sceAgcDcbSetShRegisterDirectGetSize", none, sceAgcDcbSetShRegisterDirectGetSize);
    SAME("sceAgcDcbSetUcRegisterDirectGetSize", none, sceAgcDcbSetUcRegisterDirectGetSize);
    SAME("sceAgcDcbSetZPassPredicationEnableGetSize", none, sceAgcDcbSetZPassPredicationEnableGetSize);
    SAME("sceAgcDcbStallCommandBufferParserGetSize", none, sceAgcDcbStallCommandBufferParserGetSize);
    for (const std::uint8_t eventType : std::initializer_list<std::uint8_t>{0x00, 0x04, 0x14, 0x16, 0x2f, 0x3f, 0xff}) {
        const std::string row = " event " + std::to_string(eventType);
        SAME("sceAgcAcbEventWriteGetSize" + row, none, sceAgcAcbEventWriteGetSize, eventType);
        SAME("sceAgcDcbEventWriteGetSize" + row, none, sceAgcDcbEventWriteGetSize, eventType);
    }
    for (const std::uint32_t count : {0u, 1u, 3u, 16u, 0x3fffu, 0x4000u}) {
        const std::string row = " count " + std::to_string(count);
        SAME("sceAgcCbNopGetSize" + row, none, sceAgcCbNopGetSize, count);
        SAME("sceAgcCbSetShRegisterRangeDirectGetSize" + row, none, sceAgcCbSetShRegisterRangeDirectGetSize, count);
        SAME("sceAgcCbSetShRegistersDirectGetSize" + row, none, sceAgcCbSetShRegistersDirectGetSize, count);
        SAME("sceAgcCbSetUcRegisterRangeDirectGetSize" + row, none, sceAgcCbSetUcRegisterRangeDirectGetSize, count);
        SAME("sceAgcDcbSetCxRegistersIndirectGetSize" + row, none, sceAgcDcbSetCxRegistersIndirectGetSize, count);
        SAME("sceAgcDcbSetShRegistersIndirectGetSize" + row, none, sceAgcDcbSetShRegistersIndirectGetSize, count);
        SAME("sceAgcDcbWriteDataGetSize" + row, none, sceAgcDcbWriteDataGetSize, count);
    }
    for (const std::int32_t queryType : {0, 1, 2, 3, -1}) {
        SAME("sceAgcDcbBeginOcclusionQueryGetSize query " + std::to_string(queryType), none, sceAgcDcbBeginOcclusionQueryGetSize, queryType);
    }
}

void CheckPatches() {
    const auto* target = At<const volatile std::uint32_t>(0x0000005566778800ull);
    const auto* compare = At<const volatile std::uint64_t>(0x0000009988776650ull);
    const auto* registers = At<const volatile ShaderRegister>(0x0000000076543210ull);
    const auto* address = At<const volatile Label>(0x0000001122334450ull);
    for (const auto& [packet, words] : Packets()) {
        const std::string row = " on " + packet;
        SAME("sceAgcBranchPatchSetCompareAddress" + row, words, sceAgcBranchPatchSetCompareAddress, cmd, compare);
        SAME("sceAgcBranchPatchSetElseTarget" + row, words, sceAgcBranchPatchSetElseTarget, cmd, target, 0x321u);
        SAME("sceAgcBranchPatchSetThenTarget" + row, words, sceAgcBranchPatchSetThenTarget, cmd, target, 0x654u);
        SAME("sceAgcCondExecPatchSetCommandAddress" + row, words, sceAgcCondExecPatchSetCommandAddress, cmd, cmd + 5);
        SAME("sceAgcCondExecPatchSetEnd" + row, words, sceAgcCondExecPatchSetEnd, cmd, cmd + 9);
        SAME("sceAgcAsyncCondExecPatchSetCommandAddress" + row, words, sceAgcAsyncCondExecPatchSetCommandAddress, cmd, cmd + 5);
        SAME("sceAgcAsyncCondExecPatchSetEnd" + row, words, sceAgcAsyncCondExecPatchSetEnd, cmd, cmd + 9);
        SAME("sceAgcDmaDataPatchSetDstAddressOrOffset" + row, words, sceAgcDmaDataPatchSetDstAddressOrOffset, cmd, 0x0000123456789000ull);
        SAME("sceAgcDmaDataPatchSetSrcAddressOrOffsetOrImmediate" + row, words, sceAgcDmaDataPatchSetSrcAddressOrOffsetOrImmediate, cmd, 0x0000654321098000ull);
        SAME("sceAgcJumpPatchSetTarget" + row, words, sceAgcJumpPatchSetTarget, cmd, target, 0x88u);
        SAME("sceAgcQueueEndOfPipeActionPatchAddress" + row, words, sceAgcQueueEndOfPipeActionPatchAddress, cmd, address);
        SAME("sceAgcQueueEndOfPipeActionPatchData" + row, words, sceAgcQueueEndOfPipeActionPatchData, cmd, 0x0123456789abcdefull);
        SAME("sceAgcQueueEndOfPipeActionPatchGcrCntl" + row, words, sceAgcQueueEndOfPipeActionPatchGcrCntl, cmd, static_cast<std::uint16_t>(0x15a));
        SAME("sceAgcQueueEndOfPipeActionPatchType" + row, words, sceAgcQueueEndOfPipeActionPatchType, cmd, static_cast<std::uint8_t>(0x14));
        SAME("sceAgcRewindPatchSetRewindState" + row, words, sceAgcRewindPatchSetRewindState, cmd, static_cast<std::uint8_t>(1));
        SAME("sceAgcAsyncRewindPatchSetRewindState" + row, words, sceAgcAsyncRewindPatchSetRewindState, cmd, static_cast<std::uint8_t>(1));
        SAME("sceAgcSetCxRegIndirectPatchAddRegisters" + row, words, sceAgcSetCxRegIndirectPatchAddRegisters, cmd, 2u);
        SAME("sceAgcSetCxRegIndirectPatchSetAddress" + row, words, sceAgcSetCxRegIndirectPatchSetAddress, cmd, registers);
        SAME("sceAgcSetCxRegIndirectPatchSetNumRegisters" + row, words, sceAgcSetCxRegIndirectPatchSetNumRegisters, cmd, 5u);
        SAME("sceAgcSetShRegIndirectPatchAddRegisters" + row, words, sceAgcSetShRegIndirectPatchAddRegisters, cmd, 2u);
        SAME("sceAgcSetShRegIndirectPatchSetAddress" + row, words, sceAgcSetShRegIndirectPatchSetAddress, cmd, registers);
        SAME("sceAgcSetUcRegIndirectPatchSetAddress" + row, words, sceAgcSetUcRegIndirectPatchSetAddress, cmd, registers);
        SAME("sceAgcWaitRegMemPatchAddress" + row, words, sceAgcWaitRegMemPatchAddress, cmd, static_cast<const volatile void*>(address));
        SAME("sceAgcWaitRegMemPatchMask" + row, words, sceAgcWaitRegMemPatchMask, cmd, 0x00ff00ffu);
        SAME("sceAgcWaitRegMemPatchReference" + row, words, sceAgcWaitRegMemPatchReference, cmd, 0x12345678u);
        SAME("sceAgcSetPacketPredication" + row, words, sceAgcSetPacketPredication, cmd, 1u);
        SAME("sceAgcSetRangePredication" + row, words, sceAgcSetRangePredication, cmd, cmd + 8, 1u);
        SAME("sceAgcGetPacketSize" + row, words, sceAgcGetPacketSize, cmd);
        const auto payloadVsh = VSH(sceAgcGetDataPacketPayloadAddress);
        for (const int type : {0, 1, 2}) {
            const auto payload = [type](auto function, std::uint32_t* cmd) {
                std::uint32_t* address = nullptr;
                const auto result = static_cast<std::uint64_t>(static_cast<std::uint32_t>(function(&address, cmd, type)));
                return address == nullptr ? result : result | (static_cast<std::uint64_t>(address - cmd + 1) << 32);
            };
            Compare("sceAgcGetDataPacketPayloadAddress type " + std::to_string(type) + row,
                    Capture(words, [&](std::uint32_t* cmd) { return payload(&sceAgcGetDataPacketPayloadAddress, cmd); }),
                    Capture(words, [&](std::uint32_t* cmd) { return payload(payloadVsh, cmd); }));
        }
    }
}

void CheckShaderRegisters() {
    const Words none;
    SAME("sceAgcLinkShaders with no shaders", none, sceAgcLinkShaders, nullptr, nullptr, nullptr, nullptr, nullptr, 0u);
    for (const std::uint32_t primType : {1u, 4u, 0x11u}) {
        for (const std::uint32_t stages : {0u, 1u << 7}) {
            std::array<ShaderRegister, 2> cxAgc{{{0x2d5u, stages}, {0x29bu, 0xffffffffu}}};
            std::array<ShaderRegister, 3> ucAgc{{{0x22bu, 1u}, {0x22cu, 2u}, {0x242u, 0xffffffffu}}};
            auto cxVsh = cxAgc;
            auto ucVsh = ucAgc;
            const auto agc = sceAgcUpdatePrimState(cxAgc.data(), ucAgc.data(), primType);
            const auto vsh = VSH(sceAgcUpdatePrimState)(cxVsh.data(), ucVsh.data(), primType);
            Require(agc == vsh, "sceAgcUpdatePrimState: the results differ");
            for (std::size_t i = 0; i < cxAgc.size(); ++i) Require(cxAgc[i].offset == cxVsh[i].offset && cxAgc[i].value == cxVsh[i].value, "sceAgcUpdatePrimState: the context registers differ");
            for (std::size_t i = 0; i < ucAgc.size(); ++i) Require(ucAgc[i].offset == ucVsh[i].offset && ucAgc[i].value == ucVsh[i].value, "sceAgcUpdatePrimState: the uconfig registers differ");
        }
    }
    const auto mappingVsh = Vsh<decltype(&sceAgcCreateInterpolantMapping_nid_postfix)>("sceAgcCreateInterpolantMapping");
    std::array<ShaderRegister, 32> registers{};
    Compare("sceAgcCreateInterpolantMapping with no registers",
            Capture(none, [&](std::uint32_t*) { return static_cast<std::uint64_t>(sceAgcCreateInterpolantMapping_nid_postfix(nullptr, nullptr, nullptr)); }),
            Capture(none, [&](std::uint32_t*) { return static_cast<std::uint64_t>(mappingVsh(nullptr, nullptr, nullptr)); }));
    Compare("sceAgcCreateInterpolantMapping with no pixel shader",
            Capture(none, [&](std::uint32_t*) { return static_cast<std::uint64_t>(sceAgcCreateInterpolantMapping_nid_postfix(registers.data(), nullptr, nullptr)); }),
            Capture(none, [&](std::uint32_t*) { return static_cast<std::uint64_t>(mappingVsh(registers.data(), nullptr, nullptr)); }));
}

void CheckTraps() {
    for (const char* name : {"sceAgcAcbAcquireMem", "sceAgcAcbAtomicGds", "sceAgcAcbAtomicMem", "sceAgcAcbCopyData", "sceAgcAcbDmaData", "sceAgcAcbJump",
                             "sceAgcAcbPrimeUtcl2", "sceAgcAcbResetQueue", "sceAgcAcbRewind", "sceAgcAcbSetFlip", "sceAgcAcbSetMarker", "sceAgcAcbWaitRegMem",
                             "sceAgcAcbWaitUntilSafeForRendering", "sceAgcAcbWriteData", "sceAgcCbMemSemaphore", "sceAgcDcbAtomicGds", "sceAgcDcbContextStateOp",
                             "sceAgcDcbResetQueue", "sceAgcDcbSetFlip", "sceAgcDcbWaitUntilSafeForRendering", "sceAgcGetRegisterDefaults",
                             "sceAgcGetRegisterDefaultsInternal", "sceAgcSuspendPoint", "sceAgcSuspendPointAndCheckStatus"}) {
        const auto trap = Vsh<int (APS5_VABI*)()>(name);
        std::string message;
        try {
            trap();
        } catch (const std::runtime_error& error) {
            message = error.what();
        }
        Require(message == std::string(name) + "() is not allowed to be called from agc vsh.", std::string(name) + " answered '" + message + "'");
    }
}

}

int main(int argc, char** argv) {
    Require(argc == 2, "usage: guest_agc_vsh_tests <libSceAgcVsh.prx>");
#ifdef _WIN32
    vshLibrary = LoadLibraryA(argv[1]);
#else
    vshLibrary = dlopen(argv[1], RTLD_NOW);
#endif
    Require(vshLibrary != nullptr, std::string("cannot load ") + argv[1]);
    CheckSizes();
    CheckPatches();
    CheckShaderRegisters();
    CheckTraps();
    std::puts("AgcVsh tests passed");
    return 0;
}
