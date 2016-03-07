/*
 * Copyright (c) 2014, Airbitz, Inc.
 * All rights reserved.
 *
 * See the LICENSE file for more information.
 */

#include "TxDatabase.hpp"
#include "Utility.hpp"
#include "WatcherBridge.hpp"
#include "../util/Debug.hpp"
#include <unordered_set>

namespace std {

/**
 * Allows `bc::point_type` to be used with `std::unordered_set`.
 */
template<> struct hash<bc::point_type>
{
    typedef bc::point_type argument_type;
    typedef std::size_t result_type;

    result_type
    operator()(argument_type const &p) const
    {
        auto h = libbitcoin::from_little_endian_unsafe<result_type>(
                     p.hash.begin());
        return h ^ p.index;
    }
};

} // namespace std

namespace abcd {

// Serialization stuff:
constexpr uint32_t old_serial_magic = 0x3eab61c3; // From the watcher
constexpr uint32_t serial_magic = 0xfecdb763;
constexpr uint8_t serial_tx = 0x42;

typedef std::unordered_set<bc::hash_digest> TxidSet;
typedef std::unordered_set<bc::point_type> PointSet;

/**
 * Knows how to check a transaction for double-spends.
 * This uses a memoized recursive function to do the graph search,
 * so the more checks this object performs,
 * the faster those checks can potentially become (for a fixed graph).
 */
class TxFilter
{
public:
    TxFilter(const TxDatabase &cache,
             const PointSet &doubleSpends,
             const AddressSet &addresses):
        cache_(cache),
        doubleSpends_(doubleSpends),
        addresses_(addresses)
    {
    }

    /**
     * Returns true if a transaction is safe to spend from.
     * @param filter true to reject unconfirmed non-change transactions.
     */
    bool
    check(bc::hash_digest txid, const TxDatabase::TxRow &row, bool filter)
    {
        // If filter is true, we want to eliminate non-change transactions:
        if (filter && TxState::confirmed != row.state)
        {
            // This is a spend if we control all the inputs:
            for (auto &input: row.tx.inputs)
            {
                bc::payment_address address;
                if (!bc::extract(address, input.script) ||
                        !addresses_.count(address.encoded()))
                    return false;
            }
        }

        // Now check for double-spends:
        return isSafe(txid);
    }

    /**
     * Recursively checks the transaction graph for double-spends.
     * @return true if the transaction never sources a double spend.
     */
    bool
    isSafe(bc::hash_digest txid)
    {
        // Just use the previous result if we have been here before:
        auto vi = visited_.find(txid);
        if (visited_.end() != vi)
            return vi->second;

        // We have to assume missing transactions are safe:
        auto i = cache_.rows_.find(txid);
        if (cache_.rows_.end() == i)
            return (visited_[txid] = true);

        // Confirmed transactions are also safe:
        if (TxState::confirmed == i->second.state)
            return (visited_[txid] = true);

        // Recursively check all the inputs against the double-spend list:
        for (const auto &input: i->second.tx.inputs)
        {
            if (doubleSpends_.count(input.previous_output))
                return (visited_[txid] = false);
            if (!isSafe(input.previous_output.hash))
                return (visited_[txid] = false);
        }
        return (visited_[txid] = true);
    }

private:
    const TxDatabase &cache_;
    const PointSet &doubleSpends_;
    const AddressSet &addresses_;
    std::unordered_map<bc::hash_digest, bool> visited_;
};

TxDatabase::~TxDatabase()
{
}

TxDatabase::TxDatabase(unsigned unconfirmed_timeout):
    last_height_(0),
    unconfirmed_timeout_(unconfirmed_timeout)
{
}

long long TxDatabase::last_height() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    return last_height_;
}

bool TxDatabase::txidExists(bc::hash_digest txid) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    return rows_.find(txid) != rows_.end();
}

bool TxDatabase::ntxidExists(bc::hash_digest ntxid)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto txRows = ntxidLookupAll(ntxid);
    return !txRows.empty();
}

bc::transaction_type TxDatabase::txidLookup(bc::hash_digest txid) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto i = rows_.find(txid);
    if (i == rows_.end())
        return bc::transaction_type();
    return i->second.tx;
}

bc::transaction_type TxDatabase::ntxidLookup(bc::hash_digest ntxid)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // Try to return the confirmed copy (if any),
    // otherwise just return any match:
    bc::transaction_type out;
    auto rows = ntxidLookupAll(ntxid);
    for (const auto &row: rows)
    {
        if (row->state == TxState::confirmed)
            return row->tx;
        out = row->tx;
    }
    return out;
}

long long TxDatabase::txidHeight(bc::hash_digest txid) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto i = rows_.find(txid);
    if (i == rows_.end())
        return 0;

    if (i->second.state != TxState::confirmed)
    {
        return 0;
    }
    return i->second.block_height;
}

Status
TxDatabase::ntxidHeight(long long &result, bc::hash_digest ntxid)
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<TxRow *> txRows = ntxidLookupAll(ntxid);
    if (txRows.empty())
        return ABC_ERROR(ABC_CC_Synchronizing, "tx isn't in the database");

    long long height = 0;
    for (auto row: txRows)
    {
        if (TxState::confirmed == row->state)
        {
            if (height < row->block_height)
                height = row->block_height;
        }
    }

    // Special signal to the GUI that the transaction is both
    // malleated and unconfirmed:
    if (1 < txRows.size() && !height)
    {
        height = -1;
    }

    result = height;
    return Status();
}

bool TxDatabase::has_history(const bc::payment_address &address) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto &row: rows_)
    {
        for (auto &output: row.second.tx.outputs)
        {
            bc::payment_address to_address;
            if (bc::extract(to_address, output.script))
                if (address == to_address)
                    return true;
        }
    }

    return false;
}

bc::output_info_list TxDatabase::get_utxos(const AddressSet &addresses,
        bool filter) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    // Build a list of spends:
    PointSet spends;
    PointSet doubleSpends;
    for (auto &row: rows_)
    {
        for (auto &input: row.second.tx.inputs)
        {
            if (!spends.insert(input.previous_output).second)
                doubleSpends.insert(input.previous_output);
        }
    }

    TxFilter checker(*this, doubleSpends, addresses);

    // Check each output against the list:
    bc::output_info_list out;
    for (auto &row: rows_)
    {
        for (uint32_t i = 0; i < row.second.tx.outputs.size(); ++i)
        {
            bc::output_point point = {row.first, i};
            const auto &output = row.second.tx.outputs[i];
            bc::payment_address address;

            // The output is interesting if it isn't spent, belongs to us,
            // and its transaction passes the safety check:
            if (!spends.count(point) &&
                    bc::extract(address, output.script) &&
                    addresses.count(address.encoded()) &&
                    checker.check(row.first, row.second, filter))
            {
                bc::output_info_type info = {point, output.value};
                out.push_back(info);
            }
        }
    }

    return out;
}

bc::data_chunk TxDatabase::serialize() const
{
    ABC_DebugLog("ENTER TxDatabase::serialize");
    std::lock_guard<std::mutex> lock(mutex_);

    std::basic_ostringstream<uint8_t> stream;
    auto serial = bc::make_serializer(std::ostreambuf_iterator<uint8_t>(stream));

    // Magic version bytes:
    serial.write_4_bytes(serial_magic);

    // Last block height:
    serial.write_8_bytes(last_height_);

    // Tx table:
    time_t now = time(nullptr);
    for (const auto &row: rows_)
    {
        // Don't save old unconfirmed transactions:
        if (row.second.timestamp + unconfirmed_timeout_ < now &&
                TxState::unconfirmed == row.second.state)
        {
            ABC_DebugLog("TxDatabase::serialize Purging unconfirmed tx");
            continue;
        }

        auto height = row.second.block_height;
        if (TxState::unconfirmed == row.second.state)
            height = row.second.timestamp;

        serial.write_byte(serial_tx);
        serial.write_hash(row.first);
        serial.set_iterator(satoshi_save(row.second.tx, serial.iterator()));
        serial.write_byte(static_cast<uint8_t>(row.second.state));
        serial.write_8_bytes(height);
        serial.write_byte(0); // Was need_check
        serial.write_hash(row.second.txid);
        serial.write_hash(row.second.ntxid);
        serial.write_byte(false); // Was bMalleated
        serial.write_byte(TxState::confirmed == row.second.state); // Was bMasterConfirm
    }

    // The copy is not very elegant:
    auto str = stream.str();
    return bc::data_chunk(str.begin(), str.end());
}

Status
TxDatabase::load(const bc::data_chunk &data)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto serial = bc::make_deserializer(data.begin(), data.end());
    size_t last_height;
    std::unordered_map<bc::hash_digest, TxRow> rows;

    try
    {
        // Header bytes:
        auto magic = serial.read_4_bytes();
        if (serial_magic != magic)
        {
            return old_serial_magic == magic ?
                   ABC_ERROR(ABC_CC_ParseError, "Outdated transaction database format") :
                   ABC_ERROR(ABC_CC_ParseError, "Unknown transaction database header");
        }

        // Last block height:
        last_height = serial.read_8_bytes();

        time_t now = time(nullptr);
        while (serial.iterator() != data.end())
        {
            if (serial.read_byte() != serial_tx)
            {
                return ABC_ERROR(ABC_CC_ParseError, "Unknown entry in transaction database");
            }

            bc::hash_digest hash = serial.read_hash();
            TxRow row;
            bc::satoshi_load(serial.iterator(), data.end(), row.tx);
            auto step = serial.iterator() + satoshi_raw_size(row.tx);
            serial.set_iterator(step);

            TxState state      = static_cast<TxState>(serial.read_byte());
            uint64_t height    = serial.read_8_bytes();
            (void)serial.read_byte(); // Was need_check
            row.txid           = serial.read_hash();
            row.ntxid          = serial.read_hash();
            auto malleated     = serial.read_byte();
            auto masterConfirm = serial.read_byte();

            // The height field is the timestamp for unconfirmed txs:
            if (TxState::unconfirmed == row.state)
            {
                row.block_height = 0;
                row.timestamp = height;
            }
            else
            {
                row.block_height = height;
                row.timestamp = now;
            }

            // Malleated transactions can have inaccurate state:
            if (malleated && !masterConfirm)
            {
                row.state = TxState::unconfirmed;
                row.block_height = 0;
            }
            else
            {
                row.state = state;
            }

            rows[hash] = std::move(row);
        }
    }
    catch (bc::end_of_stream)
    {
        return ABC_ERROR(ABC_CC_ParseError, "Truncated transaction database");
    }
    last_height_ = last_height;
    rows_ = rows;
    ABC_DebugLog("Loaded transaction database at height %d", last_height);
    return Status();
}

void TxDatabase::dump(std::ostream &out) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    out << "height: " << last_height_ << std::endl;
    for (const auto &row: rows_)
    {
        out << "================" << std::endl;
        out << "hash: " << bc::encode_hash(row.first) << std::endl;
        std::string state;
        switch (row.second.state)
        {
        case TxState::unconfirmed:
            out << "state: unconfirmed" << std::endl;
            out << "timestamp: " << row.second.timestamp << std::endl;
            break;
        case TxState::confirmed:
            out << "state: confirmed" << std::endl;
            out << "height: " << row.second.block_height << std::endl;
            break;
        }
        for (auto &input: row.second.tx.inputs)
        {
            bc::payment_address address;
            if (bc::extract(address, input.script))
                out << "input: " << address.encoded() << std::endl;
        }
        for (auto &output: row.second.tx.outputs)
        {
            bc::payment_address address;
            if (bc::extract(address, output.script))
                out << "output: " << address.encoded() << " " <<
                    output.value << std::endl;
        }
    }
}

bool TxDatabase::insert(const bc::transaction_type &tx)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // Do not stomp existing tx's:
    auto txid = bc::hash_transaction(tx);
    if (rows_.find(txid) == rows_.end())
    {
        rows_[txid] = TxRow
        {
            tx, txid, makeNtxid(tx),
            TxState::unconfirmed, 0, time(nullptr)
        };
        return true;
    }

    return false;
}

void
TxDatabase::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    last_height_ = 0;
    rows_.clear();
}

void TxDatabase::at_height(size_t height)
{
    std::lock_guard<std::mutex> lock(mutex_);
    last_height_ = height;
}

void TxDatabase::confirmed(bc::hash_digest txid, long long block_height)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = rows_.find(txid);
    BITCOIN_ASSERT(it != rows_.end());
    auto &row = it->second;

    row.state = TxState::confirmed;
    row.block_height = block_height;
}

void TxDatabase::unconfirmed(bc::hash_digest txid)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = rows_.find(txid);
    BITCOIN_ASSERT(it != rows_.end());
    auto &row = it->second;

    row.state = TxState::unconfirmed;
    row.block_height = 0;
}

void TxDatabase::reset_timestamp(bc::hash_digest txid)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto i = rows_.find(txid);
    if (i != rows_.end())
        i->second.timestamp = time(nullptr);
}

void TxDatabase::foreach_unconfirmed(HashFn &&f)
{
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto row: rows_)
        if (row.second.state != TxState::confirmed)
            f(row.first);
}

std::vector<TxDatabase::TxRow *>
TxDatabase::ntxidLookupAll(bc::hash_digest ntxid)
{
    std::vector<TxRow *> out;
    for (auto &row: rows_)
    {
        if (row.second.ntxid == ntxid)
            out.push_back(&row.second);
    }
    return out;
}

} // namespace abcd
