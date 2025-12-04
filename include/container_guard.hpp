#pragma once

#include "text.hpp"

#include <vector>

#include <userver/engine/subprocess/process_starter.hpp>

namespace us = userver;
namespace engine = us::engine;

/**
 * @brief RAII guard for a container created for a crawl.
 *
 * On construction it invokes `create` with the provided arguments. On
 * destruction (or explicit `remove()`), it force removes the container.
 * Instances are movable but not copyable.
 */
class [[nodiscard]] ContainerGuard {
public:
    /**
     * @param starter Process starter to execute command.
     * @param name Container name to manage.
     * @param createArgs Arguments passed to `create`.
     * @throws std::runtime_error if creation fails.
     */
    ContainerGuard(
        engine::subprocess::ProcessStarter &starter, String name,
        const std::vector<String> &createArgs
    );
    ~ContainerGuard();

    ContainerGuard(const ContainerGuard &) = delete;
    ContainerGuard &operator=(const ContainerGuard &) = delete;
    ContainerGuard(ContainerGuard &&) = delete;
    ContainerGuard &operator=(ContainerGuard &&) = delete;

    /**
     * @brief Explicitly remove the container now. Safe to call multiple times.
     */
    void remove() noexcept;

private:
    engine::subprocess::ProcessStarter *starter = nullptr;
    String name;
    bool removed;
};
