#pragma once

#include <string>
#include <vector>

#include <userver/engine/subprocess/process_starter.hpp>

namespace us = userver;
namespace engine = us::engine;

/**
 * @brief RAII guard for a Docker container created for a crawl.
 *
 * On construction it invokes `docker create` with the provided arguments. On
 * destruction (or explicit `remove()`), it force removes the container.
 * Instances are movable but not copyable.
 */
class [[nodiscard]] ContainerGuard {
public:
    /**
     * @param starter Process starter to execute Docker.
     * @param name Container name to manage.
     * @param createArgs Arguments passed to `docker create`.
     * @throws std::runtime_error if creation fails.
     */
    ContainerGuard(
        engine::subprocess::ProcessStarter &starter, std::string name,
        const std::vector<std::string> &createArgs
    );
    ~ContainerGuard();

    ContainerGuard(const ContainerGuard &) = delete;
    ContainerGuard &operator=(const ContainerGuard &) = delete;
    ContainerGuard(ContainerGuard &&) noexcept;
    ContainerGuard &operator=(ContainerGuard &&) noexcept;

    /** @return Managed container name. */
    const std::string &name() const noexcept { return name_; }

    /**
     * @brief Explicitly remove the container now. Safe to call multiple times.
     */
    void remove() noexcept;

private:
    engine::subprocess::ProcessStarter *starter_ = nullptr;
    std::string name_;
    bool removed_;
};
