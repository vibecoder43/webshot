#include <cstdlib>
#include <string>

#include <limits.h>
#include <unistd.h>

#include <userver/engine/subprocess/process_starter.hpp>
#include <userver/engine/task/current_task.hpp>
#include <userver/utest/utest.hpp>

#include "container_guard.hpp"

namespace {

using namespace text::literals;

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

    std::vector<String> args{
        "create"_t,
        "ok-container"_t,
    };
    ContainerGuard guard(starter, "ok-container"_t, args);
}

UTEST(ContainerGuard, CreateFailureThrows)
{
    prependCurrentDirToPath();
    auto starter = makeStarter();

    const String name = "fail-container"_t;
    std::vector<String> args{
        "create"_t,
        "fail"_t,
    };

    EXPECT_THROW(ContainerGuard guard(starter, name, args), std::runtime_error);
}

UTEST(ContainerGuard, ExplicitRemoveIsIdempotent)
{
    prependCurrentDirToPath();
    auto starter = makeStarter();

    ContainerGuard guard(
        starter, "idempotent-container"_t,
        std::vector<String>{
            "create"_t,
            "idempotent-container"_t,
        }
    );
    guard.remove();
    guard.remove(); // should be a no-op after first call
}

UTEST(ContainerGuard, RemoveFailureIsSwallowed)
{
    prependCurrentDirToPath();
    auto starter = makeStarter();

    ContainerGuard guard(
        starter, "fail-rm"_t,
        std::vector<String>{
            "create"_t,
            "fail-rm"_t,
        }
    );
    EXPECT_NO_THROW(guard.remove());
}

UTEST(ContainerGuard, RemoveHandlesAbortedSubprocess)
{
    prependCurrentDirToPath();
    auto starter = makeStarter();

    ContainerGuard guard(
        starter, "fail-abort"_t,
        std::vector<String>{
            "create"_t,
            "fail-abort"_t,
        }
    );
    EXPECT_NO_THROW(guard.remove());
}

UTEST(ContainerGuard, RemoveHandlesSignaledSubprocess)
{
    prependCurrentDirToPath();
    auto starter = makeStarter();

    ContainerGuard guard(
        starter, "fail-signal"_t,
        std::vector<String>{
            "create"_t,
            "fail-signal"_t,
        }
    );
    EXPECT_NO_THROW(guard.remove());
}
