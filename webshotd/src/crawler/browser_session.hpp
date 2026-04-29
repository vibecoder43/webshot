#pragma once

#include "crawler/browser_page_lifecycle.hpp"
#include "crawler/limits.hpp"
#include "expected.hpp"
#include "integers.hpp"
#include "text.hpp"
#include "userver_namespaces.hpp"

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

namespace v1::crawler {

class CdpClient;
class CdpSession;

struct [[nodiscard]] BrowserSessionConfig final {
    usize urlBytesMax{0};
    i64 proxyDownBytesMax{0};
    std::string browserRunsRoot;
    std::string cgroupRootPath;
    std::optional<CgroupLimits> cgroupLimits;
    std::string localFixtureTrustDbSourcePath;
    std::chrono::milliseconds devtoolsStartupTimeout;
    std::chrono::milliseconds cdpHandshakeTimeout;
    std::chrono::milliseconds cdpCommandTimeout;
    std::chrono::milliseconds devtoolsPollInterval;
    std::chrono::milliseconds browserStopTimeout;
    i64 cdpMaxRemotePayloadBytes{0};
    bool proxyRequireAuth;
    bool enableLocalFixtureRewrite;
    std::vector<u16> testsuiteLoopbackPorts;
    std::string cgroupNamePrefix;
};

class [[nodiscard]] BrowserSession final {
public:
    BrowserSession(
        us::clients::dns::Resolver &dnsResolver, eng::subprocess::ProcessStarter &processStarter,
        eng::TaskProcessor &fsTaskProcessor, BrowserSessionConfig config
    );
    ~BrowserSession();

    BrowserSession(const BrowserSession &) = delete;
    BrowserSession(BrowserSession &&) = delete;
    BrowserSession &operator=(const BrowserSession &) = delete;
    BrowserSession &operator=(BrowserSession &&) = delete;

    [[nodiscard]] Expected<void, String> launch();
    [[nodiscard]] Expected<std::unique_ptr<CdpClient>, String>
    connectCdp(eng::Deadline overallDeadline) const;
    [[nodiscard]] std::pair<std::string, std::string> drainBrowserLogs() const;
    void markPhase(std::string_view phase) const;
    [[nodiscard]] std::string currentLaunchLogs() const;
    [[nodiscard]] String buildFailureDetail(const String &message);
    void close();
    [[nodiscard]] i64 proxyDownBytes() const noexcept;
    [[nodiscard]] const std::string &runId() const noexcept;
    [[nodiscard]] std::optional<String> proxyFailureReason() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

class [[nodiscard]] BrowserPageSession final {
public:
    explicit BrowserPageSession(CdpClient &cdpClient);

    BrowserPageSession(const BrowserPageSession &) = delete;
    BrowserPageSession(BrowserPageSession &&) = delete;
    BrowserPageSession &operator=(const BrowserPageSession &) = delete;
    BrowserPageSession &operator=(BrowserPageSession &&) = delete;

    [[nodiscard]] Expected<void, String> createBrowserContext();
    [[nodiscard]] Expected<void, String> createBlankTarget();
    [[nodiscard]] Expected<void, String> attachToTarget();
    [[nodiscard]] Expected<void, String>
    attachFreshTarget(const std::function<void(std::string_view)> &markPhase);
    [[nodiscard]] Expected<void, String>
    enableBaseDomains(const std::function<void(std::string_view)> &markPhase);
    [[nodiscard]] Expected<void, String>
    close(const std::function<void(std::string_view)> &markPhase);
    [[nodiscard]] Expected<void, String> detach();
    [[nodiscard]] Expected<void, String> disposeBrowserContext();
    [[nodiscard]] Expected<void, String> close();
    [[nodiscard]] const String &browserContextId() const;
    [[nodiscard]] CdpSession &cdpSession() const;
    [[nodiscard]] const String &targetId() const;
    [[nodiscard]] const String &sessionId() const;

private:
    CdpClient &cdpClient;
    BrowserPageSessionLifecycle lifecycle;
    std::unique_ptr<CdpSession> cdpSessionValue;
    std::optional<String> browserContextIdValue;
    std::optional<String> targetIdValue;
    std::optional<String> sessionIdValue;
};

[[nodiscard]] std::string buildBrowserRunsRoot(std::string stateDir);
[[nodiscard]] std::string resolveDelegatedCgroupRootPath(eng::TaskProcessor &fsTaskProcessor);
[[nodiscard]] std::string localFixtureTrustDbSourcePath(std::string_view stateDir);

} // namespace v1::crawler
