#include "crawler/runner.hpp"

#include "crawler/artifacts.hpp"
#include "crawler/browser_sandbox.hpp"
#include "crawler/cdp_client.hpp"
#include "integers.hpp"
#include "schema/cdp.hpp"
#include "text.hpp"
#include "url.hpp"

#include <algorithm>
#include <boost/filesystem/path.hpp>
#include <cctype>
#include <chrono>
#include <csignal>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <userver/clients/http/response.hpp>
#include <userver/crypto/base64.hpp>
#include <userver/engine/sleep.hpp>
#include <userver/engine/subprocess/process_starter.hpp>
#include <userver/formats/json.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/fs/blocking/read.hpp>
#include <userver/fs/blocking/temp_directory.hpp>
#include <userver/fs/blocking/write.hpp>
#include <userver/utils/assert.hpp>
#include <userver/utils/datetime.hpp>

namespace us = userver;
namespace chrono = std::chrono;
namespace json = us::formats::json;

using namespace text::literals;

namespace v1 {
namespace {

constexpr auto kPostLoadDelayMs = 1000_i64;
constexpr auto kNetIdleWaitMs = 0_i64;
constexpr auto kPageExtraDelayMs = 0_i64;
constexpr auto kBehaviorTimeoutMs = 1000_i64;
constexpr auto kMaxBodyBytes = 50_i64 * 1024_i64 * 1024_i64;
constexpr std::string_view kLang = "en";
constexpr std::string_view kBwrapBin = "bwrap";
constexpr std::string_view kBashBin = "bash";
constexpr std::string_view kSocatBin = "socat";
constexpr std::string_view kBrowserSandboxScriptPath = WEBSHOT_BROWSER_SANDBOX_SCRIPT_PATH;
constexpr size_t kMaxLogBytes = 64 * 1024;

[[nodiscard]] std::string lowerAscii(std::string_view text)
{
    std::string out(text);
    std::transform(std::begin(out), std::end(out), std::begin(out), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

[[nodiscard]] String currentTimestamp()
{
    return String::fromBytesThrow(
        us::utils::datetime::UtcTimestring(
            us::utils::datetime::Now(), us::utils::datetime::kRfc3339Format
        )
    );
}

[[nodiscard]] std::unordered_map<std::string, std::string>
normalizeHeaders(const dto::CdpHeaders &headers)
{
    std::unordered_map<std::string, std::string> out;
    for (const auto &[name, value] : headers.extra)
        out.emplace(lowerAscii(name), value);
    return out;
}

[[nodiscard]] std::unordered_map<std::string, std::string>
normalizeHeadersOrEmpty(const std::optional<dto::CdpHeaders> &headers)
{
    if (!headers)
        return {};
    return normalizeHeaders(*headers);
}

[[nodiscard]] String extractWebsocketPath(const String &websocketUrlText)
{
    try {
        return Url::fromTextThrow(websocketUrlText).pathWithSearch();
    } catch (const std::exception &) {
        throw std::runtime_error("invalid websocket debugger url");
    }
}

class CaptureFailure final : public std::runtime_error {
public:
    CaptureFailure(std::string messageIn, std::optional<crawler::SeedPageProbe> seedProbeIn)
        : std::runtime_error(std::move(messageIn)), seedProbe(std::move(seedProbeIn))
    {
    }

    std::optional<crawler::SeedPageProbe> seedProbe;
};

[[nodiscard]] chrono::milliseconds remainingBudget(chrono::steady_clock::time_point deadline)
{
    const auto now = us::utils::datetime::SteadyNow();
    if (deadline <= now)
        return chrono::milliseconds(0);
    return chrono::duration_cast<chrono::milliseconds>(deadline - now);
}

[[nodiscard]] chrono::milliseconds
remainingBudgetOrThrow(chrono::steady_clock::time_point deadline, std::string_view timeoutMessage)
{
    const auto remaining = remainingBudget(deadline);
    if (remaining <= chrono::milliseconds(0))
        throw std::runtime_error(std::string(timeoutMessage));
    return remaining;
}

void sleepWithinBudget(
    chrono::steady_clock::time_point deadline, chrono::milliseconds delay,
    std::string_view timeoutMessage
)
{
    if (delay <= chrono::milliseconds(0))
        return;

    const auto remaining = remainingBudgetOrThrow(deadline, timeoutMessage);
    const auto sleepFor = std::min(delay, remaining);
    us::engine::SleepFor(sleepFor);
    if (sleepFor != delay)
        throw std::runtime_error(std::string(timeoutMessage));
}

[[nodiscard]] const dto::CdpEventMessage::Params &requireEventParams(const crawler::CdpEvent &event)
{
    if (!event.params)
        throw std::runtime_error(fmt::format("{} missing params", event.method.view()));
    return *event.params;
}

struct [[nodiscard]] BrowserPaths {
    us::fs::blocking::TempDirectory tempDir;
    std::string rootDir;
    std::string userDataDir;
    std::string xdgConfigHome;
    std::string xdgCacheHome;
    std::string crashpadDir;
    std::string proxySocketPath;
    std::string cdpSocketPath;
    std::string netlogPath;
    std::string stdoutLogPath;
    std::string stderrLogPath;
    std::string devNullPath;
};

[[nodiscard]] BrowserPaths createBrowserPaths()
{
    BrowserPaths paths;
    paths.tempDir = us::fs::blocking::TempDirectory::Create("/tmp", "webshotd-browser-");

    const auto rootDir = boost::filesystem::path(paths.tempDir.GetPath());
    paths.rootDir = rootDir.string();
    paths.userDataDir = (rootDir / "profile").string();
    paths.xdgConfigHome = (rootDir / "xdg-config").string();
    paths.xdgCacheHome = (rootDir / "xdg-cache").string();
    paths.crashpadDir = (rootDir / "crashpad").string();
    paths.proxySocketPath = (rootDir / "proxy.sock").string();
    paths.cdpSocketPath = (rootDir / "cdp.sock").string();
    paths.netlogPath = (rootDir / "netlog.json").string();
    paths.stdoutLogPath = (rootDir / "stdout.log").string();
    paths.stderrLogPath = (rootDir / "stderr.log").string();
    paths.devNullPath = (rootDir / "devnull").string();

    us::fs::blocking::CreateDirectories(paths.userDataDir);
    us::fs::blocking::CreateDirectories(paths.xdgConfigHome);
    us::fs::blocking::CreateDirectories(paths.xdgCacheHome);
    us::fs::blocking::CreateDirectories(paths.crashpadDir);
    us::fs::blocking::RewriteFileContents(paths.devNullPath, {});
    return paths;
}

struct [[nodiscard]] ProxyUrlParts {
    String host;
    int port;
};

[[nodiscard]] ProxyUrlParts parseProxyUrlParts(const String &proxyServer)
{
    const auto url = [&proxyServer] {
        try {
            return Url::fromTextThrow(proxyServer);
        } catch (const std::exception &) {
            throw std::runtime_error("invalid proxy url");
        }
    }();
    if (!url.isHttp())
        throw std::runtime_error("unsupported proxy protocol");
    if (!url.hasHostname())
        throw std::runtime_error("proxy host is required");
    if (!url.hasPort())
        throw std::runtime_error("proxy port is required");

    ProxyUrlParts proxyUrlParts;
    proxyUrlParts.host = url.hostname();
    proxyUrlParts.port = std::stoi(std::string(url.port().view()));
    return proxyUrlParts;
}

void truncateLogBuffer(std::string &value)
{
    if (value.size() <= kMaxLogBytes)
        return;
    value.erase(0, value.size() - kMaxLogBytes);
}

[[nodiscard]] std::string readLogTail(const std::string &path)
{
    if (!us::fs::blocking::FileExists(path))
        return {};
    auto value = us::fs::blocking::ReadFileContents(path);
    truncateLogBuffer(value);
    return value;
}

[[nodiscard]] us::engine::subprocess::ChildProcess spawnProcess(
    us::engine::subprocess::ProcessStarter &processStarter, const std::string &executablePath,
    const std::vector<std::string> &args, const std::string &stdoutPath,
    const std::string &stderrPath
)
{
    us::engine::subprocess::ExecOptions options;
    options.use_path = true;
    options.stdout_file = stdoutPath;
    options.stderr_file = stderrPath;
    return processStarter.Exec(executablePath, args, std::move(options));
}

[[nodiscard]] us::engine::subprocess::ChildProcess spawnProxyBridge(
    us::engine::subprocess::ProcessStarter &processStarter, const BrowserPaths &paths,
    const ProxyUrlParts &proxyUrlParts
)
{
    auto args = std::vector<std::string>{
        "UNIX-LISTEN:" + paths.proxySocketPath + ",fork,unlink-early",
        "TCP-CONNECT:" + std::string(proxyUrlParts.host.view()) + ":" +
            std::to_string(proxyUrlParts.port),
    };
    return spawnProcess(
        processStarter, std::string(kSocatBin), args, paths.devNullPath, paths.devNullPath
    );
}

[[nodiscard]] us::engine::subprocess::ChildProcess spawnSandboxedBrowser(
    us::engine::subprocess::ProcessStarter &processStarter, const BrowserPaths &paths,
    const std::string &browserBin, const String &geometry
)
{
    crawler::BrowserSandboxOptions sandboxOptions;
    sandboxOptions.browserBin = browserBin;
    sandboxOptions.userDataDir = paths.userDataDir;
    sandboxOptions.proxyUpstreamSocket = paths.proxySocketPath;
    sandboxOptions.cdpSocket = paths.cdpSocketPath;
    sandboxOptions.netlogPath = paths.netlogPath;
    sandboxOptions.geometry = geometry;

    auto chromiumArgs = crawler::buildChromiumArgs(sandboxOptions);
    auto args = std::vector<std::string>{
        "--die-with-parent",
        "--unshare-user",
        "--unshare-net",
        "--unshare-pid",
        "--unshare-ipc",
        "--proc",
        "/proc",
        "--dev",
        "/dev",
        "--ro-bind",
        "/",
        "/",
        "--tmpfs",
        "/tmp",
        "--chmod",
        "1777",
        "/tmp",
        "--bind",
        paths.rootDir,
        paths.rootDir,
        "--bind",
        paths.devNullPath,
        "/dev/null",
        "--setenv",
        "HOME",
        paths.rootDir,
        "--setenv",
        "TMPDIR",
        "/tmp",
        "--setenv",
        "XDG_CONFIG_HOME",
        paths.xdgConfigHome,
        "--setenv",
        "XDG_CACHE_HOME",
        paths.xdgCacheHome,
        "--setenv",
        "BREAKPAD_DUMP_LOCATION",
        paths.crashpadDir,
        "--chdir",
        paths.rootDir,
        std::string(kBashBin),
        std::string(kBrowserSandboxScriptPath),
        paths.proxySocketPath,
        paths.cdpSocketPath,
        "--",
        browserBin,
    };
    args.insert(std::end(args), std::begin(chromiumArgs), std::end(chromiumArgs));
    return spawnProcess(
        processStarter, std::string(kBwrapBin), args, paths.stdoutLogPath, paths.stderrLogPath
    );
}

template <typename Process> void stopProcess(Process &process, chrono::milliseconds timeout)
{
    if (!process)
        return;

    if (!process->WaitFor(chrono::milliseconds(0))) {
        process->SendSignal(SIGTERM);
        if (!process->WaitFor(timeout)) {
            process->SendSignal(SIGKILL);
            process->Wait();
        }
    } else {
        process->Wait();
    }
    process.reset();
}

class [[nodiscard]] BrowserInstance final {
public:
    BrowserInstance(
        us::clients::http::Client &httpClient,
        us::engine::subprocess::ProcessStarter &processStarter, std::string browserBin,
        String geometry, String proxyServer
    )
        : httpClient(httpClient), processStarter(processStarter), browserBin(std::move(browserBin)),
          geometry(std::move(geometry)), proxyServer(std::move(proxyServer))
    {
    }

    ~BrowserInstance() { close(); }

    BrowserInstance(const BrowserInstance &) = delete;
    BrowserInstance(BrowserInstance &&) = delete;
    BrowserInstance &operator=(const BrowserInstance &) = delete;
    BrowserInstance &operator=(BrowserInstance &&) = delete;

    void launch()
    {
        paths = createBrowserPaths();
        proxyBridge.emplace(
            spawnProxyBridge(processStarter, paths, parseProxyUrlParts(proxyServer))
        );
        process.emplace(spawnSandboxedBrowser(processStarter, paths, browserBin, geometry));
        websocketPath = waitForDevtoolsPath();
    }

    [[nodiscard]] crawler::CdpClient connectCdp() const
    {
        return {httpClient, paths.cdpSocketPath, websocketPath};
    }

    [[nodiscard]] std::pair<std::string, std::string> drainLogs() const
    {
        return {readLogTail(paths.stdoutLogPath), readLogTail(paths.stderrLogPath)};
    }

    void close()
    {
        if (closed)
            return;
        closed = true;

        stopProcess(process, chrono::milliseconds(1500));
        stopProcess(proxyBridge, chrono::milliseconds(500));
        if (!paths.rootDir.empty())
            std::move(paths.tempDir).Remove();
    }

private:
    [[nodiscard]] String waitForDevtoolsPath()
    {
        const auto deadline = us::utils::datetime::SteadyNow() + chrono::seconds(8);
        while (us::utils::datetime::SteadyNow() < deadline) {
            if (process && process->WaitFor(chrono::milliseconds(0))) {
                const auto logs = drainLogs();
                throw std::runtime_error(
                    fmt::format(
                        "chromium exited before exposing devtools (stderr={})",
                        logs.second.empty() ? "empty" : logs.second
                    )
                );
            }
            try {
                const auto response = httpClient.CreateRequest()
                                          .get("http://localhost/json/version")
                                          .unix_socket_path(paths.cdpSocketPath.c_str())
                                          .follow_redirects(false)
                                          .perform();
                if (response->status_code() == us::clients::http::Status::kOk) {
                    const auto body =
                        json::FromString(response->body_view()).As<dto::DevtoolsVersionResponse>();
                    return extractWebsocketPath(String::fromBytesThrow(body.webSocketDebuggerUrl));
                }
            } catch (const std::exception &) {
            }
            us::engine::SleepFor(chrono::milliseconds(100));
        }
        throw std::runtime_error("timed out waiting for chromium devtools url");
    }

    us::clients::http::Client &httpClient;
    us::engine::subprocess::ProcessStarter &processStarter;
    std::string browserBin;
    String geometry;
    String proxyServer;
    BrowserPaths paths;
    std::optional<us::engine::subprocess::ChildProcess> proxyBridge;
    std::optional<us::engine::subprocess::ChildProcess> process;
    String websocketPath;
    bool closed{false};
};

struct [[nodiscard]] RetainedBodyBudget {
    i64 maxBytes;
    i64 retainedBytes;
};

[[nodiscard]] std::string retainBody(const std::string &body, RetainedBodyBudget &budget)
{
    const auto nextRetainedBytes = budget.retainedBytes + i64(body.size());
    if (nextRetainedBytes > budget.maxBytes) {
        throw std::runtime_error(
            fmt::format(
                "retained body bytes {} exceeded size limit {}", toNative(nextRetainedBytes),
                toNative(budget.maxBytes)
            )
        );
    }
    budget.retainedBytes = nextRetainedBytes;
    return body;
}

[[nodiscard]] bool responseCanHaveBody(const String &method, i64 statusCode)
{
    if (method.view() == "HEAD")
        return false;
    return (statusCode < 100_i64 || statusCode >= 200_i64) && statusCode != 204_i64 &&
           statusCode != 304_i64;
}

struct [[nodiscard]] CapturedResourceState {
    String resourceUrl;
    String method;
    std::optional<i64> statusCode;
    std::optional<String> statusMessage;
    std::optional<std::unordered_map<std::string, std::string>> headers;
    std::optional<String> timestamp;
    bool loaded{false};
};

class [[nodiscard]] PageTracker final {
public:
    PageTracker(String sessionIdIn, String targetIdIn)
        : sessionId(std::move(sessionIdIn)), targetId(std::move(targetIdIn))
    {
    }

    void handleEvent(const crawler::CdpEvent &event)
    {
        if (event.method.view() == "Target.targetCrashed") {
            if (event.params) {
                const auto crashed = event.params->extra.As<dto::TargetTargetCrashedEvent>();
                if (crashed.targetId && targetId.view() == *crashed.targetId)
                    mainRequestFailure = "page target crashed"_t;
            }
            return;
        }

        if (!event.sessionId || event.sessionId->view() != sessionId.view())
            return;

        if (event.method.view() == "Page.loadEventFired") {
            loaded = true;
            return;
        }
        if (event.method.view() == "Network.requestWillBeSent") {
            handleRequestWillBeSent(requireEventParams(event));
            return;
        }
        if (event.method.view() == "Network.responseReceived") {
            handleResponseReceived(requireEventParams(event));
            return;
        }
        if (event.method.view() == "Network.loadingFinished") {
            handleLoadingFinished(requireEventParams(event));
            return;
        }
        if (event.method.view() == "Network.loadingFailed") {
            handleLoadingFailed(requireEventParams(event));
        }
    }

    void waitForLoad(crawler::CdpClient &cdp, chrono::milliseconds timeout)
    {
        cdp.waitUntil(
            [this]() { return loaded || mainRequestFailure.has_value(); }, timeout,
            "timed out waiting for page load"
        );
        if (mainRequestFailure)
            throw std::runtime_error(std::string(mainRequestFailure->view()));
    }

    void waitForMainDocument(crawler::CdpClient &cdp, chrono::milliseconds timeout)
    {
        cdp.waitUntil(
            [this]() {
                return mainRequestFailure.has_value() ||
                       (mainResponse.has_value() && mainRequestLoaded);
            },
            timeout, "timed out waiting for main document response"
        );
        if (mainRequestFailure)
            throw std::runtime_error(std::string(mainRequestFailure->view()));
    }

    void
    waitForIdle(crawler::CdpClient &cdp, chrono::milliseconds idle, chrono::milliseconds timeout)
    {
        cdp.waitUntil(
            [this, idle]() {
                return inflight.empty() && us::utils::datetime::SteadyNow() - lastNetworkAt >= idle;
            },
            timeout, "timed out waiting for network idle"
        );
    }

    [[nodiscard]] std::optional<crawler::SeedPageProbe> currentSeedProbe() const
    {
        if (mainResponse) {
            return crawler::SeedPageProbe{
                toNative(mainResponse->statusCode),
                mainRequestLoaded && !mainRequestFailure ? std::optional<int64_t>{2}
                                                         : std::optional<int64_t>{0},
            };
        }

        if (mainRequestId || mainRequestFailure || loaded)
            return crawler::SeedPageProbe{0, 0};

        return {};
    }

    [[nodiscard]] std::string readBody(
        crawler::CdpClient &cdp, const String &sessionIdIn, RetainedBodyBudget &budget,
        const std::string &fallbackBody
    ) const
    {
        if (!mainRequestId)
            return retainBody(fallbackBody, budget);

        try {
            dto::NetworkGetResponseBodyParams params;
            params.requestId = std::string(mainRequestId->view());
            const auto body = cdp.send<dto::NetworkGetResponseBodyResult>(
                "Network.getResponseBody", params, sessionIdIn
            );
            return retainBody(
                body.base64Encoded ? us::crypto::base64::Base64Decode(body.body) : body.body, budget
            );
        } catch (const std::exception &) {
            return retainBody(fallbackBody, budget);
        }
    }

    [[nodiscard]] std::vector<crawler::CapturedResource> readResources(
        crawler::CdpClient &cdp, const String &sessionIdIn, RetainedBodyBudget &budget
    ) const
    {
        auto resources = redirectedResources;

        for (const auto &[requestId, resource] : resourcesByRequestId) {
            if (!resource.loaded || !resource.statusCode || !resource.statusMessage ||
                !resource.headers || !resource.timestamp) {
                continue;
            }

            if (!responseCanHaveBody(resource.method, *resource.statusCode)) {
                resources.push_back({
                    resource.resourceUrl,
                    resource.method,
                    *resource.statusCode,
                    *resource.statusMessage,
                    *resource.headers,
                    {},
                    *resource.timestamp,
                });
                continue;
            }

            try {
                dto::NetworkGetResponseBodyParams params;
                params.requestId = std::string(requestId.view());
                const auto bodyValue = cdp.send<dto::NetworkGetResponseBodyResult>(
                    "Network.getResponseBody", params, sessionIdIn
                );
                resources.push_back({
                    resource.resourceUrl,
                    resource.method,
                    *resource.statusCode,
                    *resource.statusMessage,
                    *resource.headers,
                    retainBody(
                        bodyValue.base64Encoded ? us::crypto::base64::Base64Decode(bodyValue.body)
                                                : bodyValue.body,
                        budget
                    ),
                    *resource.timestamp,
                });
            } catch (const std::exception &) {
            }
        }

        std::sort(
            std::begin(resources), std::end(resources),
            [](const auto &left, const auto &right) { return left.timestamp < right.timestamp; }
        );
        return resources;
    }

    [[nodiscard]] crawler::CapturedExchange buildExchange(
        String finalUrl, std::optional<String> title, std::string body,
        std::vector<crawler::CapturedResource> resources
    ) const
    {
        crawler::CapturedExchange exchange;
        exchange.finalUrl = std::move(finalUrl);
        exchange.statusCode = 0_i64;
        exchange.timestamp = currentTimestamp();
        if (mainResponse) {
            exchange.statusCode = mainResponse->statusCode;
            exchange.statusMessage = mainResponse->statusMessage;
            exchange.headers = mainResponse->headers;
            exchange.timestamp = mainResponse->timestamp;
        }
        exchange.redirectChain = redirectChain.empty() ? std::vector<String>{exchange.finalUrl}
                                                       : redirectChain;
        exchange.body = std::move(body);
        exchange.mainDocumentRedirects = mainDocumentRedirects;
        exchange.resources = std::move(resources);
        exchange.title = std::move(title);
        return exchange;
    }

    std::optional<String> mainFrameId;

private:
    struct [[nodiscard]] MainResponse {
        i64 statusCode;
        String statusMessage;
        std::unordered_map<std::string, std::string> headers;
        String timestamp;
    };

    void handleRequestWillBeSent(const dto::CdpEventMessage::Params &params)
    {
        const auto requestWillBeSent = params.extra.As<dto::NetworkRequestWillBeSentEvent>();
        if (requestWillBeSent.request.url.starts_with("data:"))
            return;

        inflight.insert(String::fromBytesThrow(requestWillBeSent.requestId));
        lastNetworkAt = us::utils::datetime::SteadyNow();

        if (requestWillBeSent.frameId && mainFrameId &&
            *requestWillBeSent.frameId == mainFrameId->view() && requestWillBeSent.type &&
            *requestWillBeSent.type == "Document") {
            if (!mainRequestId)
                mainRequestId = String::fromBytesThrow(requestWillBeSent.requestId);
            if (mainRequestId->view() != requestWillBeSent.requestId)
                return;
            recordMainDocumentRedirect(requestWillBeSent.redirectResponse);
            const auto urlText = String::fromBytesThrow(requestWillBeSent.request.url);
            if (redirectChain.empty() || redirectChain.back() != urlText)
                redirectChain.push_back(urlText);
            return;
        }

        const auto requestIdText = String::fromBytesThrow(requestWillBeSent.requestId);
        recordResourceRedirect(requestIdText, requestWillBeSent.redirectResponse);
        resourcesByRequestId.insert_or_assign(
            requestIdText, CapturedResourceState{
                               String::fromBytesThrow(requestWillBeSent.request.url),
                               String::fromBytesThrow(requestWillBeSent.request.method),
                               {},
                               {},
                               {},
                               {},
                               false,
                           }
        );
    }

    void handleResponseReceived(const dto::CdpEventMessage::Params &params)
    {
        const auto responseReceived = params.extra.As<dto::NetworkResponseReceivedEvent>();
        const auto requestIdText = String::fromBytesThrow(responseReceived.requestId);
        const auto headers = normalizeHeadersOrEmpty(responseReceived.response.headers);
        auto resourceIt = resourcesByRequestId.find(requestIdText);
        if (resourceIt != resourcesByRequestId.end()) {
            if (responseReceived.response.url)
                resourceIt->second.resourceUrl = String::fromBytesThrow(
                    *responseReceived.response.url
                );
            resourceIt->second.statusCode = responseReceived.response.status
                                                ? i64(*responseReceived.response.status)
                                                : 0_i64;
            resourceIt->second.statusMessage = String::fromBytesThrow(
                responseReceived.response.statusText.value_or("")
            );
            resourceIt->second.headers = headers;
            resourceIt->second.timestamp = currentTimestamp();
        }

        if (!mainRequestId || requestIdText != *mainRequestId)
            return;

        mainResponse = MainResponse{
            responseReceived.response.status ? i64(*responseReceived.response.status) : 0_i64,
            String::fromBytesThrow(responseReceived.response.statusText.value_or("")),
            headers,
            currentTimestamp(),
        };
    }

    void handleLoadingFinished(const dto::CdpEventMessage::Params &params)
    {
        const auto loadingFinished = params.extra.As<dto::NetworkLoadingFinishedEvent>();
        const auto requestIdText = String::fromBytesThrow(loadingFinished.requestId);
        inflight.erase(requestIdText);
        lastNetworkAt = us::utils::datetime::SteadyNow();
        if (auto it = resourcesByRequestId.find(requestIdText); it != resourcesByRequestId.end())
            it->second.loaded = true;
        if (mainRequestId && requestIdText == *mainRequestId)
            mainRequestLoaded = true;
    }

    void handleLoadingFailed(const dto::CdpEventMessage::Params &params)
    {
        const auto loadingFailed = params.extra.As<dto::NetworkLoadingFailedEvent>();
        const auto requestIdText = String::fromBytesThrow(loadingFailed.requestId);
        inflight.erase(requestIdText);
        lastNetworkAt = us::utils::datetime::SteadyNow();
        if (auto it = resourcesByRequestId.find(requestIdText); it != resourcesByRequestId.end()) {
            if (it->second.statusCode &&
                responseCanHaveBody(it->second.method, *it->second.statusCode)) {
                resourcesByRequestId.erase(it);
            } else {
                it->second.loaded = true;
            }
        }
        if (mainRequestId && requestIdText == *mainRequestId) {
            mainRequestFailure = String::fromBytesThrow(
                loadingFailed.errorText.value_or("main document request failed")
            );
        }
    }

    void recordMainDocumentRedirect(const std::optional<dto::NetworkResponse> &redirectResponse)
    {
        if (!redirectResponse || !redirectResponse->url || !redirectResponse->status)
            return;

        crawler::CapturedMainDocumentRedirect redirect;
        redirect.redirectUrl = String::fromBytesThrow(*redirectResponse->url);
        redirect.statusCode = i64(*redirectResponse->status);
        redirect.statusMessage = String::fromBytesThrow(redirectResponse->statusText.value_or(""));
        redirect.headers = normalizeHeadersOrEmpty(redirectResponse->headers);
        redirect.timestamp = currentTimestamp();

        if (!mainDocumentRedirects.empty()) {
            const auto &previous = mainDocumentRedirects.back();
            if (previous.redirectUrl == redirect.redirectUrl &&
                previous.statusCode == redirect.statusCode) {
                return;
            }
        }
        mainDocumentRedirects.push_back(std::move(redirect));
    }

    void recordResourceRedirect(
        const String &requestId, const std::optional<dto::NetworkResponse> &redirectResponse
    )
    {
        const auto resourceIt = resourcesByRequestId.find(requestId);
        if (resourceIt == resourcesByRequestId.end() || !redirectResponse ||
            !redirectResponse->status) {
            return;
        }

        redirectedResources.push_back({
            redirectResponse->url ? String::fromBytesThrow(*redirectResponse->url)
                                  : resourceIt->second.resourceUrl,
            resourceIt->second.method,
            i64(*redirectResponse->status),
            String::fromBytesThrow(redirectResponse->statusText.value_or("")),
            normalizeHeadersOrEmpty(redirectResponse->headers),
            {},
            currentTimestamp(),
        });
    }

    String sessionId;
    String targetId;
    std::unordered_map<String, CapturedResourceState> resourcesByRequestId;
    std::vector<crawler::CapturedResource> redirectedResources;
    std::vector<String> redirectChain;
    std::vector<crawler::CapturedMainDocumentRedirect> mainDocumentRedirects;
    std::unordered_set<String> inflight;
    std::optional<String> mainRequestId;
    std::optional<MainResponse> mainResponse;
    bool mainRequestLoaded{false};
    bool loaded{false};
    std::optional<String> mainRequestFailure;
    chrono::steady_clock::time_point lastNetworkAt{us::utils::datetime::SteadyNow()};
};

struct [[nodiscard]] DomState {
    String finalUrl;
    std::optional<String> title;
    std::string html;
};

[[nodiscard]] DomState readDomState(crawler::CdpClient &cdp, const String &sessionId)
{
    dto::RuntimeEvaluateParams params;
    params.expression =
        "(() => ({ finalUrl: location.href, title: document.title || undefined, html: "
        "document.documentElement ? document.documentElement.outerHTML : \"\" }))()";
    params.returnByValue = true;
    params.awaitPromise = false;

    const auto result = cdp.send<dto::RuntimeEvaluateDomStateResult>(
        "Runtime.evaluate", params, sessionId
    );
    const auto &value = result.result.value;
    std::optional<String> title;
    if (value.title)
        title = String::fromBytesThrow(*value.title);
    DomState out{
        String::fromBytesThrow(value.finalUrl),
        std::move(title),
        value.html,
    };
    return out;
}

void runSiteBehavior(crawler::CdpClient &cdp, const String &sessionId, chrono::milliseconds timeout)
{
    dto::RuntimeEvaluateParams params;
    params.expression = fmt::format(
        "(() => new Promise((resolve) => {{ const startedAt = Date.now(); const stepDelayMs = "
        "100; const maxSteps = Math.max(1, Math.floor({0} / stepDelayMs)); let steps = 0; const "
        "tick = () => {{ const root = document.scrollingElement || document.documentElement || "
        "document.body; if (!root) {{ resolve(true); return; }} const previous = root.scrollTop; "
        "root.scrollBy(0, Math.max(window.innerHeight, 600)); steps++; const exhausted = "
        "Date.now() - startedAt >= {0} || steps >= maxSteps; const stuck = root.scrollTop === "
        "previous; if (stuck || exhausted) {{ root.scrollTo(0, 0); resolve(true); return; }} "
        "setTimeout(tick, stepDelayMs); }}; tick(); }}))()",
        timeout.count()
    );
    params.awaitPromise = true;
    params.returnByValue = true;
    static_cast<void>(cdp.send<json::Value>("Runtime.evaluate", params, sessionId));
}

struct [[nodiscard]] CaptureResult {
    crawler::CapturedExchange exchange;
};

[[nodiscard]] CaptureResult captureViaProxy(
    us::clients::http::Client &httpClient, us::engine::subprocess::ProcessStarter &processStarter,
    const crawler::RunRequest &run, const std::string &browserBin, const String &geometry,
    const String &proxyServer
)
{
    const auto deadline = us::utils::datetime::SteadyNow() + toMilliseconds(run.jobTimeoutMs);
    auto browser = BrowserInstance(httpClient, processStarter, browserBin, geometry, proxyServer);
    browser.launch();

    try {
        auto cdp = browser.connectCdp();

        const auto browserContext = cdp.send<dto::TargetCreateBrowserContextResult>(
            "Target.createBrowserContext"
        );
        const auto browserContextId = String::fromBytesThrow(browserContext.browserContextId);

        dto::TargetCreateTargetParams targetParams;
        targetParams.url = "about:blank";
        targetParams.browserContextId = std::string(browserContextId.view());
        const auto target = cdp.send<dto::TargetCreateTargetResult>(
            "Target.createTarget", targetParams
        );
        const auto targetId = String::fromBytesThrow(target.targetId);

        dto::TargetAttachToTargetParams attachParams;
        attachParams.targetId = std::string(targetId.view());
        attachParams.flatten = true;
        const auto attached = cdp.send<dto::TargetAttachToTargetResult>(
            "Target.attachToTarget", attachParams
        );
        const auto sessionId = String::fromBytesThrow(attached.sessionId);

        PageTracker tracker(sessionId, targetId);
        const auto listenerId = cdp.addListener([&tracker](const crawler::CdpEvent &event) {
            tracker.handleEvent(event);
        });

        try {
            static_cast<void>(cdp.send<dto::CdpEmptyObject>("Page.enable", sessionId));
            static_cast<void>(cdp.send<dto::CdpEmptyObject>("Runtime.enable", sessionId));
            static_cast<void>(cdp.send<dto::CdpEmptyObject>("Network.enable", sessionId));

            dto::PageSetLifecycleEventsEnabledParams lifecycleParams;
            lifecycleParams.enabled = true;
            static_cast<void>(cdp.send<dto::CdpEmptyObject>(
                "Page.setLifecycleEventsEnabled", lifecycleParams, sessionId
            ));

            dto::NetworkSetCacheDisabledParams cacheParams;
            cacheParams.cacheDisabled = true;
            static_cast<void>(
                cdp.send<dto::CdpEmptyObject>("Network.setCacheDisabled", cacheParams, sessionId)
            );

            dto::NetworkSetBypassServiceWorkerParams serviceWorkerParams;
            serviceWorkerParams.bypass = true;
            static_cast<void>(cdp.send<dto::CdpEmptyObject>(
                "Network.setBypassServiceWorker", serviceWorkerParams, sessionId
            ));

            dto::NetworkSetExtraHTTPHeadersParams headerParams;
            headerParams.headers.extra.emplace("Accept-Language", std::string(kLang));
            static_cast<void>(cdp.send<dto::CdpEmptyObject>(
                "Network.setExtraHTTPHeaders", headerParams, sessionId
            ));

            const auto frameTree = cdp.send<dto::PageGetFrameTreeResult>(
                "Page.getFrameTree", sessionId
            );
            tracker.mainFrameId = String::fromBytesThrow(frameTree.frameTree.frame.id);

            dto::PageNavigateParams navigateParams;
            navigateParams.url = std::string(run.seedUrl.view());
            static_cast<void>(cdp.send<json::Value>("Page.navigate", navigateParams, sessionId));

            tracker.waitForLoad(
                cdp, remainingBudgetOrThrow(deadline, "timed out waiting for page load")
            );
            if (kPostLoadDelayMs > 0_i64)
                sleepWithinBudget(
                    deadline, toMilliseconds(kPostLoadDelayMs),
                    "timed out waiting for post-load delay"
                );
            if (kBehaviorTimeoutMs > 0_i64)
                runSiteBehavior(
                    cdp, sessionId,
                    std::min(
                        toMilliseconds(kBehaviorTimeoutMs),
                        remainingBudgetOrThrow(deadline, "timed out running site behavior")
                    )
                );
            if (kNetIdleWaitMs > 0_i64)
                tracker.waitForIdle(
                    cdp, toMilliseconds(kNetIdleWaitMs),
                    remainingBudgetOrThrow(deadline, "timed out waiting for network idle")
                );
            if (kPageExtraDelayMs > 0_i64)
                sleepWithinBudget(
                    deadline, toMilliseconds(kPageExtraDelayMs),
                    "timed out waiting for extra page delay"
                );
            tracker.waitForMainDocument(
                cdp,
                remainingBudgetOrThrow(deadline, "timed out waiting for main document response")
            );

            const auto domState = readDomState(cdp, sessionId);
            RetainedBodyBudget budget{kMaxBodyBytes, 0_i64};
            const auto fallbackBody = domState.html;
            const auto body = tracker.readBody(cdp, sessionId, budget, fallbackBody);
            auto resources = tracker.readResources(cdp, sessionId, budget);

            dto::TargetDetachFromTargetParams detachParams;
            detachParams.sessionId = std::string(sessionId.view());
            static_cast<void>(
                cdp.send<dto::CdpEmptyObject>("Target.detachFromTarget", detachParams)
            );
            dto::TargetDisposeBrowserContextParams disposeParams;
            disposeParams.browserContextId = std::string(browserContextId.view());
            static_cast<void>(
                cdp.send<dto::CdpEmptyObject>("Target.disposeBrowserContext", disposeParams)
            );
            cdp.removeListener(listenerId);

            auto exchange = tracker.buildExchange(
                domState.finalUrl, domState.title, body, std::move(resources)
            );
            auto logs = browser.drainLogs();
            browser.close();
            static_cast<void>(logs);
            return {std::move(exchange)};
        } catch (const std::exception &e) {
            cdp.removeListener(listenerId);
            throw CaptureFailure(std::string(e.what()), tracker.currentSeedProbe());
        }
    } catch (const std::exception &) {
        browser.close();
        throw;
    }
}

struct [[nodiscard]] RunExecutionResult {
    i64 exitCode;
    std::optional<crawler::SeedPageProbe> seedProbe;
    std::optional<String> failureDetail;
    std::string stdoutLog;
    std::string stderrLog;
    std::optional<std::string> wacz;
    std::optional<std::string> pages;
};

[[nodiscard]] RunExecutionResult executeRun(
    us::clients::http::Client &httpClient, us::engine::subprocess::ProcessStarter &processStarter,
    const crawler::RunRequest &run, const std::string &browserBin, const String &geometry,
    const String &proxyServer
)
{
    try {
        auto capture = captureViaProxy(
            httpClient, processStarter, run, browserBin, geometry, proxyServer
        );
        auto pages = crawler::buildPagesJsonl(capture.exchange);
        auto stdoutLog = crawler::buildSuccessStdoutLog(
            run, capture.exchange, browserBin, std::optional<String>{geometry}, 0_i64, false
        );
        std::string stderrLog;
        auto warc = crawler::buildWarc(capture.exchange);
        auto wacz = crawler::buildWacz(run, pages, warc, stdoutLog, stderrLog);

        const auto retainedBytes = i64(wacz.size()) + i64(pages.size()) + i64(stdoutLog.size()) +
                                   i64(stderrLog.size());
        if (retainedBytes > kMaxBodyBytes) {
            const auto detail = text::format(
                "retained artifact bytes {} exceeded size limit {}", toNative(retainedBytes),
                toNative(kMaxBodyBytes)
            );
            auto limitStderr = stderrLog;
            limitStderr += std::string(detail.view()) + "\n";
            return {
                14_i64,
                crawler::SeedPageProbe{capture.exchange.statusCode, 0_i64},
                detail,
                std::move(stdoutLog),
                std::move(limitStderr),
                {},
                {},
            };
        }

        auto exitCode = capture.exchange.statusCode >= 400_i64 ? 9_i64 : 0_i64;
        auto loadState = exitCode != 0_i64 || capture.exchange.statusCode >= 400_i64 ? 0_i64
                                                                                     : 2_i64;
        std::optional<String> failureDetail;
        if (capture.exchange.statusCode >= 400_i64) {
            failureDetail = text::format(
                "seed returned HTTP {}", toNative(capture.exchange.statusCode)
            );
        }

        return {
            exitCode,
            crawler::SeedPageProbe{capture.exchange.statusCode, loadState},
            failureDetail,
            std::move(stdoutLog),
            std::move(stderrLog),
            std::move(wacz),
            std::move(pages),
        };
    } catch (const CaptureFailure &e) {
        return {
            9_i64, e.seedProbe, String::fromBytesThrow(e.what()), {}, std::string(e.what()) + "\n",
            {},    {},
        };
    } catch (const std::exception &e) {
        return {
            9_i64, {}, String::fromBytesThrow(e.what()), {}, std::string(e.what()) + "\n", {}, {},
        };
    }
}

} // namespace

CrawlerRunner::CrawlerRunner(
    us::clients::http::Client &httpClientIn,
    us::engine::subprocess::ProcessStarter &processStarterIn, i64 runTimeoutSecIn
)
    : CrawlerRunner(
          httpClientIn, processStarterIn, runTimeoutSecIn, "http://127.0.0.1:3128"_t, "chromium",
          "1600x900"_t
      )
{
}

CrawlerRunner::CrawlerRunner(
    us::clients::http::Client &httpClientIn,
    us::engine::subprocess::ProcessStarter &processStarterIn, i64 runTimeoutSecIn,
    String proxyServerIn, std::string browserBinIn, String geometryIn
)
    : httpClient(httpClientIn), processStarter(processStarterIn), runTimeoutSec(runTimeoutSecIn),
      proxyServer(std::move(proxyServerIn)), browserBin(std::move(browserBinIn)),
      geometry(std::move(geometryIn))
{
}

CrawlerRunArtifacts CrawlerRunner::run(const String &seedUrl) const
{
    auto result = executeRun(
        httpClient, processStarter, crawler::RunRequest{seedUrl, runTimeoutSec * 1000_i64},
        browserBin, geometry, proxyServer
    );

    CrawlerRunArtifacts out;
    out.attempt.exited = true;
    out.attempt.exitCode = numericCast<int>(result.exitCode);
    out.attempt.waczExists = result.wacz.has_value();
    out.attempt.seedProbe = result.seedProbe;
    if (result.failureDetail)
        out.attempt.failureDetail = *result.failureDetail;

    out.stdoutLog = result.stdoutLog;
    out.stderrLog = result.stderrLog;
    if (result.wacz)
        out.wacz = *result.wacz;
    if (result.pages)
        out.pagesJsonl = *result.pages;
    return out;
}

} // namespace v1
