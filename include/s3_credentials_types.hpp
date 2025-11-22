#pragma once

#include <string>

#include <userver/utils/strong_typedef.hpp>

namespace v1::s3v4 {

using AccessKeyId = userver::utils::NonLoggable<class AccessKeyIdTag, std::string>;
using SecretAccessKey = userver::utils::NonLoggable<class SecretAccessKeyTag, std::string>;
using SessionToken = userver::utils::NonLoggable<class SessionTokenTag, std::string>;

} // namespace v1::s3v4
