#include "prx/libSceNpPartnerSubscription/include/InventoryExports.hpp"

#include <cstdlib>

namespace {

void Require(bool value) {
    if (!value) std::abort();
}

} // namespace

int main() {
    constexpr int notInitialized = static_cast<int>(0x819D2001u);
    Require(sceNpPartnerSubscriptionTerminate() == notInitialized);
    Require(sceNpPartnerSubscriptionTerminate() == notInitialized);
    return 0;
}
