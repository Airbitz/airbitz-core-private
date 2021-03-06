/*
 * Copyright (c) 2015, Airbitz, Inc.
 * All rights reserved.
 *
 * See the LICENSE file for more information.
 */

#include "Wallet.hpp"
#include "../Context.hpp"
#include "../account/Account.hpp"
#include "../bitcoin/cache/Cache.hpp"
#include "../crypto/Encoding.hpp"
#include "../crypto/Random.hpp"
#include "../json/JsonObject.hpp"
#include "../login/Login.hpp"
#include "../login/server/LoginServer.hpp"
#include "../util/FileIO.hpp"
#include "../util/Sync.hpp"
#include <assert.h>

namespace abcd {

struct WalletJson:
    public JsonObject
{
    ABC_JSON_STRING(bitcoinKey, "BitcoinSeed", nullptr)
    ABC_JSON_STRING(dataKey,    "MK",          nullptr)
    ABC_JSON_STRING(syncKey,    "SyncKey",     nullptr)
};

struct CurrencyJson:
    public JsonObject
{
    ABC_JSON_INTEGER(currency, "num", 840)
};

struct NameJson:
    public JsonObject
{
    ABC_JSON_STRING(name, "walletName", "Wallet With No Name")
};

Wallet::~Wallet()
{
    delete &cache;
}

Status
Wallet::create(std::shared_ptr<Wallet> &result, Account &account,
               const std::string &id)
{
    std::shared_ptr<Wallet> out(new Wallet(account, id));
    ABC_CHECK(out->loadKeys());
    ABC_CHECK(out->loadSync());

    // Load the transaction cache (failure is fine):
    if (!out->cache.load().log())
        out->cache.loadLegacy(out->paths.cachePathOld());

    result = std::move(out);
    return Status();
}

Status
Wallet::createNew(std::shared_ptr<Wallet> &result, Account &account,
                  const std::string &name, int currency)
{
    std::string id;
    ABC_CHECK(randomUuid(id));
    std::shared_ptr<Wallet> out(new Wallet(account, id));
    ABC_CHECK(out->createNew(name, currency));

    // Load the transaction cache (failure is fine):
    if (!out->cache.load().log())
        out->cache.loadLegacy(out->paths.cachePathOld());

    result = std::move(out);
    return Status();
}

const DataChunk &
Wallet::bitcoinKey() const
{
    // We do not want memory corruption here.
    // Otherwise, we might generate a bad bitcoin address and lose money:
    assert(bitcoinKeyBackup_ == bitcoinKey_);
    return bitcoinKey_;
}

std::string
Wallet::bitcoinXPub()
{
    assert(bitcoinXPub_ == bitcoinXPubBackup_);
    return bitcoinXPub_;
}

int
Wallet::currency() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return currency_;
}

std::string
Wallet::name() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return name_;
}

Status
Wallet::currencySet(int currency)
{
    std::lock_guard<std::mutex> lock(mutex_);

    currency_ = currency;
    CurrencyJson currencyJson;
    ABC_CHECK(currencyJson.currencySet(currency));
    ABC_CHECK(currencyJson.save(paths.currencyPath(), dataKey()));

    return Status();
}

Status
Wallet::nameSet(const std::string &name)
{
    std::lock_guard<std::mutex> lock(mutex_);

    name_ = name;
    name_.erase(std::remove(name_.begin(), name_.end(), '\''), name_.end());

    NameJson json;
    ABC_CHECK(json.nameSet(name));
    ABC_CHECK(json.save(paths.namePath(), dataKey()));

    return Status();
}

Status
Wallet::balance(int64_t &result)
{
    // We cannot put a mutex in `balanceDirty()`, since that will deadlock
    // the transaction database during the balance calculation.
    // Instead, we access `balanceDirty_` atomically outside the mutex:
    bool dirty = balanceDirty_;
    balanceDirty_ = false;

    std::lock_guard<std::mutex> lock(mutex_);
    if (dirty)
    {
        const auto utxos = cache.txs.utxos(addresses.list());

        balance_ = 0;
        for (const auto &utxo: utxos)
            balance_ += utxo.value;
    }

    result = balance_;
    return Status();
}

void
Wallet::balanceDirty()
{
    balanceDirty_ = true;
}

Status
Wallet::sync(bool &dirty)
{
    ABC_CHECK(syncRepo(paths.syncDir(), syncKey_, dirty));
    if (dirty)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ABC_CHECK(loadSync());
    }

    return Status();
}

Wallet::Wallet(Account &account, const std::string &id):
    account(account),
    paths(gContext->paths.walletDir(id)),
    parent_(account.shared_from_this()),
    id_(id),
    balanceDirty_(true),
    addresses(*this),
    txs(*this),
    cache(*new Cache(paths.cachePath(), gContext->blockCache,
                     gContext->serverCache))
{}

Status
Wallet::createNew(const std::string &name, int currency)
{
    // Set up the keys:
    ABC_CHECK(randomData(bitcoinKey_, BITCOIN_SEED_LENGTH));
    bitcoinKeyBackup_ = bitcoinKey_;
    ABC_CHECK(randomData(dataKey_, DATA_KEY_LENGTH));
    DataChunk syncKey;
    ABC_CHECK(randomData(syncKey, SYNC_KEY_LENGTH));
    syncKey_ = base16Encode(syncKey);

    // Create the sync directory:
    ABC_CHECK(fileEnsureDir(gContext->paths.walletsDir()));
    ABC_CHECK(fileEnsureDir(paths.dir()));
    ABC_CHECK(syncMakeRepo(paths.syncDir()));

    // Populate the sync directory:
    ABC_CHECK(currencySet(currency));
    ABC_CHECK(nameSet(name));
    ABC_CHECK(addresses.load());

    // Push the wallet to the server:
    bool dirty = false;
    ABC_CHECK(loginServerWalletCreate(account.login, syncKey_));
    ABC_CHECK(syncRepo(paths.syncDir(), syncKey_, dirty));
    ABC_CHECK(loginServerWalletActivate(account.login, syncKey_));

    // If everything worked, add the wallet to the account:
    WalletJson json;
    ABC_CHECK(json.bitcoinKeySet(base16Encode(bitcoinKey_)));
    ABC_CHECK(json.dataKeySet(base16Encode(dataKey_)));
    ABC_CHECK(json.syncKeySet(syncKey_));
    ABC_CHECK(account.wallets.insert(id_, json));
    ABC_CHECK(account.sync(dirty));

    return Status();
}

Status
Wallet::loadKeys()
{
    WalletJson json;
    ABC_CHECK(account.wallets.json(json, id()));
    ABC_CHECK(json.bitcoinKeyOk());
    ABC_CHECK(json.dataKeyOk());
    ABC_CHECK(json.syncKeyOk());

    ABC_CHECK(base16Decode(bitcoinKey_, json.bitcoinKey()));
    bitcoinKeyBackup_ = bitcoinKey_;
    ABC_CHECK(base16Decode(dataKey_, json.dataKey()));
    syncKey_ = json.syncKey();

    const auto m0 = bc::hd_private_key(bitcoinKey_).generate_public_key(0);
    bitcoinXPub_ = m0.encoded();
    bitcoinXPubBackup_ = bitcoinXPub_;

    return Status();
}

Status
Wallet::loadSync()
{
    ABC_CHECK(fileEnsureDir(gContext->paths.walletsDir()));
    ABC_CHECK(fileEnsureDir(paths.dir()));
    ABC_CHECK(syncEnsureRepo(paths.syncDir(), paths.dir() + "tmp/", syncKey_));

    // Load the currency:
    CurrencyJson currencyJson;
    currencyJson.load(paths.currencyPath(), dataKey());
    currency_ = currencyJson.currency();

    // Load the name (failure is acceptable):
    NameJson json;
    json.load(paths.namePath(), dataKey());
    name_ = json.name();

    // Load the databases:
    ABC_CHECK(addresses.load());
    ABC_CHECK(txs.load());

    return Status();
}

} // namespace abcd
