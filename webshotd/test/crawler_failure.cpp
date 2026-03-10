#include <fstream>
#include <string>

#include <userver/engine/subprocess/process_starter.hpp>
#include <userver/engine/task/current_task.hpp>
#include <userver/fs/blocking/temp_directory.hpp>
#include <userver/utest/utest.hpp>

#include "crawler_failure.hpp"

namespace {
namespace engine = userver::engine;

using namespace text::literals;

engine::subprocess::ProcessStarter makeStarter()
{
    auto &tp = userver::engine::current_task::GetTaskProcessor();
    return engine::subprocess::ProcessStarter(tp);
}

void writeTextFile(const std::string &path, const std::string &content)
{
    std::ofstream out(path);
    ASSERT_TRUE(static_cast<bool>(out));
    out << content;
    ASSERT_TRUE(static_cast<bool>(out));
}

} // namespace

UTEST(CrawlerFailure, SanitizeProcessOutputTailNormalizesWhitespaceAndEscapesQuotes)
{
    const auto sanitized = v1::crawler::sanitizeProcessOutputTail(
        "line1\nline\t2\x01 \"quoted\" \\ path"
    );

    EXPECT_EQ(sanitized, "line1 line 2? \\\"quoted\\\" \\\\ path");
}

UTEST(CrawlerFailure, SummarizeProcessOutputsReadsBoundedStdoutAndStderr)
{
    auto tempDir = userver::fs::blocking::TempDirectory::Create();
    const auto stdoutPath = tempDir.GetPath() + "/stdout.log";
    const auto stderrPath = tempDir.GetPath() + "/stderr.log";

    writeTextFile(stdoutPath, "hello from stdout\n");
    writeTextFile(stderrPath, "failure on stderr\n");

    const auto summary = v1::crawler::summarizeProcessOutputs(stdoutPath, stderrPath);

    ASSERT_TRUE(summary.has_value());
    EXPECT_EQ(summary->view(), "stdout=\"hello from stdout\", stderr=\"failure on stderr\"");
}

UTEST(CrawlerFailure, ProcessOutputFormattingIncludesStdoutAndStderrTails)
{
    auto starter = makeStarter();
    auto tempDir = userver::fs::blocking::TempDirectory::Create();
    const auto stdoutPath = tempDir.GetPath() + "/stdout.log";
    const auto stderrPath = tempDir.GetPath() + "/stderr.log";

    auto proc = starter.Exec(
        "/bin/sh",
        std::vector<std::string>{
            "-c",
            "printf '%s\\n' 'proxy denied'; printf '%s\\n' 'tls alert' >&2; exit 21",
        },
        engine::subprocess::ExecOptions{
            .stdout_file = stdoutPath,
            .stderr_file = stderrPath,
        }
    );
    const auto status = proc.Get();

    ASSERT_TRUE(status.IsExited());
    EXPECT_EQ(status.GetExitCode(), 21);

    v1::crawler::AttemptSummary attempt{
        .exited = true,
        .exitCode = 21,
        .waczExists = false,
        .seedProbe = v1::crawler::SeedPageProbe{.status = 0, .loadState = 0},
        .failureDetail = v1::crawler::summarizeProcessOutputs(stdoutPath, stderrPath),
    };

    const auto msg = v1::crawler::formatAttemptStatus("https", attempt);

    EXPECT_EQ(
        msg.view(),
        "https exit code 21: crawler failed due to proxy error, seedProbe status=0 loadState=0, "
        "stdout=\"proxy denied\", stderr=\"tls alert\""
    );
}
