#include "crawler/runner.hpp"

#include "config.hpp"
#include "crawler/artifacts.hpp"
#include "crawler/browser_sandbox.hpp"
#include "crawler/cdp_client.hpp"
#include "crawler/egress_proxy.hpp"
#include "crawler/failure.hpp"
#include "crawler/launch_policy.hpp"
#include "crawler/limits.hpp"
#include "deadline_utils.hpp"
#include "denylist.hpp"
#include "grab_value.hpp"
#include "integers.hpp"
#include "link.hpp"
#include "prefix_utils.hpp"
#include "schema/cdp.hpp"
#include "text.hpp"
#include "url.hpp"
#include "uuid_format.hpp"

#include <generated/browser_sandbox.sh.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <absl/strings/ascii.h>

#include <userver/clients/dns/resolver.hpp>
#include <userver/crypto/base64.hpp>
#include <userver/crypto/exception.hpp>
#include <userver/engine/deadline.hpp>
#include <userver/engine/exception.hpp>
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
#include <userver/utils/resources.hpp>
#include <userver/utils/traceful_exception.hpp>
namespace chrono = std::chrono;
namespace dns = us::clients::dns;

using namespace text::literals;

namespace v1 {
namespace {

using v1::Expected;

constexpr auto kMaxLogBytes = 64_i64 * 1024_i64;
constexpr auto kCdpWsPayloadSlackBytes = 2_i64 * 1024_i64 * 1024_i64;
constexpr auto kLocalFixtureHttpPort = "18080";
constexpr auto kLocalFixtureHttpsPort = "18443";
const auto kLocalFixtureHostA = "test-target"_t;
const auto kLocalFixtureHostB = "asset.test-target"_t;

[[nodiscard]] i64 computeCdpMaxRemotePayloadBytes(i64 maxArchiveBytes)
{
    return (maxArchiveBytes * 4_i64) / 3_i64 + kCdpWsPayloadSlackBytes;
}

[[nodiscard]] const std::string &browserSandboxScript()
{
    static const std::string script = us::utils::FindResource("webshot_browser_sandbox_sh");
    return script;
}

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
    return std::format("{}/browser_runs", root);
}

[[noreturn]] void abortCgroupConfig(std::string_view message) noexcept
{
    us::utils::AbortWithStacktrace(std::string(message));
}

[[nodiscard]] String describeCdpFailure(const String &action, const crawler::CdpFailure &failure)
{
    auto message = action;
    if (failure.detail)
        message = text::format("{}: {}", message, failure.detail.value());
    return message;
}

[[nodiscard]] std::string readSelfCgroupV2Path()
{
    const auto raw = us::fs::blocking::ReadFileContents("/proc/self/cgroup");
    auto remaining = std::string_view{raw};
    while (true) {
        const auto next = remaining.find('\n');
        const auto line = next == std::string::npos ? remaining : remaining.substr(0, next);
        if (line.rfind("0::", 0) == 0) {
            auto path = normalizeDirPath(std::string(line.substr(3)));
            if (path.empty() || path.front() != '/')
                abortCgroupConfig("invalid cgroup v2 path in /proc/self/cgroup");
            return path;
        }
        if (next == std::string::npos)
            break;
        remaining.remove_prefix(next + 1);
    }
    abortCgroupConfig("failed to locate cgroup v2 path in /proc/self/cgroup");
}

[[nodiscard]] std::string parentCgroupPath(const std::string &path)
{
    if (path.empty() || path.front() != '/')
        abortCgroupConfig("cgroup path must be absolute");
    if (path == "/")
        abortCgroupConfig("webshotd must run inside a delegated systemd subgroup");

    const auto slashPos = path.find_last_of('/');
    if (slashPos == std::string::npos)
        abortCgroupConfig("failed to locate parent cgroup path");
    if (slashPos == 0)
        return "/";
    return path.substr(0, slashPos);
}

[[nodiscard]] std::string resolveDelegatedCgroupRootPath()
{
    const auto currentPath = readSelfCgroupV2Path();
    const auto parentPath = parentCgroupPath(currentPath);
    if (parentPath == "/")
        return "/sys/fs/cgroup";
    return std::format("/sys/fs/cgroup{}", parentPath);
}

[[nodiscard]] String currentTimestamp()
{
    return String::fromBytes(
               us::utils::datetime::UtcTimestring(
                   us::utils::datetime::Now(), us::utils::datetime::kRfc3339Format
               )
    )
        .expect();
}

[[nodiscard]] std::unordered_map<std::string, std::string>
normalizeHeaders(const dto::CdpHeaders &headers)
{
    std::unordered_map<std::string, std::string> out;
    for (const auto &[name, value] : headers.extra)
        out.emplace(absl::AsciiStrToLower(std::string_view{name}), value);
    return out;
}

[[nodiscard]] bool isLocalFixtureHost(std::string_view host) noexcept
{
    return host == kLocalFixtureHostA.view() || host == kLocalFixtureHostB.view();
}

[[nodiscard]] String canonicalizeCapturedUrl(const String &urlText)
{
    const auto maybeUrl = Url::fromText(urlText);
    if (!maybeUrl)
        return urlText;
    if (!maybeUrl->isHttp() && !maybeUrl->isHttps())
        return urlText;
    if (!maybeUrl->hasPort())
        return urlText;
    if (!isLocalFixtureHost(maybeUrl->hostname().view()))
        return urlText;

    const auto port = maybeUrl->port();
    const auto matchesFixturePort = (maybeUrl->isHttp() && port.view() == kLocalFixtureHttpPort) ||
                                    (maybeUrl->isHttps() && port.view() == kLocalFixtureHttpsPort);
    if (!matchesFixturePort)
        return urlText;

    auto parsed = maybeUrl->copyParsed();
    parsed.clear_port();
    return Url::fromParsed(std::move(parsed)).href();
}

[[nodiscard]] std::string
canonicalizeCapturedLocationHeader(const String &responseUrl, std::string_view locationValue)
{
    auto location = String::fromBytes(locationValue);
    if (!location)
        return std::string(locationValue);
    if (location->empty() || location->startsWith('/') || location->startsWith('?') ||
        location->startsWith("//")) {
        return std::string(location->view());
    }

    const auto canonicalLocation = canonicalizeCapturedUrl(location.value());
    const auto maybeCanonicalUrl = Url::fromText(canonicalLocation);
    const auto maybeResponseUrl = Url::fromText(responseUrl);
    if (!maybeCanonicalUrl || !maybeResponseUrl)
        return std::string(canonicalLocation.view());

    if (maybeCanonicalUrl->isHttp() == maybeResponseUrl->isHttp() &&
        maybeCanonicalUrl->host() == maybeResponseUrl->host()) {
        return std::string(maybeCanonicalUrl->pathWithSearch().view());
    }

    return std::string(canonicalLocation.view());
}

[[nodiscard]] std::unordered_map<std::string, std::string>
normalizeHeadersOrEmpty(const std::optional<dto::CdpHeaders> &headers)
{
    if (!headers)
        return {};
    return normalizeHeaders(headers.value());
}

[[nodiscard]] std::unordered_map<std::string, std::string>
normalizeHeadersForCapture(const std::optional<dto::CdpHeaders> &headers, const String &responseUrl)
{
    auto normalized = normalizeHeadersOrEmpty(headers);
    if (const auto it = normalized.find("location"); it != std::end(normalized))
        it->second = canonicalizeCapturedLocationHeader(responseUrl, it->second);
    return normalized;
}

[[nodiscard]] std::optional<String> stringOrNull(const std::optional<std::string> &value)
{
    if (!value)
        return {};
    auto parsed = String::fromBytes(value.value());
    if (!parsed)
        return {};
    return grabValueOf(parsed);
}

[[nodiscard]] String generatePageId()
{
    return text::format("{}", us::utils::generators::GenerateBoostUuid());
}

struct [[nodiscard]] CaptureFailure final {
    String detail;
    std::optional<crawler::SeedPageProbe> seedProbe;
};

template <typename T>
[[nodiscard]] Expected<T, String> parseEventParams(const crawler::CdpEvent &event)
{
    if (!event.params)
        return std::unexpected(text::format("{} missing params", event.method.view()));
    try {
        return event.params->extra.As<T>();
    } catch (const json::Exception &) {
        return std::unexpected(text::format("{} has invalid params", event.method.view()));
    }
}

struct [[nodiscard]] BrowserPaths {
    std::string rootDir;
    std::string runId;
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

    paths.runId = std::format("{}", us::utils::generators::GenerateBoostUuid());
    paths.rootDir = std::format("{}/browser-{}", tempRoot, paths.runId);
    us::fs::blocking::CreateDirectories(paths.rootDir);

    const auto rootDir = paths.rootDir;
    us::fs::blocking::RewriteFileContents(rootDir + "/browser_sandbox.sh", browserSandboxScript());
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
    if (ssize(value) <= kMaxLogBytes)
        return;
    const auto dropBytes = ssize(value) - kMaxLogBytes;
    value.erase(0, numericCast<size_t>(dropBytes));
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
        path, std::format("{} {}\n", currentTimestamp().view(), phase)
    );
}

[[nodiscard]] std::string formatBrowserLogs(const std::pair<std::string, std::string> &logs)
{
    return std::format(
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
        value = std::format("{}, bwrap_status={}", value, bwrapStatus);
    return value;
}

[[nodiscard]] bool isProcessRunning(std::optional<us::engine::subprocess::ChildProcess> &process)
{
    return process && !process->WaitFor(chrono::milliseconds(0));
}

[[nodiscard]] std::optional<String> readSanitizedLogTail(const std::string &path)
{
    try {
        if (!us::fs::blocking::FileExists(path))
            return {};

        const auto sanitized = crawler::sanitizeProcessOutputTail(
            us::fs::blocking::ReadFileContents(path)
        );
        if (!sanitized.empty())
            return sanitized;
    } catch (const std::runtime_error &) {
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

void removeBrowserRunDir(const std::string &path) noexcept
{
    if (path.empty())
        return;

    try {
        auto tempDir = us::fs::blocking::TempDirectory::Adopt(path);
        std::move(tempDir).Remove();
    } catch (const std::runtime_error &e) {
        LOG_WARNING() << std::format("Failed to remove browser dir {}: {}", path, e.what());
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
    auto parsed = String::fromBytes(value);
    if (!parsed)
        return {};
    return grabValueOf(parsed);
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

[[nodiscard]] us::engine::subprocess::ChildProcess spawnSandboxedBrowser(
    us::engine::subprocess::ProcessStarter &processStarter, const BrowserPaths &paths,
    std::string_view cgroupRootPath, const std::optional<crawler::CgroupLimits> &cgroupLimits
)
{
    const auto cpuCores = cgroupLimits ? cgroupLimits->cpuCores : 0_i64;
    const auto memoryBytes = cgroupLimits ? cgroupLimits->memoryBytes : 0_i64;
    const auto cgroupName = std::format("webshotd_crawler_{}", paths.runId);

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
        "--bind",
        "/sys/fs/cgroup",
        "/sys/fs/cgroup",
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
        std::format("{}", crawler::kProxyListenPort),
        std::format("{}", crawler::kDevtoolsPort),
        std::string(cgroupRootPath),
        cgroupName,
        std::format("{}", cpuCores),
        std::format("{}", memoryBytes),
        "--",
        "chromium",
    };
    bwrapArgs.insert(std::end(bwrapArgs), std::begin(chromiumArgs), std::end(chromiumArgs));

    auto args = std::vector<std::string>{
        "-c",
        std::string(crawler::kBwrapStatusWrapperScript),
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
        dns::Resolver &dnsResolver, usize urlBytesMax, i64 proxyDownBytesMax,
        us::engine::subprocess::ProcessStarter &processStarter, std::string browserRunsRootIn,
        std::string cgroupRootPath, std::optional<crawler::CgroupLimits> cgroupLimits,
        crawler::CrawlerTunables tunables, i64 cdpMaxRemotePayloadBytes
    )
        : dnsResolver(dnsResolver), urlBytesMax(urlBytesMax), proxyDownBytesMax(proxyDownBytesMax),
          processStarter(processStarter),
          browserRunsRoot(normalizeDirPath(std::move(browserRunsRootIn))),
          cgroupRootPath(std::move(cgroupRootPath)), cgroupLimits(std::move(cgroupLimits)),
          tunables(std::move(tunables)), cdpMaxRemotePayloadBytes(cdpMaxRemotePayloadBytes)
    {
    }

    ~BrowserInstance() { close(); }

    BrowserInstance(const BrowserInstance &) = delete;
    BrowserInstance(BrowserInstance &&) = delete;
    BrowserInstance &operator=(const BrowserInstance &) = delete;
    BrowserInstance &operator=(BrowserInstance &&) = delete;

    Expected<void, String> launch()
    {
        paths = createBrowserPaths(browserRunsRoot);
        markPhase("launch_browser");
        const auto devtoolsDeadline = us::engine::Deadline::FromDuration(
            tunables.devtoolsStartupTimeout
        );
        markPhase("start_proxy");
        proxy = std::make_unique<crawler::EgressProxy>(crawler::EgressProxyConfig{
            .socketPath = paths.proxySocketPath,
            .runId = paths.runId,
            .urlBytesMax = urlBytesMax,
            .downBytesMax = proxyDownBytesMax,
            .enableLocalFixtureRewrite = tunables.enableLocalFixtureRewrite,
        });
        auto proxyStarted = proxy->start(dnsResolver, devtoolsDeadline);
        if (!proxyStarted)
            return std::unexpected(text::format("proxy failed to start: {}", proxyStarted.error()));
        markPhase("start_proxy_done");
        process.emplace(spawnSandboxedBrowser(processStarter, paths, cgroupRootPath, cgroupLimits));
        auto ready = waitForDevtoolsPath(devtoolsDeadline);
        if (!ready)
            return std::unexpected(std::move(ready).error());
        websocketPath = grabValueOf(ready);
        return {};
    }

    [[nodiscard]] Expected<std::unique_ptr<crawler::CdpClient>, String>
    connectCdp(us::engine::Deadline overallDeadline) const
    {
        auto cdp = crawler::CdpClient::connect(
            paths.cdpSocketPath, websocketPath, paths.cdpTracePath, overallDeadline,
            tunables.cdpHandshakeTimeout, tunables.cdpCommandTimeout, tunables.cdpWaitPollInterval,
            cdpMaxRemotePayloadBytes
        );
        if (!cdp) {
            auto detail = describeCdpFailure("devtools websocket handshake failed"_t, cdp.error());
            return std::unexpected(text::format("{} ({})", detail, currentLaunchLogs()));
        }
        return grabValueOf(cdp);
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
        if (const auto websocketPathFromFile = readWebsocketPathFile(paths.websocketPathFilePath))
            appendDiagnosticField(diagnostics, "websocket_path", websocketPathFromFile->view());
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
        if (proxy) {
            appendDiagnosticField(
                diagnostics, "proxy_down_bytes", std::format("{}", proxy->downBytes())
            );
            if (const auto proxyFailure = proxy->failureReason())
                appendDiagnosticField(diagnostics, "proxy_failure", proxyFailure->view());
        }
        if (preservedDir)
            appendDiagnosticField(diagnostics, "preserved_browser_dir", preservedDir.value());

        if (diagnostics.empty())
            return std::string{message};
        return std::format("{}, {}", message, diagnostics);
    }

    void close()
    {
        if (closed)
            return;
        closed = true;

        stopProcess(process, tunables.browserStopTimeout);
        if (proxy)
            proxy->close();
        if (!paths.rootDir.empty() && !preserveRunDir)
            removeBrowserRunDir(paths.rootDir);
    }

    [[nodiscard]] i64 proxyDownBytes() const noexcept { return proxy ? proxy->downBytes() : 0_i64; }
    [[nodiscard]] const std::string &runId() const noexcept { return paths.runId; }
    [[nodiscard]] std::optional<String> proxyFailureReason() const noexcept
    {
        if (!proxy)
            return {};
        return proxy->failureReason();
    }

private:
    [[nodiscard]] Expected<String, String> waitForDevtoolsPath(us::engine::Deadline deadline)
    {
        UINVARIANT(deadline.IsReachable(), "devtools deadline must be reachable");
        auto sawCdpSocket = false;
        auto sawWebsocketPath = false;
        while (!deadline.IsReached()) {
            sawCdpSocket = sawCdpSocket || us::fs::blocking::FileExists(paths.cdpSocketPath);
            sawWebsocketPath = sawWebsocketPath ||
                               us::fs::blocking::FileExists(paths.websocketPathFilePath);
            if (process && process->WaitFor(chrono::milliseconds(0))) {
                return std::unexpected(
                    text::format(
                        "chromium exited before exposing devtools ({})", currentLaunchLogs()
                    )
                );
            }
            auto websocketPathFromFile = readWebsocketPathFile(paths.websocketPathFilePath);
            if (sawCdpSocket && websocketPathFromFile) {
                return grabValueOf(websocketPathFromFile);
            }
            us::engine::SleepFor(tunables.devtoolsPollInterval);
        }
        if (process && process->WaitFor(chrono::milliseconds(0))) {
            return std::unexpected(
                text::format("chromium exited before exposing devtools ({})", currentLaunchLogs())
            );
        }
        return std::unexpected(
            text::format(
                "{} ({})",
                !sawWebsocketPath ? "devtools websocket path was never written"
                : !sawCdpSocket
                    ? "devtools websocket path was written but cdp socket never appeared"
                    : "devtools websocket path and cdp socket appeared but handshake never started",
                currentLaunchLogs()
            )
        );
    }

    dns::Resolver &dnsResolver;
    usize urlBytesMax;
    i64 proxyDownBytesMax;
    us::engine::subprocess::ProcessStarter &processStarter;
    std::string browserRunsRoot;
    std::string cgroupRootPath;
    std::optional<crawler::CgroupLimits> cgroupLimits;
    crawler::CrawlerTunables tunables;
    i64 cdpMaxRemotePayloadBytes;
    BrowserPaths paths;
    std::unique_ptr<crawler::EgressProxy> proxy;
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

struct [[nodiscard]] CaptureWithNetwork final {
    crawler::CapturedExchange exchange;
    i64 proxyDownBytes;
};

[[nodiscard]] Expected<std::string, String>
retainBody(const std::string &body, RetainedBodyBudget &budget)
{
    const auto nextRetainedBytes = budget.retainedBytes + i64(body.size());
    if (nextRetainedBytes > budget.maxBytes)
        return std::unexpected(
            text::format(
                "size_limit: retained body bytes {} exceeded size limit {}", nextRetainedBytes,
                budget.maxBytes
            )
        );
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
        return canonicalizeCapturedUrl(requestUrl);

    const auto headers = normalizeHeadersForCapture(redirectResponse->headers, baseUrl);
    const auto locationIt = headers.find("location");
    if (locationIt == std::end(headers) || locationIt->second.empty())
        return canonicalizeCapturedUrl(requestUrl);

    const auto location = String::fromBytes(locationIt->second);
    if (!location)
        return canonicalizeCapturedUrl(requestUrl);
    if (const auto absoluteLocation = Url::fromText(location.value()))
        return canonicalizeCapturedUrl(absoluteLocation->href());

    const auto origin = buildUrlOrigin(baseUrl);
    if (!origin)
        return canonicalizeCapturedUrl(requestUrl);

    if (location->startsWith("//")) {
        const auto maybeBaseUrl = Url::fromText(baseUrl);
        if (!maybeBaseUrl)
            return canonicalizeCapturedUrl(requestUrl);
        return canonicalizeCapturedUrl(
            text::format("{}:{}", maybeBaseUrl->isHttps() ? "https" : "http", location.value())
        );
    }

    if (location->startsWith('/'))
        return canonicalizeCapturedUrl(text::format("{}{}", origin.value(), location.value()));

    if (location->startsWith('?')) {
        const auto maybeBaseUrl = Url::fromText(baseUrl);
        if (!maybeBaseUrl)
            return canonicalizeCapturedUrl(requestUrl);
        return canonicalizeCapturedUrl(
            text::format("{}{}{}", origin.value(), maybeBaseUrl->pathname(), location.value())
        );
    }

    return canonicalizeCapturedUrl(requestUrl);
}

[[nodiscard]] std::optional<String> normalizeInterceptedUrlForDenylist(const String &requestUrl)
{
    if (requestUrl.startsWith("ws://")) {
        auto normalized = String::fromBytes("http://" + std::string(requestUrl.view().substr(5)));
        if (!normalized)
            return {};
        return grabValueOf(normalized);
    }
    if (requestUrl.startsWith("wss://")) {
        auto normalized = String::fromBytes("https://" + std::string(requestUrl.view().substr(6)));
        if (!normalized)
            return {};
        return grabValueOf(normalized);
    }

    const auto parsed = Url::fromText(requestUrl);
    if (!parsed)
        return {};
    if (!parsed->isHttp() && !parsed->isHttps())
        return {};
    return requestUrl;
}

[[nodiscard]] Expected<bool, String>
isAllowedByDenylist(Denylist &denylist, const Config &config, const String &requestUrl)
{
    const auto normalized = normalizeInterceptedUrlForDenylist(requestUrl);
    if (!normalized)
        return true;

    const auto link = Link::fromText(
        normalized.value(), config.urlBytesMax(), Link::FromTextOptions::kStripPort
    );
    if (!link)
        return std::unexpected(
            text::format("failed to normalize intercepted request url {}", normalized->view())
        );

    const auto allowed = denylist.isAllowedPrefix(prefix::makePrefixKey(link.value()));
    if (!allowed)
        return std::unexpected("denylist check failed during fetch interception"_t);
    return allowed.value();
}

[[nodiscard]] std::vector<dto::FetchHeaderEntry> buildBlockedFetchHeaders(usize bodyBytes)
{
    std::vector<dto::FetchHeaderEntry> headers;
    headers.push_back(
        dto::FetchHeaderEntry{
            .name = "Content-Type",
            .value = "text/plain; charset=utf-8",
        }
    );
    headers.push_back(
        dto::FetchHeaderEntry{
            .name = "Content-Length",
            .value = std::format("{}", bodyBytes),
        }
    );
    headers.push_back(
        dto::FetchHeaderEntry{
            .name = "Cache-Control",
            .value = "no-store",
        }
    );
    return headers;
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
    PageTracker(String sessionId, String targetId)
        : sessionId(std::move(sessionId)), targetId(std::move(targetId)), pageId(generatePageId())
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
            auto parsed = parseEventParams<dto::NetworkRequestWillBeSentEvent>(event);
            if (!parsed)
                mainRequestFailure = parsed.error();
            else
                handleRequestWillBeSent(grabValueOf(parsed));
            return;
        }
        if (method == "Network.responseReceived") {
            auto parsed = parseEventParams<dto::NetworkResponseReceivedEvent>(event);
            if (!parsed)
                mainRequestFailure = parsed.error();
            else
                handleResponseReceived(grabValueOf(parsed));
            return;
        }
        if (method == "Network.loadingFinished") {
            auto parsed = parseEventParams<dto::NetworkLoadingFinishedEvent>(event);
            if (!parsed)
                mainRequestFailure = parsed.error();
            else
                handleLoadingFinished(grabValueOf(parsed));
            return;
        }
        if (method == "Network.loadingFailed") {
            auto parsed = parseEventParams<dto::NetworkLoadingFailedEvent>(event);
            if (!parsed)
                mainRequestFailure = parsed.error();
            else
                handleLoadingFailed(grabValueOf(parsed));
        }
    }

    [[nodiscard]] Expected<void, String>
    waitForLoad(crawler::CdpClient &cdp, us::engine::Deadline deadline)
    {
        auto waited = cdp.waitUntil(
            [this]() { return loaded || mainRequestFailure.has_value(); }, deadline,
            "timed out waiting for page load"
        );
        if (!waited) {
            using enum crawler::CdpError;
            const auto action = waited.error().code == kTimeout
                                    ? "timed out waiting for page load"_t
                                    : "failed while waiting for page load"_t;
            return std::unexpected(describeCdpFailure(action, waited.error()));
        }
        if (mainRequestFailure)
            return std::unexpected(mainRequestFailure.value());
        return {};
    }

    [[nodiscard]] Expected<void, String>
    waitForMainDocument(crawler::CdpClient &cdp, us::engine::Deadline deadline)
    {
        auto waited = cdp.waitUntil(
            [this]() {
                const auto *request = activeMainRequest();
                return mainRequestFailure.has_value() ||
                       (completedMainRequest.has_value() && completedMainRequest->loaded &&
                        hasResponse(completedMainRequest.value())) ||
                       (request != nullptr && hasResponse(*request) && request->loaded);
            },
            deadline, "timed out waiting for main document response"
        );
        if (!waited) {
            using enum crawler::CdpError;
            const auto action = waited.error().code == kTimeout
                                    ? "timed out waiting for main document response"_t
                                    : "failed while waiting for main document response"_t;
            return std::unexpected(describeCdpFailure(action, waited.error()));
        }
        if (mainRequestFailure)
            return std::unexpected(mainRequestFailure.value());
        return {};
    }

    [[nodiscard]] Expected<void, String>
    waitForIdle(crawler::CdpClient &cdp, chrono::seconds idle, us::engine::Deadline deadline)
    {
        auto waited = cdp.waitUntil(
            [this, idle]() {
                return inflight.empty() && us::utils::datetime::SteadyNow() - lastNetworkAt >= idle;
            },
            deadline, "timed out waiting for network idle"
        );
        if (!waited) {
            using enum crawler::CdpError;
            const auto action = waited.error().code == kTimeout
                                    ? "timed out waiting for network idle"_t
                                    : "failed while waiting for network idle"_t;
            return std::unexpected(describeCdpFailure(action, waited.error()));
        }
        return {};
    }

    [[nodiscard]] std::optional<crawler::SeedPageProbe> currentSeedProbe() const
    {
        if (const auto *request = resolvedMainRequest();
            request != nullptr && request->statusCode) {
            const auto loadState = request->loaded && !mainRequestFailure ? 2_i64 : 0_i64;
            return crawler::SeedPageProbe{
                .status = raw(request->statusCode.value()),
                .loadState = raw(loadState),
            };
        }

        if (mainRequestId || mainRequestFailure || loaded)
            return crawler::SeedPageProbe{.status = raw(0_i64), .loadState = raw(0_i64)};

        return {};
    }

    [[nodiscard]] const std::optional<String> &failureReason() const { return mainRequestFailure; }
    void fail(String reason) { mainRequestFailure = std::move(reason); }

    [[nodiscard]] Expected<std::string, String> readBody(
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
            if (!body)
                return retainBody(fallbackBody, budget);
            return retainBody(
                body->base64Encoded ? us::crypto::base64::Base64Decode(body->body) : body->body,
                budget
            );
        } catch (const us::crypto::CryptoException &) {
            return retainBody(fallbackBody, budget);
        }
    }

    [[nodiscard]] Expected<std::vector<crawler::CapturedResource>, String> readResources(
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
                if (!bodyValue) {
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
                auto body = retainBody(
                    bodyValue->base64Encoded ? us::crypto::base64::Base64Decode(bodyValue->body)
                                             : bodyValue->body,
                    budget
                );
                if (!body)
                    return std::unexpected(std::move(body).error());
                resources.push_back({
                    request.requestUrl,
                    request.method,
                    request.resourceType,
                    response.statusCode,
                    response.statusMessage,
                    response.headers,
                    grabValueOf(body),
                    response.timestamp,
                });
            } catch (const us::crypto::CryptoException &) {
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

        std::ranges::sort(resources, [](const auto &left, const auto &right) {
            return left.timestamp < right.timestamp;
        });
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

        const auto requestIdText = String::fromBytes(requestWillBeSent.requestId).expect();
        const auto rawRequestUrl = String::fromBytes(requestWillBeSent.request.url).expect();
        const auto requestMethod = String::fromBytes(requestWillBeSent.request.method).expect();

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
                                             : canonicalizeCapturedUrl(rawRequestUrl);

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
        const auto requestIdText = String::fromBytes(responseReceived.requestId).expect();
        const auto requestIt = activeRequests.find(requestIdText);
        if (requestIt == std::end(activeRequests)) {
            if (mainRequestId && mainRequestId.value() == requestIdText) {
                UINVARIANT(
                    false, std::format(
                               "main document response received for unknown request id {}",
                               requestIdText.view()
                           )
                );
            }
            return;
        }

        const auto timestamp = currentTimestamp();
        auto &request = requestIt->second;
        request.statusCode = responseReceived.response.status
                                 ? i64(responseReceived.response.status.value())
                                 : 0_i64;
        request.statusMessage =
            String::fromBytes(responseReceived.response.statusText.value_or("")).expect();
        request.headers = normalizeHeadersForCapture(
            responseReceived.response.headers, request.requestUrl
        );
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
        const auto requestIdText = String::fromBytes(loadingFinished.requestId).expect();
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
                std::format(
                    "main document loading finished for unknown request id {}", requestIdText.view()
                )
            );
        }
    }

    void handleLoadingFailed(dto::NetworkLoadingFailedEvent loadingFailed)
    {
        const auto requestIdText = String::fromBytes(loadingFailed.requestId).expect();
        inflight.erase(requestIdText);
        lastNetworkAt = us::utils::datetime::SteadyNow();

        const auto requestIt = activeRequests.find(requestIdText);
        if (requestIt == std::end(activeRequests)) {
            if (mainRequestId && mainRequestId.value() == requestIdText) {
                UINVARIANT(
                    false, std::format(
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

        mainRequestFailure = String::fromBytes(
                                 loadingFailed.errorText.value_or("main document request failed")
        )
                                 .expect();
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
            std::format("redirect response for unknown request id {}", requestId.view())
        );

        auto request = std::move(requestIt->second);
        activeRequests.erase(requestIt);

        request.statusCode = i64(redirectResponse->status.value());
        request.statusMessage =
            String::fromBytes(redirectResponse->statusText.value_or("")).expect();
        request.headers = normalizeHeadersForCapture(redirectResponse->headers, request.requestUrl);
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
            std::format(
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
            hasResponse(request), std::format(
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

[[nodiscard]] Expected<DomState, String>
readDomState(crawler::CdpClient &cdp, const String &sessionId)
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
    if (!result)
        return std::unexpected(describeCdpFailure("failed to read dom state"_t, result.error()));
    const auto &value = result->result.value;
    auto title = value.title.transform([](const auto &t) -> std::optional<String> {
        auto parsed = String::fromBytes(t);
        if (!parsed)
            return {};
        return grabValueOf(parsed);
    });
    auto finalUrl = String::fromBytes(value.finalUrl);
    if (!finalUrl)
        return std::unexpected("Runtime.evaluate returned invalid finalUrl"_t);
    finalUrl = canonicalizeCapturedUrl(finalUrl.value());
    return DomState{
        .finalUrl = grabValueOf(finalUrl),
        .title = title.value_or(std::nullopt),
        .html = value.html,
    };
}

Expected<void, String>
runSiteBehavior(crawler::CdpClient &cdp, const String &sessionId, us::engine::Deadline deadline)
{
    UINVARIANT(deadline.IsReachable(), "site behavior deadline must be reachable");
    auto budgetExpected = timeLeftMs(deadline);
    if (!budgetExpected)
        return std::unexpected("timed out running site behavior"_t);
    const auto budget = grabValueOf(budgetExpected);

    dto::RuntimeEvaluateParams params;
    params.expression = std::format(
        "(() => new Promise((resolve) => {{ const startedAt = Date.now(); const stepDelayMs = "
        "100; const maxSteps = Math.max(1, Math.floor({0} / stepDelayMs)); let steps = 0; const "
        "tick = () => {{ const root = document.scrollingElement || document.documentElement || "
        "document.body; if (!root) {{ resolve(true); return; }} const previous = root.scrollTop; "
        "root.scrollBy(0, Math.max(window.innerHeight, 600)); steps++; const exhausted = "
        "Date.now() - startedAt >= {0} || steps >= maxSteps; const stuck = root.scrollTop === "
        "previous; if (stuck || exhausted) {{ root.scrollTo(0, 0); resolve(true); return; }} "
        "setTimeout(tick, stepDelayMs); }}; tick(); }}))()",
        budget.count()
    );
    params.awaitPromise = true;
    params.returnByValue = true;
    const auto result = cdp.send<json::Value>("Runtime.evaluate", params, sessionId);
    if (!result)
        return std::unexpected(describeCdpFailure("failed to run site behavior"_t, result.error()));
    return {};
}

class [[nodiscard]] CaptureSession final {
public:
    CaptureSession(
        Denylist &denylist, const Config &config, dns::Resolver &dnsResolver, usize urlBytesMax,
        i64 proxyDownBytesMax, us::engine::subprocess::ProcessStarter &processStarter,
        std::string browserRunsRootIn, std::string cgroupRootPathIn,
        std::optional<crawler::CgroupLimits> cgroupLimitsIn, crawler::CaptureTimings timings,
        crawler::CrawlerTunables tunablesIn, i64 maxArchiveBytesIn, us::engine::Deadline deadline,
        crawler::RunRequest run
    )
        : denylist(denylist), config(config), timings(std::move(timings)), run(std::move(run)),
          deadline(deadline), maxArchiveBytes(maxArchiveBytesIn),
          browser(
              dnsResolver, urlBytesMax, proxyDownBytesMax, processStarter,
              std::move(browserRunsRootIn), std::move(cgroupRootPathIn), std::move(cgroupLimitsIn),
              std::move(tunablesIn), computeCdpMaxRemotePayloadBytes(maxArchiveBytesIn)
          )
    {
    }

    [[nodiscard]] Expected<CaptureWithNetwork, CaptureFailure> capture()
    {
        auto launched = launch();
        if (!launched) {
            auto failureDetail = String::fromBytes(
                                     browser.buildRuntimeFailureDetail(launched.error().view())
            )
                                     .expect();
            closeCdpForFailure();
            browser.close();
            return std::unexpected(CaptureFailure{std::move(failureDetail), {}});
        }

        auto captured = captureAttachedTarget();
        if (!captured) {
            auto failureDetail = browser.buildRuntimeFailureDetail(captured.error().view());
            if (tracker && tracker->failureReason()) {
                failureDetail = std::format(
                    "{}, tracker_failure={}", failureDetail, tracker->failureReason()->view()
                );
            }
            if (const auto proxyFailure = browser.proxyFailureReason())
                failureDetail = std::format(
                    "{}, proxy_failure={}", failureDetail, proxyFailure->view()
                );
            auto seedProbe = currentSeedProbe();
            closeCdpForFailure();
            browser.close();
            return std::unexpected(
                CaptureFailure{
                    String::fromBytes(failureDetail).expect(),
                    std::move(seedProbe),
                }
            );
        }
        auto value = grabValueOf(captured);
        return CaptureWithNetwork{
            .exchange = std::move(value),
            .proxyDownBytes = browser.proxyDownBytes(),
        };
    }

private:
    template <typename T, typename... Args>
    [[nodiscard]] Expected<T, String> send(std::string_view method, Args &&...args) const
    {
        auto result = cdpClient().send<T>(method, std::forward<Args>(args)...);
        if (!result)
            return std::unexpected(
                describeCdpFailure(text::format("{} failed", method), result.error())
            );
        return grabValueOf(result);
    }

    template <typename... Args>
    [[nodiscard]] Expected<void, String> sendVoid(std::string_view method, Args &&...args) const
    {
        auto result = send<dto::CdpEmptyObject>(method, std::forward<Args>(args)...);
        if (!result)
            return std::unexpected(result.error());
        return {};
    }

    [[nodiscard]] Expected<void, String> launch()
    {
        auto launched = browser.launch();
        if (!launched)
            return std::unexpected(launched.error());
        browser.markPhase("connect_cdp");
        auto connected = browser.connectCdp(deadline);
        if (!connected)
            return std::unexpected(connected.error());
        cdp = grabValueOf(connected);

        browser.markPhase("create_browser_context");
        auto browserContext = send<dto::TargetCreateBrowserContextResult>(
            "Target.createBrowserContext"
        );
        if (!browserContext)
            return std::unexpected(browserContext.error());
        browserContextId = String::fromBytes(browserContext->browserContextId).expect();

        browser.markPhase("create_target");
        dto::TargetCreateTargetParams targetParams;
        targetParams.url = "about:blank";
        targetParams.browserContextId = std::string(browserContextId->view());
        auto target = send<dto::TargetCreateTargetResult>("Target.createTarget", targetParams);
        if (!target)
            return std::unexpected(target.error());
        targetId = String::fromBytes(target->targetId).expect();

        browser.markPhase("attach_target");
        dto::TargetAttachToTargetParams attachParams;
        attachParams.targetId = std::string(targetId->view());
        attachParams.flatten = true;
        auto attached = send<dto::TargetAttachToTargetResult>(
            "Target.attachToTarget", attachParams
        );
        if (!attached)
            return std::unexpected(attached.error());
        sessionId = String::fromBytes(attached->sessionId).expect();
        tracker = std::make_unique<PageTracker>(sessionId.value(), targetId.value());
        listenerId = cdpClient().addListener([this](crawler::CdpEvent event) {
            if (event.method == "Fetch.authRequired"_t) {
                handleFetchAuthRequired(event);
                return;
            }
            if (event.method == "Fetch.requestPaused"_t) {
                handleFetchRequestPaused(event);
                return;
            }
            if (tracker)
                tracker->handleEvent(event);
        });
        return {};
    }

    [[nodiscard]] Expected<crawler::CapturedExchange, String> captureAttachedTarget()
    {
        browser.markPhase("enable_page");
        if (auto ok = sendVoid("Page.enable", attachedSessionId()); !ok)
            return std::unexpected(ok.error());
        browser.markPhase("enable_runtime");
        if (auto ok = sendVoid("Runtime.enable", attachedSessionId()); !ok)
            return std::unexpected(ok.error());
        browser.markPhase("enable_network");
        if (auto ok = sendVoid("Network.enable", attachedSessionId()); !ok)
            return std::unexpected(ok.error());

        browser.markPhase("enable_fetch");
        dto::FetchEnableParams fetchParams;
        fetchParams.handleAuthRequests = true;
        if (auto ok = sendVoid("Fetch.enable", fetchParams, attachedSessionId()); !ok)
            return std::unexpected(ok.error());

        browser.markPhase("enable_lifecycle_events");
        dto::PageSetLifecycleEventsEnabledParams lifecycleParams;
        lifecycleParams.enabled = true;
        if (auto ok =
                sendVoid("Page.setLifecycleEventsEnabled", lifecycleParams, attachedSessionId());
            !ok)
            return std::unexpected(ok.error());

        browser.markPhase("disable_cache");
        dto::NetworkSetCacheDisabledParams cacheParams;
        cacheParams.cacheDisabled = true;
        if (auto ok = sendVoid("Network.setCacheDisabled", cacheParams, attachedSessionId()); !ok)
            return std::unexpected(ok.error());

        browser.markPhase("bypass_service_worker");
        dto::NetworkSetBypassServiceWorkerParams serviceWorkerParams;
        serviceWorkerParams.bypass = true;
        if (auto ok = sendVoid(
                "Network.setBypassServiceWorker", serviceWorkerParams, attachedSessionId()
            );
            !ok)
            return std::unexpected(ok.error());

        browser.markPhase("set_extra_headers");
        dto::NetworkSetExtraHTTPHeadersParams headerParams;
        headerParams.headers.extra.emplace(
            "Accept-Language", std::string(crawler::kBrowserAcceptLanguage)
        );
        if (auto ok = sendVoid("Network.setExtraHTTPHeaders", headerParams, attachedSessionId());
            !ok)
            return std::unexpected(ok.error());

        browser.markPhase("get_frame_tree");
        auto frameTree = send<dto::PageGetFrameTreeResult>(
            "Page.getFrameTree", attachedSessionId()
        );
        if (!frameTree)
            return std::unexpected(frameTree.error());
        pageTracker().mainFrameId = String::fromBytes(frameTree->frameTree.frame.id).expect();

        browser.markPhase("navigate");
        dto::PageNavigateParams navigateParams;
        navigateParams.url = std::string(run.seedUrl.view());
        pageTracker().beginSeedNavigation(run.seedUrl);
        auto navigateResult = send<dto::PageNavigateResult>(
            "Page.navigate", navigateParams, attachedSessionId()
        );
        if (!navigateResult)
            return std::unexpected(navigateResult.error());
        if (navigateResult->errorText)
            return std::unexpected(String::fromBytes(navigateResult->errorText.value()).expect());
        pageTracker().setExpectedMainLoaderId(stringOrNull(navigateResult->loaderId));

        browser.markPhase("wait_for_load");
        if (auto waited = pageTracker().waitForLoad(cdpClient(), deadline); !waited)
            return std::unexpected(waited.error());
        if (timings.postLoadDelay > chrono::seconds::zero()) {
            browser.markPhase("post_load_delay");
            const auto phaseDeadline = pickEarlierDeadline(
                deadline, us::engine::Deadline::FromDuration(timings.postLoadDelay)
            );
            const auto ok = sleepUntilDeadline(phaseDeadline);
            if (!ok)
                return std::unexpected("timed out waiting for post-load delay"_t);
            browser.markPhase("post_load_delay_done");
        }
        if (timings.behaviorTimeout > chrono::seconds::zero()) {
            browser.markPhase("run_site_behavior");
            browser.markPhase("run_site_behavior_runtime_evaluate");
            const auto behaviorDeadline = pickEarlierDeadline(
                deadline, us::engine::Deadline::FromDuration(timings.behaviorTimeout)
            );
            auto ranSiteBehavior = runSiteBehavior(
                cdpClient(), attachedSessionId(), behaviorDeadline
            );
            if (!ranSiteBehavior)
                return std::unexpected(ranSiteBehavior.error());
            browser.markPhase("run_site_behavior_done");
        }
        if (timings.netIdleWait > chrono::seconds::zero()) {
            browser.markPhase("wait_for_idle");
            browser.markPhase("wait_for_idle_wait");
            auto waited = pageTracker().waitForIdle(cdpClient(), timings.netIdleWait, deadline);
            if (!waited)
                return std::unexpected(waited.error());
            browser.markPhase("wait_for_idle_done");
        }
        if (timings.pageExtraDelay > chrono::seconds::zero()) {
            browser.markPhase("page_extra_delay");
            const auto phaseDeadline = pickEarlierDeadline(
                deadline, us::engine::Deadline::FromDuration(timings.pageExtraDelay)
            );
            const auto ok = sleepUntilDeadline(phaseDeadline);
            if (!ok)
                return std::unexpected("timed out waiting for extra page delay"_t);
            browser.markPhase("page_extra_delay_done");
        }
        browser.markPhase("wait_for_main_document");
        browser.markPhase("wait_for_main_document_wait");
        if (auto waited = pageTracker().waitForMainDocument(cdpClient(), deadline); !waited)
            return std::unexpected(waited.error());
        browser.markPhase("wait_for_main_document_done");

        browser.markPhase("read_dom_state");
        browser.markPhase("read_dom_state_runtime_evaluate");
        auto domState = readDomState(cdpClient(), attachedSessionId());
        if (!domState)
            return std::unexpected(domState.error());
        browser.markPhase("read_dom_state_done");
        RetainedBodyBudget budget{maxArchiveBytes, 0_i64};
        browser.markPhase("read_main_body");
        auto body = pageTracker().readBody(
            cdpClient(), attachedSessionId(), budget, domState->html
        );
        if (!body)
            return std::unexpected(body.error());
        browser.markPhase("read_resources");
        auto resources = pageTracker().readResources(cdpClient(), attachedSessionId(), budget);
        if (!resources)
            return std::unexpected(resources.error());

        removeTrackerListener();
        if (auto detached = detachTarget(); !detached)
            return std::unexpected(detached.error());
        if (auto disposed = disposeBrowserContext(); !disposed)
            return std::unexpected(disposed.error());

        browser.markPhase("build_exchange_start");
        LOG_INFO() << std::format(
            "captureViaProxy building exchange for {} (body_bytes={}, resources={})", run.seedUrl,
            body->size(), resources->size()
        );
        auto exchange = pageTracker().buildExchange(
            std::move(domState->finalUrl), std::move(domState->title), grabValueOf(body),
            grabValueOf(resources)
        );
        browser.markPhase("build_exchange_done");
        LOG_INFO() << std::format(
            "captureViaProxy built exchange for {} (status={}, resources={}, body_bytes={})",
            run.seedUrl, exchange.statusCode, exchange.resources.size(), exchange.body.size()
        );
        tracker.reset();
        cdp.reset();

        browser.markPhase("close_browser_success");
        browser.markPhase("before_browser_close");
        LOG_INFO() << std::format("captureViaProxy closing browser for {}", run.seedUrl);
        browser.close();
        if (const auto proxyFailure = browser.proxyFailureReason())
            return std::unexpected(proxyFailure.value());
        LOG_INFO() << std::format("captureViaProxy returning capture for {}", run.seedUrl);
        return exchange;
    }

    [[nodiscard]] Expected<void, String> detachTarget()
    {
        if (!sessionId)
            return {};

        browser.markPhase("detach_target");
        dto::TargetDetachFromTargetParams detachParams;
        detachParams.sessionId = std::string(sessionId->view());
        auto detached = sendVoid("Target.detachFromTarget", detachParams);
        if (!detached)
            return std::unexpected(detached.error());
        sessionId.reset();
        return {};
    }

    [[nodiscard]] Expected<void, String> disposeBrowserContext()
    {
        if (!browserContextId)
            return {};

        browser.markPhase("dispose_browser_context");
        dto::TargetDisposeBrowserContextParams disposeParams;
        disposeParams.browserContextId = std::string(browserContextId->view());
        auto disposed = sendVoid("Target.disposeBrowserContext", disposeParams);
        if (!disposed)
            return std::unexpected(disposed.error());
        browserContextId.reset();
        targetId.reset();
        return {};
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

        if (auto closed = cdp->close(); !closed) {
            LOG_WARNING() << std::format(
                "Suppressing CDP close failure during capture cleanup: code={}{}",
                numericCast<int>(closed.error().code),
                closed.error().detail ? std::format(", detail={}", closed.error().detail.value())
                                      : std::string{}
            );
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

    void noteInterceptionFailure(String reason)
    {
        if (interceptionFailure)
            return;
        if (tracker)
            tracker->fail(reason);
        interceptionFailure = std::move(reason);
    }

    void handleFetchAuthRequired(const crawler::CdpEvent &event)
    {
        if (!sessionId || !event.sessionId || event.sessionId->view() != sessionId->view())
            return;

        const auto authRequired = parseEventParams<dto::FetchAuthRequiredEvent>(event);
        if (!authRequired) {
            noteInterceptionFailure(authRequired.error());
            return;
        }

        dto::FetchContinueWithAuthParams params;
        params.requestId = authRequired->requestId;

        dto::FetchAuthChallengeResponse authChallengeResponse;
        const auto isProxyChallenge = !authRequired->authChallenge.source ||
                                      authRequired->authChallenge.source.value() == "Proxy";
        if (isProxyChallenge) {
            authChallengeResponse.response = "ProvideCredentials";
            authChallengeResponse.username = browser.runId();
            authChallengeResponse.password = "x";
        } else {
            authChallengeResponse.response = "Default";
        }
        params.authChallengeResponse = std::move(authChallengeResponse);

        const auto continued = cdpClient().sendNoWait(
            "Fetch.continueWithAuth", params, attachedSessionId()
        );
        if (!continued)
            noteInterceptionFailure(
                describeCdpFailure("Fetch.continueWithAuth failed"_t, continued.error())
            );
    }

    void handleFetchRequestPaused(const crawler::CdpEvent &event)
    {
        if (!sessionId || !event.sessionId || event.sessionId->view() != sessionId->view())
            return;

        const auto paused = parseEventParams<dto::FetchRequestPausedEvent>(event);
        if (!paused) {
            noteInterceptionFailure(paused.error());
            return;
        }

        const auto requestUrl = String::fromBytes(paused->request.url);
        if (!requestUrl) {
            noteInterceptionFailure("Fetch.requestPaused contained invalid request url"_t);
            return;
        }

        const auto allowed = isAllowedByDenylist(denylist, config, requestUrl.value());
        if (!allowed) {
            noteInterceptionFailure(allowed.error());
            return;
        }

        if (allowed.value()) {
            dto::FetchContinueRequestParams params;
            params.requestId = paused->requestId;
            const auto continued = cdpClient().sendNoWait(
                "Fetch.continueRequest", params, attachedSessionId()
            );
            if (!continued)
                noteInterceptionFailure(
                    describeCdpFailure("Fetch.continueRequest failed"_t, continued.error())
                );
            return;
        }

        static constexpr std::string_view kBody = "Blocked by webshot denylist\n";
        dto::FetchFulfillRequestParams params;
        params.requestId = paused->requestId;
        params.responseCode = 403;
        params.responsePhrase = "Forbidden";
        params.responseHeaders = buildBlockedFetchHeaders(kBody.size());
        params.body = us::crypto::base64::Base64Encode(kBody);
        const auto fulfilled = cdpClient().sendNoWait(
            "Fetch.fulfillRequest", params, attachedSessionId()
        );
        if (!fulfilled)
            noteInterceptionFailure(
                describeCdpFailure("Fetch.fulfillRequest failed"_t, fulfilled.error())
            );
    }

    Denylist &denylist;
    const Config &config;
    crawler::CaptureTimings timings;
    crawler::RunRequest run;
    us::engine::Deadline deadline;
    i64 maxArchiveBytes;
    BrowserInstance browser;
    std::unique_ptr<crawler::CdpClient> cdp;
    std::unique_ptr<PageTracker> tracker;
    std::optional<String> browserContextId;
    std::optional<String> targetId;
    std::optional<String> sessionId;
    std::optional<crawler::CdpClient::ListenerId> listenerId;
    std::optional<String> interceptionFailure;
};

[[nodiscard]] Expected<CaptureWithNetwork, CaptureFailure> captureViaProxy(
    Denylist &denylist, const Config &config, dns::Resolver &dnsResolver, usize urlBytesMax,
    i64 proxyDownBytesMax, us::engine::subprocess::ProcessStarter &processStarter,
    const std::string &browserRunsRoot, const std::string &cgroupRootPath,
    std::optional<crawler::CgroupLimits> cgroupLimits, crawler::CaptureTimings timings,
    const crawler::CrawlerTunables &tunables, i64 maxArchiveBytes, us::engine::Deadline deadline,
    const crawler::RunRequest &run
)
{
    auto session = CaptureSession(
        denylist, config, dnsResolver, urlBytesMax, proxyDownBytesMax, processStarter,
        std::string(browserRunsRoot), std::string(cgroupRootPath), std::move(cgroupLimits),
        std::move(timings), tunables, maxArchiveBytes, deadline,
        crawler::RunRequest{.seedUrl = run.seedUrl}
    );
    return session.capture();
}

[[nodiscard]] CrawlerRunArtifacts executeRun(
    Denylist &denylist, const Config &config, dns::Resolver &dnsResolver,
    us::engine::subprocess::ProcessStarter &processStarter, const std::string &browserRunsRoot,
    const std::string &cgroupRootPath, std::optional<crawler::CgroupLimits> cgroupLimits,
    const crawler::CaptureTimings &timings, const crawler::CrawlerTunables &tunables,
    i64 maxArchiveBytes, i64 networkDownBytesRatioMax, us::engine::Deadline deadline,
    const crawler::RunRequest &run
)
{
    CrawlerRunArtifacts out;
    out.attempt.exited = true;
    try {
        LOG_INFO() << std::format("crawler executeRun starting for {}", run.seedUrl);

        const auto failArtifact =
            [&out](const crawler::ArtifactFailure &failure) -> CrawlerRunArtifacts {
            auto failureDetailOpt = String::fromBytes(failure.detail)
                                        .transform([](String s) -> std::optional<String> {
                                            return {std::move(s)};
                                        })
                                        .valueOr(std::nullopt);
            out.attempt.exitCode = 9;
            out.attempt.waczExists = false;
            out.attempt.seedProbe.reset();
            out.attempt.failureDetail = std::move(failureDetailOpt);
            out.stdoutLog.clear();
            out.stderrLog = failure.detail + "\n";
            out.wacz.reset();
            out.pagesJsonl.reset();
            out.contentSha256.reset();
            return out;
        };

        const auto maxDownBytes = [&]() -> i64 {
            const auto max = maxArchiveBytes;
            const auto ratio = networkDownBytesRatioMax;
            const auto maxI64 = std::numeric_limits<i64>::max();
            if (ratio > maxI64 / max)
                return maxI64;
            return ratio * max;
        }();

        auto captured = captureViaProxy(
            denylist, config, dnsResolver, config.urlBytesMax(), maxDownBytes, processStarter,
            browserRunsRoot, cgroupRootPath, std::move(cgroupLimits), timings, tunables,
            maxArchiveBytes, deadline, run
        );
        if (!captured) {
            constexpr std::string_view kSizeLimitPrefix = "size_limit:";
            constexpr std::string_view kNetLimitPrefix = "net_limit:";
            if (captured.error().detail.startsWith(kSizeLimitPrefix)) {
                auto detailText = captured.error().detail.view();
                detailText.remove_prefix(kSizeLimitPrefix.size());
                if (!detailText.empty() && detailText.front() == ' ')
                    detailText.remove_prefix(1);
                auto parsed = String::fromBytes(std::string(detailText));
                out.attempt.exitCode = us::utils::UnderlyingValue(
                    crawler::CrawlerExitCode::kSizeLimit
                );
                out.attempt.waczExists = false;
                out.attempt.seedProbe = captured.error().seedProbe;
                out.attempt.failureDetail.reset();
                if (parsed)
                    out.attempt.failureDetail = grabValueOf(parsed);
            } else if (captured.error().detail.startsWith(kNetLimitPrefix)) {
                out.attempt.exitCode = us::utils::UnderlyingValue(
                    crawler::CrawlerExitCode::kFailedLimit
                );
                out.attempt.waczExists = false;
                out.attempt.seedProbe = captured.error().seedProbe;
                out.attempt.failureDetail = captured.error().detail;
            } else {
                out.attempt.exitCode = 9;
                out.attempt.waczExists = false;
                out.attempt.seedProbe = captured.error().seedProbe;
                out.attempt.failureDetail = captured.error().detail;
            }
            out.stdoutLog.clear();
            out.stderrLog = std::string(captured.error().detail.view()) + "\n";
            out.wacz.reset();
            out.pagesJsonl.reset();
            out.contentSha256.reset();
            return out;
        }

        auto exchange = std::move(captured->exchange);
        const auto proxyDownBytes = captured->proxyDownBytes;
        LOG_INFO() << std::format(
            "crawler captureViaProxy finished for {} with status={}", run.seedUrl,
            exchange.statusCode
        );
        auto pages = crawler::buildPagesJsonl(exchange);
        LOG_INFO() << std::format("crawler buildPagesJsonl finished for {}", run.seedUrl);
        out.contentSha256 = crawler::computeContentSha256(exchange);
        {
            auto log = crawler::buildSuccessStdoutLog(
                run, exchange, 0_i64, crawler::ReusedBrowser::kNo
            );
            if (!log)
                UINVARIANT(false, log.error().detail);
            out.stdoutLog = grabValueOf(log);
        }
        out.stderrLog.clear();
        auto warc = crawler::buildWarc(exchange);
        if (!warc)
            return failArtifact(warc.error());
        LOG_INFO() << std::format("crawler buildWarc finished for {}", run.seedUrl);
        auto wacz = crawler::buildWacz(run, pages, grabValueOf(warc), out.stdoutLog, out.stderrLog);
        if (!wacz)
            return failArtifact(wacz.error());
        LOG_INFO() << std::format(
            "crawler buildWacz finished for {} (wacz_bytes={}, pages_bytes={})", run.seedUrl,
            wacz->size(), pages.size()
        );

        const auto waczBytes = i64(wacz->size());
        if (waczBytes > maxArchiveBytes) {
            const auto maxArchiveMiB = maxArchiveBytes / (1024_i64 * 1024_i64);
            const auto detail = text::format(
                "archive bytes {} exceeded size limit {} MiB", waczBytes, maxArchiveMiB
            );
            out.attempt.exitCode = us::utils::UnderlyingValue(crawler::CrawlerExitCode::kSizeLimit);
            out.attempt.waczExists = false;
            out.attempt.seedProbe = crawler::SeedPageProbe{
                .status = raw(exchange.statusCode),
                .loadState = raw(0_i64),
            };
            out.attempt.failureDetail = detail;
            out.wacz.reset();
            out.pagesJsonl.reset();
            out.contentSha256.reset();
            out.stderrLog += std::string(detail.view()) + "\n";
            return out;
        }

        const auto maxDownByFinal = [&]() -> i64 {
            const auto ratio = networkDownBytesRatioMax;
            const auto maxI64 = std::numeric_limits<i64>::max();
            if (waczBytes <= 0_i64)
                return 0_i64;
            if (ratio > maxI64 / waczBytes)
                return maxI64;
            return ratio * waczBytes;
        }();
        if (proxyDownBytes > maxDownByFinal) {
            const auto detail = text::format(
                "net_limit: proxy downstream bytes {} exceeded post-run limit {}", proxyDownBytes,
                maxDownByFinal
            );
            out.attempt.exitCode = us::utils::UnderlyingValue(
                crawler::CrawlerExitCode::kFailedLimit
            );
            out.attempt.waczExists = false;
            out.attempt.seedProbe = crawler::SeedPageProbe{
                .status = raw(exchange.statusCode),
                .loadState = raw(0_i64),
            };
            out.attempt.failureDetail = detail;
            out.wacz.reset();
            out.pagesJsonl.reset();
            out.contentSha256.reset();
            out.stderrLog += std::string(detail.view()) + "\n";
            return out;
        }

        const auto exitCode = exchange.statusCode >= 400_i64 ? 9_i64 : 0_i64;
        const auto loadState = exitCode != 0_i64 || exchange.statusCode >= 400_i64 ? 0_i64 : 2_i64;
        if (exchange.statusCode >= 400_i64) {
            out.attempt.failureDetail = text::format("seed returned HTTP {}", exchange.statusCode);
        }

        LOG_INFO() << std::format(
            "crawler executeRun finished for {} (exit_code={}, wacz_exists=true)", run.seedUrl,
            exitCode
        );

        out.attempt.exitCode = numericCast<int>(exitCode);
        out.attempt.waczExists = true;
        out.attempt.seedProbe = crawler::SeedPageProbe{
            .status = raw(exchange.statusCode),
            .loadState = raw(loadState),
        };
        out.wacz = grabValueOf(wacz);
        out.pagesJsonl = std::move(pages);
        return out;
    } catch (const us::utils::TracefulException &e) {
        out.attempt.exitCode = 9;
        out.attempt.waczExists = false;
        out.attempt.seedProbe.reset();
        {
            auto parsed = String::fromBytes(e.what());
            if (parsed)
                out.attempt.failureDetail = grabValueOf(parsed);
            else
                out.attempt.failureDetail.reset();
        }
        out.stdoutLog.clear();
        out.stderrLog = std::string(e.what()) + "\n";
        out.wacz.reset();
        out.pagesJsonl.reset();
        out.contentSha256.reset();
        return out;
    }
}

} // namespace

CrawlerRunner::CrawlerRunner(
    Denylist &denylist, const Config &config, dns::Resolver &dnsResolver,
    us::engine::subprocess::ProcessStarter &processStarter, chrono::seconds runTimeout,
    std::string stateDir, std::optional<crawler::CgroupLimits> limits, i64 maxArchiveBytes,
    crawler::CaptureTimings timings, crawler::CrawlerTunables tunables, i64 networkDownBytesRatioMax
)
    : denylist(denylist), config(config), dnsResolver(dnsResolver), processStarter(processStarter),
      runTimeout(runTimeout), browserRunsRoot(buildBrowserRunsRoot(std::move(stateDir))),
      cgroupRootPath(limits ? resolveDelegatedCgroupRootPath() : std::string()),
      cgroupLimits(std::move(limits)), maxArchiveBytes(maxArchiveBytes),
      timings(std::move(timings)), tunables(std::move(tunables)),
      networkDownBytesRatioMax(networkDownBytesRatioMax)
{
}

CrawlerRunArtifacts CrawlerRunner::run(const String &seedUrl) const
{
    const auto deadline = us::engine::Deadline::FromDuration(runTimeout);
    return executeRun(
        denylist, config, dnsResolver, processStarter, browserRunsRoot, cgroupRootPath,
        cgroupLimits, timings, tunables, maxArchiveBytes, networkDownBytesRatioMax, deadline,
        crawler::RunRequest{.seedUrl = seedUrl}
    );
}

} // namespace v1
