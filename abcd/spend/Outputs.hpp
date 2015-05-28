/*
 *  Copyright (c) 2015, AirBitz, Inc.
 *  All rights reserved.
 */

#ifndef ABCD_SPEND_OUTPUTS_HPP
#define ABCD_SPEND_OUTPUTS_HPP

#include "../util/Status.hpp"
#include <bitcoin/bitcoin.hpp>

namespace abcd {

typedef struct sABC_TxSendInfo tABC_SendInfo;

bc::script_type
outputScriptForPubkey(const bc::short_hash &hash);

/**
 * Creates an output script for sending money to an address.
 */
Status
outputScriptForAddress(bc::script_type &result, const std::string &address);

/**
 * Creates a set of outputs corresponding to a sABC_TxSendInfo structure.
 * Updates the info structure with the Airbitz fees, if any.
 */
Status
outputsForSendInfo(bc::transaction_output_list &result, sABC_TxSendInfo *pInfo);

} // namespace abcd

#endif
