---
title: Integration with other services
---

stellar-core is at the bottom of the Stellar stack, many other services can be
 built on top of it.

 Example of such services include Horizon, a service to assist with
 submitting transactions to the network as well as exposing a data model to
 query ledger history.

 In the rest of this document we'll use Horizon to better illustrate data flows.

# Integration points: using data generated by stellar-core

stellar-core generates several types of data that can be used by applications, depending on use cases.

## Ledger State

Full [Ledger](ledger.md) snapshots are available via both:
  * [history archives](history.md) (checkpoints, every 64 ledgers, updated every 5 minutes)
* a stellar-core instance, where the ledger is maintained within the stellar-core process and ledger-state need to be tracked as it changes via "meta" updates.

## Ledger State transition information (transactions, etc)

When ledgers close, we have what we call a ledger state transition from a (previous) ledger to a new ledger.

As part of this transition, stellar-core records:
* the ledger header for the new ledger
* the transactions that got applied as part of that transition
* the corresponding transaction results
* SCP messages recorded by the validator during the consensus round
* meta-data describing fine grain changes of ledger entries (creation, updates, deletes)
  * NB: the meta-data presents changes to ledger entries in a simple to process format (old value/new value)

In captive-core mode, this entire per-ledger transition dataset is emitted by stellar-core over a (optionally named) pipe, as an xdr encoded `LedgerCloseMeta` object.

## Custom ledger view

At a high level, the idea behind a custom ledger view is to maintain a "view" of the ledger (and any other derived data)
such that applications can manage a data model tailor-made for their access patterns and use cases.

This is achieved in two steps:
1. build a view of the ledger for a given ledger number
2. apply changes to the view as ledgers close

As a consequence, a custom view has many advantages:
* it's minimalist ; ie, it only contains what the application needs
* it's efficient, data is stored the way the application needs it (indexed, in memory, etc)
* it's easy to reason about ledgers
* it's easy to mix "native Stellar" data with non native data (such as internal customer information) and keep those consistent
* for more demanding applications, custom views can be built using a pub/sub model where data from stellar-core is published and consumed by many different services

### Low resolution custom view

The simplest way to build a low resolution custom view is to use history archives.

A few reasons to use low resolution views:
* it's the quickest way to build a custom view
* it's the most scalable: archives is a very efficient way to propagate state to other services
* it gives access to the full list of transaction (`Transaction`) and results (`TransactionResults`) applied on a per ledger basis

Your application is limited to:
* 64 ledgers (5 minutes) latency
* aggregated (64 ledgers) changes to ledger entries, your application cannot see why ledger entries change, only that they changed in the last 64 ledgers (no meta)

#### Maintaining a low resolution view

In a low resolution view, the bootstrap logic (what to do when starting from an empty state) is roughly the same than the one used when processing a checkpoint as the unit of work is a bucket.

The difference is in how many buckets need to be processed for each as to take advantage of the fact that the bucket list is organized in temporal fashion.

When bootstrapping based on a checkpoint:
* all 19 buckets need to be processed (from oldest to newest)

When processing a new checkpoint, the process needs to know which buckets to process:
1. compare the ledger number from the last processing to the ledger number being processed
2. compute the oldest bucket inside the bucket list that needs to be processed
3. process each bucket from oldest to newest

Processing a bucket is fairly mechanical:
Read each `BucketEntry`, filter as appropriate, and reflect the data in your local store.

Bucket entries convey a change (but do not specify the previous value), so for deletes, it tells what got deleted, for updates it tells what the new value is.

In addition, when processing a checkpoint, the application can process other data such as transaction sets for any ledger (not just the checkpoint ledger).

#### Example: asset stats service

Services used for reporting purpose are prime candidates for low resolution views.

In this example, let's say you want to build a service that:
* maintains a custom view of all assets on the ledger with their outstanding supply
* you want to update this view every hour

In this case, the data ingested from buckets is the balance, asset contained in each `TrustLine` ledger entry (and throw away everything else).

The main process `process(bucket)` looks like this in pseudo code:
```
foreach bucketEntry in bucket:
  if bucketEntry.type() == DEADENTRY
    oldValue = db.loadTrustLine(bucketEntry.deadEntry())
    if (oldValue && oldValue.type() == TRUSTLINE)
      # Trustline got deleted: deduct its balance for this asset
      updateAsset(oldValue.trustLine().asset, -oldValue.trustLine().balance);
      db.deleteTrustLine(bucketEntry.deadEntry())
  if bucketEntry.type() == LIVEENTRY
    oldValue = db.loadTrustLine(bucketEntry.liveEntry().key())
    if (oldValue && oldValue.type() == TRUSTLINE)
      # Trustline got update: reflect changes in stats for this asset
      delta = bucketEntry.liveEntry().balance - oldValue.trustLine().balance
      updateAsset(oldValue.trustLine().asset, delta);
      db.upsertTrustLine(bucketEntry.liveEntry())
```

Running every hour means that it will need to process around 3600/5=720 worth of ledgers, which is contained within the top 10 buckets in the bucket list. 

### High resolution custom view

A high resolution custom view allows to build the most sophisticated data model and derive complex data from the Stellar network.

With such view, systems like Horizon can:
* maintain a view that cross references ledger entry changes (such as account balances) down to individual operations
* build complex aggregate views accurate to the ledger (or operation even)
* build latency sensitive APIs such as transaction submission (that need the latest account sequence number)
* process changes to ledger entries without having to know how individual operations work

Keeping a high resolution view involves two separate steps:
* building an initial state of the high resolution view
* applying changes to that state, one ledger at a time

#### Initialization of the high resolution view

When starting fresh, before being able to use ledger changes, the processor (Horizon) needs to initialize its initial state.

Easiest here is to follow the same procedure than for [low resolution custom view](#low-resolution-custom-view) and construct the application specific data set using buckets.

#### Applying changes in the high resolution custom view

A process (singleton) can import data that corresponds to [the ledger state transition](#ledger-state-transition-information-transactions-etc) and publish it to processes that need to maintain a custom view.

The data that needs to be processed as ledger closes is, in order:
* the ledger header (ie `ledgerheader`)
* transaction set "meta" that corresponds to the different phases of applying transactions
  * before and after changes related to fee processing (fees are processed before everything else)
  * for each transaction, the before and after state encoded as `TransactionMetaV2` for the different phases of transaction application
    * changes before operations get applied
    * changes for each operation
    * changes after operations get applied
    * also contains associated `TransactionResult`
* ledger upgrades "meta"
  * contains ledger entries before/after of ledger entries that potentially get modified by upgrades
  * contains network wide settings upgrade information (protocol version, base fee, etc)
* other data (SCP messages)

In addition to being a lot more detailed, unlike buckets, the meta data contains both previous and new state of ledger entries; this allows to reason locally about changes without having to actually maintain an actual copy of the ledger (if it's not needed).

For example, to calculate trading volume for certain asset, all that is needed is to track the creation, deletion or updates to balances (for trustlines and accounts), as changes occur in the context of operations, the volume can be tracked by operation type, asset, account id, etc.

NB: a simple way to initialize state is to watch for those ledger changes, and wait for a checkpoint (triggering the [bootstrap](#initialization-of-the-high-resolution-view)), which guarantees that the processor can process changes. 

#### Example 1: transaction submission subsystem

This example describes how to maintain a view to be used in the context of transaction submission, that contains:
* a near real time view of account entries (for sequence number mostly)
* a record of transactions that were processed recently, and their result

Processing here consists of keeping track for each `AccountID` of the sequence number and signers for that account (to perform additional checks).

Only that information would be extracted from buckets and meta data.

For the meta data, the processor would discard most information as well, only keeping the net changes on accounts in addition to the transaction ID (hash) and the associated result (to be returned to clients).

#### Example 2: "blame" service that watches accounts

This example describes how to build a view that allows to cross
reference *any* change made by transactions with account balances.

Such a service could be used to audit debits and credits for certain accounts on the network.

This service would reliably be able to blame any transaction for changes, not just the ones that it would understand when the code was written, this makes it future proof. In this example (and at the time of this writing), it would see a balance change that results in operations such as `payment`, `pathpayment` but also `manageoffer` and `mergeaccount`.

As this service only tracks *activity* it doesn't need to record any data stored in buckets: processing is based entirely on the content of the "meta" associated with transactions.

The logic for this would look at each `LedgerEntryChange` (in the context of a transaction processed during a specific ledger) and
1. filter it (by account ID, by type)
2. compute the balance change `delta_balance` (difference between old and new balance)
3. generate and store the XRef, for example `(Account ID, ledgerID, txID, Asset, delta_balance)`


### Additional reading

An overview of how the data flows within stellar-core is [this data flow presentation](software/core-data-flow.pdf)

# API to stellar-core

stellar-core exposes a simple HTTP based API in particular the `tx` endpoint is used to submit transactions.

See [`software/commands.md`](software/commands.md) for more information.
