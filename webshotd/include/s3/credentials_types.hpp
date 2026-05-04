#pragma once

#include "text.hpp"

#include <userver/utils/strong_typedef.hpp>

namespace ws::s3 {

namespace us = userver;
using AccessKeyId = us::utils::NonLoggable<class AccessKeyIdTag, String>;
using SecretAccessKey = us::utils::NonLoggable<class SecretAccessKeyTag, String>;
using SessionToken = us::utils::NonLoggable<class SessionTokenTag, String>;

} // namespace ws::s3
