/*
 * Copyright (c) 2014, AirBitz, Inc.
 * All rights reserved.
 *
 * See the LICENSE file for more information.
 */

#include "Scrypt.hpp"
#include "Random.hpp"
#include "../util/Debug.hpp"
#include "../bitcoin/Testnet.hpp"
#include "../../minilibs/scrypt/crypto_scrypt.h"
#include <sys/time.h>
#include <math.h>

namespace abcd {

#define SCRYPT_DEFAULT_SERVER_N         16384    // can't change as server uses this as well
#define SCRYPT_DEFAULT_SERVER_R         1        // can't change as server uses this as well
#define SCRYPT_DEFAULT_SERVER_P         1        // can't change as server uses this as well
#define SCRYPT_DEFAULT_CLIENT_N_SHIFT   14
#define SCRYPT_DEFAULT_CLIENT_N         (1 << SCRYPT_DEFAULT_CLIENT_N_SHIFT) //16384
#define SCRYPT_DEFAULT_CLIENT_R         1
#define SCRYPT_DEFAULT_CLIENT_P         1
#define SCRYPT_MAX_CLIENT_N_SHIFT       17
#define SCRYPT_MAX_CLIENT_N             (1 << SCRYPT_MAX_CLIENT_N_SHIFT)
#define SCRYPT_MAX_CLIENT_R             8
#define SCRYPT_TARGET_USECONDS          250000

#define SCRYPT_DEFAULT_SALT_LENGTH 32

Status
ScryptSnrp::create()
{
    // Set up default values:
    ABC_CHECK(randomData(salt, SCRYPT_DEFAULT_SALT_LENGTH));
    n = SCRYPT_DEFAULT_CLIENT_N;
    r = SCRYPT_DEFAULT_CLIENT_R;
    p = SCRYPT_DEFAULT_CLIENT_P;

    // Benchmark the CPU:
    DataChunk temp;
    struct timeval timerStart;
    struct timeval timerEnd;
    gettimeofday(&timerStart, nullptr);
    ABC_CHECK(hash(temp, salt));
    gettimeofday(&timerEnd, nullptr);

    // Find the time in microseconds:
    int totalTime = 1000000 * (timerEnd.tv_sec - timerStart.tv_sec);
    totalTime += timerEnd.tv_usec;
    totalTime -= timerStart.tv_usec;
    int diffShift = 0;

    ABC_DebugLevel(1, "Scrypt target:%d timing:%d", SCRYPT_TARGET_USECONDS, totalTime);

    if (totalTime >= SCRYPT_TARGET_USECONDS)
    {
        ABC_DebugLevel(1, "Scrypt timing: Slow device");
        // Very slow device.
        // Do nothing, use default scrypt settings which are lowest we'll go
    }
    else if (totalTime >= SCRYPT_TARGET_USECONDS / SCRYPT_MAX_CLIENT_R)
    {
        // Medium speed device.
        // Scale R between 1 to 8 assuming linear effect on hashing time.
        // Don't touch N.
        ABC_DebugLevel(1, "Scrypt timing: Medium device");
        r = SCRYPT_TARGET_USECONDS / totalTime;
    }
    else if (totalTime > 0)
    {
        // Very fast device.
        r = SCRYPT_MAX_CLIENT_R;

        // Need to adjust scryptN to make scrypt even stronger:
        unsigned int numShifts = (SCRYPT_TARGET_USECONDS / SCRYPT_MAX_CLIENT_R) / totalTime;
        numShifts--;

        if (SCRYPT_MAX_CLIENT_N_SHIFT < numShifts + SCRYPT_DEFAULT_CLIENT_N_SHIFT)
        {
            diffShift = numShifts + SCRYPT_DEFAULT_CLIENT_N_SHIFT - SCRYPT_MAX_CLIENT_N_SHIFT;
            n = (1 << SCRYPT_MAX_CLIENT_N_SHIFT);
        }
        else
        {
            n = (1 << (numShifts + SCRYPT_DEFAULT_CLIENT_N_SHIFT));
        }

        ABC_DebugLevel(1, "Scrypt timing: Fast device diffShift:%d n:%d", diffShift, n);
    }

    if (diffShift)
    {
        int addP = diffShift;
        ABC_DebugLevel(1, "Scrypt timing: Fast device addP:%d", addP);
        p += addP;
    }

    ABC_DebugLevel(1, "NRp = %d %d %d", n, r, p);

    return Status();
}

Status
ScryptSnrp::hash(DataChunk &result, DataSlice data, size_t size) const
{
    DataChunk out(size);

    int rc = crypto_scrypt(data.data(), data.size(),
        salt.data(), salt.size(), n, r, p, out.data(), size);
    if (rc)
        return ABC_ERROR(ABC_CC_ScryptError, "Error calculating Scrypt hash");

    result = std::move(out);
    return Status();
}

const ScryptSnrp &
usernameSnrp()
{
    static const ScryptSnrp mainnet =
    {
        {
            0xb5, 0x86, 0x5f, 0xfb, 0x9f, 0xa7, 0xb3, 0xbf,
            0xe4, 0xb2, 0x38, 0x4d, 0x47, 0xce, 0x83, 0x1e,
            0xe2, 0x2a, 0x4a, 0x9d, 0x5c, 0x34, 0xc7, 0xef,
            0x7d, 0x21, 0x46, 0x7c, 0xc7, 0x58, 0xf8, 0x1b
        },
        SCRYPT_DEFAULT_SERVER_N,
        SCRYPT_DEFAULT_SERVER_R,
        SCRYPT_DEFAULT_SERVER_P
    };
    static const ScryptSnrp testnet =
    {
        {
            0xa5, 0x96, 0x3f, 0x3b, 0x9c, 0xa6, 0xb3, 0xbf,
            0xe4, 0xb2, 0x36, 0x42, 0x37, 0xfe, 0x87, 0x1e,
            0xf2, 0x2a, 0x4a, 0x9d, 0x4c, 0x34, 0xa7, 0xef,
            0x3d, 0x21, 0x47, 0x8c, 0xc7, 0x58, 0xf8, 0x1b
        },
        SCRYPT_DEFAULT_SERVER_N,
        SCRYPT_DEFAULT_SERVER_R,
        SCRYPT_DEFAULT_SERVER_P
    };

    return isTestnet() ? testnet : mainnet;
}

} // namespace abcd
