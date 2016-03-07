/*
 * Copyright (c) 2014, Airbitz, Inc.
 * All rights reserved.
 *
 * See the LICENSE file for more information.
 */

#include "JsonPtr.hpp"
#include "../crypto/Crypto.hpp"
#include "../util/Debug.hpp"
#include "../util/FileIO.hpp"
#include "../util/Util.hpp"
#include <new>

namespace abcd {

constexpr size_t loadFlags = 0;
constexpr size_t saveFlags = JSON_INDENT(4) | JSON_SORT_KEYS;
constexpr size_t saveFlagsCompact = JSON_COMPACT | JSON_SORT_KEYS;

/**
 * Overrides the jansson malloc function so we can clear the memory on free.
 * https://github.com/akheron/jansson/blob/master/doc/apiref.rst#id97
 */
static void *
janssonSecureMalloc(size_t size)
{
    // Store the memory area size in the beginning of the block:
    char *ptr = (char *)malloc(size + 8);
    *((size_t *)ptr) = size;
    return ptr + 8;
}

/**
 * Overrides the jansson free function so we can clear the memory.
 */
static void
janssonSecureFree(void *ptr)
{
    if (ptr)
    {
        ptr = (char *)ptr - 8;
        size_t size = *((size_t *)ptr);
        ABC_UtilGuaranteedMemset(ptr, 0, size + 8);
        free(ptr);
    }
}

/**
 * Sets up the secure JSON free functions.
 */
class JsonInitializer
{
public:
    JsonInitializer()
    {
        json_set_alloc_funcs(janssonSecureMalloc, janssonSecureFree);
    }
};

JsonInitializer staticJsonInitializer;

JsonPtr::~JsonPtr()
{
    reset();
}

JsonPtr::JsonPtr():
    root_(nullptr)
{}

JsonPtr::JsonPtr(JsonPtr &&move):
    root_(move.root_)
{
    move.root_ = nullptr;
}

JsonPtr::JsonPtr(const JsonPtr &copy):
    root_(json_incref(copy.root_))
{}

JsonPtr &
JsonPtr::operator=(const JsonPtr &copy)
{
    reset(json_incref(copy.root_));
    return *this;
}

JsonPtr::JsonPtr(json_t *root):
    root_(root)
{}

void
JsonPtr::reset(json_t *root)
{
    if (root_)
        json_decref(root_);
    root_ = root;
}

JsonPtr
JsonPtr::clone() const
{
    auto out = json_deep_copy(root_);
    if (!out)
        throw std::bad_alloc();
    return out;
}

Status
JsonPtr::load(const std::string &path)
{
    json_error_t error;
    json_t *root = json_load_file(path.c_str(), loadFlags, &error);
    if (!root)
        return ABC_ERROR(ABC_CC_JSONError, error.text);
    reset(root);
    return Status();
}

Status
JsonPtr::load(const std::string &path, DataSlice dataKey)
{
    json_t *root = nullptr;
    ABC_CHECK_OLD(ABC_CryptoDecryptJSONFileObject(path.c_str(),
                  toU08Buf(dataKey), &root, &error));
    reset(root);
    return Status();
}

Status
JsonPtr::decode(const std::string &data)
{
    json_error_t error;
    json_t *root = json_loadb(data.data(), data.size(), loadFlags, &error);
    if (!root)
        return ABC_ERROR(ABC_CC_JSONError, error.text);
    reset(root);
    return Status();
}

Status
JsonPtr::save(const std::string &path) const
{
    ABC_DebugLog("Writing JSON file %s", path.c_str());

    const auto pathTmp = path + ".tmp";
    if (json_dump_file(root_, pathTmp.c_str(), saveFlags))
        return ABC_ERROR(ABC_CC_FileWriteError, "Cannot write " + pathTmp);

    if (rename(pathTmp.c_str(), path.c_str()))
        return ABC_ERROR(ABC_CC_FileWriteError,
                         "Cannot rename " + pathTmp + " to " + path);

    return Status();
}

Status
JsonPtr::save(const std::string &path, DataSlice dataKey) const
{
    ABC_CHECK_OLD(ABC_CryptoEncryptJSONFileObject(root_,
                  toU08Buf(dataKey), ABC_CryptoType_AES256,
                  path.c_str(), &error));
    return Status();
}

std::string
JsonPtr::encode(bool compact) const
{
    auto raw = json_dumps(root_, compact ? saveFlagsCompact : saveFlags);
    if (!raw)
        throw std::bad_alloc();
    std::string out(raw);
    janssonSecureFree(raw);
    return out;
}

} // namespace abcd
