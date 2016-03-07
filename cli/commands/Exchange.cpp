/*
 * Copyright (c) 2015, Airbitz, Inc.
 * All rights reserved.
 *
 * See the LICENSE file for more information.
 */

#include "../Command.hpp"
#include "../../abcd/Context.hpp"
#include <iostream>

using namespace abcd;

COMMAND(InitLevel::context, ExchangeFetch, "exchange-fetch",
        "")
{
    if (argc != 0)
        return ABC_ERROR(ABC_CC_Error, helpString(*this));

    for (const auto &source: exchangeSources)
    {
        ExchangeRates rates;
        ABC_CHECK(exchangeSourceFetch(rates, source));

        std::cout << source << ":" << std::endl;
        for (auto &i: rates)
        {
            std::string code, name;
            ABC_CHECK(currencyCode(code, i.first));
            ABC_CHECK(currencyName(name, i.first));
            std::cout << code << ": " << i.second << "\t# " << name << std::endl;
        }
        std::cout << std::endl;
    }

    return Status();
}

COMMAND(InitLevel::account, ExchangeUpdate, "exchange-update",
        " <currency>")
{
    if (argc != 1)
        return ABC_ERROR(ABC_CC_Error, helpString(*this));
    const auto currencyName = argv[0];

    Currency currency;
    ABC_CHECK(currencyNumber(currency, currencyName));
    ABC_CHECK_OLD(ABC_RequestExchangeRateUpdate(session.username.c_str(),
                  session.password.c_str(),
                  static_cast<int>(currency), &error));

    double rate;
    ABC_CHECK(gContext->exchangeCache.satoshiToCurrency(rate, 100000000, currency));
    std::cout << "result: " << rate << std::endl;

    return Status();
}

#define CURRENCY_SET_ROW(code, number, name) Currency::code,

COMMAND(InitLevel::context, ExchangeValidate, "exchange-validate",
        "\n"
        "Validates that all currencies have sources.")
{
    if (argc != 0)
        return ABC_ERROR(ABC_CC_Error, helpString(*this));

    Currencies currencies
    {
        ABC_CURRENCY_LIST(CURRENCY_SET_ROW)
    };

    // Eliminate any currencies the exchange sources provide:
    for (const auto &source: exchangeSources)
    {
        ExchangeRates rates;
        ABC_CHECK(exchangeSourceFetch(rates, source));

        for (auto &rate: rates)
        {
            auto i = currencies.find(rate.first);
            if (currencies.end() != i)
                currencies.erase(i);
        }
    }

    // Print an message if there is anything left:
    if (currencies.size())
    {
        std::cout << "The following currencies have no sources:" << std::endl;
        for (auto &currency: currencies)
        {
            std::string code;
            ABC_CHECK(currencyCode(code, currency));
            std::cout << code << std::endl;
        }
    }

    return Status();
}
