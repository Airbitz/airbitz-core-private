/*
 * Copyright (c) 2016, Airbitz, Inc.
 * All rights reserved.
 *
 * See the LICENSE file for more information.
 */

#include "../abcd/bitcoin/TxDatabase.hpp"
#include "../abcd/bitcoin/Utility.hpp"
#include "../abcd/spend/Outputs.hpp"
#include "../minilibs/catch/catch.hpp"

namespace abcd {

/**
 * Fills a transaction database with carefully-crafted test data.
 */
class TxDatabaseTest
{
public:
    abcd::AddressSet ourAddresses;

    bc::hash_digest irrelevantId;
    bc::hash_digest incomingId;
    bc::hash_digest buriedId;
    bc::hash_digest confirmedId;
    bc::hash_digest changeId;
    bc::hash_digest doubleSpendId;
    bc::hash_digest badSpendId;

    TxDatabaseTest(abcd::TxDatabase &txdb)
    {
        // Create an address for ourselves:
        bc::ec_secret ourSecret{{0xff}};
        auto ourPubkey = bc::secret_to_public_key(ourSecret);
        bc::payment_address ourAddress(bc::payment_address::pubkey_version,
                                       bc::bitcoin_short_hash(ourPubkey));
        ourAddresses.insert(ourAddress.encoded());

        // Script sending money to our address:
        bc::script_type ourReceive;
        abcd::outputScriptForAddress(ourReceive, ourAddress.encoded());

        // Script spending money from our address (fake signature):
        bc::script_type ourSpend;
        ourSpend.push_operation(makePushOperation(bc::data_chunk{0xff}));
        ourSpend.push_operation(makePushOperation(ourPubkey));

        // Script sending money to a fake address:
        bc::script_type otherReceive;
        abcd::outputScriptForAddress(otherReceive,
                                     "1QLbz7JHiBTspS962RLKV8GndWFwi5j6Qr");

        bc::hash_digest fakeTxid{};

        txdb.at_height(100);

        // One output, not connected to anything:
        bc::transaction_type irrelevant
        {
            0, 0,
            {
                {{fakeTxid, 0}, {}, 0}
            },
            {
                {1, otherReceive}
            }
        };
        irrelevantId = bc::hash_transaction(irrelevant);
        txdb.insert(irrelevant);

        // One output to an address we control:
        bc::transaction_type incoming
        {
            0, 0,
            {
                {{fakeTxid, 1}, {}, 0}
            },
            {
                {2, ourReceive}
            }
        };
        incomingId = bc::hash_transaction(incoming);
        txdb.insert(incoming);

        // Two spent outputs to addresses we control (confirmed):
        bc::transaction_type buried
        {
            0, 0,
            {
                {{fakeTxid, 2}, {}, 0}
            },
            {
                {3, ourReceive},
                {4, ourReceive}
            }
        };
        buriedId = bc::hash_transaction(buried);
        txdb.insert(buried);
        txdb.confirmed(buriedId, 100);

        // Spend from buried[0], one output (confirmed):
        bc::transaction_type confirmed
        {
            0, 0,
            {
                {{buriedId, 0}, ourSpend, 0}
            },
            {
                {5, ourReceive}
            }
        };
        confirmedId = bc::hash_transaction(confirmed);
        txdb.insert(confirmed);
        txdb.confirmed(confirmedId, 100);

        // Double-spend from buried[0]:
        bc::transaction_type doubleSpend
        {
            0, 0,
            {
                {{buriedId, 0}, ourSpend, 0}
            },
            {
                {6, ourReceive}
            }
        };
        doubleSpendId = bc::hash_transaction(doubleSpend);
        txdb.insert(doubleSpend);

        // Spend from buried[1], two outputs:
        bc::transaction_type change
        {
            0, 0,
            {
                {{buriedId, 1}, ourSpend, 0}
            },
            {
                {7, ourReceive},
                {8, ourReceive}
            }
        };
        changeId = bc::hash_transaction(change);
        txdb.insert(change);

        // Spend from doubleSpend[0] and change[0]:
        bc::transaction_type badSpend
        {
            0, 0,
            {
                {{doubleSpendId, 0}, ourSpend, 0},
                {{changeId, 0}, ourSpend, 0}
            },
            {
                {9, ourReceive}
            }
        };
        badSpendId = bc::hash_transaction(badSpend);
        txdb.insert(badSpend);
    }
};

} // namespace abcd

static void
dumpUtxos(const bc::output_info_list &utxos)
{
    for (const auto &i: utxos)
    {
        std::cout << bc::encode_hash(i.point.hash) << ":" << i.point.index;
        std::cout << " " << i.value << std::endl;
    }
}

static bool
hasTxid(const bc::output_info_list &utxos, bc::hash_digest txid, uint32_t index)
{
    auto pred = [&](const bc::output_info_type &item) -> bool
    {
        return item.point.hash == txid && item.point.index == index;
    };
    return utxos.end() != std::find_if(utxos.begin(), utxos.end(), pred);
}

TEST_CASE("Transaction database", "[bitcoin][database]")
{
    abcd::TxDatabase txdb;
    abcd::TxDatabaseTest test(txdb);

    SECTION("height")
    {
        REQUIRE(txdb.last_height() == 100);
    }

    SECTION("filtered utxos")
    {
        auto utxos = txdb.get_utxos(test.ourAddresses, true);
        if (false)
            dumpUtxos(utxos);
        REQUIRE(2 == utxos.size());
        REQUIRE(hasTxid(utxos, test.confirmedId, 0));
        REQUIRE(hasTxid(utxos, test.changeId, 1));
        REQUIRE(!hasTxid(utxos, test.badSpendId, 0));
    }

    SECTION("all utxos")
    {
        auto utxos = txdb.get_utxos(test.ourAddresses, false);
        REQUIRE(3 == utxos.size());
        REQUIRE(hasTxid(utxos, test.incomingId, 0));
        REQUIRE(hasTxid(utxos, test.confirmedId, 0));
        REQUIRE(hasTxid(utxos, test.changeId, 1));
        REQUIRE(!hasTxid(utxos, test.badSpendId, 0));
    }
}
