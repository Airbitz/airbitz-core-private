/*
 * Copyright (c) 2014, AirBitz, Inc.
 * All rights reserved.
 *
 * See the LICENSE file for more information.
 */

#include "Inputs.hpp"
#include "Outputs.hpp"
#include "../General.hpp"
#include "../bitcoin/TxDatabase.hpp"
#include "../wallet/Wallet.hpp"
#include <unistd.h>
#include <bitcoin/bitcoin.hpp>

namespace abcd {

using namespace libbitcoin;

static std::map<data_chunk, std::string> address_map;
static operation create_data_operation(data_chunk &data);

Status
signTx(bc::transaction_type &result, const Wallet &wallet, const KeyTable &keys)
{
    for (size_t i = 0; i < result.inputs.size(); ++i)
    {
        // Find the utxo this input refers to:
        bc::input_point &point = result.inputs[i].previous_output;
        bc::transaction_type tx = wallet.txdb.txidLookup(point.hash);

        // Find the address for that utxo:
        bc::payment_address pa;
        bc::script_type &script = tx.outputs[point.index].script;
        bc::extract(pa, script);
        if (payment_address::invalid_version == pa.version())
            return ABC_ERROR(ABC_CC_Error, "Invalid address");

        // Find the elliptic curve key for this input:
        auto key = keys.find(pa.encoded());
        if (key == keys.end())
            return ABC_ERROR(ABC_CC_Error, "Missing signing key");
        bc::ec_secret secret = bc::wif_to_secret(key->second);
        bc::ec_point pubkey = bc::secret_to_public_key(secret,
                              bc::is_wif_compressed(key->second));

        // Gererate the previous output's signature:
        // TODO: We already have this; process it and use it
        script_type sig_script = outputScriptForPubkey(pa.hash());

        // Generate the signature for this input:
        hash_digest sig_hash =
            script_type::generate_signature_hash(result, i, sig_script, 1);
        if (sig_hash == null_hash)
            return ABC_ERROR(ABC_CC_Error, "Unable to sign");
        data_chunk signature = sign(secret, sig_hash,
                                    create_nonce(secret, sig_hash));
        signature.push_back(0x01);

        // Create out scriptsig:
        script_type scriptsig;
        scriptsig.push_operation(create_data_operation(signature));
        scriptsig.push_operation(create_data_operation(pubkey));
        result.inputs[i].script = scriptsig;
    }

    return Status();
}

static operation create_data_operation(data_chunk &data)
{
    BITCOIN_ASSERT(data.size() < std::numeric_limits<uint32_t>::max());
    operation op;
    op.data = data;
    if (data.size() <= 75)
        op.code = opcode::special;
    else if (data.size() < std::numeric_limits<uint8_t>::max())
        op.code = opcode::pushdata1;
    else if (data.size() < std::numeric_limits<uint16_t>::max())
        op.code = opcode::pushdata2;
    else if (data.size() < std::numeric_limits<uint32_t>::max())
        op.code = opcode::pushdata4;
    return op;
}

bool gather_challenges(unsigned_transaction &utx, Wallet &wallet)
{
    utx.challenges.resize(utx.tx.inputs.size());

    for (size_t i = 0; i < utx.tx.inputs.size(); ++i)
    {
        bc::input_point &point = utx.tx.inputs[i].previous_output;
        if (!wallet.txdb.txidExists(point.hash))
            return false;
        bc::transaction_type tx = wallet.txdb.txidLookup(point.hash);
        utx.challenges[i] = tx.outputs[point.index].script;
    }

    return true;
}

bool sign_tx(unsigned_transaction &utx, const key_table &keys)
{
    bool all_done = true;

    for (size_t i = 0; i < utx.tx.inputs.size(); ++i)
    {
        auto &input = utx.tx.inputs[i];
        auto &challenge = utx.challenges[i];

        // Already signed?
        if (input.script.operations().size())
            continue;

        // Extract the address:
        bc::payment_address from_address;
        if (!bc::extract(from_address, challenge))
        {
            all_done = false;
            continue;
        }

        // Find a matching key:
        auto key = keys.find(from_address);
        if (key == keys.end())
        {
            all_done = false;
            continue;
        }
        auto &secret = key->second.secret;
        auto pubkey = bc::secret_to_public_key(secret, key->second.compressed);

        // Create the sighash for this input:
        hash_digest sighash =
            script_type::generate_signature_hash(utx.tx, i, challenge, 1);
        if (sighash == null_hash)
        {
            all_done = false;
            continue;
        }

        // Sign:
        data_chunk signature = sign(secret, sighash,
                                    create_nonce(secret, sighash));
        signature.push_back(0x01);

        // Save:
        script_type scriptsig;
        scriptsig.push_operation(create_data_operation(signature));
        scriptsig.push_operation(create_data_operation(pubkey));
        utx.tx.inputs[i].script = scriptsig;
    }

    return all_done;
}

static uint64_t
minerFee(const bc::transaction_type &tx, uint64_t sourced,
         const BitcoinFeeInfo &feeInfo)
{
    // Signature scripts have a 72-byte signature plus a 32-byte pubkey:
    size_t size = satoshi_raw_size(tx) + 104 * tx.inputs.size();

    // Look up the size-based fees from the table:
    uint64_t sizeFee = 0;
    for (const auto &row: feeInfo)
    {
        if (size <= row.first)
        {
            sizeFee = row.second;
            break;
        }
    }
    if (!sizeFee)
        sizeFee = feeInfo.rbegin()->second;

    // The amount-based fee is 0.1% of total funds sent:
    uint64_t amountFee = sourced / 1000;

    // Clamp the amount fee between 50% and 100% of the size-based fee:
    uint64_t minFee = sizeFee / 2;
    uint64_t incFee = sizeFee / 10;
    amountFee = std::max(amountFee, minFee);
    amountFee = std::min(amountFee, sizeFee);

    // Make the result an integer multiple of the minimum fee:
    return amountFee - amountFee % incFee;
}

Status
inputsPickOptimal(uint64_t &resultFee, uint64_t &resultChange,
                  bc::transaction_type &tx, bc::output_info_list &utxos)
{
    auto totalOut = outputsTotal(tx.outputs);

    const auto feeInfo = generalBitcoinFeeInfo();
    uint64_t sourced = 0;
    uint64_t fee = 0;
    do
    {
        // Select a collection of outputs that satisfies our requirements:
        auto chosen = select_outputs(utxos, totalOut + fee);
        if (!chosen.points.size())
            return ABC_ERROR(ABC_CC_InsufficientFunds, "Insufficent funds");
        sourced = totalOut + fee + chosen.change;

        // Calculate the fees for this input combination:
        tx.inputs.clear();
        for (auto &point: chosen.points)
        {
            transaction_input_type input;
            input.sequence = 4294967295;
            input.previous_output = point;
            tx.inputs.push_back(input);
        }
        fee = minerFee(tx, sourced, feeInfo);

        // Guard against any potential fee insanity:
        fee = std::min<uint64_t>(1000000, fee);
    }
    while (sourced < totalOut + fee);

    resultFee = fee;
    resultChange = sourced - (totalOut + fee);
    return Status();
}

Status
inputsPickMaximum(uint64_t &resultFee, uint64_t &resultChange,
                  bc::transaction_type &tx, bc::output_info_list &utxos)
{
    auto totalOut = outputsTotal(tx.outputs);

    // Calculate the fees for this input combination:
    tx.inputs.clear();
    uint64_t sourced = 0;
    for (auto &utxo: utxos)
    {
        bc::transaction_input_type input;
        input.sequence = 4294967295;
        input.previous_output = utxo.point;
        tx.inputs.push_back(input);
        sourced += utxo.value;
    }
    const auto feeInfo = generalBitcoinFeeInfo();
    uint64_t fee = minerFee(tx, sourced, feeInfo);

    // Verify that we have enough:
    uint64_t totalIn = 0;
    for (auto &utxo: utxos)
        totalIn += utxo.value;
    if (totalIn < totalOut + fee)
        return ABC_ERROR(ABC_CC_InsufficientFunds, "Insufficent funds");

    resultFee = fee;
    resultChange = totalIn - (totalOut + fee);
    return Status();
}

} // namespace abcd
