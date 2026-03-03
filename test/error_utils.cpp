#include <string>

#include <userver/formats/json.hpp>
#include <userver/utest/utest.hpp>

#include "error_utils.hpp"
#include "text.hpp"

using v1::errors::makeError;
using v1::errors::makeParamError;
using namespace text::literals;

UTEST(ErrorUtils, WrapsMessage)
{
    const auto value = makeError("something went wrong"_t);
    ASSERT_TRUE(value.HasMember("error"));
    const auto &err = value["error"];
    ASSERT_TRUE(err.HasMember("message"));
    EXPECT_EQ(err["message"].As<std::string>(), std::string{"something went wrong"});
}

UTEST(ErrorUtils, FormatsParamError)
{
    const auto value = makeParamError("host"_t, "missing parameter"_t);
    ASSERT_TRUE(value.HasMember("error"));
    const auto &err = value["error"];
    ASSERT_TRUE(err.HasMember("message"));
    EXPECT_EQ(err["message"].As<std::string>(), std::string{"host: missing parameter"});
}
