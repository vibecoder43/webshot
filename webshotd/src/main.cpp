/**
 * @file
 * @brief Service entry point wiring userver components and HTTP handlers.
 */
#include "access_policy.hpp"
#include "allowlist_handler.hpp"
#include "by_id_handler.hpp"
#include "by_prefix_handler.hpp"
#include "client_ip_ratelimiter.hpp"
#include "config.hpp"
#include "crud.hpp"
#include "deny_and_purge_handler.hpp"
#include "denylist_check_handler.hpp"
#include "docs_handler.hpp"
#include "exception_handling_middleware.hpp"
#include "handler.hpp"
#include "job_handler.hpp"
#include "metrics.hpp"
#include "test_only_components.hpp"
#include "ui_replay_handler.hpp"

#include <userver/clients/dns/component.hpp>
#include <userver/clients/http/component_list.hpp>
#include <userver/components/component_list.hpp>
#include <userver/components/fs_cache.hpp>
#include <userver/components/minimal_server_component_list.hpp>
#include <userver/components/process_starter.hpp>
#include <userver/congestion_control/component.hpp>
#include <userver/engine/task_processors_load_monitor.hpp>
#include <userver/server/handlers/http_handler_static.hpp>
#include <userver/server/handlers/server_monitor.hpp>
#include <userver/server/middlewares/http_middleware_base.hpp>
#include <userver/storages/postgres/component.hpp>
#include <userver/storages/secdist/component.hpp>
#include <userver/storages/secdist/provider_component.hpp>
#include <userver/testsuite/testsuite_support.hpp>
#include <userver/utils/daemon_run.hpp>

namespace ws {
namespace us = userver;
namespace eng = us::engine;
namespace httpc = us::clients::http;
} // namespace ws

using namespace ws;

int main(int argc, char *argv[])
{
    auto component_list =
        us::components::MinimalServerComponentList()
            .Append<us::clients::dns::Component>()
            .AppendComponentList(httpc::ComponentList())
            .Append<us::components::Secdist>()
            .Append<us::components::DefaultSecdistProvider>()
            .Append<us::components::ProcessStarter>()
            .Append<us::components::TestsuiteSupport>()
            .Append<us::components::Postgres>("capture_meta_db")
            .Append<us::components::Postgres>("shared_state_db")
            .Append<us::congestion_control::Component>()
            .Append<eng::TaskProcessorsLoadMonitor>()
            .Append<ws::AccessPolicyStore>()
            .Append<ws::Config>()
            .Append<ws::Metrics>()
            .Append<ws::ClientIpRatelimiter>()
            .Append<ws::Crud>()
            .Append<us::server::middlewares::SimpleHttpMiddlewareFactory<
                ws::ExceptionHandlingMiddleware>>()
            .Append<ws::ByPrefixHandler>()
            .Append<ws::CaptureByLinkHandler>()
            .Append<ws::JobHandler>()
            .Append<ws::DenyPrefixAndPurgeHandler>()
            .Append<ws::AccessPolicyCheckHandler>()
            .Append<ws::AllowlistCheckHandler>()
            .Append<ws::AllowlistAddHandler>()
            .Append<ws::AllowlistRemoveHandler>()
            .Append<ws::ByIdHandler>()
            .Append<ws::DocsHandler>()
            .Append<ws::DocsHandler>("docs_admin")
            .Append<ws::UiReplayHandler>()
            .Append<us::components::FsCache>("rapidoc_assets_cache")
            .Append<us::components::FsCache>("openapi_public_cache")
            .Append<us::components::FsCache>("openapi_common_cache")
            .Append<us::components::FsCache>("openapi_admin_cache")
            .Append<us::components::FsCache>("web_ui_cache")
            .Append<us::components::FsCache>("web_ui_vendor_cache")
            .Append<us::server::handlers::HttpHandlerStatic>("rapidoc_assets")
            .Append<us::server::handlers::HttpHandlerStatic>("rapidoc_assets_admin")
            .Append<us::server::handlers::HttpHandlerStatic>("openapi_public_static")
            .Append<us::server::handlers::HttpHandlerStatic>("openapi_common_static")
            .Append<us::server::handlers::HttpHandlerStatic>("openapi_admin_static")
            .Append<us::server::handlers::HttpHandlerStatic>("openapi_admin_common_static")
            .Append<us::server::handlers::HttpHandlerStatic>("web_ui_static")
            .Append<us::server::handlers::HttpHandlerStatic>("web_ui_style_static")
            .Append<us::server::handlers::HttpHandlerStatic>("web_ui_index_static")
            .Append<us::server::handlers::HttpHandlerStatic>("web_ui_root_static")
            .Append<us::server::handlers::ServerMonitor>();
    ws::AppendTestOnlyComponents(component_list);
    return us::utils::DaemonMain(argc, argv, component_list);
}
