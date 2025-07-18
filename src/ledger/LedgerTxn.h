#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/LiveBucketList.h"
#include "ledger/InternalLedgerEntry.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "util/UnorderedMap.h"
#include "util/UnorderedSet.h"
#include "util/types.h"
#include "xdr/Stellar-ledger.h"
#include <functional>
#include <ledger/LedgerHashUtils.h>
#include <map>
#include <memory>
#include <set>
#include <soci.h>

/////////////////////////////////////////////////////////////////////////////
//  Overview
/////////////////////////////////////////////////////////////////////////////
//
// The LedgerTxn subsystem consists of a number of classes (made a bit
// more numerous through the use of inner ::Impl "compiler firewall"
// classes and abstract base classes), of which the essential members and
// relationships are diagrammed here.
//
//
//  +-----------------------------------+
//  |LedgerTxnRoot                      |
//  |(will commit child entries to DB)  |
//  |                                   |
//  |Database &mDatabase                |
//  |AbstractLedgerTxn *mChild -------------+
//  +-----------------------------------+   |
//      ^                                   v
//      |   +-----------------------------------+
//      |   |LedgerTxn                          |
//      |   |(will commit child entries to self)|
//      |   |                                   |
//      +----AbstractLedgerTxnParent &mParent   |
//          |AbstracLedgerTxn *mChild ------------+
//          +-----------------------------------+ |
//                ^                               v
//                |    +-----------------------------------------------------+
//                |    |LedgerTxn : AbstractLedgerTxn                        |
//                |    |(an in-memory transaction-in-progress)               |
//                |    |                                                     |
//                |    |          void commit()                              |
//                |    |          void rollback()                            |
//                |    |LedgerTxnEntry create(InternalLedgerEntry)           |
//                |    |LedgerTxnEntry load(InternalLedgerKey)               |
//                |    |          void erase(InternalLedgerKey)              |
//                |    |                                                     |
//                |    |+---------------------------------------------------+|
//                |    ||LedgerTxn::Impl                                    ||
//                |    ||                                                   ||
//                +------AbstractLedgerTxnParent &mParent                   ||
//                     ||AbstractLedgerTxn *mChild = nullptr                ||
//                     ||                                                   ||
//  +----------------+ ||+------------------------------+                   ||
//  |LedgerTxnEntry  | |||mActive                       |                   ||
//  |(for client use)| |||                              |                   ||
//  |                | |||map<InternalLedgerKey,        |                   ||
//  |weak_ptr<Impl>  | |||    shared_ptr<EntryImplBase>>|                   ||
//  +----------------+ ||+------------------------------+                   ||
//           |         ||+----------------------------+                     ||
//                     |||mEntry                      |                     ||
//           |         |||                            |                     ||
//                     |||map<InternalLedgerKey,      |                     ||
//           |         |||    InternalLedgerEntry>    |                     ||
//                     ||+---------------------------+|                     ||
//           |         |+---------------------------------------------------+|
//                     +-----------------------------------------------------+
//           |                                          ^
//                       +-------------------------+    |
//           |           |+-------------------------+   |
//                       ||+-------------------------+  |
//           |           |||LedgerTxnEntry::Impl     |  |
//         weak - - - - >|||(indicates "entry is     |  |
//                       |||active in this state")   |  |
//                       |||                         |  |
//                       +||AbstractLedgerTxn &  -------+
//                        +|InternalLedgerEntry &    |
//                         +-------------------------+
//
//
// The following notes may help with orientation and understanding:
//
//  - A LedgerTxn is an in-memory transaction-in-progress against the
//    ledger in the database. Its ultimate purpose is to model a collection
//    of InternalLedgerEntry, which are wrappers around LedgerEntry (XDR)
//    objects, to commit to the database.
//
//  - At any given time, a LedgerTxn may have zero-or-one active
//    sub-transactions, arranged in a parent/child relationship. The terms
//    "parent" and "child" refer exclusively to this nesting-relationship
//    of transactions. The presence of an active sub-LedgerTxn is indicated
//    by a non-null mChild pointer.
//
//  - Once a child is closed and the mChild pointer is reset to null,
//    a new child may be opened. Attempting to open two children at once
//    will throw an exception.
//
//  - The entries to be committed in each transaction are stored in the
//    mEntry map, keyed by InternalLedgerKey. This much is straightforward!
//
//  - Committing any LedgerTxn merges its entries into its parent. In the
//    case where the parent is simply another in-memory LedgerTxn, this
//    means writing the entries into the parent's mEntries map. In the case
//    where the parent is the LedgerTxnRoot, this means opening a Real SQL
//    Transaction against the database and writing the entries to it.
//
//  - Each entry may also be designated as _active_ in a given LedgerTxn;
//    tracking active-ness is the purpose of the other (mActive) map in
//    the diagram above. Active-ness is a logical state that simply means
//    "it is ok, from a concurrency-control perspective, for a client to
//    access this entry in this LedgerTxn." See below for the
//    concurrency-control issues this is designed to trap.
//
//  - Entries are made-active by calling load() or create(), each of which
//    returns a LedgerTxnEntry which is a handle that can be used to get at
//    the underlying LedgerEntry. References to the underlying
//    LedgerEntries should generally not be retained anywhere, because the
//    LedgerTxnEntry handles may be "deactivated", and access to a
//    deactivated entry is a _logic error_ in the client that this
//    machinery is set up to try to trap. If you hold a reference to the
//    underlying entry, you're bypassing the checking machinery that is
//    here to catch such errors. Don't do it.
//
//  - load()ing an entry will either check the current LedgerTxn for an
//    entry, or if none is found it will ask its parent. This process
//    recurses until it hits an entry or terminates at the root, where an
//    LRU cache is consulted and then (finally!) the database itself.
//
//  - The LedgerTxnEntry handles that clients should use are
//    double-indirect references.
//
//      - The first level of indirection is a LedgerTxnEntry::Impl, which
//        is an internal 2-word binding stored in the mActive map that
//        serves simply track the fact that an entry _is_ active, and to
//        facilitate deactivating the entry.
//
//      - The second level of indirection is the client-facing type
//        LedgerTxnEntry, which is _weakly_ linked to its ::Impl type (via
//        std::weak_ptr). This weak linkage enables the LedgerTxn to
//        deactivate entries without worrying that some handle might remain
//        able to access them (assuming they did not hold references to the
//        inner LedgerEntries).
//
//  - The purpose of the double-indirection is to maintain one critical
//    invariant in the system: clients can _only access_ the entries in the
//    innermost (child-most) LedgerTxn open at any given time. This is
//    enforced by deactivating all the entries in a parent LedgerTxn when a
//    child is opened. The entries in the parent still exist in its mEntry
//    map (and will be committed to the parent's parent when the parent
//    commits); but they are not _active_, meaning that attempts to access
//    them through any LedgerTxnEntry handles will throw an exception.
//
//  - The _reason_ for this invariant is to prevent concurrency anomalies:
//
//      - Stale reads: a client could open a sub-transaction, write some
//        entries into it, and then accidentally read from the parent and
//        thereby observe stale data.
//
//      - Lost updates: a client could open a sub-transaction, write some
//        entries to it, and then accidentally write more updates to those
//        same entries to the parent, which would be overwritten by the
//        child when it commits.
//
//    Both these anomalies are harder to cause if the interface refuses all
//    accesses to a parent's entries when a child is open.
//

namespace stellar
{

class InMemorySorobanState;

/* LedgerEntryPtr holds a shared_ptr to a InternalLedgerEntry along with
  information about the state of the entry (or lack thereof)

  EntryPtrState definitions
  1. INIT - InternalLedgerEntry was created at this level
  2. LIVE - InternalLedgerEntry was modified at this level
  3. DELETED - InternalLedgerEntry was deleted at this level
*/
enum class EntryPtrState
{
    INIT,
    LIVE,
    DELETED
};

class LedgerEntryPtr
{
  public:
    static LedgerEntryPtr
    Init(std::shared_ptr<InternalLedgerEntry> const& lePtr);
    static LedgerEntryPtr
    Live(std::shared_ptr<InternalLedgerEntry> const& lePtr);
    static LedgerEntryPtr Delete();

    // These methods have the strong exception safety guarantee
    InternalLedgerEntry& operator*() const;
    InternalLedgerEntry* operator->() const;
    void mergeFrom(LedgerEntryPtr const& entryPtr);

    // These methods do not throw
    std::shared_ptr<InternalLedgerEntry> get() const;
    EntryPtrState getState() const;
    bool isInit() const;
    bool isLive() const;
    bool isDeleted() const;

  private:
    LedgerEntryPtr(std::shared_ptr<InternalLedgerEntry> const& lePtr,
                   EntryPtrState state);

    std::shared_ptr<InternalLedgerEntry> mEntryPtr;
    EntryPtrState mState;
};

// A heuristic number that is used to batch together groups of
// LedgerEntries for bulk commit at the database interface layer. For sake
// of mechanical sympathy with said batching, one should attempt to group
// incoming work (if it is otherwise unbounded) into transactions of the
// same number of entries. It does no semantic harm to pick a different
// size, just fail to batch quite as evenly.
static const size_t LEDGER_ENTRY_BATCH_COMMIT_SIZE = 0xfff;

// If a LedgerTxn has had an eraseWithoutLoading call, the usual "exact"
// level of consistency that a LedgerTxn maintains with the database will
// be very slightly weakened: one or more "erase" events may be in
// memory that would normally (in the "loading" case) have been annihilated
// on contact with an in-memory insert.
//
// This "extra deletes" inconsistency is mostly harmless, it only has two
// effects:
//
//    - LedgerTxnDeltas, LedgerChanges and DeadEntries should not be
//      calculated from a LedgerTxn in this state (since it will report
//      extra deletes for keys that don't exist in the database, were
//      added-then-deleted in the current txn). LiveEntries can be
//      calculated from a LedgerTxn with EXTRA_DELETES, however: the
//      live entries that should have been annihilated will be judged
//      dead, and the same set of live entries will be returned as would
//      be in the loading case.
//
//    - The count of rows in the database effected when applying the
//      "erase" events might not be the expected number, so the consistency
//      check we do there should be relaxed.
//
// Neither issue happens when a createOrUpdateWithoutLoading call occurs,
// as there's no assumption that a pending _delete_ will be annihilated
// in-memory by a create: delete-then-create is stored the same way as
// create, which is stored the same way as update. Further, when writing to
// the database, the row count is the same whether a row is inserted or
// updated.
enum class LedgerTxnConsistency
{
    EXACT,
    EXTRA_DELETES
};

// NOTE: Remove READ_ONLY_WITHOUT_SQL_TXN mode when BucketListDB is required
// and we stop supporting SQL backend for ledger state.
enum class TransactionMode
{
    READ_ONLY_WITHOUT_SQL_TXN,
    READ_WRITE_WITH_SQL_TXN
};

class Application;
class Database;
struct InflationVotes;
struct LedgerEntry;
struct LedgerKey;
struct LedgerRange;
class SessionWrapper;

struct OfferDescriptor
{
    Price price;
    int64_t offerID;
};
bool operator==(OfferDescriptor const& lhs, OfferDescriptor const& rhs);

bool isBetterOffer(LedgerEntry const& lhsEntry, LedgerEntry const& rhsEntry);
bool isBetterOffer(OfferDescriptor const& lhs, LedgerEntry const& rhsEntry);
bool isBetterOffer(OfferDescriptor const& lhs, OfferDescriptor const& rhs);

struct IsBetterOfferComparator
{
    bool operator()(OfferDescriptor const& lhs,
                    OfferDescriptor const& rhs) const;
};

struct AssetPair
{
    Asset buying;
    Asset selling;
};
bool operator==(AssetPair const& lhs, AssetPair const& rhs);

struct AssetPairHash
{
    size_t operator()(AssetPair const& key) const;
};

struct InflationWinner
{
    AccountID accountID;
    int64_t votes;
};

// Tracks the set of both TTL keys and corresponding code/data keys that have
// been restored. Maps LedgerKey -> LedgerEntry at the point of restoration. For
// contract code/data, this is the original, restored value. For TTL entries,
// this is the value after applying the minimum rent required to restore.
struct RestoredEntries
{
    // Restoration can take two forms. In the first form, the key
    // had been evicted to the hotArchive BL and restoration involved
    // doing IO to bring it back into memory.
    UnorderedMap<LedgerKey, LedgerEntry> hotArchive;
    // In the second form, the key was in the live BL but its TTL was
    // past so it was considered expired, just not evicted. Restoring
    // this does not cost any IO, just writing a new TTL.
    UnorderedMap<LedgerKey, LedgerEntry> liveBucketList;

    std::optional<LedgerEntry> getEntryOpt(LedgerKey const& key) const;

    bool entryWasRestored(LedgerKey const& k) const;

    static bool
    entryWasRestoredFromMap(LedgerKey const& k,
                            UnorderedMap<LedgerKey, LedgerEntry> const& map);
    static void addRestoreToMap(LedgerKey const& key, LedgerEntry const& entry,
                                LedgerKey const& ttlKey,
                                LedgerEntry const& ttlEntry,
                                UnorderedMap<LedgerKey, LedgerEntry>& map);

    void addHotArchiveRestore(LedgerKey const& key, LedgerEntry const& entry,
                              LedgerKey const& ttlKey,
                              LedgerEntry const& ttlEntry);
    void addLiveBucketlistRestore(LedgerKey const& key,
                                  LedgerEntry const& entry,
                                  LedgerKey const& ttlKey,
                                  LedgerEntry const& ttlEntry);
    void addRestoresFrom(RestoredEntries const& other,
                         bool allowDuplicates = false);
};

class AbstractLedgerTxn;

// LedgerTxnDelta represents the difference between a LedgerTxn and its
// parent. Used in the Invariants subsystem.
struct LedgerTxnDelta
{
    struct EntryDelta
    {
        std::shared_ptr<InternalLedgerEntry const> current;
        std::shared_ptr<InternalLedgerEntry const> previous;
    };

    struct HeaderDelta
    {
        LedgerHeader current;
        LedgerHeader previous;
    };

    UnorderedMap<InternalLedgerKey, EntryDelta> entry;
    HeaderDelta header;
};

// An abstraction for an object that is iterator-like and permits enumerating
// the LedgerTxnEntry objects managed by an AbstractLedgerTxn. This enables
// an AbstractLedgerTxnParent to iterate over the entries managed by its child
// without any knowledge of the implementation of the child.
class EntryIterator
{
  public:
    class AbstractImpl;

  private:
    std::unique_ptr<AbstractImpl> mImpl;

    std::unique_ptr<AbstractImpl> const& getImpl() const;

  public:
    EntryIterator(std::unique_ptr<AbstractImpl>&& impl);

    EntryIterator(EntryIterator const& other);

    EntryIterator(EntryIterator&& other);

    EntryIterator& operator++();

    explicit operator bool() const;

    InternalLedgerEntry const& entry() const;
    LedgerEntryPtr const& entryPtr() const;

    bool entryExists() const;

    InternalLedgerKey const& key() const;
};

void validateTrustLineKey(uint32_t ledgerVersion, LedgerKey const& key);

// An abstraction for an object that can be the parent of an AbstractLedgerTxn
// (discussed below). Allows children to commit atomically to the parent. Has no
// notion of a LedgerTxnEntry or LedgerTxnHeader (discussed respectively in
// LedgerTxnEntry.h and LedgerTxnHeader.h) but allows access to XDR objects
// such as LedgerEntry and LedgerHeader. This interface is designed such that
// concrete implementations can be databases or AbstractLedgerTxn objects. In
// general, this interface was not designed to be used directly by end users.
// Rather, end users should interact with AbstractLedgerTxnParent through the
// AbstractLedgerTxn interface.
class AbstractLedgerTxnParent
{
  public:
    virtual ~AbstractLedgerTxnParent();

    // addChild is called by a newly constructed AbstractLedgerTxn to become a
    // child of AbstractLedgerTxnParent. Throws if AbstractLedgerTxnParent
    // is in the sealed state or already has a child.
    virtual void addChild(AbstractLedgerTxn& child, TransactionMode mode) = 0;

    // commitChild and rollbackChild are called by a child AbstractLedgerTxn
    // to trigger an atomic commit or an atomic rollback of the data stored in
    // the child.
    virtual void commitChild(EntryIterator iter,
                             RestoredEntries const& restoredEntries,
                             LedgerTxnConsistency cons) noexcept = 0;
    virtual void rollbackChild() noexcept = 0;

    // getAllOffers, getBestOffer, and getOffersByAccountAndAsset are used to
    // handle some specific queries related to Offers.
    // - getAllOffers
    //     Get XDR for every offer, grouped by account.
    // - getBestOffer
    //     Get XDR for the best offer with specified buying and selling assets.
    // - getOffersByAccountAndAsset
    //     Get XDR for every offer owned by the specified account that is either
    //     buying or selling the specified asset.
    virtual UnorderedMap<LedgerKey, LedgerEntry> getAllOffers() = 0;
    virtual std::shared_ptr<LedgerEntry const>
    getBestOffer(Asset const& buying, Asset const& selling) = 0;
    virtual std::shared_ptr<LedgerEntry const>
    getBestOffer(Asset const& buying, Asset const& selling,
                 OfferDescriptor const& worseThan) = 0;
    virtual UnorderedMap<LedgerKey, LedgerEntry>
    getOffersByAccountAndAsset(AccountID const& account,
                               Asset const& asset) = 0;

    // Get XDR for every pool share trust line owned by the specified account
    // that contains the specified asset.
    virtual UnorderedMap<LedgerKey, LedgerEntry>
    getPoolShareTrustLinesByAccountAndAsset(AccountID const& account,
                                            Asset const& asset) = 0;

    // getHeader returns the LedgerHeader stored by AbstractLedgerTxnParent.
    // Used to allow the LedgerHeader to propagate to a child.
    virtual LedgerHeader const& getHeader() const = 0;

    // getInflationWinners is used to handle the specific queries related to
    // inflation. Returns a maximum of maxWinners winners, each of which has a
    // minimum of minBalance votes.
    virtual std::vector<InflationWinner>
    getInflationWinners(size_t maxWinners, int64_t minBalance) = 0;

    // getNewestVersion finds the newest version of the InternalLedgerEntry
    // associated with the InternalLedgerKey key by checking if there is a
    // version stored in this AbstractLedgerTxnParent, and if not recursively
    // invoking getNewestVersion on its parent. Returns nullptr if the key does
    // not exist or if the corresponding LedgerEntry has been erased.
    virtual std::shared_ptr<InternalLedgerEntry const>
    getNewestVersion(InternalLedgerKey const& key) const = 0;

    // getNewestVersionBelowRoot finds the newest version of the
    // InternalLedgerEntry associated with the InternalLedgerKey key by
    // checking if there is a version stored in this AbstractLedgerTxnParent.
    // The difference with getNewestVersion is that this function does not do
    // any lookups in the root, and instead returns nullptr. This is used to
    // determine which entries need to be loaded from LedgerTxn instead of
    // the liveSnapshot during ledger apply.
    virtual std::pair<bool, std::shared_ptr<InternalLedgerEntry const> const>
    getNewestVersionBelowRoot(InternalLedgerKey const& key) const = 0;

    // Return the count of the number of offer objects within
    // range of ledgers `ledgers`. Will throw when called on anything other than
    // a (real or stub) root LedgerTxn.
    virtual uint64_t countOffers(LedgerRange const& ledgers) const = 0;

    // Delete all ledger entries modified on-or-after `ledger`. Will throw
    // when called on anything other than a (real or stub) root LedgerTxn.
    virtual void deleteOffersModifiedOnOrAfterLedger(uint32_t ledger) const = 0;

    // Delete all offer ledger entries. Will throw when called on anything other
    // than a (real or stub) root LedgerTxn.
    virtual void dropOffers() = 0;

    // Return the current cache hit rate for prefetched ledger entries, as a
    // fraction from 0.0 to 1.0. Will throw when called on anything other than a
    // (real or stub) root LedgerTxn.
    virtual double getPrefetchHitRate() const = 0;

    // Prefetch a set of ledger entries into memory, anticipating their use.
    // This is purely advisory and can be a no-op, or do any level of actual
    // work, while still being correct. Will throw when called on anything other
    // than a (real or stub) root LedgerTxn. Throws if any key is a Soroban key,
    // as these are stored in-memory and should not be loaded from disk.
    virtual uint32_t prefetch(UnorderedSet<LedgerKey> const& keys) = 0;

    // prepares to increase the capacity of pending changes by up to "s" changes
    virtual void prepareNewObjects(size_t s) = 0;

    virtual SessionWrapper& getSession() const = 0;

    // Returns map of TTL and corresponding contract/data keys that have been
    // restored from the Hot Archive/Live Bucket List. Note that this returns
    // all keys that have been restored this ledger, including those that have
    // been restored via earlier LedgerTxns commited to the same parent.
    virtual UnorderedMap<LedgerKey, LedgerEntry>
    getRestoredHotArchiveKeys() const = 0;
    virtual UnorderedMap<LedgerKey, LedgerEntry>
    getRestoredLiveBucketListKeys() const = 0;

#ifdef BUILD_TESTS
    virtual void resetForFuzzer() = 0;
#endif // BUILD_TESTS

#ifdef BEST_OFFER_DEBUGGING
    virtual bool bestOfferDebuggingEnabled() const = 0;

    virtual std::shared_ptr<LedgerEntry const>
    getBestOfferSlow(Asset const& buying, Asset const& selling,
                     OfferDescriptor const* worseThan,
                     std::unordered_set<int64_t>& exclude) = 0;
#endif
};

// An abstraction for an object that is an AbstractLedgerTxnParent and has
// transaction semantics. AbstractLedgerTxns manage LedgerTxnEntry and
// LedgerTxnHeader objects to allow data to be created, modified, and erased.
class AbstractLedgerTxn : public AbstractLedgerTxnParent
{
    // deactivate is used to deactivate the LedgerTxnEntry associated with the
    // given key.
    friend class LedgerTxnEntry::Impl;
    friend class ConstLedgerTxnEntry::Impl;
    virtual void deactivate(InternalLedgerKey const& key) = 0;

    // deactivateHeader is used to deactivate the LedgerTxnHeader.
    friend class LedgerTxnHeader::Impl;
    virtual void deactivateHeader() = 0;

  public:
    // Automatically rollback the data stored in the AbstractLedgerTxn if it
    // has not already been committed or rolled back.
    virtual ~AbstractLedgerTxn();

    // commit and rollback trigger an atomic commit into the parent or an atomic
    // rollback of the data stored in the AbstractLedgerTxn.
    virtual void commit() noexcept = 0;
    virtual void rollback() noexcept = 0;

    // loadHeader, create, erase, load, loadWithoutRecord,
    // and restoreFromLiveBucketList provide the main
    // interface to interact with data stored in the AbstractLedgerTxn. These
    // functions only allow one instance of a particular data to be active at a
    // time.
    // - loadHeader
    //     Loads the current LedgerHeader. Throws if there is already an active
    //     LedgerTxnHeader.
    // - create
    //     Creates a new LedgerTxnEntry from entry. Throws if the key
    //     associated with this entry is already associated with an entry in
    //     this AbstractLedgerTxn or any parent.
    // - erase
    //     Erases the existing entry associated with key. Throws if the key is
    //     not already associated with an entry in this AbstractLedgerTxn or
    //     any parent. Throws if there is an active LedgerTxnEntry associated
    //     with this key.
    // - load:
    //     Loads an entry by key. Returns nullptr if the key is not associated
    //     with an entry in this AbstractLedgerTxn or in any parent. Throws if
    //     there is an active LedgerTxnEntry associated with this key.
    // - loadWithoutRecord:
    //     Similar to load, but the load is not recorded (meaning that it does
    //     not lead to a LIVE entry in the bucket list) and the loaded data is
    //     const as a consequence. Note that if the key was already recorded
    //     then it will still be recorded after calling loadWithoutRecord.
    //     Throws if there is an active LedgerTxnEntry associated with this
    //     key.
    // - restoreFromLiveBucketlist:
    //     Indicates that an entry in the live BucketList is being restored and
    //     updates the TTL entry accordingly. TTL key must exist, throws
    //     otherwise. Returns the TTL entry that was modified.
    // - markRestoredFromHotArchive:
    //     Indicates that an entry in the hot archive BucketList is being
    //     restored. Used by the parallel apply path to signal to LedgerTxn
    //     that the entry and TTL should be treated as if they have been
    //     restored. This just adds the information to the map tracking entries
    //     restored from the hot archive. The actual restoration of the entry is
    //     handled separately.
    // All of these functions throw if the AbstractLedgerTxn is sealed or if
    // the AbstractLedgerTxn has a child.
    virtual LedgerTxnHeader loadHeader() = 0;
    virtual LedgerTxnEntry create(InternalLedgerEntry const& entry) = 0;
    virtual void erase(InternalLedgerKey const& key) = 0;
    virtual LedgerTxnEntry restoreFromLiveBucketList(LedgerEntry const& entry,
                                                     uint32_t ttl) = 0;
    virtual void markRestoredFromHotArchive(LedgerEntry const& ledgerEntry,
                                            LedgerEntry const& ttlEntry) = 0;
    virtual LedgerTxnEntry load(InternalLedgerKey const& key) = 0;
    virtual ConstLedgerTxnEntry
    loadWithoutRecord(InternalLedgerKey const& key) = 0;

    // Somewhat unsafe, non-recommended access methods: for use only during
    // bulk-loading as in catchup from buckets. These methods set an entry
    // to a new live (or dead) value in the transaction _without consulting
    // with the database_ about the current state of it.
    //
    // REITERATED WARNING: do _not_ call these methods from normal online
    // transaction processing code, or any code that is sensitive to the
    // state of the database. These are only here for clobbering it with
    // new data.

    virtual void createWithoutLoading(InternalLedgerEntry const& entry) = 0;
    virtual void updateWithoutLoading(InternalLedgerEntry const& entry) = 0;
    virtual void eraseWithoutLoading(InternalLedgerKey const& key) = 0;

    // getChanges, getDelta, and getAllEntries are used to
    // extract information about changes contained in the AbstractLedgerTxn
    // in different formats. These functions also cause the AbstractLedgerTxn
    // to enter the sealed state, simultaneously updating last modified if
    // necessary.
    // - getChanges
    //     Extract all changes of the given type from this AbstractLedgerTxn in
    //     XDR format. To be stored as meta.
    // - getDelta
    //     Extract all changes from this AbstractLedgerTxn (including changes
    //     to the LedgerHeader) in a format convenient for answering queries
    //     about how specific entries and the header have changed. To be used
    //     for invariants.
    // - getAllEntries
    //     extracts a list of keys that were created (init), updated (live) or
    //     deleted (dead) in this AbstractLedgerTxn. All these are to be
    //     inserted into the BucketList.
    //
    // All of these functions throw if the AbstractLedgerTxn has a child.
    virtual LedgerEntryChanges getChanges() = 0;
    virtual LedgerTxnDelta getDelta() = 0;
    virtual void getAllEntries(std::vector<LedgerEntry>& initEntries,
                               std::vector<LedgerEntry>& liveEntries,
                               std::vector<LedgerKey>& deadEntries) = 0;

    // Returns all TTL keys that have been modified (create, update, and
    // delete), but does not cause the AbstractLedgerTxn or update last
    // modified.
    virtual LedgerKeySet getAllTTLKeysWithoutSealing() const = 0;

    // forAllWorstBestOffers allows a parent AbstractLedgerTxn to process the
    // worst best offers (an offer is a worst best offer if every better offer
    // in any parent AbstractLedgerTxn has already been loaded). This function
    // is intended for use with commit.
    using WorstOfferProcessor =
        std::function<void(Asset const& buying, Asset const& selling,
                           std::shared_ptr<OfferDescriptor const>& desc)>;
    virtual void forAllWorstBestOffers(WorstOfferProcessor proc) = 0;

    // loadAllOffers, loadBestOffer, and loadOffersByAccountAndAsset are used to
    // handle some specific queries related to Offers. These functions are built
    // on top of load, and so share many properties with that function.
    // - loadAllOffers
    //     Load every offer, grouped by account.
    // - loadBestOffer
    //     Load the best offer with specified buying and selling assets.
    // - loadOffersByAccountAndAsset
    //     Load every offer owned by the specified account that is either buying
    //     or selling the specified asset.
    // All of these functions throw if the AbstractLedgerTxn is sealed or if
    // the AbstractLedgerTxn has a child. These functions also throw if any
    // LedgerKey they try to load is already active.
    virtual std::map<AccountID, std::vector<LedgerTxnEntry>>
    loadAllOffers() = 0;
    virtual LedgerTxnEntry loadBestOffer(Asset const& buying,
                                         Asset const& selling) = 0;
    virtual std::vector<LedgerTxnEntry>
    loadOffersByAccountAndAsset(AccountID const& accountID,
                                Asset const& asset) = 0;

    // Loads every pool share trust line owned by the specified account that
    // contains the specified asset. This function is built on top of load, so
    // it shares many properties with that function.
    virtual std::vector<LedgerTxnEntry>
    loadPoolShareTrustLinesByAccountAndAsset(AccountID const& account,
                                             Asset const& asset) = 0;

    // queryInflationWinners is a wrapper around getInflationWinners that throws
    // if the AbstractLedgerTxn is sealed or if the AbstractLedgerTxn has a
    // child.
    virtual std::vector<InflationWinner>
    queryInflationWinners(size_t maxWinners, int64_t minBalance) = 0;

    // unsealHeader is used to modify the LedgerHeader after AbstractLedgerTxn
    // has entered the sealed state. This is required to update bucketListHash,
    // which can only be done after getDeadEntries and getLiveEntries have been
    // called.
    virtual void unsealHeader(std::function<void(LedgerHeader&)> f) = 0;

    // returns true if mEntry has any record of a SPONSORSHIP or
    // SPONSORSHIP_COUNTER entry type. Throws if the AbstractLedgerTxn has a
    // child.
    virtual bool hasSponsorshipEntry() const = 0;
};

class LedgerTxn : public AbstractLedgerTxn
{
    class Impl;
    friend class Impl;
    std::unique_ptr<Impl> mImpl;

    void deactivate(InternalLedgerKey const& key) override;

    void deactivateHeader() override;

    std::unique_ptr<Impl> const& getImpl() const;

  protected:
    LedgerHeader const& getHeader() const override;

  public:
    // WARNING: use useTransaction flag with caution. It does not start a SQL
    // transaction, which uses the strongest SERIALIZABLE level isolation.
    // Therefore, if you have concurrent transactions, you are risking getting
    // inconsistent view of the database. Only use this mode for read-only
    // transactions with no concurrent writers present.
    explicit LedgerTxn(
        AbstractLedgerTxnParent& parent, bool shouldUpdateLastModified = true,
        TransactionMode mode = TransactionMode::READ_WRITE_WITH_SQL_TXN);
    explicit LedgerTxn(
        LedgerTxn& parent, bool shouldUpdateLastModified = true,
        TransactionMode mode = TransactionMode::READ_WRITE_WITH_SQL_TXN);

    virtual ~LedgerTxn();

    void addChild(AbstractLedgerTxn& child, TransactionMode mode) override;

    void commit() noexcept override;

    void commitChild(EntryIterator iter, RestoredEntries const& restoredEntries,
                     LedgerTxnConsistency cons) noexcept override;

    LedgerTxnEntry create(InternalLedgerEntry const& entry) override;

    void erase(InternalLedgerKey const& key) override;

    LedgerTxnEntry restoreFromLiveBucketList(LedgerEntry const& entry,
                                             uint32_t ttl) override;
    void markRestoredFromHotArchive(LedgerEntry const& ledgerEntry,
                                    LedgerEntry const& ttlEntry) override;

    UnorderedMap<LedgerKey, LedgerEntry> getAllOffers() override;

    std::shared_ptr<LedgerEntry const>
    getBestOffer(Asset const& buying, Asset const& selling) override;
    std::shared_ptr<LedgerEntry const>
    getBestOffer(Asset const& buying, Asset const& selling,
                 OfferDescriptor const& worseThan) override;

    void forAllWorstBestOffers(WorstOfferProcessor proc) override;

    LedgerEntryChanges getChanges() override;

    LedgerTxnDelta getDelta() override;

    UnorderedMap<LedgerKey, LedgerEntry>
    getOffersByAccountAndAsset(AccountID const& account,
                               Asset const& asset) override;

    UnorderedMap<LedgerKey, LedgerEntry>
    getPoolShareTrustLinesByAccountAndAsset(AccountID const& account,
                                            Asset const& asset) override;

    std::vector<InflationWinner>
    getInflationWinners(size_t maxWinners, int64_t minBalance) override;

    std::vector<InflationWinner>
    queryInflationWinners(size_t maxWinners, int64_t minBalance) override;

    void getAllEntries(std::vector<LedgerEntry>& initEntries,
                       std::vector<LedgerEntry>& liveEntries,
                       std::vector<LedgerKey>& deadEntries) override;
    LedgerKeySet getAllTTLKeysWithoutSealing() const override;

    UnorderedMap<LedgerKey, LedgerEntry>
    getRestoredHotArchiveKeys() const override;
    UnorderedMap<LedgerKey, LedgerEntry>
    getRestoredLiveBucketListKeys() const override;

    std::shared_ptr<InternalLedgerEntry const>
    getNewestVersion(InternalLedgerKey const& key) const override;

    std::pair<bool, std::shared_ptr<InternalLedgerEntry const> const>
    getNewestVersionBelowRoot(InternalLedgerKey const& key) const override;

    LedgerTxnEntry load(InternalLedgerKey const& key) override;

    void createWithoutLoading(InternalLedgerEntry const& entry) override;
    void updateWithoutLoading(InternalLedgerEntry const& entry) override;
    void eraseWithoutLoading(InternalLedgerKey const& key) override;

    std::map<AccountID, std::vector<LedgerTxnEntry>> loadAllOffers() override;

    LedgerTxnEntry loadBestOffer(Asset const& buying,
                                 Asset const& selling) override;

    LedgerTxnHeader loadHeader() override;

    std::vector<LedgerTxnEntry>
    loadOffersByAccountAndAsset(AccountID const& accountID,
                                Asset const& asset) override;

    std::vector<LedgerTxnEntry>
    loadPoolShareTrustLinesByAccountAndAsset(AccountID const& account,
                                             Asset const& asset) override;

    ConstLedgerTxnEntry
    loadWithoutRecord(InternalLedgerKey const& key) override;

    void rollback() noexcept override;

    void rollbackChild() noexcept override;

    void unsealHeader(std::function<void(LedgerHeader&)> f) override;

    uint64_t countOffers(LedgerRange const& ledgers) const override;
    void deleteOffersModifiedOnOrAfterLedger(uint32_t ledger) const override;
    void dropOffers() override;

    double getPrefetchHitRate() const override;
    uint32_t prefetch(UnorderedSet<LedgerKey> const& keys) override;
    void prepareNewObjects(size_t s) override;
    SessionWrapper& getSession() const override;

    bool hasSponsorshipEntry() const override;

#ifdef BUILD_TESTS
    UnorderedMap<AssetPair,
                 std::map<OfferDescriptor, LedgerKey, IsBetterOfferComparator>,
                 AssetPairHash>
    getOrderBook() const;

    void resetForFuzzer() override;
    void
    deactivateHeaderTestOnly()
    {
        deactivateHeader();
    }
#endif // BUILD_TESTS

#ifdef BEST_OFFER_DEBUGGING
    bool bestOfferDebuggingEnabled() const override;

    std::shared_ptr<LedgerEntry const>
    getBestOfferSlow(Asset const& buying, Asset const& selling,
                     OfferDescriptor const* worseThan,
                     std::unordered_set<int64_t>& exclude) override;
#endif
};

class LedgerTxnRoot : public AbstractLedgerTxnParent
{
    class Impl;
    friend class Impl;
    std::unique_ptr<Impl> const mImpl;

  protected:
    LedgerHeader const& getHeader() const override;

  public:
    explicit LedgerTxnRoot(Application& app,
                           InMemorySorobanState const& inMemorySorobanState,
                           size_t entryCacheSize, size_t prefetchBatchSize
#ifdef BEST_OFFER_DEBUGGING
                           ,
                           bool bestOfferDebuggingEnabled
#endif
    );

    virtual ~LedgerTxnRoot();

    void addChild(AbstractLedgerTxn& child, TransactionMode mode) override;

    void commitChild(EntryIterator iter, RestoredEntries const& restoredEntries,
                     LedgerTxnConsistency cons) noexcept override;

    uint64_t countOffers(LedgerRange const& ledgers) const override;

    void deleteOffersModifiedOnOrAfterLedger(uint32_t ledger) const override;

    void dropOffers() override;

#ifdef BUILD_TESTS
    void resetForFuzzer() override;
#endif // BUILD_TESTS

    UnorderedMap<LedgerKey, LedgerEntry> getAllOffers() override;

    std::shared_ptr<LedgerEntry const>
    getBestOffer(Asset const& buying, Asset const& selling) override;
    std::shared_ptr<LedgerEntry const>
    getBestOffer(Asset const& buying, Asset const& selling,
                 OfferDescriptor const& worseThan) override;

    UnorderedMap<LedgerKey, LedgerEntry>
    getOffersByAccountAndAsset(AccountID const& account,
                               Asset const& asset) override;

    UnorderedMap<LedgerKey, LedgerEntry>
    getPoolShareTrustLinesByAccountAndAsset(AccountID const& account,
                                            Asset const& asset) override;

    std::vector<InflationWinner>
    getInflationWinners(size_t maxWinners, int64_t minBalance) override;

    std::shared_ptr<InternalLedgerEntry const>
    getNewestVersion(InternalLedgerKey const& key) const override;

    UnorderedMap<LedgerKey, LedgerEntry>
    getRestoredHotArchiveKeys() const override;
    UnorderedMap<LedgerKey, LedgerEntry>
    getRestoredLiveBucketListKeys() const override;

    std::pair<bool, std::shared_ptr<InternalLedgerEntry const> const>
    getNewestVersionBelowRoot(InternalLedgerKey const& key) const override;

    void rollbackChild() noexcept override;

    uint32_t prefetch(UnorderedSet<LedgerKey> const& keys) override;

    double getPrefetchHitRate() const override;
    void prepareNewObjects(size_t s) override;

#ifdef BEST_OFFER_DEBUGGING
    bool bestOfferDebuggingEnabled() const override;

    std::shared_ptr<LedgerEntry const>
    getBestOfferSlow(Asset const& buying, Asset const& selling,
                     OfferDescriptor const* worseThan,
                     std::unordered_set<int64_t>& exclude) override;
#endif
    SessionWrapper& getSession() const override;
};
}
