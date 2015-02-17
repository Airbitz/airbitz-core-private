/*
 *  Copyright (c) 2014, Airbitz
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms are permitted provided that
 *  the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice, this
 *  list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *  this list of conditions and the following disclaimer in the documentation
 *  and/or other materials provided with the distribution.
 *  3. Redistribution or use of modified source code requires the express written
 *  permission of Airbitz Inc.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 *  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 *  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 *  ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 *  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  The views and conclusions contained in the software and documentation are those
 *  of the authors and should not be interpreted as representing official policies,
 *  either expressed or implied, of the Airbitz Project.
 */

#include "Exchanges.hpp"
#include "Account.hpp"
#include "util/FileIO.hpp"
#include "util/URL.hpp"
#include "util/Util.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <curl/curl.h>
#include <mutex>
#include <string>

namespace abcd {

#define SATOSHI_PER_BITCOIN                     100000000

#define EXCHANGE_RATE_DIRECTORY "Exchanges"

#define BITSTAMP_RATE_URL "https://www.bitstamp.net/api/ticker/"
#define COINBASE_RATE_URL "https://coinbase.com/api/v1/currencies/exchange_rates"
#define BNC_RATE_URL      "http://api.bravenewcoin.com/ticker/"

const tABC_ExchangeDefaults EXCHANGE_DEFAULTS[] =
{
    {CURRENCY_NUM_AUD, ABC_BNC},
    {CURRENCY_NUM_CAD, ABC_BNC},
    {CURRENCY_NUM_CNY, ABC_BNC},
    {CURRENCY_NUM_CUP, ABC_COINBASE},
    {CURRENCY_NUM_HKD, ABC_BNC},
    {CURRENCY_NUM_MXN, ABC_BNC},
    {CURRENCY_NUM_NZD, ABC_BNC},
    {CURRENCY_NUM_PHP, ABC_COINBASE},
    {CURRENCY_NUM_GBP, ABC_BNC},
    {CURRENCY_NUM_USD, ABC_BITSTAMP},
    {CURRENCY_NUM_EUR, ABC_BNC},
};

const size_t EXCHANGE_DEFAULTS_SIZE = sizeof(EXCHANGE_DEFAULTS)
                                    / sizeof(tABC_ExchangeDefaults);

typedef struct sABC_ExchangeCacheEntry
{
    int currencyNum;
    time_t last_updated;
    double exchange_rate;
} tABC_ExchangeCacheEntry;

static unsigned int gExchangesCacheCount = 0;
static tABC_ExchangeCacheEntry **gaExchangeCacheArray = NULL;
std::recursive_mutex gExchangeMutex;
typedef std::lock_guard<std::recursive_mutex> AutoExchangeLock;

static tABC_CC ABC_ExchangeNeedsUpdate(int currencyNum, bool *bUpdateRequired, double *szRate, tABC_Error *pError);
static tABC_CC ABC_ExchangeBitStampRate(int currencyNum, tABC_Error *pError);
static tABC_CC ABC_ExchangeCoinBaseRates(int currencyNum, tABC_Error *pError);
static tABC_CC ABC_ExchangeCoinBaseMap(int currencyNum, std::string& field, tABC_Error *pError);
static tABC_CC ABC_ExchangeBncRates(int currencyNum, tABC_Error *pError);
static tABC_CC ABC_ExchangeBncMap(int currencyNum, std::string& url, tABC_Error *pError);
static tABC_CC ABC_ExchangeExtractAndSave(json_t *pJSON_Root, const char *szField, int currencyNum, tABC_Error *pError);
static tABC_CC ABC_ExchangeGet(const char *szUrl, tABC_U08Buf *pData, tABC_Error *pError);
static tABC_CC ABC_ExchangeGetString(const char *szURL, char **pszResults, tABC_Error *pError);
static size_t  ABC_ExchangeCurlWriteData(void *pBuffer, size_t memberSize, size_t numMembers, void *pUserData);
static tABC_CC ABC_ExchangeGetFilename(char **pszFilename, int currencyNum, tABC_Error *pError);
static tABC_CC ABC_ExchangeExtractSource(tABC_SyncKeys *pKeys, int currencyNum, char **szSource, tABC_Error *pError);

static tABC_CC ABC_ExchangeGetFromCache(int currencyNum, tABC_ExchangeCacheEntry **ppData, tABC_Error *pError);
static tABC_CC ABC_ExchangeAddToCache(tABC_ExchangeCacheEntry *pData, tABC_Error *pError);
static tABC_CC ABC_ExchangeAllocCacheEntry(tABC_ExchangeCacheEntry **ppCache, int currencyNum, time_t last_updated, double exchange_rate, tABC_Error *pError);
static void ABC_ExchangeFreeCacheEntry(tABC_ExchangeCacheEntry *pCache);

/**
 * Fetches the current rate and requests a new value if the current value is old.
 */
tABC_CC ABC_ExchangeCurrentRate(int currencyNum, double *pRate, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    tABC_ExchangeCacheEntry *pCache = NULL;

    ABC_CHECK_RET(ABC_ExchangeGetFromCache(currencyNum, &pCache, pError));
    if (pCache)
    {
        *pRate = pCache->exchange_rate;
    }
    else
    {
        bool bUpdateRequired = true; // Ignored
        ABC_CHECK_RET(ABC_ExchangeNeedsUpdate(currencyNum, &bUpdateRequired, pRate, pError));
    }
exit:
    return cc;
}

tABC_CC ABC_ExchangeUpdate(tABC_SyncKeys *pKeys, int currencyNum, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    char *szSource = NULL;
    double rate;
    bool bUpdateRequired = true;

    ABC_CHECK_RET(ABC_ExchangeNeedsUpdate(currencyNum, &bUpdateRequired, &rate, pError));
    if (bUpdateRequired)
    {
        ABC_CHECK_RET(ABC_ExchangeExtractSource(pKeys, currencyNum, &szSource, pError));
        if (szSource)
        {
            if (strcmp(ABC_BITSTAMP, szSource) == 0)
            {
                ABC_CHECK_RET(ABC_ExchangeBitStampRate(currencyNum, pError));
            }
            else if (strcmp(ABC_COINBASE, szSource) == 0)
            {
                ABC_CHECK_RET(ABC_ExchangeCoinBaseRates(currencyNum, pError));
            }
            else if (strcmp(ABC_BNC, szSource) == 0)
            {
                ABC_CHECK_RET(ABC_ExchangeBncRates(currencyNum,  pError));
            }
        }
    }
exit:
    return cc;
}

static
tABC_CC ABC_ExchangeNeedsUpdate(int currencyNum, bool *bUpdateRequired, double *pRate, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    char *szFilename = NULL;
    char *szRate = NULL;
    tABC_ExchangeCacheEntry *pCache = NULL;
    time_t timeNow = time(NULL);
    bool bExists = false;
    AutoExchangeLock lock(gExchangeMutex);

    ABC_CHECK_RET(ABC_ExchangeGetFromCache(currencyNum, &pCache, pError));
    if (pCache)
    {
        if ((timeNow - pCache->last_updated) < ABC_EXCHANGE_RATE_REFRESH_INTERVAL_SECONDS)
        {
            *bUpdateRequired = false;
        }
        *pRate = pCache->exchange_rate;
    }
    else
    {
        ABC_CHECK_RET(ABC_ExchangeGetFilename(&szFilename, currencyNum, pError));
        ABC_CHECK_RET(ABC_FileIOFileExists(szFilename, &bExists, pError));
        if (true == bExists)
        {
            ABC_CHECK_RET(ABC_FileIOReadFileStr(szFilename, &szRate, pError));
            // Set the exchange rate
            *pRate = strtod(szRate, NULL);
            // get the time the file was last changed
            time_t timeFileMod;
            ABC_CHECK_RET(ABC_FileIOFileModTime(szFilename, &timeFileMod, pError));

            // if it isn't too old then don't update
            if ((timeNow - timeFileMod) < ABC_EXCHANGE_RATE_REFRESH_INTERVAL_SECONDS)
            {
                *bUpdateRequired = false;
            }
        }
        else
        {
            *bUpdateRequired = true;
            *pRate = 0.0;
        }
        ABC_CHECK_RET(ABC_ExchangeAllocCacheEntry(&pCache, currencyNum,
                                                  timeNow, *pRate, pError));
        if (ABC_CC_WalletAlreadyExists == ABC_ExchangeAddToCache(pCache, pError))
        {
            ABC_ExchangeFreeCacheEntry(pCache);
        }
    }
exit:
    ABC_FREE_STR(szFilename);
    ABC_FREE_STR(szRate);
    return cc;
}

static
tABC_CC ABC_ExchangeBitStampRate(int currencyNum, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    json_error_t error;
    json_t *pJSON_Root = NULL;
    char *szResponse = NULL;

    // Fetch exchanges from bitstamp
    ABC_CHECK_RET(ABC_ExchangeGetString(BITSTAMP_RATE_URL, &szResponse, pError));

    // Parse the json
    pJSON_Root = json_loads(szResponse, 0, &error);
    ABC_CHECK_ASSERT(pJSON_Root != NULL, ABC_CC_JSONError, "Error parsing JSON");
    ABC_CHECK_ASSERT(json_is_object(pJSON_Root), ABC_CC_JSONError, "Error parsing JSON");

    // USD
    ABC_ExchangeExtractAndSave(pJSON_Root, "last", CURRENCY_NUM_USD, pError);
exit:
    ABC_FREE_STR(szResponse);
    if (pJSON_Root) json_decref(pJSON_Root);
    return cc;
}

static
tABC_CC ABC_ExchangeCoinBaseRates(int currencyNum, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    json_error_t error;
    json_t *pJSON_Root = NULL;
    char *szResponse = NULL;
    std::string field;

    // Fetch exchanges from coinbase
    ABC_CHECK_RET(ABC_ExchangeGetString(COINBASE_RATE_URL, &szResponse, pError));

    // Parse the json
    pJSON_Root = json_loads(szResponse, 0, &error);
    ABC_CHECK_ASSERT(pJSON_Root != NULL, ABC_CC_JSONError, "Error parsing JSON");
    ABC_CHECK_ASSERT(json_is_object(pJSON_Root), ABC_CC_JSONError, "Error parsing JSON");

    ABC_CHECK_RET(ABC_ExchangeCoinBaseMap(currencyNum, field, pError));
    ABC_ExchangeExtractAndSave(pJSON_Root, field.c_str(), currencyNum, pError);
exit:
    ABC_FREE_STR(szResponse);
    if (pJSON_Root) json_decref(pJSON_Root);

    return cc;
}

static
tABC_CC ABC_ExchangeCoinBaseMap(int currencyNum, std::string& field, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    switch (currencyNum)
    {
        case CURRENCY_NUM_USD:
            field = "btc_to_usd";
            break;
        case CURRENCY_NUM_CAD:
            field = "btc_to_cad";
            break;
        case CURRENCY_NUM_EUR:
            field = "btc_to_eur";
            break;
        case CURRENCY_NUM_CUP:
            field = "btc_to_cup";
            break;
        case CURRENCY_NUM_GBP:
            field = "btc_to_gbp";
            break;
        case CURRENCY_NUM_MXN:
            field = "btc_to_mxn";
            break;
        case CURRENCY_NUM_CNY:
            field = "btc_to_cny";
            break;
        case CURRENCY_NUM_AUD:
            field = "btc_to_aud";
            break;
        case CURRENCY_NUM_PHP:
            field = "btc_to_php";
            break;
        case CURRENCY_NUM_HKD:
            field = "btc_to_hkd";
            break;
        case CURRENCY_NUM_NZD:
            field = "btc_to_nzd";
            break;
        default:
            ABC_CHECK_ASSERT(false, ABC_CC_Error, "Unsupported currency");
    }
exit:
    return cc;
}

static
tABC_CC ABC_ExchangeBncRates(int currencyNum, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    json_error_t error;
    json_t *pJSON_Root = NULL;
    char *szResponse = NULL;

    std::string url;
    ABC_CHECK_RET(ABC_ExchangeBncMap(currencyNum, url, pError));
    ABC_CHECK_RET(ABC_ExchangeGetString(url.c_str(), &szResponse, pError));

    // Parse the json
    pJSON_Root = json_loads(szResponse, 0, &error);
    ABC_CHECK_ASSERT(pJSON_Root != NULL, ABC_CC_JSONError, "Error parsing JSON");
    ABC_CHECK_ASSERT(json_is_object(pJSON_Root), ABC_CC_JSONError, "Error parsing JSON");

    ABC_ExchangeExtractAndSave(pJSON_Root, "last_price", currencyNum, pError);
exit:
    ABC_FREE_STR(szResponse);
    if (pJSON_Root) json_decref(pJSON_Root);

    return cc;
}

static
tABC_CC ABC_ExchangeBncMap(int currencyNum, std::string& url, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;

    url += BNC_RATE_URL;
    switch (currencyNum) {
        case CURRENCY_NUM_USD:
            url += "bnc_ticker_btc_usd.json";
            break;
        case CURRENCY_NUM_AUD:
            url += "bnc_ticker_btc_aud.json";
            break;
        case CURRENCY_NUM_CAD:
            url += "bnc_ticker_btc_cad.json";
            break;
        case CURRENCY_NUM_CNY:
            url += "bnc_ticker_btc_cny.json";
            break;
        case CURRENCY_NUM_HKD:
            url += "bnc_ticker_btc_hkd.json";
            break;
        case CURRENCY_NUM_MXN:
            url += "bnc_ticker_btc_mxn.json";
            break;
        case CURRENCY_NUM_NZD:
            url += "bnc_ticker_btc_nzd.json";
            break;
        case CURRENCY_NUM_GBP:
            url += "bnc_ticker_btc_gbp.json";
            break;
        case CURRENCY_NUM_EUR:
            url += "bnc_ticker_btc_eur.json";
            break;
        default:
            ABC_CHECK_ASSERT(false, ABC_CC_Error, "Unsupported currency");
    }
exit:
    return cc;
}

static
tABC_CC ABC_ExchangeExtractAndSave(json_t *pJSON_Root, const char *szField,
                                   int currencyNum, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    char *szFilename = NULL;
    char *szValue = NULL;
    json_t *jsonVal = NULL;
    tABC_ExchangeCacheEntry *pCache = NULL;
    time_t timeNow = time(NULL);
    AutoExchangeLock lock(gExchangeMutex);

    // Extract value from json
    jsonVal = json_object_get(pJSON_Root, szField);
    ABC_CHECK_ASSERT((jsonVal && json_is_string(jsonVal)), ABC_CC_JSONError, "Error parsing JSON");
    ABC_STRDUP(szValue, json_string_value(jsonVal));

    ABC_DebugLog("Exchange Response: %s = %s\n", szField, szValue);
    // Right changes to disk
    ABC_CHECK_RET(ABC_ExchangeGetFilename(&szFilename, currencyNum, pError));
    ABC_CHECK_RET(ABC_FileIOWriteFileStr(szFilename, szValue, pError));

    // Update the cache
    ABC_CHECK_RET(ABC_ExchangeAllocCacheEntry(&pCache, currencyNum, timeNow, strtod(szValue, NULL), pError));
    if (ABC_CC_WalletAlreadyExists == ABC_ExchangeAddToCache(pCache, pError))
    {
        ABC_ExchangeFreeCacheEntry(pCache);
    }
exit:
    ABC_FREE_STR(szFilename);
    ABC_FREE_STR(szValue);

    return cc;
}

static
tABC_CC ABC_ExchangeGet(const char *szUrl, tABC_U08Buf *pData, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    AutoCurlLock lock(gCurlMutex);
    CURL *pCurlHandle = NULL;
    CURLcode curlCode;
    long resCode;

    ABC_CHECK_RET(ABC_URLCurlHandleInit(&pCurlHandle, pError))
    ABC_CHECK_ASSERT((curlCode = curl_easy_setopt(pCurlHandle, CURLOPT_SSL_VERIFYPEER, 1L)) == 0,
        ABC_CC_Error, "Unable to verify servers cert");
    ABC_CHECK_ASSERT((curlCode = curl_easy_setopt(pCurlHandle, CURLOPT_URL, szUrl)) == 0,
        ABC_CC_Error, "Curl failed to set URL\n");
    ABC_CHECK_ASSERT((curlCode = curl_easy_setopt(pCurlHandle, CURLOPT_WRITEDATA, pData)) == 0,
        ABC_CC_Error, "Curl failed to set data\n");
    ABC_CHECK_ASSERT((curlCode = curl_easy_setopt(pCurlHandle, CURLOPT_WRITEFUNCTION, ABC_ExchangeCurlWriteData)) == 0,
        ABC_CC_Error, "Curl failed to set callback\n");
    ABC_CHECK_ASSERT((curlCode = curl_easy_perform(pCurlHandle)) == 0,
        ABC_CC_Error, "Curl failed to perform\n");
    ABC_CHECK_ASSERT((curlCode = curl_easy_getinfo(pCurlHandle, CURLINFO_RESPONSE_CODE, &resCode)) == 0,
        ABC_CC_Error, "Curl failed to retrieve response info\n");
    ABC_CHECK_ASSERT(resCode == 200, ABC_CC_Error, "Response code should be 200");
exit:

    if (pCurlHandle != NULL)
        curl_easy_cleanup(pCurlHandle);

    return cc;
}

static
tABC_CC ABC_ExchangeGetString(const char *szURL, char **pszResults, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    AutoCurlLock lock(gCurlMutex);

    AutoU08Buf Data;

    ABC_CHECK_NULL(szURL);
    ABC_CHECK_NULL(pszResults);

    // make the request
    ABC_CHECK_RET(ABC_ExchangeGet(szURL, &Data, pError));

    // add the null
    ABC_BUF_APPEND_PTR(Data, "", 1);

    // assign the results
    *pszResults = (char *)ABC_BUF_PTR(Data);
    ABC_BUF_CLEAR(Data);

exit:
    return cc;
}

static
size_t ABC_ExchangeCurlWriteData(void *pBuffer, size_t memberSize, size_t numMembers, void *pUserData)
{
    tABC_U08Buf *pCurlBuffer = (tABC_U08Buf *)pUserData;
    unsigned int dataAvailLength = (unsigned int) numMembers * (unsigned int) memberSize;
    size_t amountWritten = 0;

    if (pCurlBuffer)
    {
        // if we don't have any buffer allocated yet
        if (ABC_BUF_PTR(*pCurlBuffer) == NULL)
        {
            ABC_BUF_DUP_PTR(*pCurlBuffer, pBuffer, dataAvailLength);
        }
        else
        {
            ABC_BUF_APPEND_PTR(*pCurlBuffer, pBuffer, dataAvailLength);
        }
        amountWritten = dataAvailLength;
    }
    return amountWritten;
}

static
tABC_CC ABC_ExchangeGetFilename(char **pszFilename, int currencyNum, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    ABC_SET_ERR_CODE(pError, ABC_CC_Ok);
    char *szRoot     = NULL;
    char *szRateRoot = NULL;
    char *szFilename = NULL;
    bool bExists     = false;

    ABC_CHECK_NULL(pszFilename);
    *pszFilename = NULL;

    ABC_CHECK_RET(ABC_FileIOGetRootDir(&szRoot, pError));
    ABC_STR_NEW(szRateRoot, ABC_FILEIO_MAX_PATH_LENGTH);
    sprintf(szRateRoot, "%s/%s", szRoot, EXCHANGE_RATE_DIRECTORY);
    ABC_CHECK_RET(ABC_FileIOFileExists(szRateRoot, &bExists, pError));
    if (true != bExists)
    {
        ABC_CHECK_RET(ABC_FileIOCreateDir(szRateRoot, pError));
    }

    ABC_STR_NEW(szFilename, ABC_FILEIO_MAX_PATH_LENGTH);
    sprintf(szFilename, "%s/%d.txt", szRateRoot, currencyNum);
    *pszFilename = szFilename;
exit:
    ABC_FREE_STR(szRoot);
    ABC_FREE_STR(szRateRoot);
    return cc;
}

static
tABC_CC ABC_ExchangeExtractSource(tABC_SyncKeys *pKeys, int currencyNum,
                                  char **szSource, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    tABC_AccountSettings *pAccountSettings = NULL;

    *szSource = NULL;
    if (pKeys)
    {
        ABC_AccountSettingsLoad(pKeys,
                                &pAccountSettings,
                                pError);
    }
    if (pAccountSettings)
    {
        tABC_ExchangeRateSources *pSources = &(pAccountSettings->exchangeRateSources);
        if (pSources->numSources > 0)
        {
            for (unsigned i = 0; i < pSources->numSources; i++)
            {
                if (pSources->aSources[i]->currencyNum == currencyNum)
                {
                    ABC_STRDUP(*szSource, pSources->aSources[i]->szSource);
                    break;
                }
            }
        }
    }
    if (!(*szSource))
    {
        // If the settings are not populated, defaults
        switch (currencyNum)
        {
            case CURRENCY_NUM_USD:
                ABC_STRDUP(*szSource, ABC_BITSTAMP);
                break;
            case CURRENCY_NUM_CAD:
            case CURRENCY_NUM_CNY:
            case CURRENCY_NUM_EUR:
            case CURRENCY_NUM_GBP:
            case CURRENCY_NUM_MXN:
            case CURRENCY_NUM_AUD:
            case CURRENCY_NUM_HKD:
            case CURRENCY_NUM_NZD:
                ABC_STRDUP(*szSource, ABC_BNC);
                break;
            case CURRENCY_NUM_CUP:
            case CURRENCY_NUM_PHP:
                ABC_STRDUP(*szSource, ABC_COINBASE);
                break;
            default:
                ABC_STRDUP(*szSource, ABC_BITSTAMP);
                break;
        }
    }
exit:
    ABC_FreeAccountSettings(pAccountSettings);

    return cc;
}

/**
 * Clears all the data from the cache
 */
void ABC_ExchangeClearCache()
{
    AutoExchangeLock lock(gExchangeMutex);

    if ((gExchangesCacheCount > 0) && (NULL != gaExchangeCacheArray))
    {
        for (unsigned i = 0; i < gExchangesCacheCount; i++)
        {
            tABC_ExchangeCacheEntry *pData = gaExchangeCacheArray[i];
            ABC_FREE(pData);
        }

        ABC_FREE(gaExchangeCacheArray);
        gExchangesCacheCount = 0;
    }
}

static
tABC_CC ABC_ExchangeGetFromCache(int currencyNum, tABC_ExchangeCacheEntry **ppData, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;

    // assume we didn't find it
    *ppData = NULL;

    if ((gExchangesCacheCount > 0) && (NULL != gaExchangeCacheArray))
    {
        for (unsigned i = 0; i < gExchangesCacheCount; i++)
        {
            tABC_ExchangeCacheEntry *pData = gaExchangeCacheArray[i];
            if (currencyNum == pData->currencyNum)
            {
                // found it
                *ppData = pData;
                break;
            }
        }
    }
    return cc;
}

static
tABC_CC ABC_ExchangeAddToCache(tABC_ExchangeCacheEntry *pData, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    AutoExchangeLock lock(gExchangeMutex);

    tABC_ExchangeCacheEntry *pExistingExchangeData = NULL;

    ABC_CHECK_NULL(pData);

    // see if it exists first
    ABC_CHECK_RET(ABC_ExchangeGetFromCache(pData->currencyNum, &pExistingExchangeData, pError));

    // if it doesn't currently exist in the array
    if (pExistingExchangeData == NULL)
    {
        // if we don't have an array yet
        if ((gExchangesCacheCount == 0) || (NULL == gaExchangeCacheArray))
        {
            // create a new one
            gExchangesCacheCount = 0;
            ABC_ARRAY_NEW(gaExchangeCacheArray, 1, tABC_ExchangeCacheEntry*);
        }
        else
        {
            // extend the current one
            ABC_ARRAY_RESIZE(gaExchangeCacheArray, gExchangesCacheCount + 1, tABC_ExchangeCacheEntry*)
        }
        gaExchangeCacheArray[gExchangesCacheCount] = pData;
        gExchangesCacheCount++;
    }
    else
    {
        pExistingExchangeData->last_updated = pData->last_updated;
        pExistingExchangeData->exchange_rate = pData->exchange_rate;
        cc = ABC_CC_WalletAlreadyExists;
    }

exit:
    return cc;
}

static
tABC_CC ABC_ExchangeAllocCacheEntry(tABC_ExchangeCacheEntry **ppCache,
                                    int currencyNum, time_t last_updated,
                                    double exchange_rate, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    tABC_ExchangeCacheEntry *pCache;

    ABC_NEW(pCache, tABC_ExchangeCacheEntry);
    pCache->currencyNum = currencyNum;
    pCache->last_updated = last_updated;
    pCache->exchange_rate = exchange_rate;

    *ppCache = pCache;
exit:
    return cc;
}

static
void ABC_ExchangeFreeCacheEntry(tABC_ExchangeCacheEntry *pCache)
{
    if (pCache)
    {
        ABC_CLEAR_FREE(pCache, sizeof(tABC_ExchangeCacheEntry));
    }
}

Status
exchangeSatoshiToCurrency(int64_t satoshi, double &currency, int currencyNum)
{
    currency = 0.0;

    double rate;
    ABC_CHECK_OLD(ABC_ExchangeCurrentRate(currencyNum, &rate, &error));
    currency = satoshi * (rate / SATOSHI_PER_BITCOIN);

    return Status();
}

Status
exchangeCurrencyToSatoshi(double currency, int64_t &satoshi, int currencyNum)
{
    satoshi = 0;

    double rate;
    ABC_CHECK_OLD(ABC_ExchangeCurrentRate(currencyNum, &rate, &error));
    satoshi = static_cast<int64_t>(currency * (SATOSHI_PER_BITCOIN / rate));

    return Status();
}

} // namespace abcd
