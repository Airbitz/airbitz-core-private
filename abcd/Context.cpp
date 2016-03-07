/*
 * Copyright (c) 2015, Airbitz, Inc.
 * All rights reserved.
 *
 * See the LICENSE file for more information.
 */

#include "Context.hpp"

namespace abcd {

std::unique_ptr<Context> gContext;

Context::Context(const std::string &rootDir, const std::string &certPath,
                 const std::string &apiKey, const std::string &hiddenBitsKey):
    apiKey_(apiKey),
    hiddenBitsKey_(hiddenBitsKey),
    paths(rootDir, certPath),
    exchangeCache(paths.exchangeCachePath())
{}

} // namespace abcd
