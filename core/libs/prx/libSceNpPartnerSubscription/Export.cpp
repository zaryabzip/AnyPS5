#include "prx/libSceNpPartnerSubscription/include/InventoryExports.hpp"
#include "prx/libc/include/General.hpp"

namespace {

constexpr int NOT_INITIALIZED = static_cast<int>(0x819D2001u);

}

extern "C" {

int APS5_VABI sceNpPartnerSubscriptionInitialize(void) {
    NotImplemented_nid_no_patch(__func__);
}

int APS5_VABI sceNpPartnerSubscriptionTerminate(void) {
    return NOT_INITIALIZED;
}

}
