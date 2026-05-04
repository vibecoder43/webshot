#include "crawler/cdp_request_tracker.hpp"

#include <userver/utest/utest.hpp>

using namespace ws::crawler;
using namespace text::literals;

namespace {

UTEST(CdpRequestTracker, WaitingRequestStartsDeliverable)
{
    CdpRequestTracker tracker;

    tracker.InsertWaiting(17_i64, "Network.getResponseBody"_t, "session-1"_t);

    const auto *request = tracker.Find(17_i64);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->method, "Network.getResponseBody"_t);
    ASSERT_TRUE(request->session_id);
    EXPECT_EQ(*request->session_id, "session-1"_t);
    EXPECT_FALSE(request->ignore_response);
}

UTEST(CdpRequestTracker, IgnoredRequestStartsIgnored)
{
    CdpRequestTracker tracker;

    tracker.InsertIgnored(23_i64, "Fetch.continueRequest"_t, {});

    const auto *request = tracker.Find(23_i64);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->method, "Fetch.continueRequest"_t);
    EXPECT_FALSE(request->session_id);
    EXPECT_TRUE(request->ignore_response);
}

UTEST(CdpRequestTracker, MarkIgnoreResponsePreservesTraceContext)
{
    CdpRequestTracker tracker;

    tracker.InsertWaiting(31_i64, "Network.getResponseBody"_t, "session-2"_t);
    tracker.MarkIgnoreResponse(31_i64);

    const auto *request = tracker.Find(31_i64);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->method, "Network.getResponseBody"_t);
    ASSERT_TRUE(request->session_id);
    EXPECT_EQ(*request->session_id, "session-2"_t);
    EXPECT_TRUE(request->ignore_response);
}

UTEST(CdpRequestTracker, EraseRemovesRequest)
{
    CdpRequestTracker tracker;

    tracker.InsertWaiting(47_i64, "Page.navigate"_t, {});
    tracker.Erase(47_i64);

    EXPECT_EQ(tracker.Find(47_i64), nullptr);
    EXPECT_EQ(tracker.Size(), 0);
}

UTEST(CdpRequestTracker, UnknownRequestReturnsNull)
{
    const CdpRequestTracker tracker;

    EXPECT_EQ(tracker.Find(99_i64), nullptr);
}

} // namespace
