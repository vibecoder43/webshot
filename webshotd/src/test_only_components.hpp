#pragma once

#include <userver/components/component_list.hpp>

namespace ws {

namespace us = userver;
void AppendTestOnlyComponents(us::components::ComponentList &component_list);

} // namespace ws
