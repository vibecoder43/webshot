#include "include/webshot_by_id_handler.hpp"
#include "include/webshot_crud.hpp"
#include "include/webshot_handler.hpp"

#include <userver/clients/dns/component.hpp>
#include <userver/components/minimal_server_component_list.hpp>
#include <userver/storages/postgres/component.hpp>
#include <userver/testsuite/testsuite_support.hpp>
#include <userver/utils/daemon_run.hpp>

namespace us = userver;
int main(int argc, char *argv[])
{
    auto component_list = us::components::MinimalServerComponentList()
                              .Append<us::clients::dns::Component>()
                              .Append<us::components::TestsuiteSupport>()
                              .Append<us::components::Postgres>("webshot-db")
                              .Append<v1::WebshotCrud>()
                              .Append<v1::WebshotHandler>()
                              .Append<v1::WebshotById>();
    return us::utils::DaemonMain(argc, argv, component_list);
}
