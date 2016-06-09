/*
 * Copyright (c) 2014, Airbitz, Inc.
 * All rights reserved.
 *
 * See the LICENSE file for more information.
 */
/**
 * @file
 * AirBitz general, non-account-specific server-supplied data.
 *
 * The data handled in this file is basically just a local cache of various
 * settings that AirBitz would like to adjust from time-to-time without
 * upgrading the entire app.
 */

#include "General.hpp"
#include "Context.hpp"
#include "auth/LoginServer.hpp"
#include "bitcoin/Testnet.hpp"
#include "json/JsonObject.hpp"
#include "json/JsonArray.hpp"
#include "util/FileIO.hpp"
#include <time.h>
#include <mutex>

namespace abcd {

#define FALLBACK_BITCOIN_SERVERS {  "tcp://obelisk.airbitz.co:9091", \
                                    "stratum://stratum-az-wusa.airbitz.co:50001", \
                                    "stratum://stratum-az-wjapan.airbitz.co:50001", \
                                    "stratum://stratum-az-neuro.airbitz.co:50001" }
#define TESTNET_BITCOIN_SERVERS {   "tcp://obelisk-testnet.airbitz.co:9091" }
#define GENERAL_ACCEPTABLE_INFO_FILE_AGE_SECS   (8 * 60 * 60) // how many seconds old can the info file be before it should be updated
#define ESTIMATED_FEES_ACCEPTABLE_INFO_FILE_AGE_SECS   (3 * 60 * 60) // how many seconds old can the info file be before it should be updated

struct AirbitzFeesJson:
    public JsonObject
{
    ABC_JSON_CONSTRUCTORS(AirbitzFeesJson, JsonObject)
    ABC_JSON_VALUE(addresses, "addresses", JsonArray)
    ABC_JSON_NUMBER(incomingRate, "incomingRate", 0)
    ABC_JSON_INTEGER(incomingMax, "incomingMax", 0)
    ABC_JSON_INTEGER(incomingMin, "incomingMin", 0)
    ABC_JSON_NUMBER(outgoingPercentage, "percentage", 0)
    ABC_JSON_INTEGER(outgoingMax, "maxSatoshi", 0)
    ABC_JSON_INTEGER(outgoingMin, "minSatoshi", 0)
    ABC_JSON_INTEGER(noFeeMinSatoshi, "noFeeMinSatoshi", 0)
    ABC_JSON_INTEGER(sendMin, "sendMin", 4000) // No dust allowed
    ABC_JSON_INTEGER(sendPeriod, "sendPeriod", 7*24*60*60) // One week
    ABC_JSON_STRING(sendPayee, "sendPayee", "Airbitz")
    ABC_JSON_STRING(sendCategory, "sendCategory", "Expense:Fees")
};

struct BitcoinFeesJson:
    public JsonObject
{
    ABC_JSON_CONSTRUCTORS(BitcoinFeesJson, JsonObject)
    ABC_JSON_INTEGER(confirmFees1, "confirmFees1", 43210)
    ABC_JSON_INTEGER(confirmFees2, "confirmFees2", 32110)
    ABC_JSON_INTEGER(confirmFees3, "confirmFees3", 21098)
    ABC_JSON_INTEGER(confirmFees4, "confirmFees4", 16001)
    ABC_JSON_INTEGER(confirmFees5, "confirmFees5", 11002)
    ABC_JSON_NUMBER(targetFeePercentage, "targetFeePercentage", 0.1)
};

struct EstimateFeesJson:
    public JsonObject
{
    ABC_JSON_CONSTRUCTORS(EstimateFeesJson, JsonObject)
    ABC_JSON_INTEGER(confirmFees1, "confirmFees1", 0)
    ABC_JSON_INTEGER(confirmFees2, "confirmFees2", 0)
    ABC_JSON_INTEGER(confirmFees3, "confirmFees3", 0)
    ABC_JSON_INTEGER(confirmFees4, "confirmFees4", 0)
    ABC_JSON_INTEGER(confirmFees5, "confirmFees5", 0)
};


struct GeneralJson:
    public JsonObject
{
    ABC_JSON_VALUE(bitcoinFees,    "minersFees2", BitcoinFeesJson)
    ABC_JSON_VALUE(airbitzFees,    "feesAirBitz", AirbitzFeesJson)
    ABC_JSON_VALUE(bitcoinServers, "obeliskServers", JsonArray)
    ABC_JSON_VALUE(syncServers,    "syncServers", JsonArray)
};

/**
 * Attempts to load the general information from disk.
 */
static GeneralJson
generalLoad()
{
    if (!gContext)
        return GeneralJson();

    const auto path = gContext->paths.generalPath();
    if (!fileExists(path))
        generalUpdate().log();

    GeneralJson out;
    out.load(path).log();
    return out;
}

Status
generalUpdate()
{
    const auto path = gContext->paths.generalPath();

    time_t lastTime;
    if (!fileTime(lastTime, path) ||
            lastTime + GENERAL_ACCEPTABLE_INFO_FILE_AGE_SECS < time(nullptr))
    {
        JsonPtr infoJson;
        ABC_CHECK(loginServerGetGeneral(infoJson));
        ABC_CHECK(infoJson.save(path));
    }

    return Status();
}

static EstimateFeesJson
estimateFeesLoad()
{
    if (!gContext)
        return EstimateFeesJson();

    const auto path = gContext->paths.feeCachePath();
    if (!fileExists(path))
        return EstimateFeesJson();

    EstimateFeesJson out;
    out.load(path).log();
    return out;
}


bool
generalEstimateFeesNeedUpdate()
{
    const auto path = gContext->paths.feeCachePath();

    time_t lastTime;
    if (!fileTime(lastTime, path) ||
            lastTime + ESTIMATED_FEES_ACCEPTABLE_INFO_FILE_AGE_SECS < time(nullptr))
    {
        return true;
    }

    return false;
}

static bool estimatedFeesInitialized = 0;
static double estimatedFees[6];
static size_t estimatedFeesNumResponses[6];
static std::mutex mutex_;

Status
generalEstimateFeesUpdate(size_t blocks, double fee)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!estimatedFeesInitialized)
    {
        estimatedFees[1] = estimatedFeesNumResponses[1] = 0;
        estimatedFees[2] = estimatedFeesNumResponses[2] = 0;
        estimatedFees[3] = estimatedFeesNumResponses[3] = 0;
        estimatedFees[4] = estimatedFeesNumResponses[4] = 0;
        estimatedFees[5] = estimatedFeesNumResponses[5] = 0;
        estimatedFeesInitialized = true;
    }

    // Take the average of all the responses for a given block target
    uint64_t tempFee = (estimatedFees[blocks] * estimatedFeesNumResponses[blocks])
                       + (uint64_t) (fee * 100000000.0);
    estimatedFeesNumResponses[blocks]++;
    estimatedFees[blocks] = tempFee / estimatedFeesNumResponses[blocks];

    if (estimatedFees[1] > 0 &&
            estimatedFees[2] > 0 &&
            estimatedFees[3] > 0 &&
            estimatedFees[4] > 0 &&
            estimatedFees[5] > 0)
    {
        // Save the fees in a Json file
        EstimateFeesJson feesJson;
        feesJson.confirmFees1Set(estimatedFees[1]);
        feesJson.confirmFees2Set(estimatedFees[2]);
        feesJson.confirmFees3Set(estimatedFees[3]);
        feesJson.confirmFees4Set(estimatedFees[4]);
        feesJson.confirmFees5Set(estimatedFees[5]);

        const auto path = gContext->paths.feeCachePath();

        ABC_CHECK(feesJson.save(path));
    }
    return Status();
}


BitcoinFeeInfo
generalBitcoinFeeInfo()
{
    BitcoinFeesJson feeJson = generalLoad().bitcoinFees();
    EstimateFeesJson estimateFeesJson = estimateFeesLoad();

    BitcoinFeeInfo out;

    out.confirmFees1 = estimateFeesJson.confirmFees1() ?
                       estimateFeesJson.confirmFees1() : feeJson.confirmFees1();
    out.confirmFees2 = estimateFeesJson.confirmFees2() ?
                       estimateFeesJson.confirmFees2() : feeJson.confirmFees2();
    out.confirmFees3 = estimateFeesJson.confirmFees3() ?
                       estimateFeesJson.confirmFees3() : feeJson.confirmFees3();
    out.confirmFees4 = estimateFeesJson.confirmFees4() ?
                       estimateFeesJson.confirmFees4() : feeJson.confirmFees4();
    out.confirmFees5 = estimateFeesJson.confirmFees5() ?
                       estimateFeesJson.confirmFees5() : feeJson.confirmFees5();
    out.targetFeePercentage = feeJson.targetFeePercentage();

    // Fix any fees that contradict. ie. confirmFees1 < confirmFees2
    if (out.confirmFees2 > out.confirmFees1)
        out.confirmFees2 = out.confirmFees1;
    if (out.confirmFees3 > out.confirmFees2)
        out.confirmFees3 = out.confirmFees2;
    if (out.confirmFees4 > out.confirmFees3)
        out.confirmFees4 = out.confirmFees3;
    if (out.confirmFees5 > out.confirmFees4)
        out.confirmFees5 = out.confirmFees4;

    return out;
}

AirbitzFeeInfo
generalAirbitzFeeInfo()
{
    AirbitzFeeInfo out;
    auto feeJson = generalLoad().airbitzFees();

    auto arrayJson = feeJson.addresses();
    size_t size = arrayJson.size();
    for (size_t i = 0; i < size; i++)
    {
        auto stringJson = arrayJson[i];
        if (json_is_string(stringJson.get()))
            out.addresses.insert(json_string_value(stringJson.get()));
    }

    out.incomingRate = feeJson.incomingRate();
    out.incomingMin  = feeJson.incomingMin();
    out.incomingMax  = feeJson.incomingMax();

    out.outgoingRate = feeJson.outgoingPercentage() / 100.0;
    out.outgoingMin  = feeJson.outgoingMin();
    out.outgoingMax  = feeJson.outgoingMax();
    out.noFeeMinSatoshi = feeJson.noFeeMinSatoshi();

    out.sendMin = feeJson.sendMin();
    out.sendPeriod = feeJson.sendPeriod();
    out.sendPayee = feeJson.sendPayee();
    out.sendCategory = feeJson.sendCategory();

    return out;
}

std::vector<std::string>
generalBitcoinServers()
{
    std::vector<std::string> out;

    if (isTestnet())
    {
        std::string serverlist[] = TESTNET_BITCOIN_SERVERS;

        size_t size = sizeof(serverlist) / sizeof(*serverlist);
        for (size_t i = 0; i < size; i++)
            out.push_back(serverlist[i]);

        return out;
    }

    auto arrayJson = generalLoad().bitcoinServers();

    size_t size = arrayJson.size();
    out.reserve(size);
    for (size_t i = 0; i < size; i++)
    {
        auto stringJson = arrayJson[i];
        if (json_is_string(stringJson.get()))
        {
            std::string servername = json_string_value(stringJson.get());
            out.push_back(servername);
        }
    }

    if (!out.size())
    {
        std::string serverlist[] = FALLBACK_BITCOIN_SERVERS;

        size_t size = sizeof(serverlist) / sizeof(*serverlist);
        for (size_t i = 0; i < size; i++)
            out.push_back(serverlist[i]);
    }
    return out;
}

std::vector<std::string>
generalSyncServers()
{
    auto arrayJson = generalLoad().syncServers();

    std::vector<std::string> out;
    size_t size = arrayJson.size();
    out.reserve(size);
    for (size_t i = 0; i < size; i++)
    {
        auto stringJson = arrayJson[i];
        if (json_is_string(stringJson.get()))
            out.push_back(json_string_value(stringJson.get()));
    }

    if (!out.size())
        out.push_back("https://git.sync.airbitz.co/repos");
    return out;
}

} // namespace abcd
