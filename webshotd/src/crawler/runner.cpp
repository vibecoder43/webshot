#include "crawler/runner.hpp"

#include "crawler/artifacts.hpp"
#include "crawler/browser_sandbox.hpp"
#include "crawler/browser_sandbox.sh.hpp"
#include "crawler/cdp_client.hpp"
#include "crawler/failure.hpp"
#include "crawler/launch_policy.hpp"
#include "deadline_utils.hpp"
#include "integers.hpp"
#include "schema/cdp.hpp"
#include "text.hpp"
#include "url.hpp"

#include <algorithm>
#include <array>
#include <boost/uuid/uuid_io.hpp>
#include <chrono>
#include <csignal>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <absl/strings/ascii.h>

#include <userver/crypto/base64.hpp>
#include <userver/engine/deadline.hpp>
#include <userver/engine/sleep.hpp>
#include <userver/engine/subprocess/process_starter.hpp>
#include <userver/formats/json.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/fs/blocking/read.hpp>
#include <userver/fs/blocking/temp_directory.hpp>
#include <userver/fs/blocking/write.hpp>
#include <userver/logging/log.hpp>
#include <userver/utils/assert.hpp>
#include <userver/utils/boost_uuid4.hpp>
#include <userver/utils/datetime.hpp>

namespace us = userver;
namespace chrono = std::chrono;
namespace json = us::formats::json;

using namespace text::literals;

namespace v1 {
namespace {

constexpr auto kMaxBodyBytes = 50_i64 * 1024_i64 * 1024_i64;
constexpr size_t kMaxLogBytes = 64UL * 1024UL;
const std::string kBwrapStatusWrapperScript = R"(exec 3>"$1"; shift; exec "$@")";

[[nodiscard]] std::string normalizeDirPath(std::string value)
{
    while (value.size() > 1 && value.back() == '/')
        value.pop_back();
    return value;
}

[[nodiscard]] std::string buildBrowserRunsRoot(std::string stateDir)
{
    auto root = normalizeDirPath(std::move(stateDir));
    UINVARIANT(!root.empty(), "state_dir must not be empty");
    if (root == "/")
        return "/browser_runs";
    return fmt::format("{}/browser_runs", root);
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
        out.emplace(absl::AsciiStrToLower(std::string_view{name}), value);
    return out;
}

[[nodiscard]] std::unordered_map<std::string, std::string>
normalizeHeadersOrEmpty(const std::optional<dto::CdpHeaders> &headers)
{
    if (!headers)
        return {};
    return normalizeHeaders(headers.value());
}

[[nodiscard]] std::optional<String> stringOrNull(const std::optional<std::string> &value)
{
    if (!value)
        return {};
    return String::fromBytesThrow(value.value());
}

[[nodiscard]] String generatePageId()
{
    return String::fromBytesThrow(
        boost::uuids::to_string(us::utils::generators::GenerateBoostUuid())
    );
}

class CaptureFailure final : public std::runtime_error {
public:
    CaptureFailure(std::string messageIn, std::optional<crawler::SeedPageProbe> seedProbeIn)
        : std::runtime_error(std::move(messageIn)), seedProbe(std::move(seedProbeIn))
    {
    }

    std::optional<crawler::SeedPageProbe> seedProbe;
};

template <typename T> [[nodiscard]] T parseEventParams(const crawler::CdpEvent &event)
{
    if (!event.params)
        throw std::runtime_error(fmt::format("{} missing params", event.method.view()));
    return event.params->extra.As<T>();
}

struct [[nodiscard]] BrowserPaths {
    std::string rootDir;
    std::string userDataDir;
    std::string xdgConfigHome;
    std::string xdgCacheHome;
    std::string crashpadDir;
    std::string proxySocketPath;
    std::string cdpSocketPath;
    std::string websocketPathFilePath;
    std::string netlogPath;
    std::string cdpTracePath;
    std::string stdoutLogPath;
    std::string stderrLogPath;
    std::string chromiumStderrLogPath;
    std::string bwrapStatusFilePath;
    std::string phaseFilePath;
    std::string devNullPath;
};

[[nodiscard]] BrowserPaths createBrowserPaths(std::string_view browserRunsRoot)
{
    BrowserPaths paths;
    auto tempRoot = normalizeDirPath(std::string(browserRunsRoot));
    us::fs::blocking::CreateDirectories(tempRoot);

    paths.rootDir = fmt::format(
        "{}/browser-{}", tempRoot,
        boost::uuids::to_string(us::utils::generators::GenerateBoostUuid())
    );
    us::fs::blocking::CreateDirectories(paths.rootDir);

    const auto rootDir = paths.rootDir;
    us::fs::blocking::RewriteFileContents(
        rootDir + "/browser_sandbox.sh", std::string(crawler::kBrowserSandboxScript)
    );
    paths.userDataDir = rootDir + "/profile";
    paths.xdgConfigHome = rootDir + "/xdg-config";
    paths.xdgCacheHome = rootDir + "/xdg-cache";
    paths.crashpadDir = rootDir + "/crashpad";
    paths.proxySocketPath = rootDir + "/proxy.sock";
    paths.cdpSocketPath = rootDir + "/cdp.sock";
    paths.websocketPathFilePath = rootDir + "/websocket_path.txt";
    paths.netlogPath = rootDir + "/netlog.json";
    paths.cdpTracePath = rootDir + "/cdp-trace.jsonl";
    paths.stdoutLogPath = rootDir + "/stdout.log";
    paths.stderrLogPath = rootDir + "/stderr.log";
    paths.chromiumStderrLogPath = rootDir + "/chromium-stderr.log";
    paths.bwrapStatusFilePath = rootDir + "/bwrap-status.jsonl";
    paths.phaseFilePath = rootDir + "/phase.txt";
    paths.devNullPath = rootDir + "/devnull";

    for (auto x : {paths.userDataDir, paths.xdgConfigHome, paths.xdgCacheHome, paths.crashpadDir})
        us::fs::blocking::CreateDirectories(x);
    for (auto x : {paths.phaseFilePath, paths.cdpTracePath, paths.devNullPath})
        us::fs::blocking::RewriteFileContents(x, {});
    return paths;
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

void writePhaseMarker(const std::string &path, std::string_view phase)
{
    us::fs::blocking::RewriteFileContents(
        path, fmt::format("{} {}\n", currentTimestamp().view(), phase)
    );
}

[[nodiscard]] std::string formatBrowserLogs(const std::pair<std::string, std::string> &logs)
{
    return fmt::format(
        "stdout={}, stderr={}", logs.first.empty() ? "empty" : logs.first,
        logs.second.empty() ? "empty" : logs.second
    );
}

[[nodiscard]] std::string formatLaunchLogs(
    const std::pair<std::string, std::string> &browserLogs, const std::string &bwrapStatus
)
{
    auto value = formatBrowserLogs(browserLogs);
    if (!bwrapStatus.empty())
        value = fmt::format("{}, bwrap_status={}", value, bwrapStatus);
    return value;
}

[[nodiscard]] bool isProcessRunning(std::optional<us::engine::subprocess::ChildProcess> &process)
{
    return process && !process->WaitFor(chrono::milliseconds(0));
}

[[nodiscard]] std::optional<String> readSanitizedLogTail(const std::string &path)
{
    if (!us::fs::blocking::FileExists(path))
        return {};

    try {
        const auto sanitized = crawler::sanitizeProcessOutputTail(
            us::fs::blocking::ReadFileContents(path)
        );
        if (!sanitized.empty())
            return sanitized;
    } catch (const std::exception &) {
    }

    return {};
}

void appendDiagnosticField(std::string &out, std::string_view label, std::string_view value)
{
    if (!out.empty())
        out += ", ";
    out += std::string(label);
    out += '=';
    out += value;
}

void removeBrowserRunDirNoThrow(const std::string &path) noexcept
{
    if (path.empty())
        return;

    try {
        auto tempDir = us::fs::blocking::TempDirectory::Adopt(path);
        std::move(tempDir).Remove();
    } catch (const std::exception &e) {
        LOG_WARNING() << fmt::format("Failed to remove browser dir {}: {}", path, e.what());
    }
}

[[nodiscard]] std::optional<String> readWebsocketPathFile(const std::string &path)
{
    if (!us::fs::blocking::FileExists(path))
        return {};
    auto value = us::fs::blocking::ReadFileContents(path);
    absl::StripTrailingAsciiWhitespace(&value);
    if (value.empty())
        return {};
    return String::fromBytesThrow(value);
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

[[nodiscard]] us::engine::subprocess::ChildProcess
spawnProxyBridge(us::engine::subprocess::ProcessStarter &processStarter, const BrowserPaths &paths)
{
    auto args = std::vector<std::string>{
        "UNIX-LISTEN:" + paths.proxySocketPath + ",fork,unlink-early",
        fmt::format("TCP-CONNECT:127.0.0.1:{}", crawler::kProxyUpstreamPort),
    };
    return spawnProcess(processStarter, "socat", args, paths.devNullPath, paths.devNullPath);
}

[[nodiscard]] us::engine::subprocess::ChildProcess spawnSandboxedBrowser(
    us::engine::subprocess::ProcessStarter &processStarter, const BrowserPaths &paths
)
{
    auto chromiumArgs = crawler::buildChromiumArgs(paths.userDataDir, paths.netlogPath);
    auto bwrapArgs = std::vector<std::string>{
        "bwrap",
        "--json-status-fd",
        "3",
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
        "bash",
        "browser_sandbox.sh",
        paths.proxySocketPath,
        paths.cdpSocketPath,
        paths.websocketPathFilePath,
        fmt::format("{}", crawler::kProxyListenPort),
        fmt::format("{}", crawler::kDevtoolsPort),
        "--",
        "chromium",
    };
    bwrapArgs.insert(std::end(bwrapArgs), std::begin(chromiumArgs), std::end(chromiumArgs));

    auto args = std::vector<std::string>{
        "-c",
        kBwrapStatusWrapperScript,
        "bash",
        paths.bwrapStatusFilePath,
    };
    args.insert(std::end(args), std::begin(bwrapArgs), std::end(bwrapArgs));
    return spawnProcess(processStarter, "bash", args, paths.stdoutLogPath, paths.stderrLogPath);
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
        us::engine::subprocess::ProcessStarter &processStarter, std::string browserRunsRootIn
    )
        : processStarter(processStarter),
          browserRunsRoot(normalizeDirPath(std::move(browserRunsRootIn)))
    {
    }

    ~BrowserInstance() { close(); }

    BrowserInstance(const BrowserInstance &) = delete;
    BrowserInstance(BrowserInstance &&) = delete;
    BrowserInstance &operator=(const BrowserInstance &) = delete;
    BrowserInstance &operator=(BrowserInstance &&) = delete;

    void launch()
    {
        paths = createBrowserPaths(browserRunsRoot);
        markPhase("launch_browser");
        const auto devtoolsDeadline = us::engine::Deadline::FromDuration(chrono::seconds(8));
        proxyBridge.emplace(spawnProxyBridge(processStarter, paths));
        process.emplace(spawnSandboxedBrowser(processStarter, paths));
        websocketPath = waitForDevtoolsPath(devtoolsDeadline);
    }

    [[nodiscard]] std::unique_ptr<crawler::CdpClient> connectCdp() const
    {
        try {
            return std::make_unique<crawler::CdpClient>(
                paths.cdpSocketPath, websocketPath, paths.cdpTracePath
            );
        } catch (const std::exception &e) {
            throw std::runtime_error(
                fmt::format(
                    "devtools websocket handshake failed: {} ({})", e.what(), currentLaunchLogs()
                )
            );
        }
    }

    [[nodiscard]] std::pair<std::string, std::string> drainBrowserLogs() const
    {
        return {readLogTail(paths.stdoutLogPath), readLogTail(paths.stderrLogPath)};
    }

    void markPhase(std::string_view phase) const
    {
        if (paths.phaseFilePath.empty())
            return;
        writePhaseMarker(paths.phaseFilePath, phase);
    }

    [[nodiscard]] std::string currentLaunchLogs() const
    {
        return formatLaunchLogs(drainBrowserLogs(), readLogTail(paths.bwrapStatusFilePath));
    }

    [[nodiscard]] std::optional<std::string> preserveFailureArtifacts()
    {
        if (preservedFailureDir)
            return preservedFailureDir;
        UINVARIANT(!paths.rootDir.empty(), "browser run dir must not be empty");

        preserveRunDir = true;
        preservedFailureDir = paths.rootDir;
        return preservedFailureDir;
    }

    [[nodiscard]] std::string buildRuntimeFailureDetail(std::string_view message)
    {
        try {
            const auto preservedDir = preserveFailureArtifacts();
            std::string diagnostics;

            if (const auto browserLogs =
                    crawler::summarizeProcessOutputs(paths.stdoutLogPath, paths.stderrLogPath)) {
                appendDiagnosticField(diagnostics, "browser_logs", browserLogs->view());
            }
            if (const auto chromiumStderr = readSanitizedLogTail(paths.chromiumStderrLogPath))
                appendDiagnosticField(diagnostics, "chromium_stderr", chromiumStderr->view());
            if (const auto bwrapStatus = readSanitizedLogTail(paths.bwrapStatusFilePath))
                appendDiagnosticField(diagnostics, "bwrap_status", bwrapStatus->view());
            if (const auto phaseMarker = readSanitizedLogTail(paths.phaseFilePath))
                appendDiagnosticField(diagnostics, "phase", phaseMarker->view());
            if (const auto cdpTrace = readSanitizedLogTail(paths.cdpTracePath))
                appendDiagnosticField(diagnostics, "cdp_trace_tail", cdpTrace->view());
            if (const auto websocketPath = readWebsocketPathFile(paths.websocketPathFilePath))
                appendDiagnosticField(diagnostics, "websocket_path", websocketPath->view());
            appendDiagnosticField(
                diagnostics, "websocket_path_file_exists",
                us::fs::blocking::FileExists(paths.websocketPathFilePath) ? "true" : "false"
            );
            appendDiagnosticField(
                diagnostics, "netlog_exists",
                us::fs::blocking::FileExists(paths.netlogPath) ? "true" : "false"
            );
            appendDiagnosticField(
                diagnostics, "browser_process_running", isProcessRunning(process) ? "true" : "false"
            );
            appendDiagnosticField(
                diagnostics, "cdp_socket_exists",
                us::fs::blocking::FileExists(paths.cdpSocketPath) ? "true" : "false"
            );
            appendDiagnosticField(
                diagnostics, "proxy_bridge_running",
                isProcessRunning(proxyBridge) ? "true" : "false"
            );
            if (preservedDir)
                appendDiagnosticField(diagnostics, "preserved_browser_dir", preservedDir.value());

            if (diagnostics.empty())
                return std::string(message);
            return fmt::format("{}, {}", message, diagnostics);
        } catch (const std::exception &) {
            return std::string(message);
        }
    }

    void close()
    {
        if (closed)
            return;
        closed = true;

        stopProcess(process, chrono::milliseconds(1500));
        stopProcess(proxyBridge, chrono::milliseconds(500));
        if (!paths.rootDir.empty() && !preserveRunDir)
            removeBrowserRunDirNoThrow(paths.rootDir);
    }

private:
    [[nodiscard]] String waitForDevtoolsPath(us::engine::Deadline deadline)
    {
        UINVARIANT(deadline.IsReachable(), "devtools deadline must be reachable");
        auto sawCdpSocket = false;
        auto sawWebsocketPath = false;
        while (!deadline.IsReached()) {
            sawCdpSocket = sawCdpSocket || us::fs::blocking::FileExists(paths.cdpSocketPath);
            sawWebsocketPath = sawWebsocketPath ||
                               us::fs::blocking::FileExists(paths.websocketPathFilePath);
            if (process && process->WaitFor(chrono::milliseconds(0))) {
                throw std::runtime_error(
                    fmt::format(
                        "chromium exited before exposing devtools ({})", currentLaunchLogs()
                    )
                );
            }
            const auto websocketPath = readWebsocketPathFile(paths.websocketPathFilePath);
            if (sawCdpSocket && websocketPath) {
                return websocketPath.value();
            }
            us::engine::SleepFor(chrono::milliseconds(100));
        }
        if (process && process->WaitFor(chrono::milliseconds(0))) {
            throw std::runtime_error(
                fmt::format("chromium exited before exposing devtools ({})", currentLaunchLogs())
            );
        }
        throw std::runtime_error(
            fmt::format(
                "{} ({})",
                !sawWebsocketPath ? "devtools websocket path was never written"
                : !sawCdpSocket
                    ? "devtools websocket path was written but cdp socket never appeared"
                    : "devtools websocket path and cdp socket appeared but handshake never started",
                currentLaunchLogs()
            )
        );
    }

    us::engine::subprocess::ProcessStarter &processStarter;
    std::string browserRunsRoot;
    BrowserPaths paths;
    std::optional<us::engine::subprocess::ChildProcess> proxyBridge;
    std::optional<us::engine::subprocess::ChildProcess> process;
    String websocketPath;
    std::optional<std::string> preservedFailureDir;
    bool preserveRunDir{false};
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
                "retained body bytes {} exceeded size limit {}", nextRetainedBytes, budget.maxBytes
            )
        );
    }
    budget.retainedBytes = nextRetainedBytes;
    return body;
}

[[nodiscard]] bool responseCanHaveBody(const String &method, i64 statusCode)
{
    if (method == "HEAD"_t)
        return false;
    return (statusCode < 100_i64 || statusCode >= 200_i64) && statusCode != 204_i64 &&
           statusCode != 304_i64;
}

[[nodiscard]] std::optional<String> buildUrlOrigin(const String &urlText)
{
    const auto maybeUrl = Url::fromText(urlText);
    if (!maybeUrl)
        return {};
    if (!maybeUrl->isHttp() && !maybeUrl->isHttps())
        return {};

    return text::format("{}://{}", maybeUrl->isHttps() ? "https" : "http", maybeUrl->host());
}

[[nodiscard]] String resolveRedirectTargetUrl(
    const String &baseUrl, const String &requestUrl,
    const std::optional<dto::NetworkResponse> &redirectResponse
)
{
    if (!redirectResponse)
        return requestUrl;

    const auto headers = normalizeHeadersOrEmpty(redirectResponse->headers);
    const auto locationIt = headers.find("location");
    if (locationIt == std::end(headers) || locationIt->second.empty())
        return requestUrl;

    const auto location = String::fromBytesThrow(locationIt->second);
    if (const auto absoluteLocation = Url::fromText(location))
        return absoluteLocation->href();

    const auto origin = buildUrlOrigin(baseUrl);
    if (!origin)
        return requestUrl;

    if (location.startsWith("//")) {
        const auto maybeBaseUrl = Url::fromText(baseUrl);
        if (!maybeBaseUrl)
            return requestUrl;
        return text::format("{}:{}", maybeBaseUrl->isHttps() ? "https" : "http", location);
    }

    if (location.startsWith('/'))
        return text::format("{}{}", origin.value(), location);

    if (location.startsWith('?')) {
        const auto maybeBaseUrl = Url::fromText(baseUrl);
        if (!maybeBaseUrl)
            return requestUrl;
        return text::format("{}{}{}", origin.value(), maybeBaseUrl->pathname(), location);
    }

    return requestUrl;
}

struct [[nodiscard]] TrackedRequest {
    String requestUrl;
    String method;
    std::optional<i64> statusCode;
    std::optional<String> statusMessage;
    std::optional<std::unordered_map<std::string, std::string>> headers;
    std::optional<String> timestamp;
    std::optional<String> loaderId;
    std::optional<String> frameId;
    std::optional<String> resourceType;
    bool loaded{false};
    bool isTrackedMainDocument{false};
};

class [[nodiscard]] PageTracker final {
public:
    PageTracker(String sessionIdIn, String targetIdIn)
        : sessionId(std::move(sessionIdIn)), targetId(std::move(targetIdIn)),
          pageId(generatePageId())
    {
    }

    void beginSeedNavigation(const String &seedUrl)
    {
        seedNavigationStarted = true;
        seedNavigationUrl = seedUrl;
    }

    void setExpectedMainLoaderId(std::optional<String> loaderId)
    {
        if (loaderId)
            mainLoaderId = std::move(loaderId.value());
    }

    void handleEvent(const crawler::CdpEvent &event)
    {
        const auto method = event.method.view();
        if (method == "Target.targetCrashed") {
            if (event.params) {
                const auto crashed = event.params->extra.As<dto::TargetTargetCrashedEvent>();
                if (crashed.targetId && targetId.view() == crashed.targetId.value())
                    mainRequestFailure = "page target crashed"_t;
            }
            return;
        }
        if (method == "Target.detachedFromTarget") {
            if (event.params) {
                const auto detachedSessionId = event.params->extra["sessionId"];
                if (!detachedSessionId.IsMissing() &&
                    detachedSessionId.As<std::string>() == sessionId.view()) {
                    mainRequestFailure = "target session detached"_t;
                }
            }
            return;
        }
        if (method == "Target.targetDestroyed") {
            if (event.params) {
                const auto destroyedTargetId = event.params->extra["targetId"];
                if (!destroyedTargetId.IsMissing() &&
                    destroyedTargetId.As<std::string>() == targetId.view()) {
                    mainRequestFailure = "page target destroyed"_t;
                }
            }
            return;
        }
        if (method == "Inspector.detached") {
            if (event.sessionId && event.sessionId->view() != sessionId.view())
                return;

            if (event.params) {
                const auto reason = event.params->extra["reason"];
                if (!reason.IsMissing()) {
                    mainRequestFailure = text::format(
                        "inspector detached: {}", reason.As<std::string>()
                    );
                    return;
                }
            }
            mainRequestFailure = "inspector detached"_t;
            return;
        }

        if (!event.sessionId || event.sessionId->view() != sessionId.view())
            return;

        if (method == "Page.loadEventFired") {
            loaded = true;
            return;
        }
        if (method == "Network.requestWillBeSent") {
            handleRequestWillBeSent(parseEventParams<dto::NetworkRequestWillBeSentEvent>(event));
            return;
        }
        if (method == "Network.responseReceived") {
            handleResponseReceived(parseEventParams<dto::NetworkResponseReceivedEvent>(event));
            return;
        }
        if (method == "Network.loadingFinished") {
            handleLoadingFinished(parseEventParams<dto::NetworkLoadingFinishedEvent>(event));
            return;
        }
        if (method == "Network.loadingFailed") {
            handleLoadingFailed(parseEventParams<dto::NetworkLoadingFailedEvent>(event));
        }
    }

    void waitForLoad(crawler::CdpClient &cdp, us::engine::Deadline deadline)
    {
        cdp.waitUntil(
            [this]() { return loaded || mainRequestFailure.has_value(); }, deadline,
            "timed out waiting for page load"
        );
        if (mainRequestFailure)
            throw std::runtime_error(std::string(mainRequestFailure->view()));
    }

    void waitForMainDocument(crawler::CdpClient &cdp, us::engine::Deadline deadline)
    {
        cdp.waitUntil(
            [this]() {
                const auto *request = activeMainRequest();
                return mainRequestFailure.has_value() ||
                       (completedMainRequest.has_value() && completedMainRequest->loaded &&
                        hasResponse(completedMainRequest.value())) ||
                       (request != nullptr && hasResponse(*request) && request->loaded);
            },
            deadline, "timed out waiting for main document response"
        );
        if (mainRequestFailure)
            throw std::runtime_error(std::string(mainRequestFailure->view()));
    }

    void
    waitForIdle(crawler::CdpClient &cdp, chrono::milliseconds idle, us::engine::Deadline deadline)
    {
        cdp.waitUntil(
            [this, idle]() {
                return inflight.empty() && us::utils::datetime::SteadyNow() - lastNetworkAt >= idle;
            },
            deadline, "timed out waiting for network idle"
        );
    }

    [[nodiscard]] std::optional<crawler::SeedPageProbe> currentSeedProbe() const
    {
        if (const auto *request = resolvedMainRequest();
            request != nullptr && request->statusCode) {
            return crawler::SeedPageProbe{
                raw(request->statusCode.value()),
                request->loaded && !mainRequestFailure ? std::optional<int64_t>{2}
                                                       : std::optional<int64_t>{0},
            };
        }

        if (mainRequestId || mainRequestFailure || loaded)
            return crawler::SeedPageProbe{0, 0};

        return {};
    }

    [[nodiscard]] const std::optional<String> &failureReason() const { return mainRequestFailure; }

    [[nodiscard]] std::string readBody(
        crawler::CdpClient &cdp, const String &sessionIdIn, RetainedBodyBudget &budget,
        const std::string &fallbackBody
    ) const
    {
        if (!fallbackBody.empty())
            return retainBody(fallbackBody, budget);
        const auto *bodyRequestId = mainResponseRequestId ? &mainResponseRequestId.value()
                                                          : nullptr;
        if (bodyRequestId == nullptr && mainRequestId)
            bodyRequestId = &mainRequestId.value();
        if (bodyRequestId == nullptr)
            return retainBody(fallbackBody, budget);

        try {
            dto::NetworkGetResponseBodyParams params;
            params.requestId = std::string(bodyRequestId->view());
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

        for (const auto &[requestId, request] : activeRequests) {
            if (request.isTrackedMainDocument || !hasResponse(request)) {
                continue;
            }

            const auto response = toMainResponse(request);

            if (!responseCanHaveBody(request.method, response.statusCode)) {
                resources.push_back({
                    request.requestUrl,
                    request.method,
                    request.resourceType,
                    response.statusCode,
                    response.statusMessage,
                    response.headers,
                    {},
                    response.timestamp,
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
                    request.requestUrl,
                    request.method,
                    request.resourceType,
                    response.statusCode,
                    response.statusMessage,
                    response.headers,
                    retainBody(
                        bodyValue.base64Encoded ? us::crypto::base64::Base64Decode(bodyValue.body)
                                                : bodyValue.body,
                        budget
                    ),
                    response.timestamp,
                });
            } catch (const std::exception &) {
                resources.push_back({
                    request.requestUrl,
                    request.method,
                    request.resourceType,
                    response.statusCode,
                    response.statusMessage,
                    response.headers,
                    {},
                    response.timestamp,
                });
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
        crawler::CapturedExchange exchange{};
        exchange.seedUrl = seedNavigationUrl ? seedNavigationUrl.value() : finalUrl;
        exchange.pageId = pageId;
        exchange.finalUrl = std::move(finalUrl);
        applyMainResponse(exchange, exchange.finalUrl);
        exchange.redirectChain = buildRedirectChainForExchange(exchange.finalUrl);
        exchange.mainDocumentRedirects = mainDocumentRedirects;
        exchange.body = std::move(body);
        exchange.resources = std::move(resources);
        exchange.title = std::move(title);
        return exchange;
    }

    std::optional<String> mainFrameId;

private:
    struct [[nodiscard]] MainResponse {
        String requestUrl;
        i64 statusCode;
        String statusMessage;
        std::unordered_map<std::string, std::string> headers;
        String timestamp;
    };

    void applyMainResponse(crawler::CapturedExchange &exchange, const String &finalUrl) const
    {
        const auto response = selectMainResponse(finalUrl);
        UINVARIANT(response, "missing main response while building exchange");
        exchange.statusCode = response->statusCode;
        exchange.statusMessage = response->statusMessage;
        exchange.headers = response->headers;
        exchange.timestamp = response->timestamp;
    }

    [[nodiscard]] std::vector<String> buildRedirectChainForExchange(const String &finalUrl) const
    {
        if (!redirectChain.empty())
            return redirectChain;
        return {finalUrl};
    }

    [[nodiscard]] static bool hasResponse(const TrackedRequest &request)
    {
        return request.statusCode && request.statusMessage && request.headers && request.timestamp;
    }

    [[nodiscard]] static MainResponse toMainResponse(const TrackedRequest &request)
    {
        UINVARIANT(
            request.statusCode && request.statusMessage && request.headers && request.timestamp,
            "tracked request missing response"
        );
        return {
            request.requestUrl,      request.statusCode.value(), request.statusMessage.value(),
            request.headers.value(), request.timestamp.value(),
        };
    }

    [[nodiscard]] TrackedRequest *activeMainRequest()
    {
        if (!mainRequestId)
            return nullptr;
        if (const auto it = activeRequests.find(mainRequestId.value());
            it != std::end(activeRequests))
            return &it->second;
        return nullptr;
    }

    [[nodiscard]] const TrackedRequest *activeMainRequest() const
    {
        if (!mainRequestId)
            return nullptr;
        if (const auto it = activeRequests.find(mainRequestId.value());
            it != std::end(activeRequests))
            return &it->second;
        return nullptr;
    }

    [[nodiscard]] const TrackedRequest *resolvedMainRequest() const
    {
        if (completedMainRequest && hasResponse(completedMainRequest.value()))
            return &completedMainRequest.value();
        return activeMainRequest();
    }

    [[nodiscard]] std::optional<MainResponse> selectMainResponse(const String &finalUrl) const
    {
        if (completedMainRequest && hasResponse(completedMainRequest.value())) {
            if (completedMainRequest->requestUrl == finalUrl)
                return toMainResponse(completedMainRequest.value());
        }
        if (const auto *request = activeMainRequest();
            request != nullptr && hasResponse(*request)) {
            if (request->requestUrl == finalUrl)
                return toMainResponse(*request);
        }
        if (completedMainRequest && hasResponse(completedMainRequest.value()))
            return toMainResponse(completedMainRequest.value());
        if (const auto *request = activeMainRequest(); request != nullptr && hasResponse(*request))
            return toMainResponse(*request);
        return {};
    }

    [[nodiscard]] bool matchesTrackedMainLoader(const std::optional<std::string> &loaderId) const
    {
        if (!mainLoaderId)
            return true;
        return loaderId && loaderId.value() == mainLoaderId->view();
    }

    [[nodiscard]] bool
    isMainFrameDocumentRequest(const dto::NetworkRequestWillBeSentEvent &requestWillBeSent) const
    {
        return requestWillBeSent.frameId && mainFrameId &&
               requestWillBeSent.frameId.value() == mainFrameId->view() && requestWillBeSent.type &&
               requestWillBeSent.type.value() == "Document";
    }

    void handleRequestWillBeSent(dto::NetworkRequestWillBeSentEvent requestWillBeSent)
    {
        if (requestWillBeSent.request.url.starts_with("data:"))
            return;

        const auto requestIdText = String::fromBytesThrow(requestWillBeSent.requestId);
        const auto rawRequestUrl = String::fromBytesThrow(requestWillBeSent.request.url);
        const auto requestMethod = String::fromBytesThrow(requestWillBeSent.request.method);

        inflight.insert(requestIdText);
        lastNetworkAt = us::utils::datetime::SteadyNow();

        auto isTrackedMainDocument = false;
        if (isMainFrameDocumentRequest(requestWillBeSent)) {
            UINVARIANT(
                seedNavigationStarted,
                "main document request observed before seed navigation started"
            );
            if (mainLoaderId && !matchesTrackedMainLoader(requestWillBeSent.loaderId))
                return;
            if (!mainLoaderId && !mainRequestId && seedNavigationUrl) {
                const auto matchesExact = rawRequestUrl == seedNavigationUrl.value();
                const auto matchesTrailingSlash =
                    !seedNavigationUrl->endsWith('/') &&
                    rawRequestUrl.sizeBytes() == seedNavigationUrl->sizeBytes() + 1 &&
                    rawRequestUrl.startsWith(seedNavigationUrl.value()) &&
                    rawRequestUrl.endsWith('/');
                if (!matchesExact && !matchesTrailingSlash)
                    return;
            }
            if (!mainLoaderId && requestWillBeSent.loaderId)
                mainLoaderId = stringOrNull(requestWillBeSent.loaderId);
            isTrackedMainDocument = true;
        }

        std::optional<String> previousRequestUrl;
        if (const auto it = activeRequests.find(requestIdText); it != std::end(activeRequests))
            previousRequestUrl = it->second.requestUrl;
        if (requestWillBeSent.redirectResponse)
            finalizeRedirectRequest(requestIdText, requestWillBeSent.redirectResponse);

        const auto canonicalRequestUrl = previousRequestUrl
                                             ? resolveRedirectTargetUrl(
                                                   previousRequestUrl.value(), rawRequestUrl,
                                                   requestWillBeSent.redirectResponse
                                               )
                                             : rawRequestUrl;

        TrackedRequest trackedRequest{
            canonicalRequestUrl,
            requestMethod,
            {},
            {},
            {},
            {},
            stringOrNull(requestWillBeSent.loaderId),
            stringOrNull(requestWillBeSent.frameId),
            stringOrNull(requestWillBeSent.type),
            false,
            isTrackedMainDocument,
        };
        activeRequests.insert_or_assign(requestIdText, std::move(trackedRequest));

        if (isTrackedMainDocument) {
            mainRequestId = requestIdText;
            if (redirectChain.empty() || redirectChain.back() != canonicalRequestUrl)
                redirectChain.push_back(canonicalRequestUrl);
            return;
        }
    }

    void handleResponseReceived(dto::NetworkResponseReceivedEvent responseReceived)
    {
        const auto requestIdText = String::fromBytesThrow(responseReceived.requestId);
        const auto requestIt = activeRequests.find(requestIdText);
        if (requestIt == std::end(activeRequests)) {
            if (mainRequestId && mainRequestId.value() == requestIdText) {
                UINVARIANT(
                    false, fmt::format(
                               "main document response received for unknown request id {}",
                               requestIdText.view()
                           )
                );
            }
            return;
        }

        const auto headers = normalizeHeadersOrEmpty(responseReceived.response.headers);
        const auto timestamp = currentTimestamp();
        auto &request = requestIt->second;
        request.statusCode = responseReceived.response.status
                                 ? i64(responseReceived.response.status.value())
                                 : 0_i64;
        request.statusMessage = String::fromBytesThrow(
            responseReceived.response.statusText.value_or("")
        );
        request.headers = std::move(headers);
        request.timestamp = timestamp;
        if (responseReceived.type)
            request.resourceType = stringOrNull(responseReceived.type);
        if (responseReceived.loaderId)
            request.loaderId = stringOrNull(responseReceived.loaderId);
        if (request.isTrackedMainDocument && hasResponse(request)) {
            completedMainRequest = request;
            mainResponseRequestId = requestIdText;
        }
    }

    void handleLoadingFinished(dto::NetworkLoadingFinishedEvent loadingFinished)
    {
        const auto requestIdText = String::fromBytesThrow(loadingFinished.requestId);
        inflight.erase(requestIdText);
        lastNetworkAt = us::utils::datetime::SteadyNow();
        if (const auto it = activeRequests.find(requestIdText); it != std::end(activeRequests)) {
            it->second.loaded = true;
            if (it->second.isTrackedMainDocument && hasResponse(it->second)) {
                completedMainRequest = it->second;
                mainResponseRequestId = requestIdText;
            }
        } else if (mainRequestId && mainRequestId.value() == requestIdText) {
            UINVARIANT(
                false,
                fmt::format(
                    "main document loading finished for unknown request id {}", requestIdText.view()
                )
            );
        }
    }

    void handleLoadingFailed(dto::NetworkLoadingFailedEvent loadingFailed)
    {
        const auto requestIdText = String::fromBytesThrow(loadingFailed.requestId);
        inflight.erase(requestIdText);
        lastNetworkAt = us::utils::datetime::SteadyNow();

        const auto requestIt = activeRequests.find(requestIdText);
        if (requestIt == std::end(activeRequests)) {
            if (mainRequestId && mainRequestId.value() == requestIdText) {
                UINVARIANT(
                    false, fmt::format(
                               "main document loading failed for unknown request id {}",
                               requestIdText.view()
                           )
                );
            }
            return;
        }

        auto &request = requestIt->second;
        request.loaded = true;
        if (!request.isTrackedMainDocument)
            return;
        if (hasResponse(request))
            return;

        mainRequestFailure = String::fromBytesThrow(
            loadingFailed.errorText.value_or("main document request failed")
        );
    }

    void finalizeRedirectRequest(
        const String &requestId, const std::optional<dto::NetworkResponse> &redirectResponse
    )
    {
        UINVARIANT(
            redirectResponse && redirectResponse->status, "redirect response must include status"
        );

        const auto requestIt = activeRequests.find(requestId);
        UINVARIANT(
            requestIt != std::end(activeRequests),
            fmt::format("redirect response for unknown request id {}", requestId.view())
        );

        auto request = std::move(requestIt->second);
        activeRequests.erase(requestIt);

        request.statusCode = i64(redirectResponse->status.value());
        request.statusMessage = String::fromBytesThrow(redirectResponse->statusText.value_or(""));
        request.headers = normalizeHeadersOrEmpty(redirectResponse->headers);
        request.timestamp = currentTimestamp();
        request.loaded = true;

        if (request.isTrackedMainDocument) {
            recordMainDocumentRedirect(request);
            if (mainRequestId && mainRequestId.value() == requestId)
                mainRequestId.reset();
            return;
        }

        recordResourceRedirect(request);
    }

    void recordMainDocumentRedirect(const TrackedRequest &request)
    {
        UINVARIANT(
            hasResponse(request),
            fmt::format(
                "main redirect request missing response fields for {}", request.requestUrl.view()
            )
        );

        crawler::CapturedMainDocumentRedirect redirect;
        redirect.redirectUrl = request.requestUrl;
        redirect.statusCode = request.statusCode.value();
        redirect.statusMessage = request.statusMessage.value();
        redirect.headers = request.headers.value();
        redirect.timestamp = request.timestamp.value();

        if (!mainDocumentRedirects.empty()) {
            const auto &previous = mainDocumentRedirects.back();
            if (previous.redirectUrl == redirect.redirectUrl &&
                previous.statusCode == redirect.statusCode) {
                return;
            }
        }
        mainDocumentRedirects.push_back(std::move(redirect));
    }

    void recordResourceRedirect(const TrackedRequest &request)
    {
        UINVARIANT(
            hasResponse(request), fmt::format(
                                      "resource redirect request missing response fields for {}",
                                      request.requestUrl.view()
                                  )
        );

        redirectedResources.push_back({
            request.requestUrl,
            request.method,
            request.resourceType,
            request.statusCode.value(),
            request.statusMessage.value(),
            request.headers.value(),
            {},
            request.timestamp.value(),
        });
    }

    String sessionId;
    String targetId;
    String pageId;
    std::unordered_map<String, TrackedRequest> activeRequests;
    std::vector<crawler::CapturedResource> redirectedResources;
    std::vector<String> redirectChain;
    std::vector<crawler::CapturedMainDocumentRedirect> mainDocumentRedirects;
    std::unordered_set<String> inflight;
    std::optional<String> mainRequestId;
    std::optional<String> mainResponseRequestId;
    std::optional<String> mainLoaderId;
    std::optional<String> seedNavigationUrl;
    std::optional<TrackedRequest> completedMainRequest;
    bool loaded{false};
    bool seedNavigationStarted{false};
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
    return {
        String::fromBytesThrow(value.finalUrl),
        value.title ? std::make_optional(String::fromBytesThrow(value.title.value()))
                    : std::optional<String>{},
        value.html,
    };
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

class [[nodiscard]] CaptureSession final {
public:
    CaptureSession(
        us::engine::subprocess::ProcessStarter &processStarter, std::string browserRunsRootIn,
        crawler::RunRequest runIn
    )
        : run(std::move(runIn)),
          deadline(us::engine::Deadline::FromDuration(chrono::milliseconds{raw(run.jobTimeoutMs)})),
          browser(processStarter, std::move(browserRunsRootIn))
    {
    }

    [[nodiscard]] crawler::CapturedExchange capture()
    {
        try {
            launch();
            try {
                return captureAttachedTarget();
            } catch (const std::exception &e) {
                auto failureDetail = browser.buildRuntimeFailureDetail(e.what());
                if (tracker && tracker->failureReason()) {
                    failureDetail = fmt::format(
                        "{}, tracker_failure={}", failureDetail, tracker->failureReason()->view()
                    );
                }
                auto seedProbe = currentSeedProbe();
                closeCdpForFailure();
                throw CaptureFailure(std::move(failureDetail), std::move(seedProbe));
            }
        } catch (const CaptureFailure &) {
            closeCdpForFailure();
            browser.close();
            throw;
        } catch (const std::exception &) {
            closeCdpForFailure();
            static_cast<void>(browser.preserveFailureArtifacts());
            browser.close();
            throw;
        }
    }

private:
    void launch()
    {
        browser.launch();
        browser.markPhase("connect_cdp");
        cdp = browser.connectCdp();

        browser.markPhase("create_browser_context");
        const auto browserContext = cdpClient().send<dto::TargetCreateBrowserContextResult>(
            "Target.createBrowserContext"
        );
        browserContextId = String::fromBytesThrow(browserContext.browserContextId);

        browser.markPhase("create_target");
        dto::TargetCreateTargetParams targetParams;
        targetParams.url = "about:blank";
        targetParams.browserContextId = std::string(browserContextId->view());
        const auto target = cdpClient().send<dto::TargetCreateTargetResult>(
            "Target.createTarget", targetParams
        );
        targetId = String::fromBytesThrow(target.targetId);

        browser.markPhase("attach_target");
        dto::TargetAttachToTargetParams attachParams;
        attachParams.targetId = std::string(targetId->view());
        attachParams.flatten = true;
        const auto attached = cdpClient().send<dto::TargetAttachToTargetResult>(
            "Target.attachToTarget", attachParams
        );
        sessionId = String::fromBytesThrow(attached.sessionId);
        tracker = std::make_unique<PageTracker>(sessionId.value(), targetId.value());
        listenerId = cdpClient().addListener([this](crawler::CdpEvent event) {
            if (tracker)
                tracker->handleEvent(event);
        });
    }

    [[nodiscard]] crawler::CapturedExchange captureAttachedTarget()
    {
        browser.markPhase("enable_page");
        static_cast<void>(
            cdpClient().send<dto::CdpEmptyObject>("Page.enable", attachedSessionId())
        );
        browser.markPhase("enable_runtime");
        static_cast<void>(
            cdpClient().send<dto::CdpEmptyObject>("Runtime.enable", attachedSessionId())
        );
        browser.markPhase("enable_network");
        static_cast<void>(
            cdpClient().send<dto::CdpEmptyObject>("Network.enable", attachedSessionId())
        );

        browser.markPhase("enable_lifecycle_events");
        dto::PageSetLifecycleEventsEnabledParams lifecycleParams;
        lifecycleParams.enabled = true;
        static_cast<void>(cdpClient().send<dto::CdpEmptyObject>(
            "Page.setLifecycleEventsEnabled", lifecycleParams, attachedSessionId()
        ));

        browser.markPhase("disable_cache");
        dto::NetworkSetCacheDisabledParams cacheParams;
        cacheParams.cacheDisabled = true;
        static_cast<void>(cdpClient().send<dto::CdpEmptyObject>(
            "Network.setCacheDisabled", cacheParams, attachedSessionId()
        ));

        browser.markPhase("bypass_service_worker");
        dto::NetworkSetBypassServiceWorkerParams serviceWorkerParams;
        serviceWorkerParams.bypass = true;
        static_cast<void>(cdpClient().send<dto::CdpEmptyObject>(
            "Network.setBypassServiceWorker", serviceWorkerParams, attachedSessionId()
        ));

        browser.markPhase("set_extra_headers");
        dto::NetworkSetExtraHTTPHeadersParams headerParams;
        headerParams.headers.extra.emplace(
            "Accept-Language", std::string(crawler::kBrowserAcceptLanguage)
        );
        static_cast<void>(cdpClient().send<dto::CdpEmptyObject>(
            "Network.setExtraHTTPHeaders", headerParams, attachedSessionId()
        ));

        browser.markPhase("get_frame_tree");
        const auto frameTree = cdpClient().send<dto::PageGetFrameTreeResult>(
            "Page.getFrameTree", attachedSessionId()
        );
        pageTracker().mainFrameId = String::fromBytesThrow(frameTree.frameTree.frame.id);

        browser.markPhase("navigate");
        dto::PageNavigateParams navigateParams;
        navigateParams.url = std::string(run.seedUrl.view());
        pageTracker().beginSeedNavigation(run.seedUrl);
        const auto navigateResult = cdpClient().send<dto::PageNavigateResult>(
            "Page.navigate", navigateParams, attachedSessionId()
        );
        if (navigateResult.errorText)
            throw std::runtime_error(navigateResult.errorText.value());
        pageTracker().setExpectedMainLoaderId(stringOrNull(navigateResult.loaderId));

        browser.markPhase("wait_for_load");
        pageTracker().waitForLoad(cdpClient(), deadline);
        if (crawler::kPostLoadDelayMs > 0_i64) {
            browser.markPhase("post_load_delay");
            sleepWithinDeadline(
                deadline, chrono::milliseconds{crawler::kPostLoadDelayMs},
                "timed out waiting for post-load delay"
            );
            browser.markPhase("post_load_delay_done");
        }
        if (crawler::kBehaviorTimeoutMs > 0_i64) {
            browser.markPhase("run_site_behavior");
            browser.markPhase("run_site_behavior_runtime_evaluate");
            const auto behaviorBudget = timeLeftOrThrowMs(
                deadline, "timed out running site behavior"
            );
            runSiteBehavior(
                cdpClient(), attachedSessionId(),
                std::min(chrono::milliseconds{crawler::kBehaviorTimeoutMs}, behaviorBudget)
            );
            browser.markPhase("run_site_behavior_done");
        }
        if (crawler::kNetIdleWaitMs > 0_i64) {
            browser.markPhase("wait_for_idle");
            browser.markPhase("wait_for_idle_wait");
            pageTracker().waitForIdle(
                cdpClient(), chrono::milliseconds{crawler::kNetIdleWaitMs}, deadline
            );
            browser.markPhase("wait_for_idle_done");
        }
        if (crawler::kPageExtraDelayMs > 0_i64) {
            browser.markPhase("page_extra_delay");
            sleepWithinDeadline(
                deadline, chrono::milliseconds{crawler::kPageExtraDelayMs},
                "timed out waiting for extra page delay"
            );
            browser.markPhase("page_extra_delay_done");
        }
        browser.markPhase("wait_for_main_document");
        browser.markPhase("wait_for_main_document_wait");
        pageTracker().waitForMainDocument(cdpClient(), deadline);
        browser.markPhase("wait_for_main_document_done");

        browser.markPhase("read_dom_state");
        browser.markPhase("read_dom_state_runtime_evaluate");
        auto domState = readDomState(cdpClient(), attachedSessionId());
        browser.markPhase("read_dom_state_done");
        RetainedBodyBudget budget{kMaxBodyBytes, 0_i64};
        browser.markPhase("read_main_body");
        auto body = pageTracker().readBody(cdpClient(), attachedSessionId(), budget, domState.html);
        browser.markPhase("read_resources");
        auto resources = pageTracker().readResources(cdpClient(), attachedSessionId(), budget);

        removeTrackerListener();
        detachTarget();
        disposeBrowserContext();

        browser.markPhase("build_exchange_start");
        LOG_INFO() << fmt::format(
            "captureViaProxy building exchange for {} (body_bytes={}, resources={})", run.seedUrl,
            body.size(), resources.size()
        );
        auto exchange = pageTracker().buildExchange(
            std::move(domState.finalUrl), std::move(domState.title), std::move(body),
            std::move(resources)
        );
        browser.markPhase("build_exchange_done");
        LOG_INFO() << fmt::format(
            "captureViaProxy built exchange for {} (status={}, resources={}, body_bytes={})",
            run.seedUrl, exchange.statusCode, exchange.resources.size(), exchange.body.size()
        );
        tracker.reset();
        cdp.reset();

        browser.markPhase("close_browser_success");
        browser.markPhase("before_browser_close");
        LOG_INFO() << fmt::format("captureViaProxy closing browser for {}", run.seedUrl);
        browser.close();
        LOG_INFO() << fmt::format("captureViaProxy returning capture for {}", run.seedUrl);
        return exchange;
    }

    void detachTarget()
    {
        if (!sessionId)
            return;

        browser.markPhase("detach_target");
        dto::TargetDetachFromTargetParams detachParams;
        detachParams.sessionId = std::string(sessionId->view());
        static_cast<void>(
            cdpClient().send<dto::CdpEmptyObject>("Target.detachFromTarget", detachParams)
        );
        sessionId.reset();
    }

    void disposeBrowserContext()
    {
        if (!browserContextId)
            return;

        browser.markPhase("dispose_browser_context");
        dto::TargetDisposeBrowserContextParams disposeParams;
        disposeParams.browserContextId = std::string(browserContextId->view());
        static_cast<void>(
            cdpClient().send<dto::CdpEmptyObject>("Target.disposeBrowserContext", disposeParams)
        );
        browserContextId.reset();
        targetId.reset();
    }

    void removeTrackerListener()
    {
        if (!cdp || !listenerId)
            return;
        cdp->removeListener(listenerId.value());
        listenerId.reset();
    }

    void closeCdpForFailure()
    {
        removeTrackerListener();
        if (!cdp)
            return;

        try {
            cdp->close();
        } catch (const std::exception &e) {
            LOG_WARNING() << "Suppressing CDP close failure during capture cleanup: " << e.what();
        }
        cdp.reset();
    }

    [[nodiscard]] std::optional<crawler::SeedPageProbe> currentSeedProbe() const
    {
        if (!tracker)
            return {};
        return tracker->currentSeedProbe();
    }

    [[nodiscard]] crawler::CdpClient &cdpClient() const
    {
        UINVARIANT(cdp, "cdp client is not connected");
        return *cdp;
    }

    [[nodiscard]] PageTracker &pageTracker() const
    {
        UINVARIANT(tracker, "page tracker is not attached");
        return *tracker;
    }

    [[nodiscard]] const String &attachedSessionId() const
    {
        UINVARIANT(sessionId, "cdp session is not attached");
        return sessionId.value();
    }

    crawler::RunRequest run;
    us::engine::Deadline deadline;
    BrowserInstance browser;
    std::unique_ptr<crawler::CdpClient> cdp;
    std::unique_ptr<PageTracker> tracker;
    std::optional<String> browserContextId;
    std::optional<String> targetId;
    std::optional<String> sessionId;
    std::optional<crawler::CdpClient::ListenerId> listenerId;
};

[[nodiscard]] crawler::CapturedExchange captureViaProxy(
    us::engine::subprocess::ProcessStarter &processStarter, const std::string &browserRunsRoot,
    const crawler::RunRequest &run
)
{
    auto session = CaptureSession(
        processStarter, std::string(browserRunsRoot),
        crawler::RunRequest{run.seedUrl, run.jobTimeoutMs}
    );
    return session.capture();
}

[[nodiscard]] CrawlerRunArtifacts executeRun(
    us::clients::http::Client &httpClient, us::engine::subprocess::ProcessStarter &processStarter,
    const std::string &browserRunsRoot, const crawler::RunRequest &run
)
{
    static_cast<void>(httpClient);
    CrawlerRunArtifacts out;
    out.attempt.exited = true;
    try {
        LOG_INFO() << fmt::format("crawler executeRun starting for {}", run.seedUrl);
        auto exchange = captureViaProxy(processStarter, browserRunsRoot, run);
        LOG_INFO() << fmt::format(
            "crawler captureViaProxy finished for {} with status={}", run.seedUrl,
            exchange.statusCode
        );
        auto pages = crawler::buildPagesJsonl(exchange);
        LOG_INFO() << fmt::format("crawler buildPagesJsonl finished for {}", run.seedUrl);
        out.stdoutLog = crawler::buildSuccessStdoutLog(run, exchange, 0_i64, false);
        out.stderrLog.clear();
        auto warc = crawler::buildWarc(exchange);
        LOG_INFO() << fmt::format("crawler buildWarc finished for {}", run.seedUrl);
        auto wacz = crawler::buildWacz(run, pages, warc, out.stdoutLog, out.stderrLog);
        LOG_INFO() << fmt::format(
            "crawler buildWacz finished for {} (wacz_bytes={}, pages_bytes={})", run.seedUrl,
            wacz.size(), pages.size()
        );

        const auto retainedBytes = i64(wacz.size()) + i64(pages.size()) +
                                   i64(out.stdoutLog.size()) + i64(out.stderrLog.size());
        if (retainedBytes > kMaxBodyBytes) {
            const auto detail = text::format(
                "retained artifact bytes {} exceeded size limit {}", retainedBytes, kMaxBodyBytes
            );
            out.attempt.exitCode = 14;
            out.attempt.waczExists = false;
            out.attempt.seedProbe = crawler::SeedPageProbe{
                numericCast<int64_t>(exchange.statusCode), 0
            };
            out.attempt.failureDetail = detail;
            out.wacz.reset();
            out.pagesJsonl.reset();
            out.stderrLog += std::string(detail.view()) + "\n";
            return out;
        }

        const auto exitCode = exchange.statusCode >= 400_i64 ? 9_i64 : 0_i64;
        const auto loadState = exitCode != 0_i64 || exchange.statusCode >= 400_i64 ? 0_i64 : 2_i64;
        if (exchange.statusCode >= 400_i64) {
            out.attempt.failureDetail = text::format("seed returned HTTP {}", exchange.statusCode);
        }

        LOG_INFO() << fmt::format(
            "crawler executeRun finished for {} (exit_code={}, wacz_exists=true)", run.seedUrl,
            exitCode
        );

        out.attempt.exitCode = numericCast<int>(exitCode);
        out.attempt.waczExists = true;
        out.attempt.seedProbe = crawler::SeedPageProbe{
            numericCast<int64_t>(exchange.statusCode), numericCast<int64_t>(loadState)
        };
        out.wacz = std::move(wacz);
        out.pagesJsonl = std::move(pages);
        return out;
    } catch (const CaptureFailure &e) {
        out.attempt.exitCode = 9;
        out.attempt.waczExists = false;
        out.attempt.seedProbe = e.seedProbe;
        out.attempt.failureDetail = String::fromBytesThrow(e.what());
        out.stdoutLog.clear();
        out.stderrLog = std::string(e.what()) + "\n";
        out.wacz.reset();
        out.pagesJsonl.reset();
        return out;
    } catch (const std::exception &e) {
        out.attempt.exitCode = 9;
        out.attempt.waczExists = false;
        out.attempt.seedProbe.reset();
        out.attempt.failureDetail = String::fromBytesThrow(e.what());
        out.stdoutLog.clear();
        out.stderrLog = std::string(e.what()) + "\n";
        out.wacz.reset();
        out.pagesJsonl.reset();
        return out;
    }
}

} // namespace

CrawlerRunner::CrawlerRunner(
    us::clients::http::Client &httpClientIn,
    us::engine::subprocess::ProcessStarter &processStarterIn, i64 runTimeoutSecIn,
    std::string stateDir
)
    : httpClient(httpClientIn), processStarter(processStarterIn), runTimeoutSec(runTimeoutSecIn),
      browserRunsRoot(buildBrowserRunsRoot(std::move(stateDir)))
{
}

CrawlerRunArtifacts CrawlerRunner::run(const String &seedUrl) const
{
    return executeRun(
        httpClient, processStarter, browserRunsRoot,
        crawler::RunRequest{seedUrl, runTimeoutSec * 1000_i64}
    );
}

} // namespace v1
