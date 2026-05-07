#pragma once

#include "crawler/browser_page_lifecycle.hpp"
#include "crawler/limits.hpp"
#include "expected.hpp"
#include "integers.hpp"
#include "text.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <userver/clients/dns/resolver_fwd.hpp>
#include <userver/engine/deadline.hpp>
#include <userver/engine/subprocess/process_starter.hpp>
#include <userver/engine/task/task_processor_fwd.hpp>

namespace ws::crawler {

namespace us = userver;
namespace eng = us::engine;
} // namespace ws::crawler

namespace ws {
class Metrics;
} // namespace ws

namespace ws::crawler {

class CdpClient;
class CdpSession;

struct [[nodiscard]] BrowserSessionConfig final {
    usize url_bytes_max{0};
    i64 proxy_down_bytes_max{0};
    std::string browser_runs_root_;
    std::string cgroup_root_path_;
    std::optional<CgroupLimits> cgroup_limits_;
    std::string local_fixture_trust_db_source_path;
    std::chrono::milliseconds devtools_startup_timeout;
    std::chrono::milliseconds cdp_handshake_timeout;
    std::chrono::milliseconds cdp_command_timeout;
    std::chrono::milliseconds devtools_poll_interval;
    std::chrono::milliseconds browser_stop_timeout;
    i64 cdp_max_remote_payload_bytes{0};
    bool proxy_require_auth;
    bool enable_local_fixture_rewrite;
    std::vector<u16> testsuite_loopback_ports;
    std::string cgroup_name_prefix;
    Metrics *metrics;
};

class [[nodiscard]] BrowserSession final {
public:
    [[nodiscard]] static Expected<std::unique_ptr<BrowserSession>, String> Create(
        us::clients::dns::Resolver &dns_resolver, eng::subprocess::ProcessStarter &process_starter,
        eng::TaskProcessor &fs_task_processor, BrowserSessionConfig config
    );
    ~BrowserSession();

    BrowserSession(const BrowserSession &) = delete;
    BrowserSession(BrowserSession &&) = delete;
    BrowserSession &operator=(const BrowserSession &) = delete;
    BrowserSession &operator=(BrowserSession &&) = delete;

    [[nodiscard]] Expected<std::unique_ptr<CdpClient>, String>
    ConnectCdp(eng::Deadline overall_deadline) const;
    [[nodiscard]] std::pair<std::string, std::string> DrainBrowserLogs() const;
    void MarkPhase(std::string_view phase) const;
    [[nodiscard]] std::string CurrentStartLogs() const;
    [[nodiscard]] String BuildErrorDetail(const String &message);
    [[nodiscard]] i64 ProxyDownBytes() const noexcept;
    [[nodiscard]] const std::string &RunId() const noexcept;
    [[nodiscard]] std::optional<String> ProxyErrorReason() const noexcept;

private:
    BrowserSession(
        us::clients::dns::Resolver &dns_resolver, eng::subprocess::ProcessStarter &process_starter,
        eng::TaskProcessor &fs_task_processor, BrowserSessionConfig config
    );

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class [[nodiscard]] BrowserPageSession final {
public:
    explicit BrowserPageSession(CdpClient &cdp_client);

    BrowserPageSession(const BrowserPageSession &) = delete;
    BrowserPageSession(BrowserPageSession &&) = delete;
    BrowserPageSession &operator=(const BrowserPageSession &) = delete;
    BrowserPageSession &operator=(BrowserPageSession &&) = delete;

    [[nodiscard]] Expected<void, String> CreateBrowserContext();
    [[nodiscard]] Expected<void, String> CreateBlankTarget();
    [[nodiscard]] Expected<void, String> AttachToTarget();
    [[nodiscard]] Expected<void, String>
    AttachFreshTarget(const std::function<void(std::string_view)> &mark_phase);
    [[nodiscard]] Expected<void, String>
    EnableBaseDomains(const std::function<void(std::string_view)> &mark_phase);
    [[nodiscard]] Expected<void, String>
    Stop(const std::function<void(std::string_view)> &mark_phase);
    [[nodiscard]] Expected<void, String> Detach();
    [[nodiscard]] Expected<void, String> DisposeBrowserContext();
    [[nodiscard]] Expected<void, String> Stop();
    [[nodiscard]] const String &BrowserContextId() const;
    [[nodiscard]] CdpSession &GetSession() const;
    [[nodiscard]] const String &TargetId() const;
    [[nodiscard]] const String &SessionId() const;

private:
    CdpClient &cdp_client_;
    BrowserPageSessionLifecycle lifecycle_;
    std::unique_ptr<CdpSession> cdp_session_;
    std::optional<String> browser_context_id_;
    std::optional<String> target_id_;
    std::optional<String> session_id_;
};

[[nodiscard]] std::string BuildBrowserRunsRoot(std::string state_dir);
[[nodiscard]] std::string ResolveDelegatedCgroupRootPath(eng::TaskProcessor &fs_task_processor);
[[nodiscard]] std::string LocalFixtureTrustDbSourcePath(std::string_view state_dir);

} // namespace ws::crawler
