#pragma once

// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

namespace stellar
{
struct TransactionMeta;
// This is used in two places 1. printing 2. checking meta sameness. i.e.
// sorting before hashing and comparison. We should only sort the fields that
// are order agonostic i.e. LedgerEntryChanges, not the events.
void normalizeMeta(TransactionMeta& m);

struct LedgerCloseMeta;

void normalizeMeta(LedgerCloseMeta& lcm);
}