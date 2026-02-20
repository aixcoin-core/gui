// Copyright (c) 2011-present The Aix Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_BITCOINADDRESSVALIDATOR_H
#define BITCOIN_QT_BITCOINADDRESSVALIDATOR_H

#include <QValidator>

/** Base58 entry widget validator, checks for valid characters and
 * removes some whitespace.
 */
class AixAddressEntryValidator : public QValidator
{
    Q_OBJECT

public:
    explicit AixAddressEntryValidator(QObject *parent);

    State validate(QString &input, int &pos) const override;
};

/** Aix address widget validator, checks for a valid bitcoin address.
 */
class AixAddressCheckValidator : public QValidator
{
    Q_OBJECT

public:
    explicit AixAddressCheckValidator(QObject *parent);

    State validate(QString &input, int &pos) const override;
};

#endif // BITCOIN_QT_BITCOINADDRESSVALIDATOR_H
