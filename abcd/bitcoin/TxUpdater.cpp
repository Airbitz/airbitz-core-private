/*
 * Copyright (c) 2014, Airbitz, Inc.
 * All rights reserved.
 *
 * See the LICENSE file for more information.
 */

#include "TxUpdater.hpp"
#include "AddressCache.hpp"
#include "../General.hpp"
#include "../util/Debug.hpp"
#include <list>

namespace abcd {

#define LIBBITCOIN_PREFIX           "tcp://"
#define STRATUM_PREFIX              "stratum://"
#define LIBBITCOIN_PREFIX_LENGTH    6
#define STRATUM_PREFIX_LENGTH       10

#define ALL_SERVERS                 -1
#define NO_SERVERS                  -9999
#define NUM_CONNECT_SERVERS         4
#define MINIMUM_LIBBITCOIN_SERVERS  1
#define MINIMUM_STRATUM_SERVERS     2

constexpr unsigned max_queries = 10;

using std::placeholders::_1;

TxUpdater::~TxUpdater()
{
    disconnect();
}

TxUpdater::TxUpdater(TxDatabase &db, AddressCache &addressCache, void *ctx,
                     TxCallbacks &callbacks):
    db_(db),
    addressCache_(addressCache),
    ctx_(ctx),
    callbacks_(callbacks),
    failed_(false),
    last_wakeup_(std::chrono::steady_clock::now())
{
}

void
TxUpdater::disconnect()
{
    wantConnection = false;

    const auto temp = connections_;
    connections_.clear();

    // The query_done callback might fire while doing these deletions,
    // so the connections_ array must already be cleared:
    for (auto &i: temp)
        delete i;

    ABC_DebugLog("Disconnected from all servers.");
}

Status
TxUpdater::connect()
{
    wantConnection = true;

    // This happens once, and never changes:
    if (serverList_.empty())
        serverList_ = generalBitcoinServers();

    for (int i = 0; i < serverList_.size(); i++)
    {
        ABC_DebugLevel(1, "serverList_[%d]=%s", i, serverList_[i].c_str());
    }

    // If we have full connections then wipe them out and start over.
    // This was likely due to a refresh
    if (NUM_CONNECT_SERVERS <= connections_.size())
    {
        disconnect();
    }

    // If we are out of fresh libbitcoin servers, reload the list:
    if (untriedLibbitcoin_.empty())
    {
        for (size_t i = 0; i < serverList_.size(); ++i)
        {
            const auto &server = serverList_[i];
            if (0 == server.compare(0, LIBBITCOIN_PREFIX_LENGTH, LIBBITCOIN_PREFIX))
                untriedLibbitcoin_.insert(i);
        }
    }

    // If we are out of fresh stratum servers, reload the list:
    if (untriedStratum_.empty())
    {
        for (size_t i = 0; i < serverList_.size(); ++i)
        {
            const auto &server = serverList_[i];
            if (0 == server.compare(0, STRATUM_PREFIX_LENGTH, STRATUM_PREFIX))
                untriedStratum_.insert(i);
        }
    }

    ABC_DebugLevel(2,"%d libbitcoin untried, %d stratrum untried",
                   untriedLibbitcoin_.size(), untriedStratum_.size());

    // Count the number of existing connections:
    auto stratumCount = std::count_if(connections_.begin(), connections_.end(),
    [](Connection *c) { return c->type == ConnectionType::stratum; });
    auto libbitcoinCount = std::count_if(connections_.begin(), connections_.end(),
    [](Connection *c) { return c->type == ConnectionType::libbitcoin; });

    // Let's make some connections:
    srand(time(nullptr));
    int numConnections = 0;
    while (connections_.size() < NUM_CONNECT_SERVERS &&
            (untriedLibbitcoin_.size() || untriedStratum_.size()))
    {
        auto *untriedPrimary = &untriedStratum_;
        auto *untriedSecondary = &untriedLibbitcoin_;
        auto *primaryCount = &stratumCount;
        auto *secondaryCount = &libbitcoinCount;
        long minPrimary = MINIMUM_STRATUM_SERVERS;
        long minSecondary = MINIMUM_LIBBITCOIN_SERVERS;

        if (numConnections % 2 == 1)
        {
            untriedPrimary = &untriedLibbitcoin_;
            untriedSecondary = &untriedStratum_;
            primaryCount = &libbitcoinCount;
            secondaryCount = &stratumCount;
            minPrimary = MINIMUM_LIBBITCOIN_SERVERS;
            minSecondary = MINIMUM_STRATUM_SERVERS;
        }

        if (untriedPrimary->size() &&
                ((minSecondary - *secondaryCount < NUM_CONNECT_SERVERS - connections_.size()) ||
                 (rand() & 8)))
        {
            auto i = untriedPrimary->begin();
            std::advance(i, rand() % untriedPrimary->size());
            if (connectTo(*i).log())
            {
                (*primaryCount)++;
                ++numConnections;
            }
        }
        else if (untriedSecondary->size() &&
                 ((minPrimary - *primaryCount < NUM_CONNECT_SERVERS - connections_.size()) ||
                  (rand() & 8)))
        {
            auto i = untriedSecondary->begin();
            std::advance(i, rand() % untriedSecondary->size());
            if (connectTo(*i).log())
            {
                (*secondaryCount)++;
                ++numConnections;
            }
        }
    }

    if (connections_.size())
    {
        // Check for new blocks:
        get_height();

        // Handle block-fork checks & unconfirmed transactions:
        db_.foreach_unconfirmed(std::bind(&TxUpdater::get_index, this, _1,
                                          ALL_SERVERS));
    }

    return Status();
}

void
TxUpdater::send(StatusCallback status, DataSlice tx)
{
    sendTx(status, tx);
}

bc::client::sleep_time TxUpdater::wakeup()
{
    bc::client::sleep_time next_wakeup(0);
    auto now = std::chrono::steady_clock::now();

    // Figure out when our next block check is:
    auto period = std::chrono::seconds(30);
    auto elapsed = std::chrono::duration_cast<bc::client::sleep_time>(
                       now - last_wakeup_);
    if (period <= elapsed)
    {
        get_height();
        last_wakeup_ = now;
        elapsed = bc::client::sleep_time::zero();
    }
    next_wakeup = period - elapsed;

    for (const auto c: connections_)
    {
        while (c->queued_queries_ < max_queries)
        {
            std::string address;
            next_wakeup = bc::client::min_sleep(next_wakeup,
                                                addressCache_.nextWakeup(address));
            if (address.empty())
                break;

            ABC_DebugLog("Check address %s", address.c_str());
            addressCache_.checkBegin(address);
            bc::payment_address a(address);
            query_address(a, c->server_index);
        }
    }

    // Update the sockets:
    for (auto &connection: connections_)
    {
        if (ConnectionType::libbitcoin == connection->type)
        {
            connection->bc_socket.forward(connection->bc_codec);
            next_wakeup = bc::client::min_sleep(next_wakeup,
                                                connection->bc_codec.wakeup());
        }
        else if (ConnectionType::stratum == connection->type)
        {
            SleepTime sleep;
            if (!connection->stratumCodec.wakeup(sleep).log())
            {
                failed_server_idx_ = connection->server_index;
                failed_ = true;
            }
            next_wakeup = bc::client::min_sleep(next_wakeup, sleep);
        }
    }

    // Handle the last server failure:
    if (failed_)
    {
        auto p = [this](Connection *c) { return failed_server_idx_ == c->server_index; };
        auto i = std::find_if(connections_.begin(), connections_.end(), p);
        if (connections_.end() != i)
        {
            delete *i;
            connections_.erase(i);
            ABC_DebugLog("Disconnected from %d (%s)", failed_server_idx_,
                         serverList_[failed_server_idx_].c_str());
        }
        failed_server_idx_ = NO_SERVERS;
        failed_ = false;
    }

    // Connect to more servers:
    if (wantConnection && connections_.size() < NUM_CONNECT_SERVERS)
        connect().log();

    return next_wakeup;
}

std::vector<zmq_pollitem_t>
TxUpdater::pollitems()
{
    std::vector<zmq_pollitem_t> out;
    for (const auto &connection: connections_)
    {
        if (ConnectionType::libbitcoin == connection->type)
        {
            out.push_back(connection->bc_socket.pollitem());
        }
        else if (ConnectionType::stratum == connection->type)
        {
            zmq_pollitem_t pollitem =
            {
                nullptr, connection->stratumCodec.pollfd(), ZMQ_POLLIN, ZMQ_POLLOUT
            };
            out.push_back(pollitem);
        }
    }
    return out;
}

Status
TxUpdater::connectTo(long index)
{
    std::string server = serverList_[index];
    std::string key;

    // Parse out the key part:
    size_t keyStart = server.find(' ');
    if (keyStart != std::string::npos)
    {
        key = server.substr(keyStart + 1);
        server.erase(keyStart);
    }

    // Make the connection:
    std::unique_ptr<Connection> bconn(new Connection(ctx_, index));
    if (0 == server.compare(0, LIBBITCOIN_PREFIX_LENGTH, LIBBITCOIN_PREFIX))
    {
        // Libbitcoin server:
        untriedLibbitcoin_.erase(index);
        bconn->type = ConnectionType::libbitcoin;
        if(!bconn->bc_socket.connect(server, key))
            return ABC_ERROR(ABC_CC_Error, "Could not connect to " + server);
    }
    else if (0 == server.compare(0, STRATUM_PREFIX_LENGTH, STRATUM_PREFIX))
    {
        // Stratum server:
        untriedStratum_.erase(index);
        bconn->type = ConnectionType::stratum;
        ABC_CHECK(bconn->stratumCodec.connect(server));
    }
    else
    {
        return ABC_ERROR(ABC_CC_Error, "Unknown server type " + server);
    }

    connections_.push_back(bconn.release());
    ABC_DebugLog("Connected to %s as %d", server.c_str(), index);

    return Status();
}

void TxUpdater::watch_tx(bc::hash_digest txid, bool want_inputs, int idx,
                         size_t index)
{
    db_.reset_timestamp(txid);
    std::string str = bc::encode_hash(txid);
    if (!db_.txidExists(txid))
    {

        ABC_DebugLevel(1,
                       "*************************************************************");
        ABC_DebugLevel(1,"*** watch_tx idx=%d FOUND NEW TRANSACTION %s ****", idx,
                       str.c_str());
        ABC_DebugLevel(1,
                       "*************************************************************");
        get_tx(txid, want_inputs, idx);
    }
    else
    {
        // XXX Hack. If we used a stratum server to find this Tx, then it may already have
        // a block height index for this tx. In this case, write the block index into the
        // tx database
        if (index)
        {
            db_.confirmed(txid, index);
        }

        ABC_DebugLevel(2,"*** watch_tx idx=%d TRANSACTION %s already in DB ****", idx,
                       str.c_str());
        if (want_inputs)
        {
            ABC_DebugLevel(2,"*** watch_tx idx=%d getting inputs for tx=%s ****", idx,
                           str.c_str());
            get_inputs(db_.txidLookup(txid), idx);
        }
    }
}

void TxUpdater::get_inputs(const bc::transaction_type &tx, int idx)
{
    for (auto &input: tx.inputs)
        watch_tx(input.previous_output.hash, false, idx, 0);
}

void TxUpdater::query_done(int idx, Connection &bconn)
{
    bconn.queued_queries_--;

    if (bconn.queued_queries_ < 0)
    {
        ABC_DebugLevel(1,"query_done idx=%d queued_queries=%d GOING NEGATIVE!!", idx,
                       bconn.queued_queries_);
    }
    else if (bconn.queued_queries_ == 0)
    {
        ABC_DebugLevel(1,"query_done idx=%d queued_queries=%d CLEARED QUEUE", idx,
                       bconn.queued_queries_);
    }
    else if (bconn.queued_queries_ + 1 >= max_queries)
    {
        ABC_DebugLevel(2,"query_done idx=%d queued_queries=%d NEAR MAX_QUERIES", idx,
                       bconn.queued_queries_);
    }

    // Iterate over all the connections. If empty, fire off the callback.
    int total_queries = 0;
    for (auto &it: connections_)
    {
        total_queries += it->queued_queries_;
    }

    if (!total_queries)
        callbacks_.on_quiet();
}

// - server queries --------------------

void TxUpdater::get_height()
{
    if (!connections_.size())
        return;

    for (auto &it: connections_)
    {
        Connection &bconn = *it;
        auto idx = bconn.server_index;

        auto on_error = [this, idx, &bconn](const std::error_code &error)
        {
            if (!failed_) ABC_DebugLevel(1, "get_height server idx=%d failed: %s", idx,
                                             error.message().c_str());
            failed_ = true;
            failed_server_idx_ = idx;
            bconn.queued_get_height_--;
            ABC_DebugLevel(1, "get_height on_error queued_get_height=%d",
                           bconn.queued_get_height_);
        };

        auto on_done = [this, idx, &bconn](size_t height)
        {
            if (db_.last_height() < height)
            {
                db_.at_height(height);
                callbacks_.on_height(height);

                // Query all unconfirmed transactions:
                db_.foreach_unconfirmed(std::bind(&TxUpdater::get_index, this, _1, idx));
                ABC_DebugLevel(2, "get_height server idx=%d height=%d", idx, height);
            }
            bconn.queued_get_height_--;
            ABC_DebugLevel(2, "get_height on_done queued_get_height=%d",
                           bconn.queued_get_height_);
        };

        bconn.queued_get_height_++;
        ABC_DebugLevel(2, "get_height queued_get_height=%d", bconn.queued_get_height_);

        if (ConnectionType::stratum == it->type)
            bconn.stratumCodec.getHeight(on_error, on_done);
        else if (ConnectionType::libbitcoin == it->type)
            bconn.bc_codec.fetch_last_height(on_error, on_done);

        // Only use the first server response.
        break;
    }
}

void TxUpdater::get_tx(bc::hash_digest txid, bool want_inputs, int server_index)
{
    std::string str = bc::encode_hash(txid);

    for (auto it: connections_)
    {
        Connection &bconn = *it;

        // If there is a preferred server index to use. Only query that server
        if (ALL_SERVERS != server_index)
        {
            if (bconn.server_index != server_index)
                continue;
        }

        auto idx = bconn.server_index;

        auto on_error = [this, txid, str, want_inputs, &bconn,
                         idx](const std::error_code &error)
        {
            // A failure means the transaction might be in the mempool:
            (void)error;
            ABC_DebugLevel(2,"get_tx ON_ERROR no idx=%d txid=%s calling get_tx_mem", idx,
                           str.c_str());
            get_tx_mem(txid, want_inputs, idx);
            query_done(idx, bconn);
        };

        auto on_done = [this, txid, str, want_inputs, &bconn,
                        idx](const bc::transaction_type &tx)
        {
            ABC_DebugLevel(2,"get_tx ENTER ON_DONE idx=%d txid=%s", idx, str.c_str());
            BITCOIN_ASSERT(txid == bc::hash_transaction(tx));
            if (db_.insert(tx))
                callbacks_.on_add(tx);
            if (want_inputs)
            {
                ABC_DebugLevel(2,"get_tx idx=%d found txid=%s calling get_inputs", idx,
                               str.c_str());
                get_inputs(tx, idx);
            }
            ABC_DebugLevel(2,"get_tx idx=%d found txid=%s calling get_index", idx,
                           str.c_str());
            get_index(txid, idx);
            query_done(idx, bconn);
            ABC_DebugLevel(2,"get_tx EXIT ON_DONE idx=%d txid=%s", idx, str.c_str());
        };

        bconn.queued_queries_++;
        ABC_DebugLevel(2,"get_tx idx=%d queued_queries=%d", idx, bconn.queued_queries_);

        if (ConnectionType::libbitcoin == bconn.type)
            bconn.bc_codec.fetch_transaction(on_error, on_done, txid);
        else if (ConnectionType::stratum == bconn.type)
            bconn.stratumCodec.getTx(on_error, on_done, txid);
    }
}

void TxUpdater::get_tx_mem(bc::hash_digest txid, bool want_inputs,
                           int server_index)
{
    std::string str = bc::encode_hash(txid);

    for (auto &it: connections_)
    {
        Connection &bconn = *it;

        // If there is a preferred server index to use. Only query that server
        if (ALL_SERVERS != server_index)
        {
            if (bconn.server_index != server_index)
                continue;
        }

        auto idx = bconn.server_index;

        auto on_error = [this, idx, str, &bconn](const std::error_code &error)
        {
            ABC_DebugLevel(1,"get_tx_mem ON_ERROR no idx=%d txid=%s NOT IN MEMPOOL", idx,
                           str.c_str());

            failed_ = true;
            failed_server_idx_ = idx;
            query_done(idx, bconn);
        };

        auto on_done = [this, txid, str, want_inputs, idx,
                        &bconn](const bc::transaction_type &tx)
        {
            ABC_DebugLevel(2,"get_tx_mem ENTER ON_DONE idx=%d txid=%s FOUND IN MEMPOOL",
                           idx, str.c_str());
            BITCOIN_ASSERT(txid == bc::hash_transaction(tx));
            if (db_.insert(tx))
                callbacks_.on_add(tx);
            if (want_inputs)
            {
                ABC_DebugLevel(2,"get_tx_mem ON_DONE calling get_inputs idx=%d txid=%s", idx,
                               str.c_str());
                get_inputs(tx, idx);
            }
            ABC_DebugLevel(2,"get_tx_mem ON_DONE calling get_index idx=%d txid=%s", idx,
                           str.c_str());
            get_index(txid, idx);
            query_done(idx, bconn);
            ABC_DebugLevel(2,"get_tx_mem EXIT ON_DONE idx=%d txid=%s", idx, str.c_str());
        };

        bconn.queued_queries_++;
        if (ConnectionType::libbitcoin == bconn.type)
            bconn.bc_codec.fetch_unconfirmed_transaction(on_error, on_done, txid);
        else if (ConnectionType::stratum == bconn.type)
            bconn.stratumCodec.getTx(on_error, on_done, txid);
    }
}

void TxUpdater::get_index(bc::hash_digest txid, int server_index)
{

    for (auto &it: connections_)
    {
        // TODO: support get_index for Stratum
        if (ConnectionType::stratum == it->type)
            continue;

        Connection &bconn = *it;

        // TODO: Removing the code below might cause unnecessary server load. Since Stratum can't query
        // txid height using just a txid, we have to rely on Libbitcoin to do this.

//        // If there is a preferred server index to use. Only query that server
//        if (ALL_SERVERS != server_index)
//        {
//            if (bconn.server_index != server_index)
//                continue;
//        }
//
        auto idx = bconn.server_index;
        auto on_error = [this, txid, idx, &bconn](const std::error_code &error)
        {
            // A failure means that the transaction is unconfirmed:
            (void)error;
            db_.unconfirmed(txid);

            bconn.queued_get_indices_--;
        };

        auto on_done = [this, txid, idx, &bconn](size_t block_height, size_t index)
        {
            // The transaction is confirmed:
            (void)index;

            db_.confirmed(txid, block_height);

            bconn.queued_get_indices_--;
            ABC_DebugLevel(2,"get_index SUCCESS server idx: %d", idx);
        };

        bconn.queued_get_indices_++;
        bconn.bc_codec.fetch_transaction_index(on_error, on_done, txid);
    }
}

void
TxUpdater::sendTx(StatusCallback status, DataSlice tx)
{
    for (auto &it: connections_)
    {
        // Pick one (and only one) stratum server for the broadcast:
        if (ConnectionType::stratum == it->type)
        {
            it->stratumCodec.sendTx(status, tx);
            return;
        }
    }

    // If we get here, there are no stratum connections:
    status(ABC_ERROR(ABC_CC_Error, "No stratum connections"));
}

void TxUpdater::query_address(const bc::payment_address &address,
                              int server_index)
{
    ABC_DebugLevel(2,"query_address ENTER %s", address.encoded().c_str());
    std::string servers = "";
    std::string maxed_servers = "";
    int total_queries = 0;
    int num_servers = 0;
    int num_maxed_servers = 0;

    if (!connections_.size())
    {
        ABC_DebugLevel(2,"query_address connections_ vector empty");
    }

    for (auto &it: connections_)
    {
        Connection &bconn = *it;
        auto idx = bconn.server_index;

        // If there is a preferred server index to use. Only query that server
        if (ALL_SERVERS != server_index)
        {
            if (bconn.server_index != server_index)
                continue;
        }

        if (bconn.queued_queries_ > max_queries)
        {
            if (num_maxed_servers)
                maxed_servers += " ";
            maxed_servers += std::to_string(idx);
            num_maxed_servers++;
            ABC_DebugLevel(2,
                           "TxUpdater::query_address() idx=%d (queued > max) for address=%s queued_queries=%d",
                           idx, address.encoded().c_str(), bconn.queued_queries_);
            continue;
        }

        if (num_servers)
            servers += " ";
        servers += std::to_string(idx);

        auto on_error = [this, idx, address, &bconn](const std::error_code &error)
        {
            ABC_DebugLevel(1,"query_address ON_ERROR idx:%d addr:%s failed:%s",
                           idx, address.encoded().c_str(), error.message().c_str());

            addressCache_.checkEnd(address.encoded(), false);
            failed_ = true;
            failed_server_idx_ = idx;
            query_done(idx, bconn);
        };

        auto on_done = [this, idx, address,
                        &bconn](const bc::client::history_list &history)
        {
            ABC_DebugLevel(2,"TxUpdater::query_address ENTER ON_DONE idx:%d addr:%s", idx,
                           address.encoded().c_str());
            ABC_DebugLevel(2,"   Looping over address transactions... ");

            addressCache_.checkEnd(address.encoded(), true);
            for (auto &row: history)
            {
                ABC_DebugLevel(2,"   Watching output tx=%s",
                               bc::encode_hash(row.output.hash).c_str());
                watch_tx(row.output.hash, true, idx, row.output_height);
                if (row.spend.hash != bc::null_hash)
                {
                    watch_tx(row.spend.hash, true, idx, 0);
                    ABC_DebugLevel(2,"   Watching spend tx=%s",
                                   bc::encode_hash(row.spend.hash).c_str());
                }
            }
            query_done(idx, bconn);
            ABC_DebugLevel(2,"TxUpdater::query_address EXIT ON_DONE idx:%d addr:%s", idx,
                           address.encoded().c_str());
        };

        bconn.queued_queries_++;
        num_servers++;
        total_queries += bconn.queued_queries_;
        ABC_DebugLevel(2,"TxUpdater::query_address idx=%d queued_queries=%d %s", idx,
                       bconn.queued_queries_, address.encoded().c_str());

        if (ConnectionType::libbitcoin == bconn.type)
            bconn.bc_codec.address_fetch_history(on_error, on_done, address);
        else if (ConnectionType::stratum == bconn.type)
            bconn.stratumCodec.getAddressHistory(on_error, on_done, address);
    }

    if (num_servers)
        ABC_DebugLevel(2,"query_address svrs=[%s] maxed_svrs=[%s] avg_q=%.1f addr=%s",
                       servers.c_str(), maxed_servers.c_str(), (float)total_queries/(float)num_servers,
                       address.encoded().c_str());

    ABC_DebugLevel(2,"query_address EXIT %s", address.encoded().c_str());

}

static void on_unknown_nop(const std::string &)
{
}

TxUpdater::Connection::Connection(void *ctx, long server_index):
    bc_socket(ctx),
    bc_codec(bc_socket, on_unknown_nop, std::chrono::seconds(10), 0),
    queued_queries_(0),
    queued_get_indices_(0),
    queued_get_height_(0),
    server_index(server_index)
{
}

} // namespace abcd
