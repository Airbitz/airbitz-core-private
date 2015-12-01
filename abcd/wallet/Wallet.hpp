/*
 * Copyright (c) 2015, AirBitz, Inc.
 * All rights reserved.
 *
 * See the LICENSE file for more information.
 */

#ifndef ABCD_WALLET_WALLET_HPP
#define ABCD_WALLET_WALLET_HPP

#include "../util/Data.hpp"
#include "../util/Status.hpp"
#include "AddressDb.hpp"
#include "TxMetaDb.hpp"
#include <atomic>
#include <memory>
#include <mutex>

namespace abcd {

class Account;
class TxDatabase;

/**
 * Manages the information stored in the top-level wallet sync directory.
 */
class Wallet:
    public std::enable_shared_from_this<Wallet>
{
public:
    ~Wallet();

    Account &account;

    static Status
    create(std::shared_ptr<Wallet> &result, Account &account,
           const std::string &id);

    static Status
    createNew(std::shared_ptr<Wallet> &result, Account &account,
              const std::string &name, int currency);

    const std::string &id() const { return id_; }
    const std::string &dir() const { return dir_; }
    std::string syncDir() const     { return dir() + "sync/"; }
    std::string txDir() const       { return syncDir() + "Transactions/"; }

    const DataChunk &bitcoinKey() const;
    const DataChunk &dataKey() const { return dataKey_; }

    int currency() const;
    std::string name() const;
    Status nameSet(const std::string &name);

    // Balance cache:
    Status balance(int64_t &result);
    void balanceDirty();

    /**
     * Syncs the account with the file server.
     * This is a blocking network operation.
     */
    Status
    sync(bool &dirty);

private:
    mutable std::mutex mutex_;
    const std::shared_ptr<Account> parent_;
    const std::string id_;
    const std::string dir_;

    // Account data:
    DataChunk bitcoinKey_;
    DataChunk bitcoinKeyBackup_;
    DataChunk dataKey_;
    std::string syncKey_;

    // Sync dir data:
    int currency_;
    std::string name_;
    Status currencySet(int currency);

    // Balance cache:
    int64_t balance_;
    std::atomic<bool> balanceDirty_;

    Wallet(Account &account, const std::string &id);

    Status
    createNew(const std::string &name, int currency);

    Status
    loadKeys();

    /**
     * Loads the synced data, performing an initial sync if necessary.
     */
    Status
    loadSync();

public:
    AddressDb addresses;
    TxMetaDb txs;
    TxDatabase &txdb;
};

} // namespace abcd

#endif
