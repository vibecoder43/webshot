#include "crawler/browser_session.hpp"

#include "crawler/cdp_client.hpp"
#include "crawler/egress_proxy.hpp"
#include "crawler/failure.hpp"
#include "crawler/launch_policy.hpp"
#include "grab_value.hpp"
#include "invariant.hpp"
#include "try.hpp"
#include "userver_namespaces.hpp"
#include "uuid_format.hpp"

#include <generated/browser_sandbox.sh.hpp>
#include <generated/browser_sandbox_closure_paths.hpp>
#include <generated/browser_sandbox_path.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <userver/engine/async.hpp>
#include <userver/engine/sleep.hpp>
#include <userver/fs/blocking/file_descriptor.hpp>
#include <userver/fs/blocking/temp_directory.hpp>
#include <userver/fs/read.hpp>
#include <userver/fs/write.hpp>
#include <userver/logging/log.hpp>
#include <userver/utils/assert.hpp>
#include <userver/utils/boost_uuid4.hpp>
#include <userver/utils/datetime.hpp>
#include <userver/utils/resources.hpp>
#include <userver/utils/text_light.hpp>

#include <absl/strings/ascii.h>
#include <seccomp.h>

namespace chrono = std::chrono;
using namespace std::chrono_literals;

using namespace text::literals;
using v1::uuidu::toBytes;

namespace v1::crawler {
namespace {

constexpr auto kMaxLogBytes = 64_i64 * 1024_i64;
constexpr std::string_view kManagedCgroupPrefix{"webshotd-"};
constexpr std::string_view kManagedCgroupScopeSuffix{".scope"};
constexpr std::string_view kManagedCgroupServiceSubgroup{"/service"};
constexpr std::string_view kBrowserSandboxRoot{"/browser"};
constexpr std::string_view kBrowserSandboxHostname{"webshot-browser"};
constexpr std::string_view kBrowserSandboxUid{"1000"};
constexpr std::string_view kBrowserSandboxGid{"1000"};
constexpr std::string_view kProxySocketFileName{"proxy.sock"};
constexpr std::string_view kCdpSocketFileName{"cdp.sock"};
constexpr std::string_view kWebsocketPathFileName{"websocket_path.txt"};
constexpr std::string_view kBwrapStatusWrapperPath{WEBSHOT_BWRAP_STATUS_WRAPPER_PATH};
constexpr std::string_view kBrowserSandboxFontconfigFile{WEBSHOT_BROWSER_SANDBOX_FONTCONFIG_FILE};

[[nodiscard]] const std::string &browserSandboxScript()
{
    static const std::string script = us::utils::FindResource("webshot_browser_sandbox_sh");
    return script;
}

[[nodiscard]] const std::string &browserSandboxClosurePaths()
{
    static const std::string paths = us::utils::FindResource(
        "webshot_browser_sandbox_closure_paths"
    );
    return paths;
}

[[nodiscard]] const std::string &browserSandboxPathEnv()
{
    static const std::string path = us::utils::FindResource("webshot_browser_sandbox_path");
    return path;
}

[[nodiscard]] std::string normalizeDirPath(std::string value)
{
    while (value.size() > 1 && value.back() == '/')
        value.pop_back();
    return value;
}

[[nodiscard]] std::string browserSandboxPath(std::string_view relativePath)
{
    invariant(!relativePath.empty(), "browser sandbox path must not be empty"_t);
    invariant(relativePath.front() != '/', "browser sandbox path must be relative"_t);
    return std::format("{}/{}", kBrowserSandboxRoot, relativePath);
}

[[noreturn]] void abortCgroupConfig(std::string_view message) noexcept
{
    us::utils::AbortWithStacktrace(std::string(message));
}

[[nodiscard]] std::string readSelfCgroupV2Path(eng::TaskProcessor &fsTaskProcessor)
{
    const auto raw = us::fs::ReadFileContents(fsTaskProcessor, "/proc/self/cgroup");
    std::string_view remaining{raw};
    while (true) {
        const auto next = remaining.find('\n');
        const auto line = next == std::string::npos ? remaining : remaining.substr(0, next);
        if (line.starts_with("0::")) {
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
        abortCgroupConfig("webshotd must run inside the managed cgroup subgroup");

    const auto slashPos = path.find_last_of('/');
    if (slashPos == std::string::npos)
        abortCgroupConfig("failed to locate parent cgroup path");
    if (slashPos == 0)
        return "/";
    return path.substr(0, slashPos);
}

[[nodiscard]] bool isManagedCgroupRootName(std::string_view name)
{
    return name.starts_with(kManagedCgroupPrefix) && name.ends_with(kManagedCgroupScopeSuffix);
}

[[nodiscard]] std::string managedCgroupRootPathFromServiceSubgroup(const std::string &path)
{
    if (!path.ends_with(kManagedCgroupServiceSubgroup)) {
        abortCgroupConfig(
            std::format(
                "webshotd must run inside managed cgroup subgroup '{}', got {}",
                kManagedCgroupServiceSubgroup, path
            )
        );
    }

    const auto managedRootPath = parentCgroupPath(path);
    const auto managedRootNamePos = managedRootPath.find_last_of('/');
    const auto managedRootName = managedRootNamePos == std::string::npos
                                     ? std::string_view{managedRootPath}
                                     : std::string_view{managedRootPath}.substr(
                                           managedRootNamePos + 1
                                       );
    if (!isManagedCgroupRootName(managedRootName)) {
        abortCgroupConfig(
            std::format("webshotd is not running inside a managed cgroup root: {}", path)
        );
    }

    return managedRootPath;
}

[[nodiscard]] String currentTimestamp()
{
    return String::fromBytes(datetime::UtcTimestring(datetime::Now(), datetime::kRfc3339Format))
        .expect();
}

[[nodiscard]] Expected<void, String> copyFileContents(
    eng::TaskProcessor &fsTaskProcessor, const std::string &sourcePath,
    const std::string &destinationPath
)
{
    if (!us::fs::FileExists(fsTaskProcessor, sourcePath))
        return Unex(text::format("source file does not exist: {}", sourcePath));
    us::fs::RewriteFileContents(
        fsTaskProcessor, destinationPath, us::fs::ReadFileContents(fsTaskProcessor, sourcePath)
    );
    return {};
}

[[nodiscard]] std::vector<std::string> buildChromiumArgs(
    const std::string &userDataDir, const std::string &netlogPath, bool useLocalFixtureTrustDb
)
{
    auto disabledFeatures = std::string("Vulkan,VulkanFromANGLE,DefaultANGLEVulkan");
    if (useLocalFixtureTrustDb)
        disabledFeatures += ",ChromeRootStoreUsed";

    return {
        "--headless=new",
        "--ozone-platform=headless",
        "--disable-gpu",
        "--disable-gpu-compositing",
        "--disable-gpu-rasterization",
        "--disable-vulkan",
        "--disable-dev-shm-usage",
        "--disable-background-networking",
        "--disable-breakpad",
        "--disable-crash-reporter",
        "--disable-quic",
        "--no-default-browser-check",
        "--no-first-run",
        "--mute-audio",
        "--hide-scrollbars",
        "--no-sandbox",
        "--no-zygote",
        "--use-gl=swiftshader",
        std::format("--disable-features={}", disabledFeatures),
        std::format("--user-data-dir={}", userDataDir),
        std::format("--log-net-log={}", netlogPath),
        "--net-log-capture-mode=IncludeSensitive",
        std::format("--proxy-server=http://127.0.0.1:{}", kProxyListenPort),
        "--proxy-bypass-list=<-loopback>",
        "--remote-debugging-address=127.0.0.1",
        std::format("--remote-debugging-port={}", kDevtoolsPort),
        "--window-size=1600,900",
        "--enable-logging=stderr",
        "--log-level=0",
        "about:blank",
    };
}

class [[nodiscard]] SeccompFilter final {
public:
    explicit SeccompFilter(uint32_t defaultAction) : ctx(seccomp_init(defaultAction))
    {
        invariant(ctx != nullptr, "failed to initialize browser seccomp filter"_t);
    }
    SeccompFilter(const SeccompFilter &) = delete;
    SeccompFilter(SeccompFilter &&) = delete;
    SeccompFilter &operator=(const SeccompFilter &) = delete;
    SeccompFilter &operator=(SeccompFilter &&) = delete;
    ~SeccompFilter() { seccomp_release(ctx); }

    [[nodiscard]] scmp_filter_ctx get() const noexcept { return ctx; }

private:
    scmp_filter_ctx ctx;
};

void denyBrowserSyscall(SeccompFilter &filter, const char *name)
{
    const auto syscall = seccomp_syscall_resolve_name(name);
    invariant(syscall >= 0, text::format("browser seccomp syscall is unknown: {}", name));

    const auto rc = seccomp_rule_add(filter.get(), SCMP_ACT_ERRNO(EPERM), syscall, 0);
    invariant(rc == 0, text::format("failed to deny browser syscall {}: {}", name, rc));
}

void writeBrowserSeccompPolicy(eng::TaskProcessor &fsTaskProcessor, const std::string &path)
{
    eng::AsyncNoSpan(fsTaskProcessor, [&path] {
        auto filter = SeccompFilter{SCMP_ACT_ALLOW};
        for (const auto *name : std::array{
                 "bpf",
                 "perf_event_open",
                 "ptrace",
                 "process_vm_readv",
                 "process_vm_writev",
                 "add_key",
                 "request_key",
                 "keyctl",
             }) {
            denyBrowserSyscall(filter, name);
        }

        auto fd = us::fs::blocking::FileDescriptor::Open(
            path, us::fs::blocking::OpenMode{
                      us::fs::blocking::OpenFlag::kWrite,
                      us::fs::blocking::OpenFlag::kCreateIfNotExists,
                      us::fs::blocking::OpenFlag::kTruncate,
                  }
        );
        const auto rc = seccomp_export_bpf(filter.get(), fd.GetNative());
        invariant(rc == 0, text::format("failed to export browser seccomp policy: {}", rc));
    }).Get();
}

struct [[nodiscard]] BrowserPaths final {
    std::string rootDir;
    std::string runId;
    std::string userDataDir;
    std::string userDataTrustDbDir;
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
    std::string seccompBpfPath;
    std::string phaseFilePath;
    std::string devNullPath;
    std::string etcDir;
    std::string passwdPath;
    std::string groupPath;
    std::string nsswitchConfPath;
    std::string hostsPath;
    std::string localFixtureTrustDbDir;
};

[[nodiscard]] BrowserPaths
createBrowserPaths(eng::TaskProcessor &fsTaskProcessor, std::string_view browserRunsRoot)
{
    auto tempRoot = normalizeDirPath(std::string(browserRunsRoot));
    us::fs::CreateDirectories(fsTaskProcessor, tempRoot);

    const auto runId = toBytes(us::utils::generators::GenerateBoostUuid());
    const auto rootDir = std::format("{}/browser_{}", tempRoot, runId);
    BrowserPaths paths{
        .rootDir = rootDir,
        .runId = runId,
        .userDataDir = rootDir + "/profile",
        .userDataTrustDbDir = rootDir + "/profile/.pki/nssdb",
        .xdgConfigHome = rootDir + "/xdg_config",
        .xdgCacheHome = rootDir + "/xdg_cache",
        .crashpadDir = rootDir + "/crashpad",
        .proxySocketPath = rootDir + "/proxy.sock",
        .cdpSocketPath = rootDir + "/cdp.sock",
        .websocketPathFilePath = rootDir + "/websocket_path.txt",
        .netlogPath = rootDir + "/netlog.json",
        .cdpTracePath = rootDir + "/cdp_trace.jsonl",
        .stdoutLogPath = rootDir + "/stdout.log",
        .stderrLogPath = rootDir + "/stderr.log",
        .chromiumStderrLogPath = rootDir + "/chromium_stderr.log",
        .bwrapStatusFilePath = rootDir + "/bwrap_status.jsonl",
        .seccompBpfPath = rootDir + "/seccomp.bpf",
        .phaseFilePath = rootDir + "/phase.txt",
        .devNullPath = rootDir + "/devnull",
        .etcDir = rootDir + "/etc",
        .passwdPath = rootDir + "/etc/passwd",
        .groupPath = rootDir + "/etc/group",
        .nsswitchConfPath = rootDir + "/etc/nsswitch.conf",
        .hostsPath = rootDir + "/etc/hosts",
        .localFixtureTrustDbDir = rootDir + "/.pki/nssdb",
    };
    us::fs::CreateDirectories(fsTaskProcessor, paths.rootDir);
    us::fs::RewriteFileContents(
        fsTaskProcessor, rootDir + "/browser_sandbox.sh", browserSandboxScript()
    );
    writeBrowserSeccompPolicy(fsTaskProcessor, paths.seccompBpfPath);

    for (const auto &path : std::array{
             paths.userDataDir,
             paths.xdgConfigHome,
             paths.xdgCacheHome,
             paths.crashpadDir,
             paths.etcDir,
         }) {
        us::fs::CreateDirectories(fsTaskProcessor, path);
    }
    for (const auto &path :
         std::array{paths.phaseFilePath, paths.cdpTracePath, paths.devNullPath}) {
        us::fs::RewriteFileContents(fsTaskProcessor, path, {});
    }
    us::fs::RewriteFileContents(
        fsTaskProcessor, paths.passwdPath,
        "root:x:0:0:root:/browser:/bin/sh\n"
        "webshot:x:1000:1000:webshot:/browser:/bin/sh\n"
    );
    us::fs::RewriteFileContents(fsTaskProcessor, paths.groupPath, "root:x:0:\nwebshot:x:1000:\n");
    us::fs::RewriteFileContents(
        fsTaskProcessor, paths.nsswitchConfPath, "passwd: files\ngroup: files\nhosts: files dns\n"
    );
    us::fs::RewriteFileContents(
        fsTaskProcessor, paths.hostsPath, "127.0.0.1 localhost\n::1 localhost\n"
    );
    return paths;
}

[[nodiscard]] Expected<us::fs::blocking::FileDescriptor, String>
openBrowserRunDir(eng::TaskProcessor &fsTaskProcessor, const BrowserPaths &paths)
{
    try {
        return eng::AsyncNoSpan(
                   fsTaskProcessor, [&paths] {
                       return us::fs::blocking::FileDescriptor::OpenDirectory(paths.rootDir);
                   }
        ).Get();
    } catch (const std::runtime_error &e) {
        return Unex(text::format("failed to open browser run dir {}: {}", paths.rootDir, e.what()));
    }
}

[[nodiscard]] std::string
browserRunFdPath(const us::fs::blocking::FileDescriptor &dirFd, std::string_view fileName)
{
    invariant(!fileName.empty(), "browser run fd path file name must not be empty"_t);
    invariant(fileName.front() != '/', "browser run fd path file name must be relative"_t);
    return std::format("/proc/self/fd/{}/{}", dirFd.GetNative(), fileName);
}

[[nodiscard]] Expected<void, String> copyLocalFixtureTrustDb(
    eng::TaskProcessor &fsTaskProcessor, const std::string &sourcePath,
    const std::string &destinationPath
)
{
    us::fs::CreateDirectories(fsTaskProcessor, destinationPath);

    for (const auto &fileName : {"cert9.db", "key4.db", "pkcs11.txt"}) {
        auto copied = copyFileContents(
            fsTaskProcessor, sourcePath + "/" + fileName, destinationPath + "/" + fileName
        );
        if (!copied)
            return copied;
    }
    return {};
}

[[nodiscard]] Expected<void, String> stageLocalFixtureTrustDb(
    eng::TaskProcessor &fsTaskProcessor, const BrowserPaths &paths, const std::string &sourcePath
)
{
    us::fs::CreateDirectories(fsTaskProcessor, paths.rootDir + "/.pki");
    us::fs::CreateDirectories(fsTaskProcessor, paths.userDataDir + "/.pki");

    TRY(copyLocalFixtureTrustDb(fsTaskProcessor, sourcePath, paths.localFixtureTrustDbDir));
    TRY(copyLocalFixtureTrustDb(fsTaskProcessor, sourcePath, paths.userDataTrustDbDir));
    return {};
}

[[nodiscard]] Expected<void, String> stageLocalFixtureTrustDbIfNeeded(
    eng::TaskProcessor &fsTaskProcessor, const BrowserPaths &paths,
    const BrowserSessionConfig &config
)
{
    if (!config.enableLocalFixtureRewrite)
        return {};

    TRY_MAP_ERR(
        stageLocalFixtureTrustDb(fsTaskProcessor, paths, config.localFixtureTrustDbSourcePath),
        [](auto error) { return text::format("failed to stage local fixture trust db: {}", error); }
    );
    return {};
}

void truncateLogBuffer(std::string &value)
{
    if (ssize(value) <= kMaxLogBytes)
        return;
    const auto dropBytes = ssize(value) - kMaxLogBytes;
    value.erase(0, numericCast<size_t>(dropBytes));
}

[[nodiscard]] std::string readLogTail(eng::TaskProcessor &fsTaskProcessor, const std::string &path)
{
    if (!us::fs::FileExists(fsTaskProcessor, path))
        return {};
    auto value = us::fs::ReadFileContents(fsTaskProcessor, path);
    truncateLogBuffer(value);
    return value;
}

void writePhaseMarker(
    eng::TaskProcessor &fsTaskProcessor, const std::string &path, std::string_view phase
)
{
    us::fs::RewriteFileContents(
        fsTaskProcessor, path, std::format("{} {}\n", currentTimestamp(), phase)
    );
}

[[nodiscard]] std::string formatBrowserLogs(const std::pair<std::string, std::string> &logs)
{
    const auto &[stdoutLog, stderrLog] = logs;
    return std::format(
        "stdout={}, stderr={}", stdoutLog.empty() ? "empty" : stdoutLog,
        stderrLog.empty() ? "empty" : stderrLog
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

void appendDiagnosticField(String &out, const String &label, const String &value)
{
    if (!out.empty())
        out += ", "_t;
    out += text::format("{}={}", label, value);
}

[[nodiscard]] std::optional<std::string>
readBrowserFileIfExists(eng::TaskProcessor &fsTaskProcessor, const std::string &path)
{
    try {
        if (!us::fs::FileExists(fsTaskProcessor, path))
            return {};
        return us::fs::ReadFileContents(fsTaskProcessor, path);
    } catch (const std::runtime_error &) {
        return {};
    }
}

[[nodiscard]] std::optional<std::string>
removeBrowserRunDirectory(eng::TaskProcessor &fsTaskProcessor, const std::string &path) noexcept
{
    try {
        eng::AsyncNoSpan(fsTaskProcessor, [&path] {
            auto tempDir = us::fs::blocking::TempDirectory::Adopt(path);
            std::move(tempDir).Remove();
        }).Get();
        return {};
    } catch (const std::runtime_error &e) {
        return std::string(e.what());
    }
}

[[nodiscard]] std::optional<String>
readSanitizedLogTail(eng::TaskProcessor &fsTaskProcessor, const std::string &path)
{
    const auto bytes = readBrowserFileIfExists(fsTaskProcessor, path);
    if (!bytes)
        return {};
    const auto sanitized = sanitizeProcessOutputTail(*bytes);
    if (!sanitized.empty())
        return sanitized;
    return {};
}

void removeBrowserRunDir(eng::TaskProcessor &fsTaskProcessor, const std::string &path) noexcept
{
    if (path.empty())
        return;
    if (const auto error = removeBrowserRunDirectory(fsTaskProcessor, path))
        LOG_WARNING() << std::format("Failed to remove browser dir {}: {}", path, *error);
}

[[nodiscard]] std::optional<String>
readWebsocketPathFile(eng::TaskProcessor &fsTaskProcessor, const std::string &path)
{
    auto value = readBrowserFileIfExists(fsTaskProcessor, path);
    if (!value)
        return {};
    absl::StripTrailingAsciiWhitespace(&*value);
    if (value->empty())
        return {};
    return TRY(text::optionalString(value));
}

[[nodiscard]] eng::subprocess::ChildProcess spawnProcess(
    eng::subprocess::ProcessStarter &processStarter, const std::string &executablePath,
    const std::vector<std::string> &args, const std::string &stdoutPath,
    const std::string &stderrPath
)
{
    eng::subprocess::ExecOptions options;
    options.use_path = true;
    options.stdout_file = stdoutPath;
    options.stderr_file = stderrPath;
    return processStarter.Exec(executablePath, args, std::move(options));
}

[[nodiscard]] eng::subprocess::ChildProcess spawnSandboxedBrowser(
    eng::subprocess::ProcessStarter &processStarter, const BrowserPaths &paths,
    std::string_view cgroupRootPath, const std::optional<CgroupLimits> &cgroupLimits,
    std::string_view cgroupNamePrefix, bool useLocalFixtureTrustDb
)
{
    const i64 cpuCores{cgroupLimits ? cgroupLimits->cpuCores : 0_i64};
    const i64 memoryBytes{cgroupLimits ? cgroupLimits->memoryBytes : 0_i64};
    const auto cgroupName = std::format("{}_{}", cgroupNamePrefix, paths.runId);

    auto chromiumArgs = buildChromiumArgs(
        browserSandboxPath("profile"), browserSandboxPath("netlog.json"), useLocalFixtureTrustDb
    );
    std::vector<std::string> bwrapArgs{
        "bwrap",
        "--json-status-fd",
        "3",
        "--die-with-parent",
        "--new-session",
        "--seccomp",
        "4",
        "--unshare-user",
        "--unshare-net",
        "--unshare-pid",
        "--unshare-ipc",
        "--unshare-uts",
        "--disable-userns",
        "--uid",
        std::string(kBrowserSandboxUid),
        "--gid",
        std::string(kBrowserSandboxGid),
        "--hostname",
        std::string(kBrowserSandboxHostname),
        "--clearenv",
        "--proc",
        "/proc",
        "--dev",
        "/dev",
        "--dir",
        "/nix",
        "--dir",
        "/nix/store",
    };
    for (const auto &closurePath : us::utils::text::Split(browserSandboxClosurePaths(), "\n")) {
        if (closurePath.empty())
            continue;
        bwrapArgs.insert(std::end(bwrapArgs), {"--ro-bind", closurePath, closurePath});
    }
    bwrapArgs.insert(
        std::end(bwrapArgs), {
                                 "--dir",
                                 "/etc",
                                 "--ro-bind",
                                 paths.passwdPath,
                                 "/etc/passwd",
                                 "--ro-bind",
                                 paths.groupPath,
                                 "/etc/group",
                                 "--ro-bind",
                                 paths.nsswitchConfPath,
                                 "/etc/nsswitch.conf",
                                 "--ro-bind",
                                 paths.hostsPath,
                                 "/etc/hosts",
                                 "--tmpfs",
                                 "/tmp",
                                 "--chmod",
                                 "1777",
                                 "/tmp",
                                 "--bind",
                                 paths.rootDir,
                                 std::string(kBrowserSandboxRoot),
                                 "--bind",
                                 paths.devNullPath,
                                 "/dev/null",
                                 "--setenv",
                                 "FONTCONFIG_FILE",
                                 std::string(kBrowserSandboxFontconfigFile),
                                 "--setenv",
                                 "PATH",
                                 browserSandboxPathEnv(),
                                 "--setenv",
                                 "HOME",
                                 std::string(kBrowserSandboxRoot),
                                 "--setenv",
                                 "TMPDIR",
                                 "/tmp",
                                 "--setenv",
                                 "XDG_CONFIG_HOME",
                                 browserSandboxPath("xdg_config"),
                                 "--setenv",
                                 "XDG_CACHE_HOME",
                                 browserSandboxPath("xdg_cache"),
                                 "--setenv",
                                 "BREAKPAD_DUMP_LOCATION",
                                 browserSandboxPath("crashpad"),
                                 "--chdir",
                                 std::string(kBrowserSandboxRoot),
                                 "setpriv",
                                 "--no-new-privs",
                                 "--",
                                 "bash",
                                 "browser_sandbox.sh",
                                 browserSandboxPath(kProxySocketFileName),
                                 browserSandboxPath(kCdpSocketFileName),
                                 browserSandboxPath(kWebsocketPathFileName),
                                 std::format("{}", kProxyListenPort),
                                 std::format("{}", kDevtoolsPort),
                                 "--",
                                 "chromium",
                             }
    );
    bwrapArgs.insert(std::end(bwrapArgs), std::begin(chromiumArgs), std::end(chromiumArgs));

    std::vector<std::string> args{
        std::string(kBwrapStatusWrapperPath),
        paths.bwrapStatusFilePath,
        std::string(cgroupRootPath),
        cgroupName,
        std::format("{}", cpuCores),
        std::format("{}", memoryBytes),
        paths.seccompBpfPath,
    };
    args.insert(std::end(args), std::begin(bwrapArgs), std::end(bwrapArgs));
    return spawnProcess(processStarter, "bash", args, paths.stdoutLogPath, paths.stderrLogPath);
}

[[nodiscard]] Expected<std::unique_ptr<EgressProxy>, String> startBrowserProxy(
    us::clients::dns::Resolver &dnsResolver, eng::TaskProcessor &fsTaskProcessor,
    const BrowserPaths &paths, const BrowserSessionConfig &config, eng::Deadline deadline
)
{
    auto runDirFd = TRY(openBrowserRunDir(fsTaskProcessor, paths));
    auto proxy = std::make_unique<EgressProxy>(EgressProxyConfig{
        browserRunFdPath(runDirFd, kProxySocketFileName),
        paths.runId,
        config.urlBytesMax,
        config.proxyDownBytesMax,
        config.proxyRequireAuth,
        config.enableLocalFixtureRewrite,
        config.testsuiteLoopbackPorts,
    });
    TRY_MAP_ERR(proxy->start(dnsResolver, deadline), [](auto detail) {
        return text::format("proxy failed to start: {}", detail);
    });
    return proxy;
}

[[nodiscard]] Expected<std::unique_ptr<CdpClient>, String> connectCdpOnce(
    eng::TaskProcessor &fsTaskProcessor, const BrowserPaths &paths,
    const BrowserSessionConfig &config, const String &websocketPath, eng::Deadline deadline
)
{
    auto runDirFd = TRY(openBrowserRunDir(fsTaskProcessor, paths));
    return TRY_MAP_ERR(
        CdpClient::connect(
            browserRunFdPath(runDirFd, kCdpSocketFileName), websocketPath, paths.cdpTracePath,
            fsTaskProcessor, deadline, config.cdpHandshakeTimeout, config.cdpCommandTimeout,
            config.cdpMaxRemotePayloadBytes
        ),
        [](auto failure) {
            return describeCdpFailure("devtools websocket handshake failed"_t, std::move(failure));
        }
    );
}

template <typename Process> void stopProcess(Process &process, chrono::milliseconds timeout)
{
    if (!process)
        return;

    if (!process->WaitFor(0ms)) {
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

} // namespace

struct BrowserSession::Impl final {
    Impl(
        us::clients::dns::Resolver &dnsResolverIn,
        eng::subprocess::ProcessStarter &processStarterIn, eng::TaskProcessor &fsTaskProcessorIn,
        BrowserSessionConfig configIn
    )
        : dnsResolver(dnsResolverIn), processStarter(processStarterIn),
          fsTaskProcessor(fsTaskProcessorIn), config(std::move(configIn))
    {
    }

    [[nodiscard]] Expected<void, String> launch()
    {
        paths = createBrowserPaths(fsTaskProcessor, config.browserRunsRoot);
        TRY(stageLocalFixtureTrustDbIfNeeded(fsTaskProcessor, paths, config));

        markPhase("launch_browser");
        const auto devtoolsDeadline = eng::Deadline::FromDuration(config.devtoolsStartupTimeout);
        proxy = TRY(
            startBrowserProxy(dnsResolver, fsTaskProcessor, paths, config, devtoolsDeadline)
        );

        process.emplace(spawnSandboxedBrowser(
            processStarter, paths, config.cgroupRootPath, config.cgroupLimits,
            config.cgroupNamePrefix, config.enableLocalFixtureRewrite
        ));
        websocketPath = TRY_MAP_ERR(waitForDevtoolsPath(devtoolsDeadline), [this](auto detail) {
            return buildFailureDetail(std::move(detail));
        });
        return {};
    }

    [[nodiscard]] Expected<std::unique_ptr<CdpClient>, String>
    connectCdp(eng::Deadline overallDeadline) const
    {
        invariant(overallDeadline.IsReachable(), "cdp overall deadline must be reachable"_t);

        std::optional<String> lastFailure;
        i64 attempts{0};
        while (!overallDeadline.IsReached()) {
            attempts++;
            auto cdp = connectCdpOnce(
                fsTaskProcessor, paths, config, websocketPath, overallDeadline
            );
            if (cdp)
                return std::move(cdp).value();

            lastFailure = text::format("{} ({})", cdp.error(), currentLaunchLogs());

            if (!overallDeadline.IsReached())
                eng::SleepFor(config.devtoolsPollInterval);
        }

        if (lastFailure)
            return Unex(std::move(*lastFailure));
        return Unex(
            text::format(
                "devtools websocket handshake failed after {} attempt(s) ({})", attempts,
                currentLaunchLogs()
            )
        );
    }

    [[nodiscard]] std::pair<std::string, std::string> drainBrowserLogs() const
    {
        return {
            readLogTail(fsTaskProcessor, paths.stdoutLogPath),
            readLogTail(fsTaskProcessor, paths.stderrLogPath),
        };
    }

    void markPhase(std::string_view phase) const
    {
        if (paths.phaseFilePath.empty())
            return;
        writePhaseMarker(fsTaskProcessor, paths.phaseFilePath, phase);
    }

    [[nodiscard]] std::string currentLaunchLogs() const
    {
        return formatLaunchLogs(
            drainBrowserLogs(), readLogTail(fsTaskProcessor, paths.bwrapStatusFilePath)
        );
    }

    [[nodiscard]] String buildFailureDetail(const String &message)
    {
        String diagnostics{};

        if (const auto browserLogs = summarizeProcessOutputs(
                fsTaskProcessor, paths.stdoutLogPath, paths.stderrLogPath
            )) {
            appendDiagnosticField(diagnostics, "browser_logs"_t, *browserLogs);
        }
        if (const auto chromiumStderr =
                readSanitizedLogTail(fsTaskProcessor, paths.chromiumStderrLogPath))
            appendDiagnosticField(diagnostics, "chromium_stderr"_t, *chromiumStderr);
        if (const auto bwrapStatus =
                readSanitizedLogTail(fsTaskProcessor, paths.bwrapStatusFilePath))
            appendDiagnosticField(diagnostics, "bwrap_status"_t, *bwrapStatus);
        if (const auto phaseMarker = readSanitizedLogTail(fsTaskProcessor, paths.phaseFilePath))
            appendDiagnosticField(diagnostics, "phase"_t, *phaseMarker);
        if (const auto cdpTrace = readSanitizedLogTail(fsTaskProcessor, paths.cdpTracePath))
            appendDiagnosticField(diagnostics, "cdp_trace_tail"_t, *cdpTrace);
        if (const auto websocketPathFromFile =
                readWebsocketPathFile(fsTaskProcessor, paths.websocketPathFilePath))
            appendDiagnosticField(diagnostics, "websocket_path"_t, *websocketPathFromFile);
        appendDiagnosticField(
            diagnostics, "websocket_path_file_exists"_t,
            us::fs::FileExists(fsTaskProcessor, paths.websocketPathFilePath) ? "true"_t : "false"_t
        );
        appendDiagnosticField(
            diagnostics, "netlog_exists"_t,
            us::fs::FileExists(fsTaskProcessor, paths.netlogPath) ? "true"_t : "false"_t
        );
        appendDiagnosticField(
            diagnostics, "browser_process_running"_t,
            process && !process->WaitFor(0ms) ? "true"_t : "false"_t
        );
        appendDiagnosticField(
            diagnostics, "cdp_socket_exists"_t,
            us::fs::FileExists(fsTaskProcessor, paths.cdpSocketPath) ? "true"_t : "false"_t
        );
        if (proxy) {
            appendDiagnosticField(
                diagnostics, "proxy_down_bytes"_t, text::format("{}", proxy->downBytes())
            );
            if (const auto proxyFailure = proxy->failureReason())
                appendDiagnosticField(diagnostics, "proxy_failure"_t, *proxyFailure);
        }

        if (diagnostics.empty())
            return message;
        return text::format("{}, {}", message, diagnostics);
    }

    void close()
    {
        if (closed)
            return;
        closed = true;

        stopProcess(process, config.browserStopTimeout);
        if (proxy)
            proxy->close();
        if (!paths.rootDir.empty())
            removeBrowserRunDir(fsTaskProcessor, paths.rootDir);
    }

    [[nodiscard]] i64 proxyDownBytes() const noexcept { return proxy ? proxy->downBytes() : 0_i64; }
    [[nodiscard]] const std::string &runId() const noexcept { return paths.runId; }
    [[nodiscard]] std::optional<String> proxyFailureReason() const noexcept
    {
        if (!proxy)
            return {};
        return proxy->failureReason();
    }

    [[nodiscard]] Expected<String, String> waitForDevtoolsPath(eng::Deadline deadline)
    {
        invariant(deadline.IsReachable(), "devtools deadline must be reachable"_t);
        auto sawCdpSocket = false;
        auto sawWebsocketPath = false;
        while (!deadline.IsReached()) {
            sawCdpSocket = sawCdpSocket || us::fs::FileExists(fsTaskProcessor, paths.cdpSocketPath);
            sawWebsocketPath = sawWebsocketPath ||
                               us::fs::FileExists(fsTaskProcessor, paths.websocketPathFilePath);
            if (process && process->WaitFor(0ms)) {
                return Unex(
                    text::format(
                        "chromium exited before exposing devtools ({})", currentLaunchLogs()
                    )
                );
            }
            auto websocketPathFromFile = readWebsocketPathFile(
                fsTaskProcessor, paths.websocketPathFilePath
            );
            if (sawCdpSocket && websocketPathFromFile)
                return grabValueOf(websocketPathFromFile);
            eng::SleepFor(config.devtoolsPollInterval);
        }
        if (process && process->WaitFor(0ms)) {
            return Unex(
                text::format("chromium exited before exposing devtools ({})", currentLaunchLogs())
            );
        }
        return Unex(
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

    us::clients::dns::Resolver &dnsResolver;
    eng::subprocess::ProcessStarter &processStarter;
    eng::TaskProcessor &fsTaskProcessor;
    BrowserSessionConfig config;
    BrowserPaths paths;
    std::unique_ptr<EgressProxy> proxy;
    std::optional<eng::subprocess::ChildProcess> process;
    String websocketPath;
    bool closed{false};
};

BrowserSession::BrowserSession(
    us::clients::dns::Resolver &dnsResolver, eng::subprocess::ProcessStarter &processStarter,
    eng::TaskProcessor &fsTaskProcessor, BrowserSessionConfig config
)
    : impl(std::make_unique<Impl>(dnsResolver, processStarter, fsTaskProcessor, std::move(config)))
{
}

BrowserSession::~BrowserSession() = default;

Expected<void, String> BrowserSession::launch() { return impl->launch(); }

Expected<std::unique_ptr<CdpClient>, String>
BrowserSession::connectCdp(eng::Deadline overallDeadline) const
{
    return impl->connectCdp(overallDeadline);
}

std::pair<std::string, std::string> BrowserSession::drainBrowserLogs() const
{
    return impl->drainBrowserLogs();
}

void BrowserSession::markPhase(std::string_view phase) const { impl->markPhase(phase); }

std::string BrowserSession::currentLaunchLogs() const { return impl->currentLaunchLogs(); }

String BrowserSession::buildFailureDetail(const String &message)
{
    return impl->buildFailureDetail(message);
}

void BrowserSession::close() { impl->close(); }

i64 BrowserSession::proxyDownBytes() const noexcept { return impl->proxyDownBytes(); }

const std::string &BrowserSession::runId() const noexcept { return impl->runId(); }

std::optional<String> BrowserSession::proxyFailureReason() const noexcept
{
    return impl->proxyFailureReason();
}

namespace {

template <typename T, typename... Args>
[[nodiscard]] Expected<T, String> sendCdp(auto &cdpEndpoint, const String &method, Args &&...args)
{
    return TRY_MAP_ERR(
        cdpEndpoint.template send<T>(method, std::forward<Args>(args)...), [&method](auto failure) {
            return describeCdpFailure(text::format("{} failed", method), std::move(failure));
        }
    );
}

template <typename... Args>
[[nodiscard]] Expected<void, String>
sendCdpVoid(auto &cdpEndpoint, const String &method, Args &&...args)
{
    TRY(sendCdp<dto::CdpEmptyObject>(cdpEndpoint, method, std::forward<Args>(args)...));
    return {};
}

} // namespace

BrowserPageSession::BrowserPageSession(CdpClient &cdpClient) : cdpClient(cdpClient) {}

Expected<void, String> BrowserPageSession::createBrowserContext()
{
    const auto browserContext = TRY(
        sendCdp<dto::TargetCreateBrowserContextResult>(cdpClient, "Target.createBrowserContext"_t)
    );
    browserContextIdValue = String::fromBytes(browserContext.browserContextId).expect();
    invariant(
        lifecycle.markBrowserContextCreated(),
        "invalid browser page lifecycle transition after creating browser context"_t
    );
    return {};
}

Expected<void, String> BrowserPageSession::createBlankTarget()
{
    invariant(browserContextIdValue, "browser context must exist before creating a target"_t);

    dto::TargetCreateTargetParams targetParams{
        .url = "about:blank",
        .browserContextId = text::toBytes(*browserContextIdValue),
    };
    const auto target = TRY(
        sendCdp<dto::TargetCreateTargetResult>(cdpClient, "Target.createTarget"_t, targetParams)
    );
    targetIdValue = String::fromBytes(target.targetId).expect();
    invariant(
        lifecycle.markTargetCreated(),
        "invalid browser page lifecycle transition after creating target"_t
    );
    return {};
}

Expected<void, String> BrowserPageSession::attachToTarget()
{
    invariant(targetIdValue, "target must exist before attaching"_t);

    dto::TargetAttachToTargetParams attachParams{
        .targetId = text::toBytes(*targetIdValue),
        .flatten = true,
    };
    const auto attached = TRY(
        sendCdp<dto::TargetAttachToTargetResult>(cdpClient, "Target.attachToTarget"_t, attachParams)
    );
    auto sessionId = String::fromBytes(attached.sessionId).expect();
    auto cdpSession = cdpClient.createSession(sessionId, *targetIdValue);
    if (!cdpSession)
        return Unex(
            describeCdpFailure("failed to register cdp target session"_t, cdpSession.error())
        );
    sessionIdValue = std::move(sessionId);
    cdpSessionValue = grabValueOf(cdpSession);
    invariant(
        lifecycle.markAttached(),
        "invalid browser page lifecycle transition after attaching target"_t
    );
    return {};
}

Expected<void, String>
BrowserPageSession::attachFreshTarget(const std::function<void(std::string_view)> &markPhase)
{
    markPhase("create_browser_context");
    TRY(createBrowserContext());
    markPhase("create_target");
    TRY(createBlankTarget());
    markPhase("attach_target");
    TRY(attachToTarget());
    return {};
}

Expected<void, String>
BrowserPageSession::enableBaseDomains(const std::function<void(std::string_view)> &markPhase)
{
    markPhase("enable_page");
    TRY(sendCdpVoid(cdpSession(), "Page.enable"_t));
    markPhase("enable_runtime");
    TRY(sendCdpVoid(cdpSession(), "Runtime.enable"_t));
    markPhase("enable_network");
    TRY(sendCdpVoid(cdpSession(), "Network.enable"_t));
    markPhase("enable_lifecycle_events");
    dto::PageSetLifecycleEventsEnabledParams lifecycleParams;
    lifecycleParams.enabled = true;
    TRY(sendCdpVoid(cdpSession(), "Page.setLifecycleEventsEnabled"_t, lifecycleParams));

    invariant(
        lifecycle.markBaseDomainsEnabled(),
        "invalid browser page lifecycle transition after enabling base CDP domains"_t
    );
    return {};
}

Expected<void, String>
BrowserPageSession::close(const std::function<void(std::string_view)> &markPhase)
{
    if (sessionIdValue) {
        markPhase("detach_target");
        TRY(detach());
    }
    if (browserContextIdValue) {
        markPhase("dispose_browser_context");
        TRY(disposeBrowserContext());
    }
    return close();
}

Expected<void, String> BrowserPageSession::detach()
{
    if (!sessionIdValue)
        return {};

    dto::TargetDetachFromTargetParams detachParams;
    detachParams.sessionId = text::toBytes(*sessionIdValue);
    TRY(sendCdpVoid(cdpClient, "Target.detachFromTarget"_t, detachParams));
    cdpSessionValue.reset();
    sessionIdValue.reset();
    invariant(
        lifecycle.markDetached(),
        "invalid browser page lifecycle transition after detaching target"_t
    );
    return {};
}

Expected<void, String> BrowserPageSession::disposeBrowserContext()
{
    if (!browserContextIdValue)
        return {};

    dto::TargetDisposeBrowserContextParams disposeParams;
    disposeParams.browserContextId = text::toBytes(*browserContextIdValue);
    TRY(sendCdpVoid(cdpClient, "Target.disposeBrowserContext"_t, disposeParams));
    browserContextIdValue.reset();
    targetIdValue.reset();
    invariant(
        lifecycle.markDisposed(),
        "invalid browser page lifecycle transition after disposing browser context"_t
    );
    return {};
}

Expected<void, String> BrowserPageSession::close()
{
    TRY(detach());
    TRY(disposeBrowserContext());
    invariant(
        lifecycle.markClosed(),
        "invalid browser page lifecycle transition after closing page session"_t
    );
    return {};
}

const String &BrowserPageSession::browserContextId() const
{
    invariant(browserContextIdValue, "browser context is not created"_t);
    return *browserContextIdValue;
}

CdpSession &BrowserPageSession::cdpSession() const
{
    invariant(cdpSessionValue, "target session is not attached"_t);
    return *cdpSessionValue;
}

const String &BrowserPageSession::targetId() const
{
    invariant(targetIdValue, "target is not created"_t);
    return *targetIdValue;
}

const String &BrowserPageSession::sessionId() const
{
    invariant(sessionIdValue, "target is not attached"_t);
    return *sessionIdValue;
}

std::string buildBrowserRunsRoot(std::string stateDir)
{
    auto root = normalizeDirPath(std::move(stateDir));
    invariant(!root.empty(), "state_dir must not be empty"_t);
    if (root == "/")
        return "/browser_runs";
    return std::format("{}/browser_runs", root);
}

std::string resolveDelegatedCgroupRootPath(eng::TaskProcessor &fsTaskProcessor)
{
    const auto currentPath = readSelfCgroupV2Path(fsTaskProcessor);
    return std::format("/sys/fs/cgroup{}", managedCgroupRootPathFromServiceSubgroup(currentPath));
}

std::string localFixtureTrustDbSourcePath(std::string_view stateDir)
{
    return normalizeDirPath(std::string(stateDir)) + "/test_pki/chromium_nssdb";
}

} // namespace v1::crawler
