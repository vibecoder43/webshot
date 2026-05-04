#include "test_only_components.hpp"

/**
 * @file
 * @brief Testsuite implementation for test-only component registration.
 */

#include "browser_probe_handler.hpp"

#include <userver/testsuite/testsuite_support.hpp>

namespace ws {

namespace us = userver;
void AppendTestOnlyComponents(us::components::ComponentList &component_list)
{
    component_list.Append<us::components::TestsuiteSupport>().Append<BrowserProbeHandler>();
}

} // namespace ws
