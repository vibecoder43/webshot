#include "crawler/browser_sandbox.hpp"
#include "crawler/launch_policy.hpp"

namespace v1::crawler {

std::vector<std::string>
buildChromiumArgs(const std::string &userDataDir, const std::string &netlogPath)
{
    std::vector<std::string> args = {
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
        "--user-data-dir=" + userDataDir,
        "--log-net-log=" + netlogPath,
        "--net-log-capture-mode=IncludeSensitive",
        std::string{"--proxy-server=http://127.0.0.1:"} +
            std::to_string(toNative(kProxyListenPort)),
        "--proxy-bypass-list=<-loopback>",
        "--remote-debugging-address=127.0.0.1",
        "--remote-debugging-port=" + std::to_string(toNative(kDevtoolsPort)),
        std::string{"--window-size="} + std::to_string(toNative(kBrowserWindowWidth)) + "," +
            std::to_string(toNative(kBrowserWindowHeight))
    };
    args.emplace_back("--enable-logging=stderr");
    args.emplace_back("--log-level=0");
    args.emplace_back("about:blank");
    return args;
}

} // namespace v1::crawler
