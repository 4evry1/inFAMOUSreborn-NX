#include "timebase.h"
#include <cassert>
#include <type_traits>
static_assert(std::is_same<decltype(ps3_timebase_from_microseconds(0)), uint64_t>::value, "uint64 result");
int main() { assert(ps3_timebase_from_microseconds(5) == 399); }
