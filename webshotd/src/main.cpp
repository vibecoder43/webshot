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
#include "handler.hpp"
#include "integers.hpp"
#include "job_handler.hpp"

#include <cstdlib>
#include <cxxabi.h>
#include <exception>
#include <string_view>
#include <unistd.h>

#include <userver/clients/dns/component.hpp>
#include <userver/clients/http/component_list.hpp>
#include <userver/components/fs_cache.hpp>
#include <userver/components/minimal_server_component_list.hpp>
#include <userver/components/process_starter.hpp>
#include <userver/congestion_control/component.hpp>
#include <userver/server/handlers/http_handler_static.hpp>
#include <userver/server/handlers/server_monitor.hpp>
#include <userver/storages/postgres/component.hpp>
#include <userver/storages/secdist/component.hpp>
#include <userver/storages/secdist/provider_component.hpp>
#include <userver/testsuite/testsuite_support.hpp>
#include <userver/utils/daemon_run.hpp>

namespace us = userver;

namespace {

void writeStderr(std::string_view text) noexcept
{
    while (!text.empty()) {
        const auto written = ::write(STDERR_FILENO, text.data(), text.size());
        if (written <= 0)
            return;
        text.remove_prefix(numericCast<size_t>(written));
    }
}

[[noreturn]] void terminateHandler() noexcept
{
    writeStderr("webshotd terminate handler invoked\n");
    if (const auto *type = abi::__cxa_current_exception_type()) {
        writeStderr("active exception type: ");
        writeStderr(type->name());
        writeStderr("\n");
    } else {
        writeStderr("active exception type: none\n");
    }
    std::abort();
}

} // namespace

int main(int argc, char *argv[])
{
    std::set_terminate(terminateHandler);

    auto componentList = us::components::MinimalServerComponentList()
                             .Append<us::clients::dns::Component>()
                             .AppendComponentList(us::clients::http::ComponentList())
                             .Append<us::components::TestsuiteSupport>()
                             .Append<us::components::Secdist>()
                             .Append<us::components::DefaultSecdistProvider>()
                             .Append<us::components::ProcessStarter>()
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
                             .Append<us::components::FsCache>("rapidoc_assets_cache")
                             .Append<us::components::FsCache>("openapi_cache")
                             .Append<us::server::handlers::HttpHandlerStatic>("rapidoc_assets")
                             .Append<us::server::handlers::HttpHandlerStatic>("openapi_static")
                             .Append<us::server::handlers::ServerMonitor>();
    return us::utils::DaemonMain(argc, argv, componentList);
}
