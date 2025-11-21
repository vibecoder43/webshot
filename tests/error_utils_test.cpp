#include <string>

#include <userver/formats/json.hpp>
#include <userver/utest/utest.hpp>

#include "error_utils.hpp"

using v1::errors::makeError;
using v1::errors::makeParamError;

UTEST(ErrorUtils, WrapsMessage)
{
    const auto value = makeError("something went wrong");
    ASSERT_TRUE(value.HasMember("error"));
    const auto &err = value["error"];
    ASSERT_TRUE(err.HasMember("message"));
    EXPECT_EQ(err["message"].As<std::string>(), std::string{"something went wrong"});
}

UTEST(ErrorUtils, FormatsParamError)
{
    const auto value = makeParamError("host", "missing parameter");
    ASSERT_TRUE(value.HasMember("error"));
    const auto &err = value["error"];
    ASSERT_TRUE(err.HasMember("message"));
    EXPECT_EQ(err["message"].As<std::string>(), std::string{"missing parameter: host"});
}
