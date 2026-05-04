#pragma once

#include "integers.hpp"

namespace ws::crawler {

struct [[nodiscard]] CgroupLimits final {
    i64 cpu_cores;
    i64 memory_bytes;
};

} // namespace ws::crawler
