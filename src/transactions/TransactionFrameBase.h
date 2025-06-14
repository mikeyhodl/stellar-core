// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <optional>

#include "ledger/LedgerHashUtils.h"
#include "ledger/LedgerStateSnapshot.h"
#include "ledger/NetworkConfig.h"
#include "main/Config.h"
#include "overlay/StellarXDR.h"
#include "util/TxResource.h"
#include "util/UnorderedSet.h"
#include "util/types.h"
#include <optional>

namespace stellar
{
class AbstractLedgerTxn;
class Application;
class Database;
class OperationFrame;
class TransactionFrame;
class FeeBumpTransactionFrame;
class AppConnector;
class SignatureChecker;

class MutableTransactionResultBase;
using MutableTxResultPtr = std::unique_ptr<MutableTransactionResultBase>;

class TransactionMetaBuilder;
class DiagnosticEventManager;

class TransactionFrameBase;
using TransactionFrameBasePtr = std::shared_ptr<TransactionFrameBase const>;
using TransactionFrameBaseConstPtr =
    std::shared_ptr<TransactionFrameBase const>;

class TransactionFrameBase
{
  public:
    static TransactionFrameBasePtr
    makeTransactionFromWire(Hash const& networkID,
                            TransactionEnvelope const& env);

    virtual bool apply(AppConnector& app, AbstractLedgerTxn& ltx,
                       TransactionMetaBuilder& meta,
                       MutableTransactionResultBase& txResult,
                       Hash const& sorobanBasePrngSeed = Hash{}) const = 0;
    virtual MutableTxResultPtr
    checkValid(AppConnector& app, LedgerSnapshot const& ls,
               SequenceNumber current, uint64_t lowerBoundCloseTimeOffset,
               uint64_t upperBoundCloseTimeOffset,
               DiagnosticEventManager& diagnosticEvents) const = 0;
    virtual bool
    checkSorobanResources(SorobanNetworkConfig const& cfg,
                          uint32_t ledgerVersion,
                          DiagnosticEventManager& diagnosticEvents) const = 0;

    virtual MutableTxResultPtr
    createTxErrorResult(TransactionResultCode txErrorCode) const = 0;

    virtual MutableTxResultPtr createValidationSuccessResult() const = 0;

    virtual TransactionEnvelope const& getEnvelope() const = 0;

    virtual bool checkSignature(SignatureChecker& signatureChecker,
                                LedgerEntryWrapper const& account,
                                int32_t neededWeight) const = 0;

#ifdef BUILD_TESTS
    virtual TransactionEnvelope& getMutableEnvelope() const = 0;
    virtual void clearCached() const = 0;
    virtual bool isTestTx() const = 0;
#endif

    // Returns the total fee of this transaction, including the 'flat',
    // non-market part.
    virtual int64_t getFullFee() const = 0;
    // Returns the part of the full fee used to make decisions as to
    // whether this transaction should be included into ledger.
    virtual int64_t getInclusionFee() const = 0;
    virtual int64_t getFee(LedgerHeader const& header,
                           std::optional<int64_t> baseFee,
                           bool applying) const = 0;

    virtual Hash const& getContentsHash() const = 0;
    virtual Hash const& getFullHash() const = 0;

    virtual uint32_t getNumOperations() const = 0;
    virtual std::vector<std::shared_ptr<OperationFrame const>> const&
    getOperationFrames() const = 0;
    virtual Resource getResources(bool useByteLimitInClassic,
                                  uint32_t ledgerVersion) const = 0;

    virtual std::vector<Operation> const& getRawOperations() const = 0;

    virtual SequenceNumber getSeqNum() const = 0;
    virtual AccountID getFeeSourceID() const = 0;
    virtual AccountID getSourceID() const = 0;
    virtual std::optional<SequenceNumber const> const getMinSeqNum() const = 0;
    virtual Duration getMinSeqAge() const = 0;
    virtual uint32 getMinSeqLedgerGap() const = 0;

    virtual void
    insertKeysForFeeProcessing(UnorderedSet<LedgerKey>& keys) const = 0;
    virtual void insertKeysForTxApply(UnorderedSet<LedgerKey>& keys,
                                      LedgerKeyMeter* lkMeter) const = 0;

    virtual MutableTxResultPtr
    processFeeSeqNum(AbstractLedgerTxn& ltx,
                     std::optional<int64_t> baseFee) const = 0;

    // After this transaction has been applied
    virtual void
    processPostApply(AppConnector& app, AbstractLedgerTxn& ltx,
                     TransactionMetaBuilder& meta,
                     MutableTransactionResultBase& txResult) const = 0;

    // After all transactions have been applied
    virtual void
    processPostTxSetApply(AppConnector& app, AbstractLedgerTxn& ltx,
                          MutableTransactionResultBase& txResult,
                          TxEventManager& txEventManager) const = 0;

    virtual std::shared_ptr<StellarMessage const> toStellarMessage() const = 0;

    virtual bool hasDexOperations() const = 0;

    virtual bool isSoroban() const = 0;
    virtual SorobanResources const& sorobanResources() const = 0;
    virtual SorobanTransactionData::_ext_t const& getResourcesExt() const = 0;
    virtual int64 declaredSorobanResourceFee() const = 0;
    virtual bool XDRProvidesValidFee() const = 0;

    // Returns true if this TX is a soroban transaction with a
    // RestoreFootprintOp.
    virtual bool isRestoreFootprintTx() const = 0;
};
}
