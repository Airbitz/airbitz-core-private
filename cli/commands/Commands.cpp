/*
 * Copyright (c) 2014, AirBitz, Inc.
 * All rights reserved.
 *
 * See the LICENSE file for more information.
 */

#include "../Command.hpp"
#include "../Util.hpp"
#include "../../abcd/General.hpp"
#include "../../abcd/account/Account.hpp"
#include "../../abcd/json/JsonBox.hpp"
#include "../../abcd/login/Login.hpp"
#include "../../abcd/util/FileIO.hpp"
#include "../../abcd/util/Util.hpp"
#include "../../abcd/wallet/Wallet.hpp"
#include <bitcoin/bitcoin.hpp>
#include <iostream>

using namespace abcd;

COMMAND(InitLevel::context, AccountAvailable, "account-available")
{
    if (argc != 1)
        return ABC_ERROR(ABC_CC_Error, "usage: ... account-available <user>");
    ABC_CHECK_OLD(ABC_AccountAvailable(argv[0], &error));
    return Status();
}

COMMAND(InitLevel::account, AccountDecrypt, "account-decrypt")
{
    if (argc != 3)
        return ABC_ERROR(ABC_CC_Error,
                         "usage: ... account-decrypt <user> <pass> <filename>\n"
                         "note: The filename is account-relative.");

    JsonBox box;
    ABC_CHECK(box.load(session.account->dir() + argv[2]));

    DataChunk data;
    ABC_CHECK(box.decrypt(data, session.login->dataKey()));
    std::cout << toString(data) << std::endl;

    return Status();
}

COMMAND(InitLevel::account, AccountEncrypt, "account-encrypt")
{
    if (argc != 3)
        return ABC_ERROR(ABC_CC_Error,
                         "usage: ... account-encrypt <user> <pass> <filename>\n"
                         "note: The filename is account-relative.");

    DataChunk contents;
    ABC_CHECK(fileLoad(contents, session.account->dir() + argv[2]));

    JsonBox box;
    ABC_CHECK(box.encrypt(contents, session.login->dataKey()));

    std::cout << box.encode() << std::endl;

    return Status();
}

COMMAND(InitLevel::login, ChangePassword, "change-password")
{
    if (argc != 4)
        return ABC_ERROR(ABC_CC_Error,
                         "usage: ... change-password <user> <pass> <new-pass>");

    ABC_CHECK_OLD(ABC_ChangePassword(session.username, session.password, argv[2],
                                     &error));

    return Status();
}

COMMAND(InitLevel::lobby, ChangePasswordRecovery, "change-password-recovery")
{
    if (argc != 4)
        return ABC_ERROR(ABC_CC_Error,
                         "usage: ... change-password-recovery <user> <ra> <new-pass>");

    ABC_CHECK_OLD(ABC_ChangePasswordWithRecoveryAnswers(session.username, argv[1],
                  argv[2], &error));

    return Status();
}

COMMAND(InitLevel::context, CheckPassword, "check-password")
{
    if (argc != 1)
        return ABC_ERROR(ABC_CC_Error, "usage: ... check-password <pass>");

    double secondsToCrack;
    unsigned int count = 0;
    tABC_PasswordRule **aRules = NULL;
    ABC_CHECK_OLD(ABC_CheckPassword(argv[0], &secondsToCrack, &aRules, &count,
                                    &error));

    for (unsigned i = 0; i < count; ++i)
    {
        printf("%s: %d\n", aRules[i]->szDescription, aRules[i]->bPassed);
    }
    printf("Time to Crack: %f\n", secondsToCrack);
    ABC_FreePasswordRuleArray(aRules, count);

    return Status();
}

COMMAND(InitLevel::lobby, CheckRecoveryAnswers, "check-recovery-answers")
{
    if (argc != 2)
        return ABC_ERROR(ABC_CC_Error,
                         "usage: ... check-recovery-answers <user> <ras>");

    AutoString szQuestions;
    ABC_CHECK_OLD(ABC_GetRecoveryQuestions(session.username, &szQuestions.get(),
                                           &error));
    printf("%s\n", szQuestions.get());

    bool bValid = false;
    ABC_CHECK_OLD(ABC_CheckRecoveryAnswers(session.username, argv[1], &bValid,
                                           &error));
    printf("%s\n", bValid ? "Valid!" : "Invalid!");

    return Status();
}

COMMAND(InitLevel::context, CreateAccount, "create-account")
{
    if (argc != 2)
        return ABC_ERROR(ABC_CC_Error, "usage: ... create-account <user> <pass>");

    ABC_CHECK_OLD(ABC_CreateAccount(argv[0], argv[1], &error));
    ABC_CHECK_OLD(ABC_SetPIN(argv[2], argv[3], "1234", &error));

    return Status();
}

COMMAND(InitLevel::account, DataSync, "data-sync")
{
    if (argc != 2)
        return ABC_ERROR(ABC_CC_Error, "usage: ... data-sync <user> <pass>");

    ABC_CHECK(syncAll(*session.account));

    return Status();
}

COMMAND(InitLevel::context, GeneralUpdate, "general-update")
{
    if (argc != 0)
        return ABC_ERROR(ABC_CC_Error, "usage: ... general-update");

    ABC_CHECK(generalUpdate());

    return Status();
}

COMMAND(InitLevel::wallet, GenerateAddresses, "generate-addresses")
{
    if (argc != 4)
        return ABC_ERROR(ABC_CC_Error,
                         "usage: ... generate-addresses <user> <pass> <wallet-name> <count>");

    bc::hd_private_key m(session.wallet->bitcoinKey());
    bc::hd_private_key m0 = m.generate_private_key(0);
    bc::hd_private_key m00 = m0.generate_private_key(0);
    long max = strtol(argv[3], 0, 10);
    for (int i = 0; i < max; ++i)
    {
        bc::hd_private_key m00n = m00.generate_private_key(i);
        std::cout << "watch " << m00n.address().encoded() << std::endl;
    }

    return Status();
}

COMMAND(InitLevel::context, GetQuestionChoices, "get-question-choices")
{
    if (argc != 0)
        return ABC_ERROR(ABC_CC_Error, "usage: ... get-question-choices");

    AutoFree<tABC_QuestionChoices, ABC_FreeQuestionChoices> pChoices;
    ABC_CHECK_OLD(ABC_GetQuestionChoices(&pChoices.get(), &error));

    printf("Choices:\n");
    for (unsigned i = 0; i < pChoices->numChoices; ++i)
    {
        printf(" %s (%s, %d)\n", pChoices->aChoices[i]->szQuestion,
               pChoices->aChoices[i]->szCategory,
               pChoices->aChoices[i]->minAnswerLength);
    }

    return Status();
}

COMMAND(InitLevel::lobby, GetQuestions, "get-questions")
{
    if (argc != 1)
        return ABC_ERROR(ABC_CC_Error, "usage: ... get-questions <user>");

    AutoString questions;
    ABC_CHECK_OLD(ABC_GetRecoveryQuestions(session.username, &questions.get(),
                                           &error));
    printf("Questions: %s\n", questions.get());

    return Status();
}

COMMAND(InitLevel::login, GetSettings, "get-settings")
{
    if (argc != 2)
        return ABC_ERROR(ABC_CC_Error, "usage: ... get-settings <user> <pass>");

    AutoFree<tABC_AccountSettings, ABC_FreeAccountSettings> pSettings;
    ABC_CHECK_OLD(ABC_LoadAccountSettings(session.username, session.password,
                                          &pSettings.get(), &error));

    printf("First name: %s\n",
           pSettings->szFirstName ? pSettings->szFirstName : "(none)");
    printf("Last name: %s\n",
           pSettings->szLastName ? pSettings->szLastName : "(none)");
    printf("Nickname: %s\n",
           pSettings->szNickname ? pSettings->szNickname : "(none)");
    printf("PIN: %s\n", pSettings->szPIN ? pSettings->szPIN : "(none)");
    printf("List name on payments: %s\n",
           pSettings->bNameOnPayments ? "yes" : "no");
    printf("Minutes before auto logout: %d\n", pSettings->minutesAutoLogout);
    printf("Language: %s\n", pSettings->szLanguage);
    printf("Currency num: %d\n", pSettings->currencyNum);
    printf("Advanced features: %s\n", pSettings->bAdvancedFeatures ? "yes" : "no");
    printf("Denomination satoshi: %ld\n", pSettings->bitcoinDenomination.satoshi);
    printf("Denomination id: %d\n",
           pSettings->bitcoinDenomination.denominationType);
    printf("Daily Spend Enabled: %d\n", pSettings->bDailySpendLimit);
    printf("Daily Spend Limit: %ld\n", (long) pSettings->dailySpendLimitSatoshis);
    printf("PIN Spend Enabled: %d\n", pSettings->bSpendRequirePin);
    printf("PIN Spend Limit: %ld\n", (long) pSettings->spendRequirePinSatoshis);
    printf("Exchange rate source: %s\n", pSettings->szExchangeRateSource );

    return Status();
}

COMMAND(InitLevel::context, ListAccounts, "list-accounts")
{
    if (argc != 0)
        return ABC_ERROR(ABC_CC_Error, "usage: ... list-accounts");

    AutoString usernames;
    ABC_CHECK_OLD(ABC_ListAccounts(&usernames.get(), &error));
    printf("Usernames:\n%s", usernames.get());

    return Status();
}

COMMAND(InitLevel::lobby, PinLogin, "pin-login")
{
    if (argc != 2)
        return ABC_ERROR(ABC_CC_Error, "usage: ... pin-login <user> <pin>");

    bool bExists;
    ABC_CHECK_OLD(ABC_PinLoginExists(session.username, &bExists, &error));
    if (bExists)
    {
        ABC_CHECK_OLD(ABC_PinLogin(session.username, argv[1], &error));
    }
    else
    {
        printf("Login expired\n");
    }

    return Status();
}


COMMAND(InitLevel::account, PinLoginSetup, "pin-login-setup")
{
    if (argc != 2)
        return ABC_ERROR(ABC_CC_Error, "usage: ... pin-login-setup <user> <pass>");

    ABC_CHECK_OLD(ABC_PinSetup(session.username, session.password, &error));

    return Status();
}

COMMAND(InitLevel::login, RecoveryReminderSet, "recovery-reminder-set")
{
    if (argc != 3)
        return ABC_ERROR(ABC_CC_Error,
                         "usage: ... recovery-reminder-set <user> <pass> <n>");

    AutoFree<tABC_AccountSettings, ABC_FreeAccountSettings> pSettings;
    ABC_CHECK_OLD(ABC_LoadAccountSettings(session.username, session.password,
                                          &pSettings.get(), &error));
    printf("Old Reminder Count: %d\n", pSettings->recoveryReminderCount);

    pSettings->recoveryReminderCount = strtol(argv[2], 0, 10);
    ABC_CHECK_OLD(ABC_UpdateAccountSettings(session.username, session.password,
                                            pSettings, &error));

    return Status();
}

COMMAND(InitLevel::wallet, SearchBitcoinSeed, "search-bitcoin-seed")
{
    if (argc != 6)
        return ABC_ERROR(ABC_CC_Error,
                         "usage: ... search-bitcoin-seed <user> <pass> <wallet-name> <addr> <start> <end>");

    long start = strtol(argv[4], 0, 10);
    long end = strtol(argv[5], 0, 10);
    char *szMatchAddr = argv[3];

    bc::hd_private_key m(session.wallet->bitcoinKey());
    bc::hd_private_key m0 = m.generate_private_key(0);
    bc::hd_private_key m00 = m0.generate_private_key(0);

    for (long i = start, c = 0; i <= end; i++, ++c)
    {
        bc::hd_private_key m00n = m00.generate_private_key(i);
        if (m00n.address().encoded() == szMatchAddr)
        {
            printf("Found %s at %ld\n", szMatchAddr, i);
            break;
        }
        if (c == 100000)
        {
            printf("%ld\n", i);
            c = 0;
        }
    }

    return Status();
}

COMMAND(InitLevel::account, SetNickname, "set-nickname")
{
    if (argc != 3)
        return ABC_ERROR(ABC_CC_Error, "usage: ... set-nickname <user> <pass> <name>");

    AutoFree<tABC_AccountSettings, ABC_FreeAccountSettings> pSettings;
    ABC_CHECK_OLD(ABC_LoadAccountSettings(session.username, session.password,
                                          &pSettings.get(), &error));
    free(pSettings->szNickname);
    pSettings->szNickname = strdup(argv[2]);
    ABC_CHECK_OLD(ABC_UpdateAccountSettings(session.username, session.password,
                                            pSettings, &error));

    return Status();
}

COMMAND(InitLevel::lobby, SignIn, "sign-in")
{
    if (argc != 2)
        return ABC_ERROR(ABC_CC_Error, "usage: ... sign-in <user> <pass>");

    tABC_Error error;
    tABC_CC cc = ABC_SignIn(session.username, argv[1], &error);
    if (ABC_CC_InvalidOTP == cc)
    {
        AutoString date;
        ABC_CHECK_OLD(ABC_OtpResetDate(&date.get(), &error));
        if (strlen(date))
            std::cout << "Pending OTP reset ends at " << date.get() << std::endl;
        std::cout << "No OTP token, resetting account 2-factor auth." << std::endl;
        ABC_CHECK_OLD(ABC_OtpResetSet(session.username, &error));
    }

    return Status();
}

COMMAND(InitLevel::account, UploadLogs, "upload-logs")
{
    if (argc != 2)
        return ABC_ERROR(ABC_CC_Error, "usage: ... upload-logs <user> <pass>");

    // TODO: Command non-functional without a watcher thread!
    ABC_CHECK_OLD(ABC_UploadLogs(session.username, session.password, &error));

    return Status();
}

COMMAND(InitLevel::none, Version, "version")
{
    AutoString version;
    ABC_CHECK_OLD(ABC_Version(&version.get(), &error));
    std::cout << "ABC version: " << version.get() << std::endl;
    return Status();
}
