// Reproduces libnx's public integer name before including the production C++ boundary.
using u128=unsigned __int128;
#include "../adapters/startup_wws_composition.h"
static_assert(sizeof(u128)==16,"libnx integer type must remain intact");
#ifdef SPU_LS_SIZE
#error "Opaque WWS composition leaked the concrete SPU context into C++"
#endif
int main() {
    ps3_startup_wws_composition *owner=nullptr;
    ps3_startup_wws_diagnostics diagnostics{};
    return owner || diagnostics.entered;
}
