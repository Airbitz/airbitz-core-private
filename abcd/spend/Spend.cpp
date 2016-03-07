/*
 * Copyright (c) 2015, Airbitz, Inc.
 * All rights reserved.
 *
 * See the LICENSE file for more information.
 */

#include "Spend.hpp"
#include "Broadcast.hpp"
#include "Inputs.hpp"
#include "Outputs.hpp"
#include "PaymentProto.hpp"
#include "../General.hpp"
#include "../Tx.hpp"
#include "../bitcoin/Watcher.hpp"
#include "../bitcoin/WatcherBridge.hpp"
#include "../bitcoin/Utility.hpp"
#include "../util/Debug.hpp"
#include "../wallet/Details.hpp"
#include "../wallet/Wallet.hpp"
#include <bitcoin/bitcoin.hpp>

namespace abcd {

static Status
spendMakeTx(libbitcoin::transaction_type &result, Wallet &self,
            SendInfo *pInfo, const std::string &changeAddress)
{
    bc::transaction_type tx;
    tx.version = 1;
    tx.locktime = 0;
    ABC_CHECK(outputsForSendInfo(tx.outputs, pInfo));

    uint64_t fee, change;
    auto utxos = self.txdb.get_utxos(self.addresses.list(), true);

    // Check if enough confirmed inputs are available,
    // otherwise use unconfirmed inputs too:
    if (!inputsPickOptimal(fee, change, tx, utxos))
    {
        auto utxos = self.txdb.get_utxos(self.addresses.list(), false);
        ABC_CHECK(inputsPickOptimal(fee, change, tx, utxos));
    }

    ABC_CHECK(outputsFinalize(tx.outputs, change, changeAddress));
    pInfo->metadata.amountFeesMinersSatoshi = fee;

    result = std::move(tx);
    return Status();
}

SendInfo::~SendInfo()
{
    delete paymentRequest;
}

SendInfo::SendInfo()
{
    paymentRequest = nullptr;
    bTransfer = false;
}

Status
spendCalculateFees(Wallet &self, SendInfo *pInfo, uint64_t &totalFees)
{
    bc::transaction_type tx;
    Address changeAddress;

    pInfo->metadata.amountFeesAirbitzSatoshi = 0;
    pInfo->metadata.amountFeesMinersSatoshi = 0;

    // Make an unsigned transaction
    ABC_CHECK(self.addresses.getNew(changeAddress));
    ABC_CHECK(spendMakeTx(tx, self, pInfo, changeAddress.address));

    totalFees = pInfo->metadata.amountFeesAirbitzSatoshi +
                pInfo->metadata.amountFeesMinersSatoshi;

    return Status();
}

Status
spendCalculateMax(Wallet &self, SendInfo *pInfo, uint64_t &maxSatoshi)
{
    bc::output_info_list utxos =
        self.txdb.get_utxos(self.addresses.list(), true);

    bc::transaction_type tx;
    tx.version = 1;
    tx.locktime = 0;
    ABC_CHECK(outputsForSendInfo(tx.outputs, pInfo));

    const auto info = generalAirbitzFeeInfo();
    uint64_t fee, usable;
    if (inputsPickMaximum(fee, usable, tx, utxos))
        maxSatoshi = generalAirbitzFeeSpendable(info, usable, pInfo->bTransfer);
    else
        maxSatoshi = 0;

    return Status();
}

Status
spendSignTx(DataChunk &result, Wallet &self, SendInfo *pInfo)
{
    Address changeAddress;
    ABC_CHECK(self.addresses.getNew(changeAddress));

    // Make an unsigned transaction:
    bc::transaction_type tx;
    ABC_CHECK(spendMakeTx(tx, self, pInfo, changeAddress.address));

    // Sign the transaction:
    KeyTable keys = self.addresses.keyTable();
    ABC_CHECK(signTx(tx, self, keys));
    result.resize(satoshi_raw_size(tx));
    bc::satoshi_save(tx, result.begin());

    ABC_DebugLog("Change: %s, Amount: %s, Contents: %s",
                 changeAddress.address.c_str(),
                 std::to_string(pInfo->metadata.amountSatoshi).c_str(),
                 bc::pretty(tx).c_str());

    return Status();
}

Status
spendBroadcastTx(Wallet &self, SendInfo *pInfo, DataSlice rawTx)
{
    // Let the merchant broadcast the transaction:
    if (pInfo->paymentRequest)
    {
        // TODO: Update the metadata with something about a refund
        Address refundAddress;
        ABC_CHECK(self.addresses.getNew(refundAddress));
        refundAddress.time = time(nullptr);
        refundAddress.metadata = pInfo->metadata;
        ABC_CHECK(self.addresses.save(refundAddress));

        bc::script_type refundScript;
        ABC_CHECK(outputScriptForAddress(refundScript, refundAddress.address));
        DataChunk refund = save_script(refundScript);

        PaymentReceipt receipt;
        ABC_CHECK(pInfo->paymentRequest->pay(receipt, rawTx, refund));

        // Append the receipt memo to the notes field:
        if (receipt.ack.has_memo())
        {
            std::string notes = pInfo->metadata.notes;
            if (!notes.empty())
                notes += '\n';
            notes += receipt.ack.memo();
            pInfo->metadata.notes = notes;
        }
    }

    // Send to the network:
    ABC_CHECK(broadcastTx(self, rawTx));

    return Status();
}

Status
spendSaveTx(Wallet &self, SendInfo *pInfo, DataSlice rawTx,
            std::string &ntxidOut)
{
    bc::transaction_type tx;
    auto deserial = bc::make_deserializer(rawTx.begin(), rawTx.end());
    bc::satoshi_load(deserial.iterator(), deserial.end(), tx);

    // Save to the transaction cache:
    if (self.txdb.insert(tx))
        watcherSave(self).log(); // Failure is not fatal

    // Update the Airbitz metadata:
    auto txid = bc::encode_hash(bc::hash_transaction(tx));
    auto ntxid = bc::encode_hash(makeNtxid(tx));
    std::vector<std::string> addresses;
    for (const auto &output: tx.outputs)
    {
        bc::payment_address addr;
        bc::extract(addr, output.script);
        addresses.push_back(addr.encoded());
    }
    ABC_CHECK(txSendSave(self, ntxid, txid, addresses, pInfo));

    ntxidOut = ntxid;
    return Status();
}

} // namespace abcd
