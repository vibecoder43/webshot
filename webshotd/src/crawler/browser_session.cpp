#include "crawler/browser_session.hpp"

#include "crawler/browser_start_policy.hpp"
#include "crawler/cdp_client.hpp"
#include "crawler/cgroup_stats.hpp"
#include "crawler/egress_proxy.hpp"
#include "crawler/error.hpp"
#include "grab_value.hpp"
#include "invariant.hpp"
#include "metrics.hpp"
#include "try.hpp"

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
namespace ws::crawler {
namespace us = userver;
namespace eng = us::engine;
namespace datetime = us::utils::datetime;
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

[[nodiscard]] const std::string &BrowserSandboxScript()
{
    static const std::string script = us::utils::FindResource("webshot_browser_sandbox_sh");
    return script;
}

[[nodiscard]] const std::string &BrowserSandboxClosurePaths()
{
    static const std::string paths = us::utils::FindResource(
        "webshot_browser_sandbox_closure_paths"
    );
    return paths;
}

[[nodiscard]] const std::string &BrowserSandboxPathEnv()
{
    static const std::string path = us::utils::FindResource("webshot_browser_sandbox_path");
    return path;
}

[[nodiscard]] std::string NormalizeDirPath(std::string value)
{
    while (value.size() > 1 && value.back() == '/')
        value.pop_back();
    return value;
}

[[nodiscard]] std::string BrowserSandboxPath(std::string_view relative_path)
{
    Invariant(relative_path.front() != '/', "browser sandbox path must be relative"_t);
    return std::format("{}/{}", kBrowserSandboxRoot, relative_path);
}

[[noreturn]] void AbortCgroupConfig(std::string_view message) noexcept
{
    us::utils::AbortWithStacktrace(std::string(message));
}

[[nodiscard]] std::string ReadSelfCgroupV2Path(eng::TaskProcessor &fs_task_processor)
{
    auto raw = us::fs::ReadFileContents(fs_task_processor, "/proc/self/cgroup");
    std::string_view remaining{raw};
    while (true) {
        auto next = remaining.find('\n');
        auto line = next == std::string::npos ? remaining : remaining.substr(0, next);
        if (line.starts_with("0::")) {
            auto path = NormalizeDirPath(std::string(line.substr(3)));
            if (path.empty() || path.front() != '/')
                AbortCgroupConfig("invalid cgroup v2 path in /proc/self/cgroup");
            return path;
        }
        if (next == std::string::npos)
            break;
        remaining.remove_prefix(next + 1);
    }
    AbortCgroupConfig("failed to locate cgroup v2 path in /proc/self/cgroup");
}

[[nodiscard]] std::string ParentCgroupPath(const std::string &path)
{
    if (path.empty() || path.front() != '/')
        AbortCgroupConfig("cgroup path must be absolute");
    if (path == "/")
        AbortCgroupConfig("webshotd must run inside the managed cgroup subgroup");

    auto slash_pos = path.find_last_of('/');
    if (slash_pos == std::string::npos)
        AbortCgroupConfig("failed to locate parent cgroup path");
    if (slash_pos == 0)
        return "/";
    return path.substr(0, slash_pos);
}

[[nodiscard]] bool IsManagedCgroupRootName(std::string_view name)
{
    return name.starts_with(kManagedCgroupPrefix) && name.ends_with(kManagedCgroupScopeSuffix);
}

[[nodiscard]] std::string ManagedCgroupRootPathFromServiceSubgroup(const std::string &path)
{
    if (!path.ends_with(kManagedCgroupServiceSubgroup)) {
        AbortCgroupConfig(
            std::format(
                "webshotd must run inside managed cgroup subgroup '{}', got {}",
                kManagedCgroupServiceSubgroup, path
            )
        );
    }

    auto managed_root_path = ParentCgroupPath(path);
    auto managed_root_name_pos = managed_root_path.find_last_of('/');
    auto managed_root_name = managed_root_name_pos == std::string::npos
                                 ? std::string_view{managed_root_path}
                                 : std::string_view{managed_root_path}.substr(
                                       managed_root_name_pos + 1
                                   );
    if (!IsManagedCgroupRootName(managed_root_name)) {
        AbortCgroupConfig(
            std::format("webshotd is not running inside a managed cgroup root: {}", path)
        );
    }

    return managed_root_path;
}

[[nodiscard]] String CurrentTimestamp()
{
    return *String::FromBytes(datetime::UtcTimestring(datetime::Now(), datetime::kRfc3339Format));
}

[[nodiscard]] Expected<void, String> CopyFileContents(
    eng::TaskProcessor &fs_task_processor, const std::string &source_path,
    const std::string &destination_path
)
{
    if (!us::fs::FileExists(fs_task_processor, source_path))
        return Unex(text::Format("source file does not exist: {}", source_path));
    us::fs::RewriteFileContents(
        fs_task_processor, destination_path,
        us::fs::ReadFileContents(fs_task_processor, source_path)
    );
    return {};
}

[[nodiscard]] std::vector<std::string> MakeChromiumArgs(
    const std::string &user_data_dir, const std::string &netlog_path,
    bool use_local_fixture_trust_db
)
{
    auto disabled_features = std::string("Vulkan,VulkanFromANGLE,DefaultANGLEVulkan");
    if (use_local_fixture_trust_db)
        disabled_features += ",ChromeRootStoreUsed";

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
        std::format("--disable-features={}", disabled_features),
        std::format("--user-data-dir={}", user_data_dir),
        std::format("--log-net-log={}", netlog_path),
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
    explicit SeccompFilter(uint32_t default_action) : ctx_(seccomp_init(default_action))
    {
        Invariant(ctx_ != nullptr, "failed to initialize browser seccomp filter"_t);
    }
    SeccompFilter(const SeccompFilter &) = delete;
    SeccompFilter(SeccompFilter &&) = delete;
    SeccompFilter &operator=(const SeccompFilter &) = delete;
    SeccompFilter &operator=(SeccompFilter &&) = delete;
    ~SeccompFilter() { seccomp_release(ctx_); }

    [[nodiscard]] scmp_filter_ctx Get() const noexcept { return ctx_; }

private:
    scmp_filter_ctx ctx_;
};

void DenyBrowserSyscall(SeccompFilter &filter, const char *name)
{
    auto syscall = seccomp_syscall_resolve_name(name);
    Invariant(syscall >= 0, text::Format("browser seccomp syscall is unknown: {}", name));

    auto rc = seccomp_rule_add(filter.Get(), SCMP_ACT_ERRNO(EPERM), syscall, 0);
    Invariant(rc == 0, text::Format("failed to deny browser syscall {}: {}", name, rc));
}

void WriteBrowserSeccompPolicy(eng::TaskProcessor &fs_task_processor, const std::string &path)
{
    eng::AsyncNoSpan(fs_task_processor, [&path] {
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
            DenyBrowserSyscall(filter, name);
        }

        auto fd = us::fs::blocking::FileDescriptor::Open(
            path, us::fs::blocking::OpenMode{
                      us::fs::blocking::OpenFlag::kWrite,
                      us::fs::blocking::OpenFlag::kCreateIfNotExists,
                      us::fs::blocking::OpenFlag::kTruncate,
                  }
        );
        auto rc = seccomp_export_bpf(filter.Get(), fd.GetNative());
        Invariant(rc == 0, text::Format("failed to export browser seccomp policy: {}", rc));
    }).Get();
}

struct [[nodiscard]] BrowserPaths final {
    std::string root_dir;
    std::string run_id;
    std::string user_data_dir;
    std::string user_data_trust_db_dir;
    std::string xdg_config_home;
    std::string xdg_cache_home;
    std::string crashpad_dir;
    std::string proxy_socket_path;
    std::string cdp_socket_path;
    std::string websocket_pathfile_path;
    std::string netlog_path;
    std::string cdp_trace_path;
    std::string stdout_log_path;
    std::string stderr_log_path;
    std::string chromium_stderr_log_path;
    std::string bwrap_status_file_path;
    std::string cgroup_stats_path;
    std::string seccomp_bpf_path;
    std::string phase_file_path;
    std::string dev_null_path;
    std::string etc_dir;
    std::string passwd_path;
    std::string group_path;
    std::string nsswitch_conf_path;
    std::string hosts_path;
    std::string local_fixture_trust_db_dir;
};

[[nodiscard]] BrowserPaths
MakeBrowserPaths(eng::TaskProcessor &fs_task_processor, std::string_view browser_runs_root)
{
    auto temp_root = NormalizeDirPath(std::string(browser_runs_root));
    us::fs::CreateDirectories(fs_task_processor, temp_root);

    const auto run_id = us::utils::ToString(us::utils::generators::GenerateBoostUuid());
    auto root_dir = std::format("{}/browser_{}", temp_root, run_id);
    BrowserPaths paths{
        .root_dir = root_dir,
        .run_id = run_id,
        .user_data_dir = root_dir + "/profile",
        .user_data_trust_db_dir = root_dir + "/profile/.pki/nssdb",
        .xdg_config_home = root_dir + "/xdg_config",
        .xdg_cache_home = root_dir + "/xdg_cache",
        .crashpad_dir = root_dir + "/crashpad",
        .proxy_socket_path = root_dir + "/proxy.sock",
        .cdp_socket_path = root_dir + "/cdp.sock",
        .websocket_pathfile_path = root_dir + "/websocket_path.txt",
        .netlog_path = root_dir + "/netlog.json",
        .cdp_trace_path = root_dir + "/cdp_trace.jsonl",
        .stdout_log_path = root_dir + "/stdout.log",
        .stderr_log_path = root_dir + "/stderr.log",
        .chromium_stderr_log_path = root_dir + "/chromium_stderr.log",
        .bwrap_status_file_path = root_dir + "/bwrap_status.jsonl",
        .cgroup_stats_path = root_dir + "/cgroup_stats.txt",
        .seccomp_bpf_path = root_dir + "/seccomp.bpf",
        .phase_file_path = root_dir + "/phase.txt",
        .dev_null_path = root_dir + "/devnull",
        .etc_dir = root_dir + "/etc",
        .passwd_path = root_dir + "/etc/passwd",
        .group_path = root_dir + "/etc/group",
        .nsswitch_conf_path = root_dir + "/etc/nsswitch.conf",
        .hosts_path = root_dir + "/etc/hosts",
        .local_fixture_trust_db_dir = root_dir + "/.pki/nssdb",
    };
    us::fs::CreateDirectories(fs_task_processor, paths.root_dir);
    us::fs::RewriteFileContents(
        fs_task_processor, root_dir + "/browser_sandbox.sh", BrowserSandboxScript()
    );
    WriteBrowserSeccompPolicy(fs_task_processor, paths.seccomp_bpf_path);

    for (const auto &path : std::array{
             paths.user_data_dir,
             paths.xdg_config_home,
             paths.xdg_cache_home,
             paths.crashpad_dir,
             paths.etc_dir,
         }) {
        us::fs::CreateDirectories(fs_task_processor, path);
    }
    for (const auto &path :
         std::array{paths.phase_file_path, paths.cdp_trace_path, paths.dev_null_path}) {
        us::fs::RewriteFileContents(fs_task_processor, path, {});
    }
    us::fs::RewriteFileContents(
        fs_task_processor, paths.passwd_path,
        "root:x:0:0:root:/browser:/bin/sh\n"
        "webshot:x:1000:1000:webshot:/browser:/bin/sh\n"
    );
    us::fs::RewriteFileContents(
        fs_task_processor, paths.group_path, "root:x:0:\nwebshot:x:1000:\n"
    );
    us::fs::RewriteFileContents(
        fs_task_processor, paths.nsswitch_conf_path,
        "passwd: files\ngroup: files\nhosts: files dns\n"
    );
    us::fs::RewriteFileContents(
        fs_task_processor, paths.hosts_path, "127.0.0.1 localhost\n::1 localhost\n"
    );
    return paths;
}

[[nodiscard]] Expected<us::fs::blocking::FileDescriptor, String>
OpenBrowserRunDir(eng::TaskProcessor &fs_task_processor, const BrowserPaths &paths)
{
    try {
        return eng::AsyncNoSpan(
                   fs_task_processor, [&paths] {
                       return us::fs::blocking::FileDescriptor::OpenDirectory(paths.root_dir);
                   }
        ).Get();
    } catch (const std::runtime_error &e) {
        return Unex(
            text::Format("failed to open browser run dir {}: {}", paths.root_dir, e.what())
        );
    }
}

[[nodiscard]] std::string
BrowserRunFdPath(const us::fs::blocking::FileDescriptor &dir_fd, std::string_view file_name)
{
    Invariant(file_name.front() != '/', "browser run fd path file name must be relative"_t);
    return std::format("/proc/self/fd/{}/{}", dir_fd.GetNative(), file_name);
}

[[nodiscard]] Expected<void, String> CopyLocalFixtureTrustDb(
    eng::TaskProcessor &fs_task_processor, const std::string &source_path,
    const std::string &destination_path
)
{
    us::fs::CreateDirectories(fs_task_processor, destination_path);

    for (const auto &file_name : {"cert9.db", "key4.db", "pkcs11.txt"}) {
        auto copied = CopyFileContents(
            fs_task_processor, source_path + "/" + file_name, destination_path + "/" + file_name
        );
        if (!copied)
            return copied;
    }
    return {};
}

[[nodiscard]] Expected<void, String> StageLocalFixtureTrustDb(
    eng::TaskProcessor &fs_task_processor, const BrowserPaths &paths, const std::string &source_path
)
{
    us::fs::CreateDirectories(fs_task_processor, paths.root_dir + "/.pki");
    us::fs::CreateDirectories(fs_task_processor, paths.user_data_dir + "/.pki");

    TRY(CopyLocalFixtureTrustDb(fs_task_processor, source_path, paths.local_fixture_trust_db_dir));
    TRY(CopyLocalFixtureTrustDb(fs_task_processor, source_path, paths.user_data_trust_db_dir));
    return {};
}

[[nodiscard]] Expected<void, String> StageLocalFixtureTrustDbIfNeeded(
    eng::TaskProcessor &fs_task_processor, const BrowserPaths &paths,
    const BrowserSessionConfig &config
)
{
    if (!config.enable_local_fixture_rewrite)
        return {};

    TRY_MAP_ERR(
        StageLocalFixtureTrustDb(
            fs_task_processor, paths, config.local_fixture_trust_db_source_path
        ),
        [](auto error) { return text::Format("failed to stage local fixture trust db: {}", error); }
    );
    return {};
}

void TruncateLogBuffer(std::string &value)
{
    if (ssize(value) <= kMaxLogBytes)
        return;
    value = RetainLogHeadAndTail(std::move(value), kMaxLogBytes);
}

[[nodiscard]] std::string
ReadLogTail(eng::TaskProcessor &fs_task_processor, const std::string &path)
{
    if (!us::fs::FileExists(fs_task_processor, path))
        return {};
    auto value = us::fs::ReadFileContents(fs_task_processor, path);
    TruncateLogBuffer(value);
    return value;
}

void WritePhaseMarker(
    eng::TaskProcessor &fs_task_processor, const std::string &path, std::string_view phase
)
{
    us::fs::RewriteFileContents(
        fs_task_processor, path, std::format("{} {}\n", CurrentTimestamp(), phase)
    );
}

[[nodiscard]] std::string FormatBrowserLogs(const std::pair<std::string, std::string> &logs)
{
    const auto &[stdout_log, stderr_log] = logs;
    return std::format(
        "stdout={}, stderr={}", stdout_log.empty() ? "empty" : stdout_log,
        stderr_log.empty() ? "empty" : stderr_log
    );
}

[[nodiscard]] std::string FormatStartLogs(
    const std::pair<std::string, std::string> &browser_logs, const std::string &bwrap_status
)
{
    auto value = FormatBrowserLogs(browser_logs);
    if (!bwrap_status.empty())
        value = std::format("{}, bwrap_status={}", value, bwrap_status);
    return value;
}

void AppendDiagnosticField(String &out, const String &label, const String &value)
{
    if (!out.Empty())
        out += ", "_t;
    out += text::Format("{}={}", label, value);
}

[[nodiscard]] std::optional<std::string>
ReadBrowserFileIfExists(eng::TaskProcessor &fs_task_processor, const std::string &path)
{
    try {
        if (!us::fs::FileExists(fs_task_processor, path))
            return {};
        return us::fs::ReadFileContents(fs_task_processor, path);
    } catch (const std::runtime_error &e) {
        LOG_WARNING() << std::format("ReadBrowserFileIfExists failed for {}: {}", path, e.what());
        return {};
    }
}

[[nodiscard]] Expected<void, String>
RemoveBrowserRunDir(eng::TaskProcessor &fs_task_processor, const std::string &path) noexcept
{
    try {
        eng::AsyncNoSpan(fs_task_processor, [&path] {
            auto temp_dir = us::fs::blocking::TempDirectory::Adopt(path);
            std::move(temp_dir).Remove();
        }).Get();
        return {};
    } catch (const std::runtime_error &e) {
        return Unex{*String::FromBytes(std::string(e.what()))};
    }
}

[[nodiscard]] std::optional<String>
ReadRetainedLogText(eng::TaskProcessor &fs_task_processor, const std::string &path)
{
    auto bytes = ReadBrowserFileIfExists(fs_task_processor, path);
    if (!bytes)
        return {};
    auto text = RetainProcessOutputText(*bytes);
    if (!text.Empty())
        return text;
    return {};
}

[[nodiscard]] std::optional<String>
ReadWebsocketPathFile(eng::TaskProcessor &fs_task_processor, const std::string &path)
{
    auto value = ReadBrowserFileIfExists(fs_task_processor, path);
    if (!value)
        return {};
    absl::StripTrailingAsciiWhitespace(&*value);
    if (value->empty())
        return {};
    return TRY(text::OptionalString(value));
}

[[nodiscard]] eng::subprocess::ChildProcess SpawnProcess(
    eng::subprocess::ProcessStarter &process_starter, const std::string &executable_path,
    const std::vector<std::string> &args, const std::string &stdout_path,
    const std::string &stderr_path
)
{
    eng::subprocess::ExecOptions options;
    options.use_path = true;
    options.stdout_file = stdout_path;
    options.stderr_file = stderr_path;
    return process_starter.Exec(executable_path, args, std::move(options));
}

[[nodiscard]] eng::subprocess::ChildProcess SpawnSandboxedBrowser(
    eng::subprocess::ProcessStarter &process_starter, const BrowserPaths &paths,
    std::string_view cgroup_root_path, const std::optional<CgroupLimits> &cgroup_limits,
    std::string_view cgroup_name_prefix, bool use_local_fixture_trust_db
)
{
    const i64 cpu_cores{cgroup_limits ? cgroup_limits->cpu_cores : 0_i64};
    const i64 memory_bytes{cgroup_limits ? cgroup_limits->memory_bytes : 0_i64};
    const auto cgroup_name = std::format("{}_{}", cgroup_name_prefix, paths.run_id);

    auto chromium_args = MakeChromiumArgs(
        BrowserSandboxPath("profile"), BrowserSandboxPath("netlog.json"), use_local_fixture_trust_db
    );
    std::vector<std::string> bwrap_args{
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
    for (const auto &closure_path : us::utils::text::Split(BrowserSandboxClosurePaths(), "\n")) {
        if (closure_path.empty())
            continue;
        bwrap_args.insert(std::end(bwrap_args), {"--ro-bind", closure_path, closure_path});
    }
    bwrap_args.insert(
        std::end(bwrap_args), {
                                  "--dir",
                                  "/etc",
                                  "--ro-bind",
                                  paths.passwd_path,
                                  "/etc/passwd",
                                  "--ro-bind",
                                  paths.group_path,
                                  "/etc/group",
                                  "--ro-bind",
                                  paths.nsswitch_conf_path,
                                  "/etc/nsswitch.conf",
                                  "--ro-bind",
                                  paths.hosts_path,
                                  "/etc/hosts",
                                  "--tmpfs",
                                  "/tmp",
                                  "--chmod",
                                  "1777",
                                  "/tmp",
                                  "--bind",
                                  paths.root_dir,
                                  std::string(kBrowserSandboxRoot),
                                  "--bind",
                                  paths.dev_null_path,
                                  "/dev/null",
                                  "--setenv",
                                  "FONTCONFIG_FILE",
                                  std::string(kBrowserSandboxFontconfigFile),
                                  "--setenv",
                                  "PATH",
                                  BrowserSandboxPathEnv(),
                                  "--setenv",
                                  "HOME",
                                  std::string(kBrowserSandboxRoot),
                                  "--setenv",
                                  "TMPDIR",
                                  "/tmp",
                                  "--setenv",
                                  "XDG_CONFIG_HOME",
                                  BrowserSandboxPath("xdg_config"),
                                  "--setenv",
                                  "XDG_CACHE_HOME",
                                  BrowserSandboxPath("xdg_cache"),
                                  "--setenv",
                                  "BREAKPAD_DUMP_LOCATION",
                                  BrowserSandboxPath("crashpad"),
                                  "--setenv",
                                  "DBUS_SESSION_BUS_ADDRESS",
                                  "=disabled:",
                                  "--chdir",
                                  std::string(kBrowserSandboxRoot),
                                  "setpriv",
                                  "--no-new-privs",
                                  "--",
                                  "bash",
                                  "browser_sandbox.sh",
                                  BrowserSandboxPath(kProxySocketFileName),
                                  BrowserSandboxPath(kCdpSocketFileName),
                                  BrowserSandboxPath(kWebsocketPathFileName),
                                  std::format("{}", kProxyListenPort),
                                  std::format("{}", kDevtoolsPort),
                                  "--",
                                  "chromium",
                              }
    );
    bwrap_args.insert(std::end(bwrap_args), std::begin(chromium_args), std::end(chromium_args));

    std::vector<std::string> args{
        std::string(kBwrapStatusWrapperPath),
        paths.bwrap_status_file_path,
        paths.cgroup_stats_path,
        std::string(cgroup_root_path),
        cgroup_name,
        std::format("{}", cpu_cores),
        std::format("{}", memory_bytes),
        paths.seccomp_bpf_path,
    };
    args.insert(std::end(args), std::begin(bwrap_args), std::end(bwrap_args));
    return SpawnProcess(
        process_starter, "bash", args, paths.stdout_log_path, paths.stderr_log_path
    );
}

[[nodiscard]] std::string
BrowserCgroupPath(const BrowserPaths &paths, const BrowserSessionConfig &config)
{
    return std::format(
        "{}/{}_{}", config.cgroup_root_path_, config.cgroup_name_prefix, paths.run_id
    );
}

[[nodiscard]] Expected<std::unique_ptr<EgressProxy>, String> StartBrowserProxy(
    us::clients::dns::Resolver &dns_resolver, eng::TaskProcessor &fs_task_processor,
    const BrowserPaths &paths, const BrowserSessionConfig &config, eng::Deadline deadline
)
{
    auto run_dir_fd = TRY(OpenBrowserRunDir(fs_task_processor, paths));
    return EgressProxy::Make(
        EgressProxyConfig{
            BrowserRunFdPath(run_dir_fd, kProxySocketFileName),
            paths.run_id,
            config.url_bytes_max,
            config.proxy_down_bytes_max,
            config.proxy_require_auth,
            config.enable_local_fixture_rewrite,
            config.testsuite_loopback_ports,
        },
        dns_resolver, deadline
    );
}

[[nodiscard]] Expected<std::unique_ptr<CdpClient>, String> ConnectCdpOnce(
    eng::TaskProcessor &fs_task_processor, const BrowserPaths &paths,
    const BrowserSessionConfig &config, const String &websocket_path, eng::Deadline deadline
)
{
    auto run_dir_fd = TRY(OpenBrowserRunDir(fs_task_processor, paths));
    return TRY_MAP_ERR(
        CdpClient::Connect(
            BrowserRunFdPath(run_dir_fd, kCdpSocketFileName), websocket_path, paths.cdp_trace_path,
            fs_task_processor, deadline, config.cdp_handshake_timeout, config.cdp_command_timeout,
            config.cdp_max_remote_payload_bytes
        ),
        [](auto error) {
            return FormatCdpError("devtools websocket handshake failed"_t, std::move(error));
        }
    );
}

template <typename Process> void StopProcess(Process &process, chrono::milliseconds timeout)
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
        us::clients::dns::Resolver &dns_resolverin,
        eng::subprocess::ProcessStarter &process_starter_in,
        eng::TaskProcessor &fs_task_processorin, BrowserSessionConfig config_in
    )
        : dns_resolver(dns_resolverin), process_starter_(process_starter_in),
          fs_task_processor(fs_task_processorin), config(std::move(config_in))
    {
    }

    [[nodiscard]] Expected<void, String> Start()
    {
        paths = MakeBrowserPaths(fs_task_processor, config.browser_runs_root_);
        TRY(StageLocalFixtureTrustDbIfNeeded(fs_task_processor, paths, config));

        MarkPhase("start_browser");
        auto devtools_deadline = eng::Deadline::FromDuration(config.devtools_startup_timeout);
        proxy = TRY(
            StartBrowserProxy(dns_resolver, fs_task_processor, paths, config, devtools_deadline)
        );

        process.emplace(SpawnSandboxedBrowser(
            process_starter_, paths, config.cgroup_root_path_, config.cgroup_limits_,
            config.cgroup_name_prefix, config.enable_local_fixture_rewrite
        ));
        if (config.cgroup_limits_ && config.metrics) {
            registered_cgroup_path = BrowserCgroupPath(paths, config);
            config.metrics->RegisterBrowserCgroup(*registered_cgroup_path);
        }
        websocket_path = TRY_MAP_ERR(WaitForDevtoolsPath(devtools_deadline), [this](auto detail) {
            return MakeErrorDetail(std::move(detail));
        });
        return {};
    }

    [[nodiscard]] Expected<std::unique_ptr<CdpClient>, String>
    ConnectCdp(eng::Deadline overall_deadline) const
    {
        Invariant(overall_deadline.IsReachable(), "cdp overall deadline must be reachable"_t);

        std::optional<String> last_error;
        i64 attempts{0};
        while (!overall_deadline.IsReached()) {
            attempts++;
            auto cdp = ConnectCdpOnce(
                fs_task_processor, paths, config, websocket_path, overall_deadline
            );
            if (cdp)
                return GrabValueOf(std::move(cdp));

            last_error = text::Format("{} ({})", cdp.Error(), CurrentStartLogs());

            if (!overall_deadline.IsReached())
                eng::SleepFor(config.devtools_poll_interval);
        }

        if (last_error)
            return Unex(std::move(*last_error));
        return Unex(
            text::Format(
                "devtools websocket handshake failed after {} attempt(s) ({})", attempts,
                CurrentStartLogs()
            )
        );
    }

    [[nodiscard]] std::pair<std::string, std::string> DrainBrowserLogs() const
    {
        return {
            ReadLogTail(fs_task_processor, paths.stdout_log_path),
            ReadLogTail(fs_task_processor, paths.stderr_log_path),
        };
    }

    void MarkPhase(std::string_view phase) const
    {
        WritePhaseMarker(fs_task_processor, paths.phase_file_path, phase);
    }

    [[nodiscard]] std::string CurrentStartLogs() const
    {
        return FormatStartLogs(
            DrainBrowserLogs(), ReadLogTail(fs_task_processor, paths.bwrap_status_file_path)
        );
    }

    [[nodiscard]] String MakeErrorDetail(const String &message)
    {
        String diagnostics{};

        if (const auto browser_logs = FormatProcessOutputDiagnostics(
                fs_task_processor, paths.stdout_log_path, paths.stderr_log_path
            )) {
            AppendDiagnosticField(diagnostics, "browser_logs"_t, *browser_logs);
        }
        if (const auto chromium_stderr =
                ReadRetainedLogText(fs_task_processor, paths.chromium_stderr_log_path))
            AppendDiagnosticField(diagnostics, "chromium_stderr"_t, *chromium_stderr);
        if (const auto bwrap_status =
                ReadRetainedLogText(fs_task_processor, paths.bwrap_status_file_path))
            AppendDiagnosticField(diagnostics, "bwrap_status"_t, *bwrap_status);
        if (const auto phase_marker = ReadRetainedLogText(fs_task_processor, paths.phase_file_path))
            AppendDiagnosticField(diagnostics, "phase"_t, *phase_marker);
        if (const auto cdp_trace = ReadRetainedLogText(fs_task_processor, paths.cdp_trace_path))
            AppendDiagnosticField(diagnostics, "cdp_trace_tail"_t, *cdp_trace);
        if (const auto raw_cgroup_stats =
                ReadBrowserFileIfExists(fs_task_processor, paths.cgroup_stats_path)) {
            auto parsed = ParseCgroupStatsSnapshot(*raw_cgroup_stats);
            if (parsed) {
                AppendDiagnosticField(diagnostics, "cgroup"_t, FormatCgroupStats(*parsed));
                if (HasBrowserOomKill(*parsed))
                    AppendDiagnosticField(diagnostics, "browser_resource_error"_t, "oom_kill"_t);
            } else if (auto cgroup_stats =
                           ReadRetainedLogText(fs_task_processor, paths.cgroup_stats_path)) {
                AppendDiagnosticField(diagnostics, "cgroup_raw"_t, *cgroup_stats);
            }
        }
        if (const auto websocket_pathfrom_file =
                ReadWebsocketPathFile(fs_task_processor, paths.websocket_pathfile_path))
            AppendDiagnosticField(diagnostics, "websocket_path"_t, *websocket_pathfrom_file);
        AppendDiagnosticField(
            diagnostics, "websocket_pathfile_exists"_t,
            us::fs::FileExists(fs_task_processor, paths.websocket_pathfile_path) ? "true"_t
                                                                                 : "false"_t
        );
        AppendDiagnosticField(
            diagnostics, "netlog_exists"_t,
            us::fs::FileExists(fs_task_processor, paths.netlog_path) ? "true"_t : "false"_t
        );
        AppendDiagnosticField(
            diagnostics, "browser_process_running"_t,
            process && !process->WaitFor(0ms) ? "true"_t : "false"_t
        );
        AppendDiagnosticField(
            diagnostics, "cdp_socket_exists"_t,
            us::fs::FileExists(fs_task_processor, paths.cdp_socket_path) ? "true"_t : "false"_t
        );
        if (proxy) {
            AppendDiagnosticField(
                diagnostics, "proxy_down_bytes"_t, text::Format("{}", proxy->DownBytes())
            );
            if (auto proxy_error = proxy->ErrorReason())
                AppendDiagnosticField(diagnostics, "proxy_error"_t, *proxy_error);
        }

        if (diagnostics.Empty())
            return message;
        return text::Format("{}, {}", message, diagnostics);
    }

    ~Impl()
    {
        if (registered_cgroup_path && config.metrics)
            config.metrics->UnregisterBrowserCgroup(*registered_cgroup_path);
        StopProcess(process, config.browser_stop_timeout);
        if (proxy)
            proxy->Stop();
        if (!paths.root_dir.empty()) {
            if (auto ret = RemoveBrowserRunDir(fs_task_processor, paths.root_dir); !ret) {
                LOG_WARNING() << std::format(
                    "Failed to remove browser dir {}: {}", paths.root_dir, ret.Error()
                );
            }
        }
    }

    [[nodiscard]] i64 ProxyDownBytes() const noexcept { return proxy ? proxy->DownBytes() : 0_i64; }
    [[nodiscard]] const std::string &RunId() const noexcept { return paths.run_id; }
    [[nodiscard]] std::optional<String> ProxyErrorReason() const noexcept
    {
        if (!proxy)
            return {};
        return proxy->ErrorReason();
    }

    [[nodiscard]] Expected<String, String> WaitForDevtoolsPath(eng::Deadline deadline)
    {
        Invariant(deadline.IsReachable(), "devtools deadline must be reachable"_t);
        auto saw_cdp_socket = false;
        auto saw_websocket_path = false;
        while (!deadline.IsReached()) {
            saw_cdp_socket = saw_cdp_socket ||
                             us::fs::FileExists(fs_task_processor, paths.cdp_socket_path);
            saw_websocket_path = saw_websocket_path ||
                                 us::fs::FileExists(
                                     fs_task_processor, paths.websocket_pathfile_path
                                 );
            if (process && process->WaitFor(0ms)) {
                return Unex(
                    text::Format(
                        "chromium exited before exposing devtools ({})", CurrentStartLogs()
                    )
                );
            }
            auto websocket_pathfrom_file = ReadWebsocketPathFile(
                fs_task_processor, paths.websocket_pathfile_path
            );
            if (saw_cdp_socket && websocket_pathfrom_file)
                return GrabValueOf(websocket_pathfrom_file);
            eng::SleepFor(config.devtools_poll_interval);
        }
        if (process && process->WaitFor(0ms)) {
            return Unex(
                text::Format("chromium exited before exposing devtools ({})", CurrentStartLogs())
            );
        }
        return Unex(
            text::Format(
                "{} ({})",
                !saw_websocket_path ? "devtools websocket path was never written"
                : !saw_cdp_socket
                    ? "devtools websocket path was written but cdp socket never appeared"
                    : "devtools websocket path and cdp socket appeared but handshake never started",
                CurrentStartLogs()
            )
        );
    }

    us::clients::dns::Resolver &dns_resolver;
    eng::subprocess::ProcessStarter &process_starter_;
    eng::TaskProcessor &fs_task_processor;
    BrowserSessionConfig config;
    BrowserPaths paths;
    std::unique_ptr<EgressProxy> proxy;
    std::optional<eng::subprocess::ChildProcess> process;
    String websocket_path;
    std::optional<std::string> registered_cgroup_path;
};

BrowserSession::BrowserSession(
    us::clients::dns::Resolver &dns_resolver, eng::subprocess::ProcessStarter &process_starter,
    eng::TaskProcessor &fs_task_processor, BrowserSessionConfig config
)
    : impl_(
          std::make_unique<Impl>(
              dns_resolver, process_starter, fs_task_processor, std::move(config)
          )
      )
{
}

BrowserSession::~BrowserSession() = default;

Expected<std::unique_ptr<BrowserSession>, String> BrowserSession::Make(
    us::clients::dns::Resolver &dns_resolver, eng::subprocess::ProcessStarter &process_starter,
    eng::TaskProcessor &fs_task_processor, BrowserSessionConfig config
)
{
    auto browser = std::unique_ptr<BrowserSession>(
        new BrowserSession(dns_resolver, process_starter, fs_task_processor, std::move(config))
    );
    TRY_MAP_ERR(browser->impl_->Start(), [](auto error) { return error; });
    return browser;
}

Expected<std::unique_ptr<CdpClient>, String>
BrowserSession::ConnectCdp(eng::Deadline overall_deadline) const
{
    return impl_->ConnectCdp(overall_deadline);
}

std::pair<std::string, std::string> BrowserSession::DrainBrowserLogs() const
{
    return impl_->DrainBrowserLogs();
}

void BrowserSession::MarkPhase(std::string_view phase) const { impl_->MarkPhase(phase); }

std::string BrowserSession::CurrentStartLogs() const { return impl_->CurrentStartLogs(); }

String BrowserSession::MakeErrorDetail(const String &message)
{
    return impl_->MakeErrorDetail(message);
}

i64 BrowserSession::ProxyDownBytes() const noexcept { return impl_->ProxyDownBytes(); }

const std::string &BrowserSession::RunId() const noexcept { return impl_->RunId(); }

std::optional<String> BrowserSession::ProxyErrorReason() const noexcept
{
    return impl_->ProxyErrorReason();
}

namespace {

template <typename T, typename... Args>
[[nodiscard]] Expected<T, String> SendCdp(auto &cdp_endpoint, const String &method, Args &&...args)
{
    return TRY_MAP_ERR(
        cdp_endpoint.template Send<T>(method, std::forward<Args>(args)...), [&method](auto error) {
            return FormatCdpError(text::Format("{} failed", method), std::move(error));
        }
    );
}

template <typename... Args>
[[nodiscard]] Expected<void, String>
SendCdpVoid(auto &cdp_endpoint, const String &method, Args &&...args)
{
    TRY(SendCdp<dto::CdpEmptyObject>(cdp_endpoint, method, std::forward<Args>(args)...));
    return {};
}

} // namespace

BrowserPageSession::BrowserPageSession(CdpClient &cdp_client) : cdp_client_(cdp_client) {}

Expected<void, String> BrowserPageSession::MakeBrowserContext()
{
    auto browser_context = TRY(
        SendCdp<dto::TargetCreateBrowserContextResult>(cdp_client_, "Target.createBrowserContext"_t)
    );
    browser_context_id_ = *String::FromBytes(browser_context.browserContextId);
    Invariant(
        lifecycle_.MarkBrowserContextCreated(),
        "invalid browser page lifecycle transition after creating browser context"_t
    );
    return {};
}

Expected<void, String> BrowserPageSession::MakeBlankTarget()
{
    Invariant(browser_context_id_, "browser context must exist before creating a target"_t);

    dto::TargetCreateTargetParams target_params{
        .url = "about:blank",
        .browserContextId = browser_context_id_->ToBytes(),
    };
    auto target = TRY(
        SendCdp<dto::TargetCreateTargetResult>(cdp_client_, "Target.createTarget"_t, target_params)
    );
    target_id_ = *String::FromBytes(target.targetId);
    Invariant(
        lifecycle_.MarkTargetCreated(),
        "invalid browser page lifecycle transition after creating target"_t
    );
    return {};
}

Expected<void, String> BrowserPageSession::AttachToTarget()
{
    Invariant(target_id_, "target must exist before attaching"_t);

    dto::TargetAttachToTargetParams attach_params{
        .targetId = target_id_->ToBytes(),
        .flatten = true,
    };
    auto attached = TRY(
        SendCdp<dto::TargetAttachToTargetResult>(
            cdp_client_, "Target.attachToTarget"_t, attach_params
        )
    );
    auto session_id = *String::FromBytes(attached.sessionId);
    auto cdp_session = cdp_client_.MakeSession(session_id, *target_id_);
    if (!cdp_session)
        return Unex(FormatCdpError("failed to register cdp target session"_t, cdp_session.Error()));
    session_id_ = std::move(session_id);
    cdp_session_ = GrabValueOf(cdp_session);
    Invariant(
        lifecycle_.MarkAttached(),
        "invalid browser page lifecycle transition after attaching target"_t
    );
    return {};
}

Expected<void, String>
BrowserPageSession::AttachFreshTarget(const std::function<void(std::string_view)> &mark_phase)
{
    mark_phase("create_browser_context");
    TRY(MakeBrowserContext());
    mark_phase("create_target");
    TRY(MakeBlankTarget());
    mark_phase("attach_target");
    TRY(AttachToTarget());
    return {};
}

Expected<void, String>
BrowserPageSession::EnableBaseDomains(const std::function<void(std::string_view)> &mark_phase)
{
    mark_phase("enable_page");
    TRY(SendCdpVoid(GetSession(), "Page.enable"_t));
    mark_phase("enable_runtime");
    TRY(SendCdpVoid(GetSession(), "Runtime.enable"_t));
    mark_phase("enable_network");
    TRY(SendCdpVoid(GetSession(), "Network.enable"_t));
    mark_phase("enable_lifecycle_events");
    dto::PageSetLifecycleEventsEnabledParams lifecycle_params;
    lifecycle_params.enabled = true;
    TRY(SendCdpVoid(GetSession(), "Page.setLifecycleEventsEnabled"_t, lifecycle_params));

    Invariant(
        lifecycle_.MarkBaseDomainsEnabled(),
        "invalid browser page lifecycle transition after enabling base CDP domains"_t
    );
    return {};
}

Expected<void, String>
BrowserPageSession::Stop(const std::function<void(std::string_view)> &mark_phase)
{
    if (session_id_) {
        mark_phase("detach_target");
        TRY(Detach());
    }
    if (browser_context_id_) {
        mark_phase("dispose_browser_context");
        TRY(DisposeBrowserContext());
    }
    return Stop();
}

Expected<void, String> BrowserPageSession::Detach()
{
    if (!session_id_)
        return {};

    dto::TargetDetachFromTargetParams detach_params;
    detach_params.sessionId = session_id_->ToBytes();
    TRY(SendCdpVoid(cdp_client_, "Target.detachFromTarget"_t, detach_params));
    cdp_session_.reset();
    session_id_.reset();
    Invariant(
        lifecycle_.MarkDetached(),
        "invalid browser page lifecycle transition after detaching target"_t
    );
    return {};
}

Expected<void, String> BrowserPageSession::DisposeBrowserContext()
{
    if (!browser_context_id_)
        return {};

    dto::TargetDisposeBrowserContextParams dispose_params;
    dispose_params.browserContextId = browser_context_id_->ToBytes();
    TRY(SendCdpVoid(cdp_client_, "Target.disposeBrowserContext"_t, dispose_params));
    browser_context_id_.reset();
    target_id_.reset();
    Invariant(
        lifecycle_.MarkDisposed(),
        "invalid browser page lifecycle transition after disposing browser context"_t
    );
    return {};
}

Expected<void, String> BrowserPageSession::Stop()
{
    TRY(Detach());
    TRY(DisposeBrowserContext());
    Invariant(
        lifecycle_.MarkStopped(),
        "invalid browser page lifecycle transition after stopping page session"_t
    );
    return {};
}

const String &BrowserPageSession::BrowserContextId() const
{
    Invariant(browser_context_id_, "browser context is not created"_t);
    return *browser_context_id_;
}

CdpSession &BrowserPageSession::GetSession() const
{
    Invariant(cdp_session_, "target session is not attached"_t);
    return *cdp_session_;
}

const String &BrowserPageSession::TargetId() const
{
    Invariant(target_id_, "target is not created"_t);
    return *target_id_;
}

const String &BrowserPageSession::SessionId() const
{
    Invariant(session_id_, "target is not attached"_t);
    return *session_id_;
}

std::string MakeBrowserRunsRoot(std::string state_dir)
{
    auto root = NormalizeDirPath(std::move(state_dir));
    if (root == "/")
        return "/browser_runs";
    return std::format("{}/browser_runs", root);
}

std::string ResolveDelegatedCgroupRootPath(eng::TaskProcessor &fs_task_processor)
{
    auto current_path = ReadSelfCgroupV2Path(fs_task_processor);
    return std::format("/sys/fs/cgroup{}", ManagedCgroupRootPathFromServiceSubgroup(current_path));
}

std::string LocalFixtureTrustDbSourcePath(std::string_view state_dir)
{
    return NormalizeDirPath(std::string(state_dir)) + "/test_pki/chromium_nssdb";
}

} // namespace ws::crawler
