/*
 * Copyright (c) 2014, Airbitz, Inc.
 * All rights reserved.
 *
 * See the LICENSE file for more information.
 */

#include "Crypto.hpp"
#include "Encoding.hpp"
#include "Random.hpp"
#include "../json/JsonPtr.hpp"
#include "../util/Util.hpp"
#include <bitcoin/bitcoin.hpp> // wow! such slow, very compile time
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/sha.h>

namespace abcd {

#define JSON_ENC_TYPE_FIELD     "encryptionType"
#define JSON_ENC_IV_FIELD       "iv_hex"
#define JSON_ENC_DATA_FIELD     "data_base64"

static
tABC_CC ABC_CryptoEncryptAES256Package(const tABC_U08Buf Data,
                                       const tABC_U08Buf Key,
                                       tABC_U08Buf       *pEncData,
                                       DataChunk         &IV,
                                       tABC_Error        *pError);
static
tABC_CC ABC_CryptoDecryptAES256Package(const tABC_U08Buf EncData,
                                       const tABC_U08Buf Key,
                                       const tABC_U08Buf IV,
                                       tABC_U08Buf       *pData,
                                       tABC_Error        *pError);
static
tABC_CC ABC_CryptoEncryptAES256(const tABC_U08Buf Data,
                                const tABC_U08Buf Key,
                                const tABC_U08Buf IV,
                                tABC_U08Buf       *pEncData,
                                tABC_Error        *pError);
static
tABC_CC ABC_CryptoDecryptAES256(const tABC_U08Buf EncData,
                                const tABC_U08Buf Key,
                                const tABC_U08Buf IV,
                                tABC_U08Buf       *pData,
                                tABC_Error        *pError);

/**
 * A constant-time alternative to memcmp.
 */
static bool
cryptoCompare(const uint8_t *a, const uint8_t *b, size_t size)
{
    uint8_t out = false;
    for (size_t i = 0; i < size; ++i)
        out |= *a++ ^ *b++;

    return !out;
}

std::string
cryptoFilename(DataSlice key, const std::string &name)
{
    return bc::encode_base58(bc::to_data_chunk(
                                 bc::hmac_sha256_hash(DataSlice(name), key)));
}

/**
 * Encrypt data into a jansson object
 */
tABC_CC ABC_CryptoEncryptJSONObject(const tABC_U08Buf Data,
                                    const tABC_U08Buf Key,
                                    tABC_CryptoType   cryptoType,
                                    json_t            **ppJSON_Enc,
                                    tABC_Error        *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    ABC_SET_ERR_CODE(pError, ABC_CC_Ok);

    AutoU08Buf     EncData;
    DataChunk      IV;
    json_t          *jsonRoot       = NULL;

    ABC_CHECK_NULL_BUF(Data);
    ABC_CHECK_NULL_BUF(Key);
    ABC_CHECK_ASSERT(cryptoType < ABC_CryptoType_Count, ABC_CC_UnknownCryptoType,
                     "Invalid encryption type");
    ABC_CHECK_NULL(ppJSON_Enc);

    if (cryptoType == ABC_CryptoType_AES256)
    {
        // encrypt
        ABC_CHECK_RET(ABC_CryptoEncryptAES256Package(Data,
                      Key,
                      &EncData,
                      IV,
                      pError));

        // Encoding
        jsonRoot = json_pack("{sissss}",
                             JSON_ENC_TYPE_FIELD, cryptoType,
                             JSON_ENC_IV_FIELD,   base16Encode(IV).c_str(),
                             JSON_ENC_DATA_FIELD, base64Encode(EncData).c_str());

        // assign our final result
        *ppJSON_Enc = jsonRoot;
        json_incref(jsonRoot);  // because we will decl below
    }
    else
    {
        ABC_RET_ERROR(ABC_CC_InvalidCryptoType, "Unsupported encryption type");
    }

exit:
    if (jsonRoot)     json_decref(jsonRoot);

    return cc;
}

/**
 * Encrypted the given data and write the json to a file
 */
tABC_CC ABC_CryptoEncryptJSONFile(const tABC_U08Buf Data,
                                  const tABC_U08Buf Key,
                                  tABC_CryptoType   cryptoType,
                                  const char *szFilename,
                                  tABC_Error        *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    ABC_SET_ERR_CODE(pError, ABC_CC_Ok);

    json_t *root = nullptr;

    ABC_CHECK_NULL_BUF(Data);
    ABC_CHECK_NULL_BUF(Key);
    ABC_CHECK_NULL(szFilename);

    ABC_CHECK_RET(ABC_CryptoEncryptJSONObject(Data, Key, cryptoType, &root,
                  pError));
    ABC_CHECK_NEW(JsonPtr(root).save(szFilename));

exit:
    return cc;
}

/**
 * Encrypted the given json and write the encrypted json to a file
 */
tABC_CC ABC_CryptoEncryptJSONFileObject(json_t *pJSON_Data,
                                        const tABC_U08Buf Key,
                                        tABC_CryptoType  cryptoType,
                                        const char *szFilename,
                                        tABC_Error  *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    ABC_SET_ERR_CODE(pError, ABC_CC_Ok);

    std::string data;

    ABC_CHECK_NULL_BUF(Key);
    ABC_CHECK_NULL(szFilename);
    ABC_CHECK_NULL(pJSON_Data);

    // Downstream decoders often forget to null-terminate their input.
    // This is a bug, but we can save the app from crashing by
    // including a null byte in the encrypted data.
    data = JsonPtr(json_incref(pJSON_Data)).encode();
    data.push_back(0);

    ABC_CHECK_RET(ABC_CryptoEncryptJSONFile(toU08Buf(data), Key, cryptoType,
                                            szFilename, pError));

exit:
    return cc;
}

/**
 * Given a JSON object holding encrypted data, this function decrypts it
 */
tABC_CC ABC_CryptoDecryptJSONObject(const json_t      *pJSON_Enc,
                                    const tABC_U08Buf Key,
                                    tABC_U08Buf       *pData,
                                    tABC_Error        *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    ABC_SET_ERR_CODE(pError, ABC_CC_Ok);

    DataChunk data;
    DataChunk iv;
    int type;
    json_t *jsonVal = NULL;

    ABC_CHECK_NULL(pJSON_Enc);
    ABC_CHECK_NULL_BUF(Key);
    ABC_CHECK_NULL(pData);

    jsonVal = json_object_get(pJSON_Enc, JSON_ENC_TYPE_FIELD);
    ABC_CHECK_ASSERT((jsonVal
                      && json_is_number(jsonVal)), ABC_CC_DecryptError,
                     "Error parsing JSON encrypt package - missing type");
    type = (int) json_integer_value(jsonVal);
    ABC_CHECK_ASSERT(ABC_CryptoType_AES256 == type, ABC_CC_UnknownCryptoType,
                     "Invalid encryption type");

    // get the IV
    jsonVal = json_object_get(pJSON_Enc, JSON_ENC_IV_FIELD);
    ABC_CHECK_ASSERT((jsonVal
                      && json_is_string(jsonVal)), ABC_CC_DecryptError,
                     "Error parsing JSON encrypt package - missing iv");
    ABC_CHECK_NEW(base16Decode(iv, json_string_value(jsonVal)));

    // get the encrypted data
    jsonVal = json_object_get(pJSON_Enc, JSON_ENC_DATA_FIELD);
    ABC_CHECK_ASSERT((jsonVal
                      && json_is_string(jsonVal)), ABC_CC_DecryptError,
                     "Error parsing JSON encrypt package - missing data");
    ABC_CHECK_NEW(base64Decode(data, json_string_value(jsonVal)));

    // decrypted the data
    ABC_CHECK_RET(ABC_CryptoDecryptAES256Package(toU08Buf(data), Key, toU08Buf(iv),
                  pData, pError));

exit:
    return cc;
}

/**
 * Given a file holding encrypted data, this function decrypts it
 */
tABC_CC ABC_CryptoDecryptJSONFile(const char *szFilename,
                                  const tABC_U08Buf Key,
                                  tABC_U08Buf       *pData,
                                  tABC_Error        *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    ABC_SET_ERR_CODE(pError, ABC_CC_Ok);

    JsonPtr json;

    ABC_CHECK_NULL(szFilename);
    ABC_CHECK_NULL_BUF(Key);
    ABC_CHECK_NULL(pData);

    ABC_CHECK_NEW(json.load(szFilename));
    ABC_CHECK_RET(ABC_CryptoDecryptJSONObject(json.get(), Key, pData, pError));

exit:
    return cc;
}

/**
 * Loads the given file, decrypts it and creates the json object from it
 *
 * @param ppJSON_Data pointer to store allocated json object
 *                   (the user is responsible for json_decref'ing)
 */
tABC_CC ABC_CryptoDecryptJSONFileObject(const char *szFilename,
                                        const tABC_U08Buf Key,
                                        json_t **ppJSON_Data,
                                        tABC_Error  *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    ABC_SET_ERR_CODE(pError, ABC_CC_Ok);

    AutoU08Buf Data;
    JsonPtr file;

    ABC_CHECK_NULL(szFilename);
    ABC_CHECK_NULL_BUF(Key);
    ABC_CHECK_NULL(ppJSON_Data);
    *ppJSON_Data = NULL;

    ABC_CHECK_RET(ABC_CryptoDecryptJSONFile(szFilename, Key, &Data, pError));
    ABC_CHECK_NEW(file.decode(toString(Data)));
    *ppJSON_Data = json_incref(file.get());

exit:
    return cc;
}

/**
 * Creates an encrypted aes256 package that includes data, random header/footer and sha256
 * Package format:
 *   1 byte:     h (the number of random header bytes)
 *   h bytes:    h random header bytes
 *   4 bytes:    length of data (big endian)
 *   x bytes:    data (x bytes)
 *   1 byte:     f (the number of random footer bytes)
 *   f bytes:    f random header bytes
 *   32 bytes:   32 bytes SHA256 of all data up to this point
 */
static
tABC_CC ABC_CryptoEncryptAES256Package(const tABC_U08Buf Data,
                                       const tABC_U08Buf Key,
                                       tABC_U08Buf       *pEncData,
                                       DataChunk         &IV,
                                       tABC_Error        *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    ABC_SET_ERR_CODE(pError, ABC_CC_Ok);

    DataChunk headerData;
    DataChunk footerData;
    AutoU08Buf UnencryptedData;
    unsigned char nRandomHeaderBytes;
    unsigned char nRandomFooterBytes;
    unsigned long totalSizeUnencrypted = 0;
    unsigned char *pCurUnencryptedData = NULL;
    unsigned char nSizeByte = 0;
    unsigned char sha256Output[SHA256_DIGEST_LENGTH];

    ABC_CHECK_NULL_BUF(Data);
    ABC_CHECK_NULL_BUF(Key);
    ABC_CHECK_NULL(pEncData);

    // create a random IV
    ABC_CHECK_NEW(randomData(IV, AES_256_IV_LENGTH));

    // create a random number of header bytes 0-255
    {
        DataChunk r;
        ABC_CHECK_NEW(randomData(r, 1));
        nRandomHeaderBytes = r[0];
    }
    //printf("rand header count: %d\n", nRandomHeaderBytes);
    ABC_CHECK_NEW(randomData(headerData, nRandomHeaderBytes));

    // create a random number of footer bytes 0-255
    {
        DataChunk r;
        ABC_CHECK_NEW(randomData(r, 1));
        nRandomFooterBytes = r[0];
    }
    //printf("rand footer count: %d\n", nRandomFooterBytes);
    ABC_CHECK_NEW(randomData(footerData, nRandomFooterBytes));

    // calculate the size of our unencrypted buffer
    totalSizeUnencrypted += 1; // header count
    totalSizeUnencrypted += nRandomHeaderBytes; // header
    totalSizeUnencrypted += 4; // space to hold data size
    totalSizeUnencrypted += Data.size(); // data
    totalSizeUnencrypted += 1; // footer count
    totalSizeUnencrypted += nRandomFooterBytes; // footer
    totalSizeUnencrypted += SHA256_DIGEST_LENGTH; // sha256
    //printf("total size unencrypted: %lu\n", (unsigned long) totalSizeUnencrypted);

    // allocate the unencrypted buffer
    ABC_BUF_NEW(UnencryptedData, totalSizeUnencrypted);
    pCurUnencryptedData = UnencryptedData.data();

    // add the random header count and bytes
    memcpy(pCurUnencryptedData, &nRandomHeaderBytes, 1);
    pCurUnencryptedData += 1;
    memcpy(pCurUnencryptedData, headerData.data(), headerData.size());
    pCurUnencryptedData += nRandomHeaderBytes;

    // add the size of the data
    nSizeByte = (Data.size() >> 24) & 0xff;
    memcpy(pCurUnencryptedData, &nSizeByte, 1);
    pCurUnencryptedData += 1;
    nSizeByte = (Data.size() >> 16) & 0xff;
    memcpy(pCurUnencryptedData, &nSizeByte, 1);
    pCurUnencryptedData += 1;
    nSizeByte = (Data.size() >> 8) & 0xff;
    memcpy(pCurUnencryptedData, &nSizeByte, 1);
    pCurUnencryptedData += 1;
    nSizeByte = (Data.size() >> 0) & 0xff;
    memcpy(pCurUnencryptedData, &nSizeByte, 1);
    pCurUnencryptedData += 1;

    // add the data
    memcpy(pCurUnencryptedData, Data.data(), Data.size());
    pCurUnencryptedData += Data.size();

    // add the random footer count and bytes
    memcpy(pCurUnencryptedData, &nRandomFooterBytes, 1);
    pCurUnencryptedData += 1;
    memcpy(pCurUnencryptedData, footerData.data(), footerData.size());
    pCurUnencryptedData += nRandomFooterBytes;

    // add the sha256
    SHA256(UnencryptedData.data(), totalSizeUnencrypted - SHA256_DIGEST_LENGTH,
           sha256Output);
    memcpy(pCurUnencryptedData, sha256Output, SHA256_DIGEST_LENGTH);
    pCurUnencryptedData += SHA256_DIGEST_LENGTH;

    // encrypted our new unencrypted package
    ABC_CHECK_RET(ABC_CryptoEncryptAES256(UnencryptedData, Key, toU08Buf(IV),
                                          pEncData, pError));

exit:
    return cc;
}

/**
 * Decrypts an encrypted aes256 package which includes data, random header/footer and sha256
 * Note: it is critical that this function returns ABC_CC_DecryptFailure if there is an issue
 *       because code is counting on this specific error to know a key is bad
 * Package format:
 *   1 byte:     h (the number of random header bytes)
 *   h bytes:    h random header bytes
 *   4 bytes:    length of data (big endian)
 *   x bytes:    data (x bytes)
 *   1 byte:     f (the number of random footer bytes)
 *   f bytes:    f random header bytes
 *   32 bytes:   32 bytes SHA256 of all data up to this point
 */
static
tABC_CC ABC_CryptoDecryptAES256Package(const tABC_U08Buf EncData,
                                       const tABC_U08Buf Key,
                                       const tABC_U08Buf IV,
                                       tABC_U08Buf       *pData,
                                       tABC_Error        *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    ABC_SET_ERR_CODE(pError, ABC_CC_Ok);

    AutoU08Buf Data;
    unsigned char headerLength;
    unsigned int minSize;
    unsigned char *pDataLengthPos;
    unsigned int dataSecLength;
    unsigned char footerLength;
    unsigned int shaCheckLength;
    unsigned char *pSHALoc;
    unsigned char sha256Output[SHA256_DIGEST_LENGTH];
    unsigned char *pFinalDataPos;

    ABC_CHECK_NULL_BUF(EncData);
    ABC_CHECK_NULL_BUF(Key);
    ABC_CHECK_NULL_BUF(IV);
    ABC_CHECK_NULL(pData);

    // start by decrypting the pacakge
    if (ABC_CC_Ok != ABC_CryptoDecryptAES256(EncData, Key, IV, &Data, pError))
    {
        cc = ABC_CC_DecryptFailure;
        if (pError)
        {
            pError->code = cc;
        }
        goto exit;
    }

    // get the size of the random header section
    headerLength = *Data.data();

    // check that we have enough data based upon this info
    minSize = 1 + headerLength + 4 + 1 + 1 +
              SHA256_DIGEST_LENGTH; // decrypted package must be at least this big
    ABC_CHECK_ASSERT(Data.size() >= minSize, ABC_CC_DecryptFailure,
                     "Decrypted data is not long enough");

    // get the size of the data section
    pDataLengthPos = Data.data() + (1 + headerLength);
    dataSecLength = ((unsigned int) *pDataLengthPos) << 24;
    pDataLengthPos++;
    dataSecLength += ((unsigned int) *pDataLengthPos) << 16;
    pDataLengthPos++;
    dataSecLength += ((unsigned int) *pDataLengthPos) << 8;
    pDataLengthPos++;
    dataSecLength += ((unsigned int) *pDataLengthPos);

    // check that we have enough data based upon this info
    minSize = 1 + headerLength + 4 + dataSecLength + 1 +
              SHA256_DIGEST_LENGTH; // decrypted package must be at least this big
    ABC_CHECK_ASSERT(Data.size() >= minSize, ABC_CC_DecryptFailure,
                     "Decrypted data is not long enough");

    // get the size of the random footer section
    footerLength = *(Data.data() + 1 + headerLength + 4 + dataSecLength);

    // check that we have enough data based upon this info
    minSize = 1 + headerLength + 4 + dataSecLength + 1 + footerLength +
              SHA256_DIGEST_LENGTH; // decrypted package must be at least this big
    ABC_CHECK_ASSERT(Data.size() >= minSize, ABC_CC_DecryptFailure,
                     "Decrypted data is not long enough");

    // set up for the SHA check
    shaCheckLength = 1 + headerLength + 4 + dataSecLength + 1 +
                     footerLength; // all but the sha
    pSHALoc = Data.data() + shaCheckLength;

    // calc the sha256
    SHA256(Data.data(), shaCheckLength, sha256Output);

    // check the sha256
    if (!cryptoCompare(pSHALoc, sha256Output, SHA256_DIGEST_LENGTH))
    {
        // this can be specifically used by the caller to possibly determine whether the key was incorrect
        ABC_RET_ERROR(ABC_CC_DecryptFailure,
                      "Decrypted data failed checksum (SHA) check");
    }

    // all is good, so create the final data
    pFinalDataPos = Data.data() + 1 + headerLength + 4;
    ABC_BUF_NEW(*pData, dataSecLength);
    memcpy(pData->data(), pFinalDataPos, dataSecLength);

exit:
    return cc;
}

/**
 * Encrypts the given data with AES256
 */
static
tABC_CC ABC_CryptoEncryptAES256(const tABC_U08Buf Data,
                                const tABC_U08Buf Key,
                                const tABC_U08Buf IV,
                                tABC_U08Buf       *pEncData,
                                tABC_Error        *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    ABC_SET_ERR_CODE(pError, ABC_CC_Ok);

    unsigned char aKey[AES_256_KEY_LENGTH];
    unsigned int keyLength = Key.size();
    unsigned char aIV[AES_256_IV_LENGTH];
    unsigned int IVLength;
    int c_len;
    int f_len = 0;
    unsigned char *pTmpEncData = NULL;

    ABC_CHECK_NULL_BUF(Data);
    ABC_CHECK_NULL_BUF(Key);
    ABC_CHECK_NULL_BUF(IV);
    ABC_CHECK_NULL(pEncData);

    // create the final key
    memset(aKey, 0, AES_256_KEY_LENGTH);
    if (keyLength > AES_256_KEY_LENGTH)
    {
        keyLength = AES_256_KEY_LENGTH;
    }
    memcpy(aKey, Key.data(), keyLength);

    // create the IV
    memset(aIV, 0, AES_256_IV_LENGTH);
    IVLength = IV.size();
    if (IVLength > AES_256_IV_LENGTH)
    {
        IVLength = AES_256_IV_LENGTH;
    }
    memcpy(aIV, IV.data(), IVLength);

    // init our cipher text struct
    EVP_CIPHER_CTX e_ctx;
    EVP_CIPHER_CTX_init(&e_ctx);
    EVP_EncryptInit_ex(&e_ctx, EVP_aes_256_cbc(), NULL, aKey, aIV);

    // max ciphertext len for a n bytes of plaintext is n + AES_256_BLOCK_LENGTH -1 bytes
    c_len = Data.size() + AES_256_BLOCK_LENGTH;
    ABC_ARRAY_NEW(pTmpEncData, c_len, unsigned char);

    // allows reusing of 'e' for multiple encryption cycles
    EVP_EncryptInit_ex(&e_ctx, NULL, NULL, NULL, NULL);

    // update pTmpEncData, c_len is filled with the length of pTmpEncData generated, dataLength is the size of plaintext in bytes
    EVP_EncryptUpdate(&e_ctx, pTmpEncData, &c_len, Data.data(), Data.size());

    // update pTmpEncData with the final remaining bytes
    EVP_EncryptFinal_ex(&e_ctx, pTmpEncData + c_len, &f_len);

    // set final values
    *pEncData = U08Buf(pTmpEncData, c_len + f_len);

    EVP_CIPHER_CTX_cleanup(&e_ctx);

exit:

    return cc;
}

/**
 * Decrypts the given data with AES256
 */
static
tABC_CC ABC_CryptoDecryptAES256(const tABC_U08Buf EncData,
                                const tABC_U08Buf Key,
                                const tABC_U08Buf IV,
                                tABC_U08Buf       *pData,
                                tABC_Error        *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    ABC_SET_ERR_CODE(pError, ABC_CC_Ok);

    unsigned char *pTmpData = NULL;
    int p_len, f_len;
    unsigned int IVLength;
    unsigned int keyLength;

    ABC_CHECK_NULL_BUF(EncData);
    ABC_CHECK_NULL_BUF(Key);
    ABC_CHECK_NULL_BUF(IV);
    ABC_CHECK_NULL(pData);

    // create the final key
    unsigned char aKey[AES_256_KEY_LENGTH];
    memset(aKey, 0, AES_256_KEY_LENGTH);
    keyLength = Key.size();
    if (keyLength > AES_256_KEY_LENGTH)
    {
        keyLength = AES_256_KEY_LENGTH;
    }
    memcpy(aKey, Key.data(), keyLength);

    // create the IV
    unsigned char aIV[AES_256_IV_LENGTH];
    memset(aIV, 0, AES_256_IV_LENGTH);
    IVLength = IV.size();
    if (IVLength > AES_256_IV_LENGTH)
    {
        IVLength = AES_256_IV_LENGTH;
    }
    memcpy(aIV, IV.data(), IVLength);

    // init our cipher text struct
    EVP_CIPHER_CTX d_ctx;
    EVP_CIPHER_CTX_init(&d_ctx);
    EVP_DecryptInit_ex(&d_ctx, EVP_aes_256_cbc(), NULL, aKey, aIV);

    /* because we have padding ON, we must allocate an extra cipher block size of memory */
    p_len = EncData.size();
    f_len = 0;
    ABC_ARRAY_NEW(pTmpData, p_len + AES_256_BLOCK_LENGTH, unsigned char);

    // decrypt
    EVP_DecryptInit_ex(&d_ctx, NULL, NULL, NULL, NULL);
    EVP_DecryptUpdate(&d_ctx, pTmpData, &p_len, EncData.data(), EncData.size());
    EVP_DecryptFinal_ex(&d_ctx, pTmpData + p_len, &f_len);

    EVP_CIPHER_CTX_cleanup(&d_ctx);

    // set final values
    *pData = U08Buf(pTmpData, p_len + f_len);

exit:
    return cc;
}

} // namespace abcd
