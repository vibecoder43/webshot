#include <cstdlib>
#include <string>

#include <limits.h>
#include <unistd.h>

#include <userver/engine/subprocess/process_starter.hpp>
#include <userver/engine/task/current_task.hpp>
#include <userver/utest/utest.hpp>

#include "container_guard.hpp"

namespace {

void prependCurrentDirToPath()
{
    char cwd[PATH_MAX] = {};
    ASSERT_NE(::getcwd(cwd, sizeof(cwd)), nullptr);

    const char *oldPath = std::getenv("PATH");
    std::string newPath(cwd);
    if (oldPath && *oldPath) {
        newPath.push_back(':');
        newPath.append(oldPath);
    }
    ASSERT_EQ(::setenv("PATH", newPath.c_str(), 1), 0);
}

engine::subprocess::ProcessStarter makeStarter()
{
    auto &tp = userver::engine::current_task::GetTaskProcessor();
    return engine::subprocess::ProcessStarter(tp);
}

} // namespace

UTEST(ContainerGuard, CreateAndRemoveOnDestruction)
{
    prependCurrentDirToPath();
    auto starter = makeStarter();

    const std::string name = "ok-container";
    const std::vector<std::string> args{"create", name};

    {
        ContainerGuard guard(starter, name, args);
        EXPECT_EQ(guard.name(), name);
    }
}

UTEST(ContainerGuard, CreateFailureThrows)
{
    prependCurrentDirToPath();
    auto starter = makeStarter();

    const std::string name = "fail-container";
    const std::vector<std::string> args{"create", "fail"};

    EXPECT_THROW(ContainerGuard guard(starter, name, args), std::runtime_error);
}

UTEST(ContainerGuard, MoveTransfersOwnership)
{
    prependCurrentDirToPath();
    auto starter = makeStarter();

    const std::string name = "move-container";
    const std::vector<std::string> args{"create", name};

    ContainerGuard first(starter, name, args);
    EXPECT_EQ(first.name(), name);

    ContainerGuard second(std::move(first));
    EXPECT_EQ(second.name(), name);
}

UTEST(ContainerGuard, MoveAssignmentRemovesPrevious)
{
    prependCurrentDirToPath();
    auto starter = makeStarter();

    ContainerGuard first(starter, "first-container", {"create", "first-container"});
    ContainerGuard second(starter, "second-container", {"create", "second-container"});

    second = std::move(first);
    EXPECT_EQ(second.name(), std::string("first-container"));
}

UTEST(ContainerGuard, ExplicitRemoveIsIdempotent)
{
    prependCurrentDirToPath();
    auto starter = makeStarter();

    ContainerGuard guard(starter, "idempotent-container", {"create", "idempotent-container"});
    guard.remove();
    guard.remove(); // should be a no-op after first call
}
