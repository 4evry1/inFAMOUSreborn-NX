#include "lwmutex.h"
#include <cassert>
int main() {
    ps3_lwmutex_condition_binding binding{};
    assert(ps3_lwmutex_condition_bind(nullptr, 8, &binding).status == PS3_LWMUTEX_INVALID_ARGUMENT);
}
