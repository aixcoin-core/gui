// Copyright (c) 2011-present The Aixcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef AIXCOIN_QT_AIXCOINADDRESSVALIDATOR_H
#define AIXCOIN_QT_AIXCOINADDRESSVALIDATOR_H

#include <QValidator>

/** Base58 entry widget validator, checks for valid characters and
 * removes some whitespace.
 */
class AixcoinAddressEntryValidator : public QValidator
{
    Q_OBJECT

public:
    explicit AixcoinAddressEntryValidator(QObject *parent);

    State validate(QString &input, int &pos) const override;
};

/** Aixcoin address widget validator, checks for a valid aixcoin address.
 */
class AixcoinAddressCheckValidator : public QValidator
{
    Q_OBJECT

public:
    explicit AixcoinAddressCheckValidator(QObject *parent);

    State validate(QString &input, int &pos) const override;
};

#endif // AIXCOIN_QT_AIXCOINADDRESSVALIDATOR_H
