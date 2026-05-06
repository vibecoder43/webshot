#include "crawler/cgroup_stats.hpp"

#include <userver/utest/utest.hpp>

using namespace ws::crawler;

UTEST(CgroupStats, ParsesSnapshot)
{
    const std::string snapshot{"[cpu.stat]\n"
                               "usage_usec 100\n"
                               "user_usec 40\n"
                               "system_usec 60\n"
                               "nr_throttled 2\n"
                               "throttled_usec 30\n"
                               "[memory.current]\n"
                               "4096\n"
                               "[memory.events]\n"
                               "oom 1\n"
                               "oom_kill 1\n"
                               "oom_group_kill 1\n"
                               "[io.stat]\n"
                               "8:0 rbytes:10 wbytes:20 rios:1 wios:2\n"
                               "8:16 rbytes:30 wbytes:40 rios:3 wios:4\n"};

    const auto parsed = ParseCgroupStatsSnapshot(snapshot);

    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->cpu_usage_usec, 100);
    EXPECT_EQ(parsed->cpu_user_usec, 40);
    EXPECT_EQ(parsed->cpu_system_usec, 60);
    EXPECT_EQ(parsed->memory_current, 4096);
    EXPECT_EQ(parsed->memory_oom, 1);
    EXPECT_EQ(parsed->memory_oom_kill, 1);
    EXPECT_EQ(parsed->memory_oom_group_kill, 1);
    EXPECT_TRUE(HasBrowserOomKill(*parsed));
    EXPECT_EQ(parsed->io_read_bytes, 40);
    EXPECT_EQ(parsed->io_write_bytes, 60);
    EXPECT_EQ(parsed->io_read_ops, 4);
    EXPECT_EQ(parsed->io_write_ops, 6);
}
