#include "crawler/cdp_request_tracker.hpp"

#include <userver/utest/utest.hpp>

using namespace v1::crawler;
using namespace text::literals;

namespace {

UTEST(CdpRequestTracker, WaitingRequestStartsDeliverable)
{
    CdpRequestTracker tracker;

    tracker.insertWaiting(17_i64, "Network.getResponseBody", "session-1"_t);

    const auto *request = tracker.find(17_i64);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->method, "Network.getResponseBody");
    ASSERT_TRUE(request->sessionId.has_value());
    EXPECT_EQ(*request->sessionId, "session-1"_t);
    EXPECT_FALSE(request->ignoreResponse);
}

UTEST(CdpRequestTracker, IgnoredRequestStartsIgnored)
{
    CdpRequestTracker tracker;

    tracker.insertIgnored(23_i64, "Fetch.continueRequest", {});

    const auto *request = tracker.find(23_i64);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->method, "Fetch.continueRequest");
    EXPECT_FALSE(request->sessionId.has_value());
    EXPECT_TRUE(request->ignoreResponse);
}

UTEST(CdpRequestTracker, MarkIgnoreResponsePreservesTraceContext)
{
    CdpRequestTracker tracker;

    tracker.insertWaiting(31_i64, "Network.getResponseBody", "session-2"_t);
    tracker.markIgnoreResponse(31_i64);

    const auto *request = tracker.find(31_i64);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->method, "Network.getResponseBody");
    ASSERT_TRUE(request->sessionId.has_value());
    EXPECT_EQ(*request->sessionId, "session-2"_t);
    EXPECT_TRUE(request->ignoreResponse);
}

UTEST(CdpRequestTracker, EraseRemovesRequest)
{
    CdpRequestTracker tracker;

    tracker.insertWaiting(47_i64, "Page.navigate", {});
    tracker.erase(47_i64);

    EXPECT_EQ(tracker.find(47_i64), nullptr);
    EXPECT_EQ(tracker.size(), 0);
}

UTEST(CdpRequestTracker, UnknownRequestReturnsNull)
{
    const CdpRequestTracker tracker;

    EXPECT_EQ(tracker.find(99_i64), nullptr);
}

} // namespace
