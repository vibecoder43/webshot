#include "crawler/browser_sandbox.hpp"

#include <stdexcept>

namespace v1::crawler {
namespace {

constexpr std::string_view kBrowserGeometry = "1600x900";
constexpr char kInvalidGeometryFormatMessage[] = "invalid geometry: must be in WIDTHxHEIGHT format";
constexpr char kInvalidGeometryRangeMessage[] =
    "invalid geometry: width and height must be positive integers";
constexpr bool kChromiumVerboseLogging = false;
constexpr std::string_view kChromiumVmodule = "";
constexpr auto kProxyPort = 3128_i64;
constexpr auto kDevtoolsPort = 9222_i64;

[[noreturn]] void throwInvalidGeometryFormat()
{
    throw std::runtime_error(kInvalidGeometryFormatMessage);
}

[[nodiscard]] String getGeometryValue(const BrowserSandboxOptions &options)
{
    if (options.geometry)
        return *options.geometry;
    return String::fromBytesThrow(kBrowserGeometry);
}

} // namespace

Geometry parseGeometry(const String &value)
{
    if (value.empty())
        throwInvalidGeometryFormat();

    const auto bytes = value.view();
    const auto delimiter = bytes.find('x');
    if (delimiter == std::string_view::npos)
        throwInvalidGeometryFormat();

    const auto widthText = bytes.substr(0, delimiter);
    const auto heightText = bytes.substr(delimiter + 1);
    if (widthText.empty() || heightText.empty())
        throwInvalidGeometryFormat();

    auto width = i64(std::stoll(std::string(widthText)));
    auto height = i64(std::stoll(std::string(heightText)));
    if (width < 1_i64 || height < 1_i64)
        throw std::runtime_error(kInvalidGeometryRangeMessage);

    return {width, height};
}

std::vector<std::string> buildChromiumArgs(const BrowserSandboxOptions &options)
{
    const auto parsedGeometry = parseGeometry(getGeometryValue(options));

    std::vector<std::string> args{
        "--headless=new",
        "--disable-gpu",
        "--disable-gpu-compositing",
        "--disable-gpu-rasterization",
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
        "--ignore-certificate-errors",
        "--use-gl=angle",
        "--use-angle=swiftshader",
        "--user-data-dir=" + options.userDataDir,
        "--log-net-log=" + options.netlogPath,
        "--net-log-capture-mode=IncludeSensitive",
        "--proxy-server=http://127.0.0.1:" + std::to_string(toNative(kProxyPort)),
        "--proxy-bypass-list=<-loopback>",
        "--remote-debugging-port=" + std::to_string(toNative(kDevtoolsPort)),
        "--window-size=" + std::to_string(toNative(parsedGeometry.width)) + "," +
            std::to_string(toNative(parsedGeometry.height)),
    };

    if (kChromiumVerboseLogging) {
        args.emplace_back("--enable-logging=stderr");
        args.emplace_back("--log-level=0");
        args.emplace_back("--v=1");
    }
    if (!kChromiumVmodule.empty())
        args.emplace_back("--vmodule=" + std::string(kChromiumVmodule));

    args.emplace_back("about:blank");
    return args;
}

} // namespace v1::crawler
