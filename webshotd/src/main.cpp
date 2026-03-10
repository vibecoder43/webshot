/**
 * @file
 * @brief Service entry point wiring userver components and HTTP handlers.
 */
#include "by_id_handler.hpp"
#include "by_prefix_handler.hpp"
#include "config.hpp"
#include "crud.hpp"
#include "denylist.hpp"
#include "denylist_check_handler.hpp"
#include "disallow_and_purge_handler.hpp"
#include "docs_handler.hpp"
#include "docs_static_handler.hpp"
#include "handler.hpp"
#include "job_handler.hpp"

#include <userver/clients/dns/component.hpp>
#include <userver/clients/http/component.hpp>
#include <userver/components/minimal_server_component_list.hpp>
#include <userver/congestion_control/component.hpp>
#include <userver/server/handlers/server_monitor.hpp>
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
                              .Append<us::components::Postgres>("capture_meta_db")
                              .Append<us::components::Postgres>("shared_state_db")
                              .Append<us::congestion_control::Component>()
                              .Append<v1::Denylist>()
                              .Append<v1::Config>()
                              .Append<v1::Crud>()
                              .Append<v1::ByPrefixHandler>()
                              .Append<v1::Handler>()
                              .Append<v1::JobHandler>()
                              .Append<v1::DisallowAndPurgeHandler>()
                              .Append<v1::DenylistCheckHandler>()
                              .Append<v1::ById>()
                              .Append<v1::DocsHandler>()
                              .Append<v1::ScalarAssetsHandler>()
                              .Append<v1::OpenApiHandler>()
                              .Append<us::server::handlers::ServerMonitor>();
    return us::utils::DaemonMain(argc, argv, component_list);
}
