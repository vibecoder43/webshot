/**
 * @file
 * @brief Service entry point wiring userver components and HTTP handlers.
 */
#include "webshot_by_id_handler.hpp"
#include "webshot_config.hpp"
#include "webshot_crud.hpp"
#include "webshot_denylist.hpp"
#include "webshot_disallow_and_purge_handler.hpp"
#include "webshot_handler.hpp"
#include "webshots_by_prefix_handler.hpp"

#include <userver/clients/dns/component.hpp>
#include <userver/clients/http/component.hpp>
#include <userver/components/minimal_server_component_list.hpp>
#include <userver/congestion_control/component.hpp>
#include <userver/storages/postgres/component.hpp>
#include <userver/storages/secdist/component.hpp>
#include <userver/storages/secdist/provider_component.hpp>
#include <userver/testsuite/testsuite_support.hpp>
#include <userver/utils/daemon_run.hpp>

namespace us = userver;
int main(int argc, char *argv[])
{
    auto component_list = us::components::MinimalServerComponentList()
                              .Append<us::clients::dns::Component>()
                              .Append<us::components::HttpClient>()
                              .Append<us::components::TestsuiteSupport>()
                              .Append<us::components::Secdist>()
                              .Append<us::components::DefaultSecdistProvider>()
                              .Append<us::components::Postgres>("webshot-meta-db")
                              .Append<us::components::Postgres>("denylist-db")
                              .Append<us::congestion_control::Component>()
                              .Append<v1::WebshotDenylist>()
                              .Append<v1::WebshotConfig>()
                              .Append<v1::WebshotCrud>()
                              .Append<v1::WebshotsByPrefixHandler>()
                              .Append<v1::WebshotHandler>()
                              .Append<v1::WebshotDisallowAndPurgeHandler>()
                              .Append<v1::WebshotById>();
    return us::utils::DaemonMain(argc, argv, component_list);
}
