/*
 * Copyright (c) 2014, Airbitz, Inc.
 * All rights reserved.
 *
 * See the LICENSE file for more information.
 */

#include "JsonObject.hpp"

namespace abcd {

Status
JsonObject::setValue(const char *key, json_t *value)
{
    if (!json_is_object(root_))
        reset(json_object());
    if (!root_)
        return ABC_ERROR(ABC_CC_JSONError, "Cannot create root object.");
    if (json_object_set_new(root_, key, value) < 0)
        return ABC_ERROR(ABC_CC_JSONError, "Cannot set " + std::string(key));
    return Status();
}

#define IMPLEMENT_HAS(test) \
    json_t *value = json_object_get(root_, key); \
    if (!value || !(test)) \
        return ABC_ERROR(ABC_CC_JSONError, "Bad JSON value for " + std::string(key)); \
    return Status();

Status
JsonObject::hasString(const char *key) const
{
    IMPLEMENT_HAS(json_is_string(value))
}

Status
JsonObject::hasNumber(const char *key) const
{
    IMPLEMENT_HAS(json_is_number(value))
}

Status
JsonObject::hasBoolean(const char *key) const
{
    IMPLEMENT_HAS(json_is_boolean(value))
}

Status
JsonObject::hasInteger(const char *key) const
{
    IMPLEMENT_HAS(json_is_integer(value))
}

#define IMPLEMENT_GET(type) \
    json_t *value = json_object_get(root_, key); \
    if (!value || !json_is_##type(value)) \
        return fallback; \
    return json_##type##_value(value);

const char *
JsonObject::getString(const char *key, const char *fallback) const
{
    IMPLEMENT_GET(string)
}

double
JsonObject::getNumber(const char *key, double fallback) const
{
    IMPLEMENT_GET(number)
}

bool
JsonObject::getBoolean(const char *key, bool fallback) const
{
    IMPLEMENT_GET(boolean)
}

json_int_t
JsonObject::getInteger(const char *key, json_int_t fallback) const
{
    IMPLEMENT_GET(integer)
}

} // namespace abcd
