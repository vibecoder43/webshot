#pragma once

#include "integers.hpp"
#include "text.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include <userver/utils/assert.hpp>

namespace v1::crawler {

struct [[nodiscard]] CdpPendingRequest final {
    std::string method;
    std::optional<String> sessionId;
    bool ignoreResponse{false};
};

class [[nodiscard]] CdpRequestTracker final {
public:
    void insertWaiting(i64 id, std::string method, std::optional<String> sessionId)
    {
        insert(
            id, CdpPendingRequest{
                    .method = std::move(method),
                    .sessionId = std::move(sessionId),
                    .ignoreResponse = false,
                }
        );
    }

    void insertIgnored(i64 id, std::string method, std::optional<String> sessionId)
    {
        insert(
            id, CdpPendingRequest{
                    .method = std::move(method),
                    .sessionId = std::move(sessionId),
                    .ignoreResponse = true,
                }
        );
    }

    [[nodiscard]] CdpPendingRequest *find(i64 id) noexcept
    {
        const auto it = requests.find(id);
        if (it == std::end(requests))
            return nullptr;
        return &it->second;
    }

    [[nodiscard]] const CdpPendingRequest *find(i64 id) const noexcept
    {
        const auto it = requests.find(id);
        if (it == std::end(requests))
            return nullptr;
        return &it->second;
    }

    void markIgnoreResponse(i64 id)
    {
        auto *request = find(id);
        UINVARIANT(request != nullptr, "cannot ignore unknown cdp request");
        request->ignoreResponse = true;
    }

    void erase(i64 id) noexcept { requests.erase(id); }

    [[nodiscard]] size_t size() const noexcept { return requests.size(); }

private:
    void insert(i64 id, CdpPendingRequest request)
    {
        const auto [_, inserted] = requests.emplace(id, std::move(request));
        UINVARIANT(inserted, "duplicate cdp request id");
    }

    std::unordered_map<i64, CdpPendingRequest> requests;
};

} // namespace v1::crawler
