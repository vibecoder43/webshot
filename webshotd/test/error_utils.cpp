#include <string>

#include <userver/formats/json.hpp>
#include <userver/utest/utest.hpp>

#include "error_utils.hpp"
#include "text.hpp"

using ws::errors::MakeError;
using ws::errors::MakeParamError;
using namespace text::literals;

UTEST(ErrorUtils, WrapsMessage)
{
    const auto value = MakeError("something went wrong"_t);
    ASSERT_TRUE(value.HasMember("error"));
    const auto &err = value["error"];
    ASSERT_TRUE(err.HasMember("message"));
    EXPECT_EQ(err["message"].As<std::string>(), std::string{"something went wrong"});
}

UTEST(ErrorUtils, FormatsParamError)
{
    const auto value = MakeParamError("host"_t, "missing parameter"_t);
    ASSERT_TRUE(value.HasMember("error"));
    const auto &err = value["error"];
    ASSERT_TRUE(err.HasMember("message"));
    EXPECT_EQ(err["message"].As<std::string>(), std::string{"host: missing parameter"});
}
