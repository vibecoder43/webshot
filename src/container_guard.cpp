#include "container_guard.hpp"
/**
 * @file
 * @brief RAII wrapper that creates and then removes a Docker container.
 */

#include <exception>

#include <fmt/core.h>

#include <userver/engine/subprocess/process_starter.hpp>
#include <userver/logging/log.hpp>

namespace engine = userver::engine;

using engine::subprocess::ExecOptions;
/** Make ExecOptions that resolve the executable from PATH. */
static inline ExecOptions makeExecOpts()
{
    ExecOptions o;
    o.use_path = true;
    return o;
}

ContainerGuard::ContainerGuard(
    engine::subprocess::ProcessStarter &starter, std::string name,
    const std::vector<std::string> &createArgs
)
    : starter_(&starter), name_(std::move(name)), removed_(false)
{
    auto proc = starter_->Exec("docker", createArgs, makeExecOpts());
    auto status = proc.Get();
    if (!status.IsExited() || status.GetExitCode() != 0) {
        removed_ = true;
        throw std::runtime_error(fmt::format("docker create failed for {}", name_));
    }
}

ContainerGuard::~ContainerGuard() { remove(); }

void ContainerGuard::remove() noexcept
{
    if (removed_ || name_.empty() || starter_ == nullptr)
        return;
    try {
        auto proc = starter_->Exec(
            "docker", std::vector<std::string>{"rm", "-f", name_}, makeExecOpts()
        );
        static_cast<void>(proc.Get());
    } catch (std::exception &) {
        LOG_INFO() << fmt::format("docker rm for {} failed:", name_);
    }
    removed_ = true;
}
