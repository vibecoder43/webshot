#include "crawler/runner.hpp"

#include "crawler/artifacts.hpp"
#include "crawler/browser_sandbox.hpp"
#include "crawler/cdp_client.hpp"
#include "crawler/failure.hpp"
#include "crawler/launch_policy.hpp"
#include "integers.hpp"
#include "schema/cdp.hpp"
#include "text.hpp"
#include "url.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <boost/uuid/uuid_io.hpp>
#include <cctype>
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

#include <userver/crypto/base64.hpp>
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
constexpr std::string_view kBwrapBin = "bwrap";
constexpr std::string_view kBashBin = "bash";
constexpr std::string_view kSocatBin = "socat";
constexpr std::string_view kBrowserSandboxScriptPath = WEBSHOT_BROWSER_SANDBOX_SCRIPT_PATH;
constexpr std::string_view kBwrapStatusWrapperScript = R"(exec 3>"$1"; shift; exec "$@")";
constexpr std::string_view kBrowserRunsRoot = "/tmp/webshot/browser-runs";
constexpr std::string_view kBrowserFailuresRoot = "/tmp/webshot/browser-failures";
constexpr size_t kMaxLogBytes = 64UL * 1024UL;

std::atomic<uint64_t> gBrowserFailureSequence = 0;

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

[[nodiscard]] std::optional<String> stringOrNull(const std::optional<std::string> &value)
{
    if (!value)
        return {};
    return String::fromBytesThrow(*value);
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

template <typename T> [[nodiscard]] T parseEventParams(const crawler::CdpEvent &event)
{
    if (!event.params)
        throw std::runtime_error(fmt::format("{} missing params", event.method.view()));
    return event.params->extra.As<T>();
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

[[nodiscard]] BrowserPaths createBrowserPaths()
{
    BrowserPaths paths;
    const auto tempRoot = std::string(kBrowserRunsRoot);
    us::fs::blocking::CreateDirectories(tempRoot);
    paths.tempDir = us::fs::blocking::TempDirectory::Create(tempRoot, "browser-");

    const auto rootDir = paths.tempDir.GetPath();
    paths.rootDir = rootDir;
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

[[nodiscard]] std::string formatBool(bool value) { return value ? "true" : "false"; }

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

void copyFileIfExists(const std::string &sourcePath, const std::string &destPath)
{
    if (!us::fs::blocking::FileExists(sourcePath))
        return;

    auto contents = us::fs::blocking::ReadFileContents(sourcePath);
    us::fs::blocking::RewriteFileContents(destPath, contents);
}

[[nodiscard]] std::optional<String> readWebsocketPathFile(const std::string &path)
{
    if (!us::fs::blocking::FileExists(path))
        return {};
    auto value = us::fs::blocking::ReadFileContents(path);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
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
        "TCP-CONNECT:127.0.0.1:" + std::to_string(toNative(crawler::kProxyUpstreamPort)),
    };
    return spawnProcess(
        processStarter, std::string(kSocatBin), args, paths.devNullPath, paths.devNullPath
    );
}

[[nodiscard]] us::engine::subprocess::ChildProcess spawnSandboxedBrowser(
    us::engine::subprocess::ProcessStarter &processStarter, const BrowserPaths &paths
)
{
    auto chromiumArgs = crawler::buildChromiumArgs(paths.userDataDir, paths.netlogPath);
    auto bwrapArgs = std::vector<std::string>{
        std::string(kBwrapBin),
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
        std::string(kBashBin),
        std::string(kBrowserSandboxScriptPath),
        paths.proxySocketPath,
        paths.cdpSocketPath,
        paths.websocketPathFilePath,
        std::to_string(toNative(crawler::kProxyListenPort)),
        std::to_string(toNative(crawler::kDevtoolsPort)),
        "--",
        std::string(crawler::kBrowserBin),
    };
    bwrapArgs.insert(std::end(bwrapArgs), std::begin(chromiumArgs), std::end(chromiumArgs));

    auto args = std::vector<std::string>{
        "-c",
        std::string(kBwrapStatusWrapperScript),
        std::string(kBashBin),
        paths.bwrapStatusFilePath,
    };
    args.insert(std::end(args), std::begin(bwrapArgs), std::end(bwrapArgs));
    return spawnProcess(
        processStarter, std::string(kBashBin), args, paths.stdoutLogPath, paths.stderrLogPath
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
    explicit BrowserInstance(us::engine::subprocess::ProcessStarter &processStarter)
        : processStarter(processStarter)
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
        markPhase("launch_browser");
        const auto devtoolsDeadline = us::utils::datetime::SteadyNow() + chrono::seconds(8);
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
        if (paths.rootDir.empty())
            return {};

        const auto targetRoot = std::string(kBrowserFailuresRoot);
        us::fs::blocking::CreateDirectories(targetRoot);

        const auto targetDir = fmt::format(
            "{}/browser-failure-{}", targetRoot, gBrowserFailureSequence.fetch_add(1) + 1
        );
        us::fs::blocking::CreateDirectories(targetDir);

        using ArtifactFile = std::pair<const std::string *, std::string_view>;
        const auto artifactFiles = std::array<ArtifactFile, 8>{{
            ArtifactFile{&paths.stdoutLogPath, "/stdout.log"},
            ArtifactFile{&paths.stderrLogPath, "/stderr.log"},
            ArtifactFile{&paths.chromiumStderrLogPath, "/chromium-stderr.log"},
            ArtifactFile{&paths.bwrapStatusFilePath, "/bwrap-status.jsonl"},
            ArtifactFile{&paths.phaseFilePath, "/phase.txt"},
            ArtifactFile{&paths.netlogPath, "/netlog.json"},
            ArtifactFile{&paths.cdpTracePath, "/cdp-trace.jsonl"},
            ArtifactFile{&paths.websocketPathFilePath, "/websocket_path.txt"},
        }};
        for (const auto &[sourcePath, suffix] : artifactFiles) {
            auto destPath = targetDir;
            destPath += suffix;
            copyFileIfExists(*sourcePath, destPath);
        }

        preservedFailureDir = targetDir;
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
                formatBool(us::fs::blocking::FileExists(paths.websocketPathFilePath))
            );
            appendDiagnosticField(
                diagnostics, "netlog_exists",
                formatBool(us::fs::blocking::FileExists(paths.netlogPath))
            );
            appendDiagnosticField(
                diagnostics, "browser_process_running", formatBool(isProcessRunning(process))
            );
            appendDiagnosticField(
                diagnostics, "cdp_socket_exists",
                formatBool(us::fs::blocking::FileExists(paths.cdpSocketPath))
            );
            appendDiagnosticField(
                diagnostics, "proxy_bridge_running", formatBool(isProcessRunning(proxyBridge))
            );
            if (preservedDir)
                appendDiagnosticField(diagnostics, "preserved_browser_dir", *preservedDir);

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
        if (!paths.rootDir.empty())
            std::move(paths.tempDir).Remove();
    }

private:
    [[nodiscard]] String waitForDevtoolsPath(chrono::steady_clock::time_point deadline)
    {
        auto sawCdpSocket = false;
        auto sawWebsocketPath = false;
        while (us::utils::datetime::SteadyNow() < deadline) {
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
                return *websocketPath;
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
    BrowserPaths paths;
    std::optional<us::engine::subprocess::ChildProcess> proxyBridge;
    std::optional<us::engine::subprocess::ChildProcess> process;
    String websocketPath;
    std::optional<std::string> preservedFailureDir;
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

[[nodiscard]] std::optional<String> buildUrlOrigin(const String &urlText)
{
    const auto maybeUrl = Url::fromText(urlText);
    if (!maybeUrl)
        return {};
    if (!maybeUrl->isHttp() && !maybeUrl->isHttps())
        return {};

    return String::fromBytesThrow(
        fmt::format("{}://{}", maybeUrl->isHttps() ? "https" : "http", maybeUrl->host())
    );
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

    if (location.view().starts_with("//")) {
        const auto maybeBaseUrl = Url::fromText(baseUrl);
        if (!maybeBaseUrl)
            return requestUrl;
        return String::fromBytesThrow(
            fmt::format("{}:{}", maybeBaseUrl->isHttps() ? "https" : "http", location)
        );
    }

    if (location.view().starts_with("/"))
        return String::fromBytesThrow(fmt::format("{}{}", *origin, location));

    if (location.view().starts_with("?")) {
        const auto maybeBaseUrl = Url::fromText(baseUrl);
        if (!maybeBaseUrl)
            return requestUrl;
        return String::fromBytesThrow(
            fmt::format("{}{}{}", *origin, maybeBaseUrl->pathname(), location)
        );
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
            mainLoaderId = std::move(*loaderId);
    }

    void handleEvent(const crawler::CdpEvent &event)
    {
        const auto method = event.method.view();
        if (method == "Target.targetCrashed") {
            if (event.params) {
                const auto crashed = event.params->extra.As<dto::TargetTargetCrashedEvent>();
                if (crashed.targetId && targetId.view() == *crashed.targetId)
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
                    mainRequestFailure = String::fromBytesThrow(
                        fmt::format("inspector detached: {}", reason.As<std::string>())
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
                const auto *request = activeMainRequest();
                return mainRequestFailure.has_value() ||
                       (completedMainRequest.has_value() && completedMainRequest->loaded &&
                        hasResponse(*completedMainRequest)) ||
                       (request != nullptr && hasResponse(*request) && request->loaded);
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
        if (const auto *request = resolvedMainRequest();
            request != nullptr && request->statusCode) {
            return crawler::SeedPageProbe{
                toNative(*request->statusCode),
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
        const auto *bodyRequestId = mainResponseRequestId ? &*mainResponseRequestId : nullptr;
        if (bodyRequestId == nullptr && mainRequestId)
            bodyRequestId = &*mainRequestId;
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

            if (!responseCanHaveBody(request.method, *request.statusCode)) {
                resources.push_back({
                    request.requestUrl,
                    request.method,
                    request.resourceType,
                    *request.statusCode,
                    *request.statusMessage,
                    *request.headers,
                    {},
                    *request.timestamp,
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
                    *request.statusCode,
                    *request.statusMessage,
                    *request.headers,
                    retainBody(
                        bodyValue.base64Encoded ? us::crypto::base64::Base64Decode(bodyValue.body)
                                                : bodyValue.body,
                        budget
                    ),
                    *request.timestamp,
                });
            } catch (const std::exception &) {
                resources.push_back({
                    request.requestUrl,
                    request.method,
                    request.resourceType,
                    *request.statusCode,
                    *request.statusMessage,
                    *request.headers,
                    {},
                    *request.timestamp,
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
        exchange.seedUrl = seedNavigationUrl ? *seedNavigationUrl : finalUrl;
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
        UINVARIANT(hasResponse(request), "tracked request missing response");
        return {
            request.requestUrl, *request.statusCode, *request.statusMessage,
            *request.headers,   *request.timestamp,
        };
    }

    [[nodiscard]] TrackedRequest *activeMainRequest()
    {
        if (!mainRequestId)
            return nullptr;
        if (const auto it = activeRequests.find(*mainRequestId); it != std::end(activeRequests))
            return &it->second;
        return nullptr;
    }

    [[nodiscard]] const TrackedRequest *activeMainRequest() const
    {
        if (!mainRequestId)
            return nullptr;
        if (const auto it = activeRequests.find(*mainRequestId); it != std::end(activeRequests))
            return &it->second;
        return nullptr;
    }

    [[nodiscard]] const TrackedRequest *resolvedMainRequest() const
    {
        if (completedMainRequest && hasResponse(*completedMainRequest))
            return &*completedMainRequest;
        return activeMainRequest();
    }

    [[nodiscard]] std::optional<MainResponse> selectMainResponse(const String &finalUrl) const
    {
        if (completedMainRequest && hasResponse(*completedMainRequest)) {
            if (completedMainRequest->requestUrl == finalUrl)
                return toMainResponse(*completedMainRequest);
        }
        if (const auto *request = activeMainRequest();
            request != nullptr && hasResponse(*request)) {
            if (request->requestUrl == finalUrl)
                return toMainResponse(*request);
        }
        if (completedMainRequest && hasResponse(*completedMainRequest))
            return toMainResponse(*completedMainRequest);
        if (const auto *request = activeMainRequest(); request != nullptr && hasResponse(*request))
            return toMainResponse(*request);
        return {};
    }

    [[nodiscard]] bool matchesTrackedMainLoader(const std::optional<std::string> &loaderId) const
    {
        if (!mainLoaderId)
            return true;
        return loaderId && *loaderId == mainLoaderId->view();
    }

    [[nodiscard]] bool
    isMainFrameDocumentRequest(const dto::NetworkRequestWillBeSentEvent &requestWillBeSent) const
    {
        return requestWillBeSent.frameId && mainFrameId &&
               *requestWillBeSent.frameId == mainFrameId->view() && requestWillBeSent.type &&
               *requestWillBeSent.type == "Document";
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
            if (!seedNavigationStarted)
                return;
            if (mainLoaderId && !matchesTrackedMainLoader(requestWillBeSent.loaderId))
                return;
            if (!mainLoaderId && !mainRequestId && seedNavigationUrl) {
                const auto seedView = seedNavigationUrl->view();
                const auto rawView = rawRequestUrl.view();
                const auto matchesExact = rawRequestUrl == *seedNavigationUrl;
                const auto matchesTrailingSlash = !seedView.ends_with('/') &&
                                                  rawView.size() == seedView.size() + 1 &&
                                                  rawView.starts_with(seedView) &&
                                                  rawView.back() == '/';
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
                                                   *previousRequestUrl, rawRequestUrl,
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
        if (requestIt == std::end(activeRequests))
            return;

        const auto headers = normalizeHeadersOrEmpty(responseReceived.response.headers);
        const auto timestamp = currentTimestamp();
        auto &request = requestIt->second;
        request.statusCode = responseReceived.response.status
                                 ? i64(*responseReceived.response.status)
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
        }
    }

    void handleLoadingFailed(dto::NetworkLoadingFailedEvent loadingFailed)
    {
        const auto requestIdText = String::fromBytesThrow(loadingFailed.requestId);
        inflight.erase(requestIdText);
        lastNetworkAt = us::utils::datetime::SteadyNow();

        const auto requestIt = activeRequests.find(requestIdText);
        if (requestIt == std::end(activeRequests))
            return;

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
        if (!redirectResponse || !redirectResponse->status)
            return;

        const auto requestIt = activeRequests.find(requestId);
        if (requestIt == std::end(activeRequests))
            return;

        auto request = std::move(requestIt->second);
        activeRequests.erase(requestIt);

        request.statusCode = i64(*redirectResponse->status);
        request.statusMessage = String::fromBytesThrow(redirectResponse->statusText.value_or(""));
        request.headers = normalizeHeadersOrEmpty(redirectResponse->headers);
        request.timestamp = currentTimestamp();
        request.loaded = true;

        if (request.isTrackedMainDocument) {
            recordMainDocumentRedirect(request);
            if (mainRequestId && *mainRequestId == requestId)
                mainRequestId.reset();
            return;
        }

        recordResourceRedirect(request);
    }

    void recordMainDocumentRedirect(const TrackedRequest &request)
    {
        if (!hasResponse(request))
            return;

        crawler::CapturedMainDocumentRedirect redirect;
        redirect.redirectUrl = request.requestUrl;
        redirect.statusCode = *request.statusCode;
        redirect.statusMessage = *request.statusMessage;
        redirect.headers = *request.headers;
        redirect.timestamp = *request.timestamp;

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
        if (!hasResponse(request))
            return;

        redirectedResources.push_back({
            request.requestUrl,
            request.method,
            request.resourceType,
            *request.statusCode,
            *request.statusMessage,
            *request.headers,
            {},
            *request.timestamp,
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
        value.title ? std::make_optional(String::fromBytesThrow(*value.title))
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

struct [[nodiscard]] CaptureResult {
    crawler::CapturedExchange exchange;
};

class [[nodiscard]] CaptureSession final {
public:
    CaptureSession(
        us::engine::subprocess::ProcessStarter &processStarter, crawler::RunRequest runIn
    )
        : run(std::move(runIn)),
          deadline(us::utils::datetime::SteadyNow() + toMilliseconds(run.jobTimeoutMs)),
          browser(processStarter)
    {
    }

    [[nodiscard]] CaptureResult capture()
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
        tracker = std::make_unique<PageTracker>(*sessionId, *targetId);
        listenerId = cdpClient().addListener([this](crawler::CdpEvent event) {
            if (tracker)
                tracker->handleEvent(event);
        });
    }

    [[nodiscard]] CaptureResult captureAttachedTarget()
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
            throw std::runtime_error(*navigateResult.errorText);
        pageTracker().setExpectedMainLoaderId(stringOrNull(navigateResult.loaderId));

        browser.markPhase("wait_for_load");
        pageTracker().waitForLoad(
            cdpClient(), remainingBudgetOrThrow(deadline, "timed out waiting for page load")
        );
        if (crawler::kPostLoadDelayMs > 0_i64) {
            browser.markPhase("post_load_delay");
            sleepWithinBudget(
                deadline, toMilliseconds(crawler::kPostLoadDelayMs),
                "timed out waiting for post-load delay"
            );
            browser.markPhase("post_load_delay_done");
        }
        if (crawler::kBehaviorTimeoutMs > 0_i64) {
            browser.markPhase("run_site_behavior");
            browser.markPhase("run_site_behavior_runtime_evaluate");
            runSiteBehavior(
                cdpClient(), attachedSessionId(),
                std::min(
                    toMilliseconds(crawler::kBehaviorTimeoutMs),
                    remainingBudgetOrThrow(deadline, "timed out running site behavior")
                )
            );
            browser.markPhase("run_site_behavior_done");
        }
        if (crawler::kNetIdleWaitMs > 0_i64) {
            browser.markPhase("wait_for_idle");
            browser.markPhase("wait_for_idle_wait");
            pageTracker().waitForIdle(
                cdpClient(), toMilliseconds(crawler::kNetIdleWaitMs),
                remainingBudgetOrThrow(deadline, "timed out waiting for network idle")
            );
            browser.markPhase("wait_for_idle_done");
        }
        if (crawler::kPageExtraDelayMs > 0_i64) {
            browser.markPhase("page_extra_delay");
            sleepWithinBudget(
                deadline, toMilliseconds(crawler::kPageExtraDelayMs),
                "timed out waiting for extra page delay"
            );
            browser.markPhase("page_extra_delay_done");
        }
        browser.markPhase("wait_for_main_document");
        browser.markPhase("wait_for_main_document_wait");
        pageTracker().waitForMainDocument(
            cdpClient(),
            remainingBudgetOrThrow(deadline, "timed out waiting for main document response")
        );
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
            run.seedUrl, toNative(exchange.statusCode), exchange.resources.size(),
            exchange.body.size()
        );
        tracker.reset();
        cdp.reset();

        browser.markPhase("close_browser_success");
        browser.markPhase("before_browser_close");
        LOG_INFO() << fmt::format("captureViaProxy closing browser for {}", run.seedUrl);
        browser.close();
        LOG_INFO() << fmt::format("captureViaProxy returning capture for {}", run.seedUrl);
        return {std::move(exchange)};
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
        cdp->removeListener(*listenerId);
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
        return *sessionId;
    }

    crawler::RunRequest run;
    chrono::steady_clock::time_point deadline;
    BrowserInstance browser;
    std::unique_ptr<crawler::CdpClient> cdp;
    std::unique_ptr<PageTracker> tracker;
    std::optional<String> browserContextId;
    std::optional<String> targetId;
    std::optional<String> sessionId;
    std::optional<crawler::CdpClient::ListenerId> listenerId;
};

[[nodiscard]] CaptureResult captureViaProxy(
    us::engine::subprocess::ProcessStarter &processStarter, const crawler::RunRequest &run
)
{
    auto session = CaptureSession(
        processStarter, crawler::RunRequest{run.seedUrl, run.jobTimeoutMs}
    );
    return session.capture();
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
    const crawler::RunRequest &run
)
{
    static_cast<void>(httpClient);
    try {
        LOG_INFO() << fmt::format("crawler executeRun starting for {}", run.seedUrl);
        auto capture = captureViaProxy(processStarter, run);
        LOG_INFO() << fmt::format(
            "crawler captureViaProxy finished for {} with status={}", run.seedUrl,
            toNative(capture.exchange.statusCode)
        );
        auto pages = crawler::buildPagesJsonl(capture.exchange);
        LOG_INFO() << fmt::format("crawler buildPagesJsonl finished for {}", run.seedUrl);
        auto stdoutLog = crawler::buildSuccessStdoutLog(run, capture.exchange, 0_i64, false);
        std::string stderrLog;
        auto warc = crawler::buildWarc(capture.exchange);
        LOG_INFO() << fmt::format("crawler buildWarc finished for {}", run.seedUrl);
        auto wacz = crawler::buildWacz(run, pages, warc, stdoutLog, stderrLog);
        LOG_INFO() << fmt::format(
            "crawler buildWacz finished for {} (wacz_bytes={}, pages_bytes={})", run.seedUrl,
            wacz.size(), pages.size()
        );

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

        LOG_INFO() << fmt::format(
            "crawler executeRun finished for {} (exit_code={}, wacz_exists=true)", run.seedUrl,
            toNative(exitCode)
        );

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
    : httpClient(httpClientIn), processStarter(processStarterIn), runTimeoutSec(runTimeoutSecIn)
{
}

CrawlerRunArtifacts CrawlerRunner::run(const String &seedUrl) const
{
    auto result = executeRun(
        httpClient, processStarter, crawler::RunRequest{seedUrl, runTimeoutSec * 1000_i64}
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
