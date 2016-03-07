/*
 * Copyright (c) 2015, Airbitz, Inc.
 * All rights reserved.
 *
 * See the LICENSE file for more information.
 */

#ifndef ABCD_ACCOUNT_WALLET_LIST_HPP
#define ABCD_ACCOUNT_WALLET_LIST_HPP

#include "../json/JsonPtr.hpp"
#include <list>
#include <map>
#include <mutex>

namespace abcd {

class Account;

/**
 * Manages the list of wallets stored under the account sync directory.
 * Uses a write-through caching scheme, where changes go straight to disk,
 * but queries come out of RAM.
 */
class WalletList
{
public:
    WalletList(const Account &account);

    /**
     * Loads the wallets off disk.
     * This should be done after logging in and after a dirty sync.
     */
    Status
    load();

    /**
     * Obtains a sorted list of wallets.
     */
    std::list<std::string>
    list() const;

    /**
     * Adjusts the sort index of a wallet.
     */
    Status
    reorder(const std::string &id, unsigned index);

    /**
     * Adds a new wallet to the account.
     */
    Status
    insert(const std::string &id, const JsonPtr &keys);

    /**
     * Remove a wallet.
     */
    Status
    remove(const std::string &id);

    /**
     * Returns the meta-data file for a wallet.
     * The wallet stores its keys and seeds in here.
     */
    Status
    json(JsonPtr &result, const std::string &id) const;

    /**
     * Returns the archived state for the given id.
     */
    Status
    archived(bool &result, const std::string &id) const;

    /**
     * Adjusts the archived status of a wallet.
     */
    Status
    archivedSet(const std::string &id, bool archived);

private:
    mutable std::mutex mutex_;
    const Account &account_;
    const std::string dir_;

    std::map<std::string, JsonPtr> wallets_;

    /**
     * Builds the path to a wallet file.
     */
    std::string
    path(const std::string &name) const;
};

} // namespace abcd

#endif
