// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "BallotProtocol.h"

#include "Slot.h"
#include "crypto/Hex.h"
#include "lib/json/json.h"
#include "scp/LocalNode.h"
#include "scp/QuorumSetUtils.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include "xdrpp/marshal.h"
#include <Tracy.hpp>
#include <functional>
#include <numeric>
#include <sstream>

namespace stellar
{
using namespace std::placeholders;

// max number of transitions that can occur from processing one message
static const int MAX_ADVANCE_SLOT_RECURSION = 50;

BallotProtocol::BallotProtocol(Slot& slot)
    : mSlot(slot)
    , mHeardFromQuorum(false)
    , mPhase(SCP_PHASE_PREPARE)
    , mCurrentMessageLevel(0)
    , mTimerExpCount(0)
{
}

bool
BallotProtocol::isNewerStatement(NodeID const& nodeID, SCPStatement const& st)
{
    auto oldp = mLatestEnvelopes.find(nodeID);
    bool res = false;

    if (oldp == mLatestEnvelopes.end())
    {
        res = true;
    }
    else
    {
        res = isNewerStatement(oldp->second->getStatement(), st);
    }
    return res;
}

bool
BallotProtocol::isNewerStatement(SCPStatement const& oldst,
                                 SCPStatement const& st)
{
    bool res = false;

    // total ordering described in SCP paper.
    auto t = st.pledges.type();

    // statement type (PREPARE < CONFIRM < EXTERNALIZE)
    if (oldst.pledges.type() != t)
    {
        res = (oldst.pledges.type() < t);
    }
    else
    {
        // can't have duplicate EXTERNALIZE statements
        if (t == SCPStatementType::SCP_ST_EXTERNALIZE)
        {
            res = false;
        }
        else if (t == SCPStatementType::SCP_ST_CONFIRM)
        {
            // sorted by (b, p, p', h) (p' = 0 implicitly)
            auto const& oldC = oldst.pledges.confirm();
            auto const& c = st.pledges.confirm();
            int compBallot = compareBallots(oldC.ballot, c.ballot);
            if (compBallot < 0)
            {
                res = true;
            }
            else if (compBallot == 0)
            {
                if (oldC.nPrepared == c.nPrepared)
                {
                    res = (oldC.nH < c.nH);
                }
                else
                {
                    res = (oldC.nPrepared < c.nPrepared);
                }
            }
        }
        else
        {
            // Lexicographical order between PREPARE statements:
            // (b, p, p', h)
            auto const& oldPrep = oldst.pledges.prepare();
            auto const& prep = st.pledges.prepare();

            int compBallot = compareBallots(oldPrep.ballot, prep.ballot);
            if (compBallot < 0)
            {
                res = true;
            }
            else if (compBallot == 0)
            {
                compBallot = compareBallots(oldPrep.prepared, prep.prepared);
                if (compBallot < 0)
                {
                    res = true;
                }
                else if (compBallot == 0)
                {
                    compBallot = compareBallots(oldPrep.preparedPrime,
                                                prep.preparedPrime);
                    if (compBallot < 0)
                    {
                        res = true;
                    }
                    else if (compBallot == 0)
                    {
                        res = (oldPrep.nH < prep.nH);
                    }
                }
            }
        }
    }

    return res;
}

void
BallotProtocol::recordEnvelope(SCPEnvelopeWrapperPtr env)
{
    auto const& st = env->getStatement();
    auto oldp = mLatestEnvelopes.find(st.nodeID);
    if (oldp == mLatestEnvelopes.end())
    {
        mLatestEnvelopes.insert(std::make_pair(st.nodeID, env));
    }
    else
    {
        oldp->second = env;
    }
    mSlot.recordStatement(env->getStatement());
}

SCP::EnvelopeState
BallotProtocol::processEnvelope(SCPEnvelopeWrapperPtr envelope, bool self)
{
    ZoneScoped;
    dbgAssert(envelope->getStatement().slotIndex == mSlot.getSlotIndex());

    SCPStatement const& statement = envelope->getStatement();
    NodeID const& nodeID = statement.nodeID;

    if (!isStatementSane(statement, self))
    {
        if (self)
        {
            CLOG_ERROR(SCP, "not sane statement from self, skipping   e: {}",
                       mSlot.getSCP().envToStr(envelope->getEnvelope()));
        }

        return SCP::EnvelopeState::INVALID;
    }

    if (!isNewerStatement(nodeID, statement))
    {
        if (self)
        {
            CLOG_ERROR(SCP, "stale statement from self, skipping   e: {}",
                       mSlot.getSCP().envToStr(envelope->getEnvelope()));
        }
        else
        {
            CLOG_TRACE(SCP, "stale statement, skipping  i: {}",
                       mSlot.getSlotIndex());
        }

        return SCP::EnvelopeState::INVALID;
    }

    auto validationRes = validateValues(statement);

    // If the value is not valid, we just ignore it.
    if (validationRes == SCPDriver::kInvalidValue)
    {
        if (self)
        {
            CLOG_ERROR(SCP, "invalid value from self, skipping   e: {}",
                       mSlot.getSCP().envToStr(envelope->getEnvelope()));
        }
        else
        {
            CLOG_TRACE(SCP, "invalid value  i: {}", mSlot.getSlotIndex());
        }

        return SCP::EnvelopeState::INVALID;
    }

    if (mPhase != SCP_PHASE_EXTERNALIZE)
    {
        if (validationRes == SCPDriver::kMaybeValidValue)
        {
            mSlot.setFullyValidated(false);
        }

        recordEnvelope(envelope);
        advanceSlot(statement);
        return SCP::EnvelopeState::VALID;
    }

    // note: this handles also our own messages
    // in particular our final EXTERNALIZE message
    dbgAssert(mPhase == SCP_PHASE_EXTERNALIZE);
    if (mCommit->getBallot().value == getWorkingBallot(statement).value)
    {
        recordEnvelope(envelope);
        return SCP::EnvelopeState::VALID;
    }

    if (self)
    {
        CLOG_ERROR(SCP,
                   "externalize statement with invalid value from "
                   "self, skipping e: {}",
                   mSlot.getSCP().envToStr(envelope->getEnvelope()));
    }

    return SCP::EnvelopeState::INVALID;
}

bool
BallotProtocol::isStatementSane(SCPStatement const& st, bool self)
{
    auto qSet = mSlot.getQuorumSetFromStatement(st);
    const char* errString = nullptr;
    bool res = qSet != nullptr && isQuorumSetSane(*qSet, false, errString);
    if (!res)
    {
        CLOG_DEBUG(SCP, "Invalid quorum set received : {}",
                   (errString ? errString : "<empty>"));

        return false;
    }

    switch (st.pledges.type())
    {
    case SCPStatementType::SCP_ST_PREPARE:
    {
        auto const& p = st.pledges.prepare();
        // self is allowed to have b = 0 (as long as it never gets emitted)
        bool isOK = self || p.ballot.counter > 0;

        isOK = isOK &&
               ((!p.preparedPrime || !p.prepared) ||
                (areBallotsLessAndIncompatible(*p.preparedPrime, *p.prepared)));

        isOK =
            isOK && (p.nH == 0 || (p.prepared && p.nH <= p.prepared->counter));

        // c != 0 -> c <= h <= b
        isOK = isOK && (p.nC == 0 || (p.nH != 0 && p.ballot.counter >= p.nH &&
                                      p.nH >= p.nC));

        if (!isOK)
        {
            CLOG_TRACE(SCP, "Malformed PREPARE message");
            res = false;
        }
    }
    break;
    case SCPStatementType::SCP_ST_CONFIRM:
    {
        auto const& c = st.pledges.confirm();
        // c <= h <= b
        res = c.ballot.counter > 0;
        res = res && (c.nH <= c.ballot.counter);
        res = res && (c.nCommit <= c.nH);
        if (!res)
        {
            CLOG_TRACE(SCP, "Malformed CONFIRM message");
        }
    }
    break;
    case SCPStatementType::SCP_ST_EXTERNALIZE:
    {
        auto const& e = st.pledges.externalize();

        res = e.commit.counter > 0;
        res = res && e.nH >= e.commit.counter;

        if (!res)
        {
            CLOG_TRACE(SCP, "Malformed EXTERNALIZE message");
        }
    }
    break;
    default:
        dbgAbort();
    }

    return res;
}

bool
BallotProtocol::abandonBallot(uint32 n)
{
    ZoneScoped;
    CLOG_TRACE(SCP, "BallotProtocol::abandonBallot");
    bool res = false;
    auto v = mSlot.getLatestCompositeCandidate();

    if (!v || v->getValue().empty())
    {
        if (mCurrentBallot)
        {
            v = mCurrentBallot->getWValue();
        }
    }
    if (v && !v->getValue().empty())
    {
        if (n == 0)
        {
            res = bumpState(v->getValue(), true);
        }
        else
        {
            res = bumpState(v->getValue(), n);
        }
    }
    return res;
}

bool
BallotProtocol::bumpState(Value const& value, bool force)
{
    uint32 n;
    if (!force && mCurrentBallot)
    {
        return false;
    }

    n = mCurrentBallot ? (mCurrentBallot->getBallot().counter + 1) : 1;

    return bumpState(value, n);
}

bool
BallotProtocol::bumpState(Value const& value, uint32 n)
{
    ZoneScoped;
    if (mPhase != SCP_PHASE_PREPARE && mPhase != SCP_PHASE_CONFIRM)
    {
        return false;
    }

    SCPBallot newb;

    newb.counter = n;

    if (mValueOverride)
    {
        // we use the value that we saw confirmed prepared
        // or that we at least voted to commit to
        newb.value = mValueOverride->getValue();
    }
    else
    {
        newb.value = value;
    }

    CLOG_TRACE(SCP, "BallotProtocol::bumpState i: {} v: {}",
               mSlot.getSlotIndex(), mSlot.getSCP().ballotToStr(newb));

    bool updated = updateCurrentValue(newb);

    if (updated)
    {
        emitCurrentStateStatement();
        checkHeardFromQuorum();
    }

    return updated;
}

// updates the local state based to the specified ballot
// (that could be a prepared ballot) enforcing invariants
bool
BallotProtocol::updateCurrentValue(SCPBallot const& ballot)
{
    ZoneScoped;
    if (mPhase != SCP_PHASE_PREPARE && mPhase != SCP_PHASE_CONFIRM)
    {
        return false;
    }

    bool updated = false;
    if (!mCurrentBallot)
    {
        bumpToBallot(ballot, true);
        updated = true;
    }
    else
    {
        dbgAssert(compareBallots(mCurrentBallot->getBallot(), ballot) <= 0);

        if (mCommit && !areBallotsCompatible(mCommit->getBallot(), ballot))
        {
            return false;
        }

        int comp = compareBallots(mCurrentBallot->getBallot(), ballot);
        if (comp < 0)
        {
            bumpToBallot(ballot, true);
            updated = true;
        }
        else if (comp > 0)
        {
            // this code probably changes with the final version
            // of the conciliator

            // this case may happen if the other nodes are not
            // following the protocol (and we end up with a smaller value)
            // not sure what is the best way to deal
            // with this situation
            CLOG_ERROR(SCP,
                       "BallotProtocol::updateCurrentValue attempt to bump to "
                       "a smaller value");
            // can't just bump to the value as we may already have
            // statements at counter+1
            return false;
        }
    }

    checkInvariants();

    return updated;
}

void
BallotProtocol::bumpToBallot(SCPBallot const& ballot, bool check)
{
    ZoneScoped;
    ZoneValue(static_cast<int64_t>(mSlot.getSlotIndex()));
    ZoneValue(static_cast<int64_t>(ballot.counter));

    CLOG_TRACE(SCP, "BallotProtocol::bumpToBallot i: {} b: {}",
               mSlot.getSlotIndex(), mSlot.getSCP().ballotToStr(ballot));

    // `bumpToBallot` should be never called once we committed.
    dbgAssert(mPhase != SCP_PHASE_EXTERNALIZE);

    if (check)
    {
        // We should move mCurrentBallot monotonically only
        dbgAssert(!mCurrentBallot ||
                  compareBallots(ballot, mCurrentBallot->getBallot()) >= 0);
    }

    bool gotBumped = !mCurrentBallot ||
                     (mCurrentBallot->getBallot().counter != ballot.counter);

    if (!mCurrentBallot)
    {
        mSlot.getSCPDriver().startedBallotProtocol(mSlot.getSlotIndex(),
                                                   ballot);
    }

    mCurrentBallot = makeBallot(ballot);

    // note: we have to clear some fields (and recompute them based on latest
    // messages)
    // invariant: h.value = b.value
    if (mHighBallot && !areBallotsCompatible(mCurrentBallot->getBallot(),
                                             mHighBallot->getBallot()))
    {
        mHighBallot.reset();
        // invariant: c set only when h is set
        mCommit.reset();
    }

    if (gotBumped)
    {
        mHeardFromQuorum = false;
    }
}

void
BallotProtocol::startBallotProtocolTimer()
{
    std::chrono::milliseconds timeout = mSlot.getSCPDriver().computeTimeout(
        mCurrentBallot->getBallot().counter, /*isNomination=*/false);

    std::shared_ptr<Slot> slot = mSlot.shared_from_this();
    mSlot.getSCPDriver().setupTimer(
        mSlot.getSlotIndex(), Slot::BALLOT_PROTOCOL_TIMER, timeout,
        [slot]() { slot->getBallotProtocol().ballotProtocolTimerExpired(); });
}

void
BallotProtocol::stopBallotProtocolTimer()
{
    std::shared_ptr<Slot> slot = mSlot.shared_from_this();
    mSlot.getSCPDriver().setupTimer(mSlot.getSlotIndex(),
                                    Slot::BALLOT_PROTOCOL_TIMER,
                                    std::chrono::seconds::zero(), nullptr);
}

void
BallotProtocol::ballotProtocolTimerExpired()
{
    mTimerExpCount++;
    abandonBallot(0);
}

SCPStatement
BallotProtocol::createStatement(SCPStatementType const& type)
{
    ZoneScoped;
    SCPStatement statement;

    checkInvariants();

    statement.pledges.type(type);
    switch (type)
    {
    case SCPStatementType::SCP_ST_PREPARE:
    {
        auto& p = statement.pledges.prepare();
        p.quorumSetHash = getLocalNode()->getQuorumSetHash();
        if (mCurrentBallot)
        {
            p.ballot = mCurrentBallot->getBallot();
        }
        if (mCommit)
        {
            p.nC = mCommit->getBallot().counter;
        }
        if (mPrepared)
        {
            p.prepared.activate() = mPrepared->getBallot();
        }
        if (mPreparedPrime)
        {
            p.preparedPrime.activate() = mPreparedPrime->getBallot();
        }
        if (mHighBallot)
        {
            p.nH = mHighBallot->getBallot().counter;
        }
    }
    break;
    case SCPStatementType::SCP_ST_CONFIRM:
    {
        auto& c = statement.pledges.confirm();
        c.quorumSetHash = getLocalNode()->getQuorumSetHash();
        c.ballot = mCurrentBallot->getBallot();
        c.nPrepared = mPrepared->getBallot().counter;
        c.nCommit = mCommit->getBallot().counter;
        c.nH = mHighBallot->getBallot().counter;
    }
    break;
    case SCPStatementType::SCP_ST_EXTERNALIZE:
    {
        auto& e = statement.pledges.externalize();
        e.commit = mCommit->getBallot();
        e.nH = mHighBallot->getBallot().counter;
        e.commitQuorumSetHash = getLocalNode()->getQuorumSetHash();
    }
    break;
    default:
        dbgAbort();
    }

    return statement;
}

void
BallotProtocol::emitCurrentStateStatement()
{
    ZoneScoped;
    SCPStatementType t;

    switch (mPhase)
    {
    case SCP_PHASE_PREPARE:
        t = SCP_ST_PREPARE;
        break;
    case SCP_PHASE_CONFIRM:
        t = SCP_ST_CONFIRM;
        break;
    case SCP_PHASE_EXTERNALIZE:
        t = SCP_ST_EXTERNALIZE;
        break;
    default:
        dbgAbort();
    }

    SCPStatement statement = createStatement(t);
    SCPEnvelope envelope = mSlot.createEnvelope(statement);

    bool canEmit = (mCurrentBallot != nullptr);

    // if we generate the same envelope, don't process it again
    // this can occur when updating h in PREPARE phase
    // as statements only keep track of h.n (but h.x could be different)
    auto lastEnv = mLatestEnvelopes.find(mSlot.getSCP().getLocalNodeID());

    if (lastEnv == mLatestEnvelopes.end() ||
        !(lastEnv->second->getEnvelope() == envelope))
    {
        auto envW = mSlot.getSCPDriver().wrapEnvelope(envelope);
        if (mSlot.processEnvelope(envW, true) == SCP::EnvelopeState::VALID)
        {
            if (canEmit && (!mLastEnvelope ||
                            isNewerStatement(mLastEnvelope->getStatement(),
                                             envelope.statement)))
            {
                mLastEnvelope = envW;
                // this will no-op if invoked from advanceSlot
                // as advanceSlot consolidates all messages sent
                sendLatestEnvelope();
            }
        }
        else
        {
            // there is a bug in the application if it queued up
            // a statement for itself that it considers invalid
            throw std::runtime_error("moved to a bad state (ballot protocol)");
        }
    }
}

void
BallotProtocol::checkInvariants()
{
    switch (mPhase)
    {
    case SCP_PHASE_PREPARE:
        break;
    case SCP_PHASE_CONFIRM:
    case SCP_PHASE_EXTERNALIZE:
        dbgAssert(mCurrentBallot);
        dbgAssert(mPrepared);
        dbgAssert(mCommit);
        dbgAssert(mHighBallot);
        break;
    default:
        dbgAbort();
    }

    if (mCurrentBallot)
    {
        dbgAssert(mCurrentBallot->getBallot().counter != 0);
    }
    if (mPrepared && mPreparedPrime)
    {
        dbgAssert(areBallotsLessAndIncompatible(mPreparedPrime->getBallot(),
                                                mPrepared->getBallot()));
    }
    if (mHighBallot)
    {
        dbgAssert(mCurrentBallot);
        dbgAssert(areBallotsLessAndCompatible(mHighBallot->getBallot(),
                                              mCurrentBallot->getBallot()));
    }
    if (mCommit)
    {
        dbgAssert(mCurrentBallot);
        dbgAssert(areBallotsLessAndCompatible(mCommit->getBallot(),
                                              mHighBallot->getBallot()));
        dbgAssert(areBallotsLessAndCompatible(mHighBallot->getBallot(),
                                              mCurrentBallot->getBallot()));
    }
}

std::set<SCPBallot>
BallotProtocol::getPrepareCandidates(SCPStatement const& hint)
{
    ZoneScoped;
    std::set<SCPBallot> hintBallots;

    switch (hint.pledges.type())
    {
    case SCP_ST_PREPARE:
    {
        auto const& prep = hint.pledges.prepare();
        hintBallots.insert(prep.ballot);
        if (prep.prepared)
        {
            hintBallots.insert(*prep.prepared);
        }
        if (prep.preparedPrime)
        {
            hintBallots.insert(*prep.preparedPrime);
        }
    }
    break;
    case SCP_ST_CONFIRM:
    {
        auto const& con = hint.pledges.confirm();
        hintBallots.insert(SCPBallot(con.nPrepared, con.ballot.value));
        hintBallots.insert(SCPBallot(UINT32_MAX, con.ballot.value));
    }
    break;
    case SCP_ST_EXTERNALIZE:
    {
        auto const& ext = hint.pledges.externalize();
        hintBallots.insert(SCPBallot(UINT32_MAX, ext.commit.value));
    }
    break;
    default:
        abort();
    };

    std::set<SCPBallot> candidates;

    while (!hintBallots.empty())
    {
        auto last = --hintBallots.end();
        SCPBallot topVote = *last;
        hintBallots.erase(last);

        auto const& val = topVote.value;

        // find candidates that may have been prepared
        for (auto const& e : mLatestEnvelopes)
        {
            SCPStatement const& st = e.second->getStatement();
            switch (st.pledges.type())
            {
            case SCP_ST_PREPARE:
            {
                auto const& prep = st.pledges.prepare();
                if (areBallotsLessAndCompatible(prep.ballot, topVote))
                {
                    candidates.insert(prep.ballot);
                }
                if (prep.prepared &&
                    areBallotsLessAndCompatible(*prep.prepared, topVote))
                {
                    candidates.insert(*prep.prepared);
                }
                if (prep.preparedPrime &&
                    areBallotsLessAndCompatible(*prep.preparedPrime, topVote))
                {
                    candidates.insert(*prep.preparedPrime);
                }
            }
            break;
            case SCP_ST_CONFIRM:
            {
                auto const& con = st.pledges.confirm();
                if (areBallotsCompatible(topVote, con.ballot))
                {
                    candidates.insert(topVote);
                    if (con.nPrepared < topVote.counter)
                    {
                        candidates.insert(SCPBallot(con.nPrepared, val));
                    }
                }
            }
            break;
            case SCP_ST_EXTERNALIZE:
            {
                auto const& ext = st.pledges.externalize();
                if (areBallotsCompatible(topVote, ext.commit))
                {
                    candidates.insert(topVote);
                }
            }
            break;
            default:
                abort();
            }
        }
    }

    return candidates;
}

bool
BallotProtocol::updateCurrentIfNeeded(SCPBallot const& h)
{
    bool didWork = false;
    if (!mCurrentBallot || compareBallots(mCurrentBallot->getBallot(), h) < 0)
    {
        bumpToBallot(h, true);
        didWork = true;
    }
    return didWork;
}

bool
BallotProtocol::attemptAcceptPrepared(SCPStatement const& hint)
{
    ZoneScoped;
    if (mPhase != SCP_PHASE_PREPARE && mPhase != SCP_PHASE_CONFIRM)
    {
        return false;
    }

    auto candidates = getPrepareCandidates(hint);

    // see if we can accept any of the candidates, starting with the highest
    for (auto cur = candidates.rbegin(); cur != candidates.rend(); cur++)
    {
        SCPBallot ballot = *cur;

        if (mPhase == SCP_PHASE_CONFIRM)
        {
            // only consider the ballot if it may help us increase
            // p (note: at this point, p ~ c)
            if (!areBallotsLessAndCompatible(mPrepared->getBallot(), ballot))
            {
                continue;
            }
            dbgAssert(areBallotsCompatible(mCommit->getBallot(), ballot));
        }

        // if we already prepared this ballot, don't bother checking again

        // if ballot <= p' ballot is neither a candidate for p nor p'
        if (mPreparedPrime &&
            compareBallots(ballot, mPreparedPrime->getBallot()) <= 0)
        {
            continue;
        }

        if (mPrepared)
        {
            // if ballot is already covered by p, skip
            if (areBallotsLessAndCompatible(ballot, mPrepared->getBallot()))
            {
                continue;
            }
            // otherwise, there is a chance it increases p'
        }

        bool accepted = federatedAccept(
            // checks if any node is voting for this ballot
            [&ballot](SCPStatement const& st) {
                bool res;

                switch (st.pledges.type())
                {
                case SCP_ST_PREPARE:
                {
                    auto const& p = st.pledges.prepare();
                    res = areBallotsLessAndCompatible(ballot, p.ballot);
                }
                break;
                case SCP_ST_CONFIRM:
                {
                    auto const& c = st.pledges.confirm();
                    res = areBallotsCompatible(ballot, c.ballot);
                }
                break;
                case SCP_ST_EXTERNALIZE:
                {
                    auto const& e = st.pledges.externalize();
                    res = areBallotsCompatible(ballot, e.commit);
                }
                break;
                default:
                    res = false;
                    dbgAbort();
                }

                return res;
            },
            std::bind(&BallotProtocol::hasPreparedBallot, ballot, _1));
        if (accepted)
        {
            return setAcceptPrepared(ballot);
        }
    }

    return false;
}

bool
BallotProtocol::setAcceptPrepared(SCPBallot const& ballot)
{
    ZoneScoped;
    CLOG_TRACE(SCP, "BallotProtocol::setAcceptPrepared i: {} b: {}",
               mSlot.getSlotIndex(), mSlot.getSCP().ballotToStr(ballot));

    // update our state
    bool didWork = setPrepared(ballot);

    // check if we also need to clear 'c'
    if (mCommit && mHighBallot)
    {
        if ((mPrepared &&
             areBallotsLessAndIncompatible(mHighBallot->getBallot(),
                                           mPrepared->getBallot())) ||
            (mPreparedPrime &&
             areBallotsLessAndIncompatible(mHighBallot->getBallot(),
                                           mPreparedPrime->getBallot())))
        {
            dbgAssert(mPhase == SCP_PHASE_PREPARE);
            mCommit.reset();
            didWork = true;
        }
    }

    if (didWork)
    {
        mSlot.getSCPDriver().acceptedBallotPrepared(mSlot.getSlotIndex(),
                                                    ballot);
        emitCurrentStateStatement();
    }

    return didWork;
}

bool
BallotProtocol::attemptConfirmPrepared(SCPStatement const& hint)
{
    ZoneScoped;
    if (mPhase != SCP_PHASE_PREPARE)
    {
        return false;
    }

    // check if we could accept this ballot as prepared
    if (!mPrepared)
    {
        return false;
    }

    auto candidates = getPrepareCandidates(hint);

    // see if we can accept any of the candidates, starting with the highest
    SCPBallot newH;
    bool newHfound = false;
    auto cur = candidates.rbegin();
    for (; cur != candidates.rend(); cur++)
    {
        SCPBallot ballot = *cur;

        // only consider it if we can potentially raise h
        if (mHighBallot &&
            compareBallots(mHighBallot->getBallot(), ballot) >= 0)
        {
            break;
        }

        bool ratified = federatedRatify(
            std::bind(&BallotProtocol::hasPreparedBallot, ballot, _1));
        if (ratified)
        {
            newH = ballot;
            newHfound = true;
            break;
        }
    }

    bool res = false;

    if (newHfound)
    {
        SCPBallot newC;
        // now, look for newC (left as 0 if no update)
        // step (3) from the paper
        SCPBallot b =
            mCurrentBallot ? mCurrentBallot->getBallot() : SCPBallot();
        if (!mCommit &&
            (!mPrepared ||
             !areBallotsLessAndIncompatible(newH, mPrepared->getBallot())) &&
            (!mPreparedPrime ||
             !areBallotsLessAndIncompatible(newH, mPreparedPrime->getBallot())))
        {
            // continue where we left off (cur is at newH at this point)
            for (; cur != candidates.rend(); cur++)
            {
                SCPBallot ballot = *cur;
                if (compareBallots(ballot, b) < 0)
                {
                    break;
                }
                // c and h must be compatible
                if (!areBallotsLessAndCompatible(*cur, newH))
                {
                    continue;
                }
                bool ratified = federatedRatify(
                    std::bind(&BallotProtocol::hasPreparedBallot, ballot, _1));
                if (ratified)
                {
                    newC = ballot;
                }
                else
                {
                    break;
                }
            }
        }
        res = setConfirmPrepared(newC, newH);
    }
    return res;
}

bool
BallotProtocol::commitPredicate(SCPBallot const& ballot, Interval const& check,
                                SCPStatement const& st)
{
    bool res = false;
    auto const& pl = st.pledges;
    switch (pl.type())
    {
    case SCP_ST_PREPARE:
        break;
    case SCP_ST_CONFIRM:
    {
        auto const& c = pl.confirm();
        if (areBallotsCompatible(ballot, c.ballot))
        {
            res = c.nCommit <= check.first && check.second <= c.nH;
        }
    }
    break;
    case SCP_ST_EXTERNALIZE:
    {
        auto const& e = pl.externalize();
        if (areBallotsCompatible(ballot, e.commit))
        {
            res = e.commit.counter <= check.first;
        }
    }
    break;
    default:
        dbgAbort();
    }
    return res;
}

bool
BallotProtocol::setConfirmPrepared(SCPBallot const& newC, SCPBallot const& newH)
{
    ZoneScoped;
    CLOG_TRACE(SCP, "BallotProtocol::setConfirmPrepared i: {} h: {}",
               mSlot.getSlotIndex(), mSlot.getSCP().ballotToStr(newH));

    bool didWork = false;

    // remember newH's value
    mValueOverride = mSlot.getSCPDriver().wrapValue(newH.value);

    // we don't set c/h if we're not on a compatible ballot
    if (!mCurrentBallot ||
        areBallotsCompatible(mCurrentBallot->getBallot(), newH))
    {
        if (!mHighBallot || compareBallots(newH, mHighBallot->getBallot()) > 0)
        {
            didWork = true;
            mHighBallot = makeBallot(newH);
        }

        if (newC.counter != 0)
        {
            dbgAssert(!mCommit);
            mCommit = makeBallot(newC);
            didWork = true;
        }

        if (didWork)
        {
            mSlot.getSCPDriver().confirmedBallotPrepared(mSlot.getSlotIndex(),
                                                         newH);
        }
    }

    // always perform step (8) with the computed value of h
    didWork = updateCurrentIfNeeded(newH) || didWork;

    if (didWork)
    {
        emitCurrentStateStatement();
    }

    return didWork;
}

void
BallotProtocol::findExtendedInterval(Interval& candidate,
                                     std::set<uint32> const& boundaries,
                                     std::function<bool(Interval const&)> pred)
{
    // iterate through interesting boundaries, starting from the top
    for (auto it = boundaries.rbegin(); it != boundaries.rend(); it++)
    {
        uint32 b = *it;

        Interval cur;
        if (candidate.first == 0)
        {
            // first, find the high bound
            cur = Interval(b, b);
        }
        else if (b > candidate.second) // invalid
        {
            continue;
        }
        else
        {
            cur.first = b;
            cur.second = candidate.second;
        }

        if (pred(cur))
        {
            candidate = cur;
        }
        else if (candidate.first != 0)
        {
            // could not extend further
            break;
        }
    }
}

std::set<uint32>
BallotProtocol::getCommitBoundariesFromStatements(SCPBallot const& ballot)
{
    std::set<uint32> res;
    for (auto const& env : mLatestEnvelopes)
    {
        auto const& pl = env.second->getStatement().pledges;
        switch (pl.type())
        {
        case SCP_ST_PREPARE:
        {
            auto const& p = pl.prepare();
            if (areBallotsCompatible(ballot, p.ballot))
            {
                if (p.nC)
                {
                    res.emplace(p.nC);
                    res.emplace(p.nH);
                }
            }
        }
        break;
        case SCP_ST_CONFIRM:
        {
            auto const& c = pl.confirm();
            if (areBallotsCompatible(ballot, c.ballot))
            {
                res.emplace(c.nCommit);
                res.emplace(c.nH);
            }
        }
        break;
        case SCP_ST_EXTERNALIZE:
        {
            auto const& e = pl.externalize();
            if (areBallotsCompatible(ballot, e.commit))
            {
                res.emplace(e.commit.counter);
                res.emplace(e.nH);
                res.emplace(UINT32_MAX);
            }
        }
        break;
        default:
            dbgAbort();
        }
    }
    return res;
}

bool
BallotProtocol::attemptAcceptCommit(SCPStatement const& hint)
{
    ZoneScoped;
    if (mPhase != SCP_PHASE_PREPARE && mPhase != SCP_PHASE_CONFIRM)
    {
        return false;
    }

    // extracts value from hint
    // note: ballot.counter is only used for logging purpose as we're looking at
    // possible value to commit
    SCPBallot ballot;
    switch (hint.pledges.type())
    {
    case SCPStatementType::SCP_ST_PREPARE:
    {
        auto const& prep = hint.pledges.prepare();
        if (prep.nC != 0)
        {
            ballot = SCPBallot(prep.nH, prep.ballot.value);
        }
        else
        {
            return false;
        }
    }
    break;
    case SCPStatementType::SCP_ST_CONFIRM:
    {
        auto const& con = hint.pledges.confirm();
        ballot = SCPBallot(con.nH, con.ballot.value);
    }
    break;
    case SCPStatementType::SCP_ST_EXTERNALIZE:
    {
        auto const& ext = hint.pledges.externalize();
        ballot = SCPBallot(ext.nH, ext.commit.value);
        break;
    }
    default:
        abort();
    };

    if (mPhase == SCP_PHASE_CONFIRM)
    {
        if (!areBallotsCompatible(ballot, mHighBallot->getBallot()))
        {
            return false;
        }
    }

    auto pred = [&ballot, this](Interval const& cur) -> bool {
        return federatedAccept(
            [&](SCPStatement const& st) -> bool {
                bool res = false;
                auto const& pl = st.pledges;
                switch (pl.type())
                {
                case SCP_ST_PREPARE:
                {
                    auto const& p = pl.prepare();
                    if (areBallotsCompatible(ballot, p.ballot))
                    {
                        if (p.nC != 0)
                        {
                            res = p.nC <= cur.first && cur.second <= p.nH;
                        }
                    }
                }
                break;
                case SCP_ST_CONFIRM:
                {
                    auto const& c = pl.confirm();
                    if (areBallotsCompatible(ballot, c.ballot))
                    {
                        res = c.nCommit <= cur.first;
                    }
                }
                break;
                case SCP_ST_EXTERNALIZE:
                {
                    auto const& e = pl.externalize();
                    if (areBallotsCompatible(ballot, e.commit))
                    {
                        res = e.commit.counter <= cur.first;
                    }
                }
                break;
                default:
                    dbgAbort();
                }
                return res;
            },
            std::bind(&BallotProtocol::commitPredicate, ballot, cur, _1));
    };

    // build the boundaries to scan
    std::set<uint32> boundaries = getCommitBoundariesFromStatements(ballot);

    if (boundaries.empty())
    {
        return false;
    }

    // now, look for the high interval
    Interval candidate;

    findExtendedInterval(candidate, boundaries, pred);

    bool res = false;

    if (candidate.first != 0)
    {
        if (mPhase != SCP_PHASE_CONFIRM ||
            candidate.second > mHighBallot->getBallot().counter)
        {
            SCPBallot c = SCPBallot(candidate.first, ballot.value);
            SCPBallot h = SCPBallot(candidate.second, ballot.value);
            res = setAcceptCommit(c, h);
        }
    }

    return res;
}

bool
BallotProtocol::setAcceptCommit(SCPBallot const& c, SCPBallot const& h)
{
    ZoneScoped;
    CLOG_TRACE(SCP, "BallotProtocol::setAcceptCommit i: {} new c: {} new h: {}",
               mSlot.getSlotIndex(), mSlot.getSCP().ballotToStr(c),
               mSlot.getSCP().ballotToStr(h));

    bool didWork = false;

    // remember h's value
    mValueOverride = mSlot.getSCPDriver().wrapValue(h.value);

    if (!mHighBallot || !mCommit ||
        compareBallots(mHighBallot->getBallot(), h) != 0 ||
        compareBallots(mCommit->getBallot(), c) != 0)
    {
        mCommit = makeBallot(c);
        mHighBallot = makeBallot(h);

        didWork = true;
    }

    if (mPhase == SCP_PHASE_PREPARE)
    {
        mPhase = SCP_PHASE_CONFIRM;
        if (mCurrentBallot &&
            !areBallotsLessAndCompatible(h, mCurrentBallot->getBallot()))
        {
            bumpToBallot(h, false);
        }
        mPreparedPrime.reset();

        didWork = true;
    }

    if (didWork)
    {
        updateCurrentIfNeeded(mHighBallot->getBallot());

        mSlot.getSCPDriver().acceptedCommit(mSlot.getSlotIndex(), h);
        emitCurrentStateStatement();
    }

    return didWork;
}

static uint32
statementBallotCounter(SCPStatement const& st)
{
    switch (st.pledges.type())
    {
    case SCP_ST_PREPARE:
        return st.pledges.prepare().ballot.counter;
    case SCP_ST_CONFIRM:
        return st.pledges.confirm().ballot.counter;
    case SCP_ST_EXTERNALIZE:
        return UINT32_MAX;
    default:
        // Should never be called with SCP_ST_NOMINATE.
        abort();
    }
}

static bool
hasVBlockingSubsetStrictlyAheadOf(
    std::shared_ptr<LocalNode> localNode,
    std::map<NodeID, SCPEnvelopeWrapperPtr> const& map, uint32_t n)
{
    return LocalNode::isVBlocking(
        localNode->getQuorumSet(), map,
        [&](SCPStatement const& st) { return statementBallotCounter(st) > n; });
}

// Step 9 from the paper (Feb 2016):
//
//   If ∃ S ⊆ M such that the set of senders {v_m | m ∈ S} is v-blocking
//   and ∀m ∈ S, b_m.n > b_v.n, then set b <- <n, z> where n is the lowest
//   counter for which no such S exists.
//
// a.k.a 4th rule for setting ballot.counter in the internet-draft (v03):
//
//   If nodes forming a blocking threshold all have ballot.counter values
//   greater than the local ballot.counter, then the local node immediately
//   cancels any pending timer, increases ballot.counter to the lowest
//   value such that this is no longer the case, and if appropriate
//   according to the rules above arms a new timer. Note that the blocking
//   threshold may include ballots from SCPCommit messages as well as
//   SCPExternalize messages, which implicitly have an infinite ballot
//   counter.

bool
BallotProtocol::attemptBump()
{
    ZoneScoped;
    if (mPhase == SCP_PHASE_PREPARE || mPhase == SCP_PHASE_CONFIRM)
    {

        // First check to see if this condition applies at all. If there
        // is no v-blocking set ahead of the local node, there's nothing
        // to do, return early.
        auto localNode = getLocalNode();
        uint32 localCounter =
            mCurrentBallot ? mCurrentBallot->getBallot().counter : 0;
        if (!hasVBlockingSubsetStrictlyAheadOf(localNode, mLatestEnvelopes,
                                               localCounter))
        {
            return false;
        }

        // Collect all possible counters we might need to advance to.
        std::set<uint32> allCounters;
        for (auto const& e : mLatestEnvelopes)
        {
            uint32_t c = statementBallotCounter(e.second->getStatement());
            if (c > localCounter)
                allCounters.insert(c);
        }

        // If we got to here, implicitly there _was_ a v-blocking subset
        // with counters above the local counter; we just need to find a
        // minimal n at which that's no longer true. So check them in
        // order, starting from the smallest.
        for (uint32_t n : allCounters)
        {
            if (!hasVBlockingSubsetStrictlyAheadOf(localNode, mLatestEnvelopes,
                                                   n))
            {
                // Move to n.
                return abandonBallot(n);
            }
        }
    }
    return false;
}

bool
BallotProtocol::attemptConfirmCommit(SCPStatement const& hint)
{
    ZoneScoped;
    if (mPhase != SCP_PHASE_CONFIRM)
    {
        return false;
    }

    if (!mHighBallot || !mCommit)
    {
        return false;
    }

    // extracts value from hint
    // note: ballot.counter is only used for logging purpose
    SCPBallot ballot;
    switch (hint.pledges.type())
    {
    case SCPStatementType::SCP_ST_PREPARE:
    {
        return false;
    }
    break;
    case SCPStatementType::SCP_ST_CONFIRM:
    {
        auto const& con = hint.pledges.confirm();
        ballot = SCPBallot(con.nH, con.ballot.value);
    }
    break;
    case SCPStatementType::SCP_ST_EXTERNALIZE:
    {
        auto const& ext = hint.pledges.externalize();
        ballot = SCPBallot(ext.nH, ext.commit.value);
        break;
    }
    default:
        abort();
    };

    if (!areBallotsCompatible(ballot, mCommit->getBallot()))
    {
        return false;
    }

    std::set<uint32> boundaries = getCommitBoundariesFromStatements(ballot);
    Interval candidate;

    auto pred = [&ballot, this](Interval const& cur) -> bool {
        return federatedRatify(
            std::bind(&BallotProtocol::commitPredicate, ballot, cur, _1));
    };

    findExtendedInterval(candidate, boundaries, pred);

    bool res = candidate.first != 0;
    if (res)
    {
        SCPBallot c = SCPBallot(candidate.first, ballot.value);
        SCPBallot h = SCPBallot(candidate.second, ballot.value);
        return setConfirmCommit(c, h);
    }
    return res;
}

bool
BallotProtocol::setConfirmCommit(SCPBallot const& c, SCPBallot const& h)
{
    ZoneScoped;
    CLOG_TRACE(SCP,
               "BallotProtocol::setConfirmCommit i: {} new c: {} new h: {}",
               mSlot.getSlotIndex(), mSlot.getSCP().ballotToStr(c),
               mSlot.getSCP().ballotToStr(h));

    mCommit = makeBallot(c);
    mHighBallot = makeBallot(h);
    updateCurrentIfNeeded(mHighBallot->getBallot());

    mPhase = SCP_PHASE_EXTERNALIZE;

    emitCurrentStateStatement();

    mSlot.stopNomination();

    mSlot.getSCPDriver().valueExternalized(mSlot.getSlotIndex(),
                                           mCommit->getBallot().value);

    return true;
}

bool
BallotProtocol::hasPreparedBallot(SCPBallot const& ballot,
                                  SCPStatement const& st)
{
    bool res;

    switch (st.pledges.type())
    {
    case SCP_ST_PREPARE:
    {
        auto const& p = st.pledges.prepare();
        res =
            (p.prepared && areBallotsLessAndCompatible(ballot, *p.prepared)) ||
            (p.preparedPrime &&
             areBallotsLessAndCompatible(ballot, *p.preparedPrime));
    }
    break;
    case SCP_ST_CONFIRM:
    {
        auto const& c = st.pledges.confirm();
        SCPBallot prepared(c.nPrepared, c.ballot.value);
        res = areBallotsLessAndCompatible(ballot, prepared);
    }
    break;
    case SCP_ST_EXTERNALIZE:
    {
        auto const& e = st.pledges.externalize();
        res = areBallotsCompatible(ballot, e.commit);
    }
    break;
    default:
        res = false;
        dbgAbort();
    }

    return res;
}

Hash
BallotProtocol::getCompanionQuorumSetHashFromStatement(SCPStatement const& st)
{
    Hash h;
    switch (st.pledges.type())
    {
    case SCP_ST_PREPARE:
        h = st.pledges.prepare().quorumSetHash;
        break;
    case SCP_ST_CONFIRM:
        h = st.pledges.confirm().quorumSetHash;
        break;
    case SCP_ST_EXTERNALIZE:
        h = st.pledges.externalize().commitQuorumSetHash;
        break;
    default:
        dbgAbort();
    }
    return h;
}

SCPBallot
BallotProtocol::getWorkingBallot(SCPStatement const& st)
{
    SCPBallot res;
    switch (st.pledges.type())
    {
    case SCP_ST_PREPARE:
        res = st.pledges.prepare().ballot;
        break;
    case SCP_ST_CONFIRM:
    {
        auto const& con = st.pledges.confirm();
        res = SCPBallot(con.nCommit, con.ballot.value);
    }
    break;
    case SCP_ST_EXTERNALIZE:
        res = st.pledges.externalize().commit;
        break;
    default:
        dbgAbort();
    }
    return res;
}

bool
BallotProtocol::setPrepared(SCPBallot const& ballot)
{
    bool didWork = false;

    // p and p' are the two highest prepared and incompatible ballots
    if (mPrepared)
    {
        int comp = compareBallots(mPrepared->getBallot(), ballot);
        if (comp < 0)
        {
            // as we're replacing p, we see if we should also replace p'
            if (!areBallotsCompatible(mPrepared->getBallot(), ballot))
            {
                mPreparedPrime = std::make_unique<SCPBallotWrapper>(*mPrepared);
            }
            mPrepared = makeBallot(ballot);
            didWork = true;
        }
        else if (comp > 0)
        {
            // check if we should update only p', this happens
            // either p' was NULL
            // or p' gets replaced by ballot
            //      (p' < ballot and ballot is incompatible with p)
            // note, the later check is here out of paranoia as this function is
            // not called with a value that would not allow us to make progress

            if (!mPreparedPrime ||
                ((compareBallots(mPreparedPrime->getBallot(), ballot) < 0) &&
                 !areBallotsCompatible(mPrepared->getBallot(), ballot)))
            {
                mPreparedPrime = makeBallot(ballot);
                didWork = true;
            }
        }
    }
    else
    {
        mPrepared = makeBallot(ballot);
        didWork = true;
    }
    return didWork;
}

int
BallotProtocol::compareBallots(std::unique_ptr<SCPBallot> const& b1,
                               std::unique_ptr<SCPBallot> const& b2)
{
    int res;
    if (b1 && b2)
    {
        res = compareBallots(*b1, *b2);
    }
    else if (b1 && !b2)
    {
        res = 1;
    }
    else if (!b1 && b2)
    {
        res = -1;
    }
    else
    {
        res = 0;
    }
    return res;
}

int
BallotProtocol::compareBallots(SCPBallot const& b1, SCPBallot const& b2)
{
    if (b1.counter < b2.counter)
    {
        return -1;
    }
    else if (b2.counter < b1.counter)
    {
        return 1;
    }
    // ballots are also strictly ordered by value
    if (b1.value < b2.value)
    {
        return -1;
    }
    else if (b2.value < b1.value)
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

bool
BallotProtocol::areBallotsCompatible(SCPBallot const& b1, SCPBallot const& b2)
{
    return b1.value == b2.value;
}

bool
BallotProtocol::areBallotsLessAndIncompatible(SCPBallot const& b1,
                                              SCPBallot const& b2)
{
    return (compareBallots(b1, b2) <= 0) && !areBallotsCompatible(b1, b2);
}

bool
BallotProtocol::areBallotsLessAndCompatible(SCPBallot const& b1,
                                            SCPBallot const& b2)
{
    return (compareBallots(b1, b2) <= 0) && areBallotsCompatible(b1, b2);
}

void
BallotProtocol::setStateFromEnvelope(SCPEnvelopeWrapperPtr e)
{
    ZoneScoped;
    if (mCurrentBallot)
    {
        throw std::runtime_error(
            "Cannot set state after starting ballot protocol");
    }

    recordEnvelope(e);

    mLastEnvelope = e;
    mLastEnvelopeEmit = mLastEnvelope;

    auto const& pl = e->getStatement().pledges;

    switch (pl.type())
    {
    case SCPStatementType::SCP_ST_PREPARE:
    {
        auto const& prep = pl.prepare();
        auto const& b = prep.ballot;
        bumpToBallot(b, true);
        if (prep.prepared)
        {
            mPrepared = makeBallot(*prep.prepared);
        }
        if (prep.preparedPrime)
        {
            mPreparedPrime = makeBallot(*prep.preparedPrime);
        }
        if (prep.nH)
        {
            mHighBallot = makeBallot(prep.nH, b.value);
        }
        if (prep.nC)
        {
            mCommit = makeBallot(prep.nC, b.value);
        }
        mPhase = SCP_PHASE_PREPARE;
    }
    break;
    case SCPStatementType::SCP_ST_CONFIRM:
    {
        auto const& c = pl.confirm();
        auto const& v = c.ballot.value;
        bumpToBallot(c.ballot, true);
        mPrepared = makeBallot(c.nPrepared, v);
        mHighBallot = makeBallot(c.nH, v);
        mCommit = makeBallot(c.nCommit, v);
        mPhase = SCP_PHASE_CONFIRM;
    }
    break;
    case SCPStatementType::SCP_ST_EXTERNALIZE:
    {
        auto const& ext = pl.externalize();
        auto const& v = ext.commit.value;
        bumpToBallot(SCPBallot(UINT32_MAX, v), true);
        mPrepared = makeBallot(UINT32_MAX, v);
        mHighBallot = makeBallot(ext.nH, v);
        mCommit = makeBallot(ext.commit);
        mPhase = SCP_PHASE_EXTERNALIZE;
    }
    break;
    default:
        dbgAbort();
    }
}

bool
BallotProtocol::processCurrentState(
    std::function<bool(SCPEnvelope const&)> const& f, bool forceSelf) const
{
    for (auto const& n : mLatestEnvelopes)
    {
        // only return messages for self if the slot is fully validated
        if (forceSelf || !(n.first == mSlot.getSCP().getLocalNodeID()) ||
            mSlot.isFullyValidated())
        {
            if (!f(n.second->getEnvelope()))
            {
                return false;
            }
        }
    }
    return true;
}

SCPEnvelope const*
BallotProtocol::getLatestMessage(NodeID const& id) const
{
    auto it = mLatestEnvelopes.find(id);
    if (it != mLatestEnvelopes.end())
    {
        return &it->second->getEnvelope();
    }
    return nullptr;
}

std::vector<SCPEnvelope>
BallotProtocol::getExternalizingState() const
{
    std::vector<SCPEnvelope> res;
    if (mPhase == SCP_PHASE_EXTERNALIZE)
    {
        res.reserve(mLatestEnvelopes.size());
        for (auto const& n : mLatestEnvelopes)
        {
            if (!(n.first == mSlot.getSCP().getLocalNodeID()))
            {
                // good approximation: statements with the value that
                // externalized
                // we could filter more using mConfirmedPrepared as well
                if (areBallotsCompatible(
                        getWorkingBallot(n.second->getStatement()),
                        mCommit->getBallot()))
                {
                    res.emplace_back(n.second->getEnvelope());
                }
            }
            else if (mSlot.isFullyValidated())
            {
                // only return messages for self if the slot is fully validated
                res.emplace_back(n.second->getEnvelope());
            }
        }
    }
    return res;
}

void
BallotProtocol::advanceSlot(SCPStatement const& hint)
{
    ZoneScoped;
    mCurrentMessageLevel++;
    CLOG_TRACE(SCP, "BallotProtocol::advanceSlot {} {}", mCurrentMessageLevel,
               getLocalState());

    if (mCurrentMessageLevel >= MAX_ADVANCE_SLOT_RECURSION)
    {
        throw std::runtime_error(
            "maximum number of transitions reached in advanceSlot");
    }

    // attempt* methods will queue up messages, causing advanceSlot to be
    // called recursively

    // done in order so that we follow the steps from the white paper in
    // order
    // allowing the state to be updated properly

    bool didWork = false;

    didWork = attemptAcceptPrepared(hint) || didWork;

    didWork = attemptConfirmPrepared(hint) || didWork;

    didWork = attemptAcceptCommit(hint) || didWork;

    didWork = attemptConfirmCommit(hint) || didWork;

    // only bump after we're done with everything else
    if (mCurrentMessageLevel == 1)
    {
        bool didBump = false;
        do
        {
            // attemptBump may invoke advanceSlot recursively
            didBump = attemptBump();
            didWork = didBump || didWork;
        } while (didBump);

        checkHeardFromQuorum();
    }

    CLOG_TRACE(SCP, "BallotProtocol::advanceSlot {} - exiting {}",
               mCurrentMessageLevel, getLocalState());

    --mCurrentMessageLevel;

    if (didWork)
    {
        sendLatestEnvelope();
    }
}

std::set<Value>
BallotProtocol::getStatementValues(SCPStatement const& st)
{
    std::set<Value> values;

    switch (st.pledges.type())
    {
    case SCPStatementType::SCP_ST_PREPARE:
    {
        auto const& prep = st.pledges.prepare();
        auto const& b = prep.ballot;
        if (b.counter != 0)
        {
            values.insert(prep.ballot.value);
        }
        if (prep.prepared)
        {
            values.insert(prep.prepared->value);
        }
        if (prep.preparedPrime)
        {
            values.insert(prep.preparedPrime->value);
        }
    }
    break;
    case SCPStatementType::SCP_ST_CONFIRM:
        values.insert(st.pledges.confirm().ballot.value);
        break;
    case SCPStatementType::SCP_ST_EXTERNALIZE:
        values.insert(st.pledges.externalize().commit.value);
        break;
    default:
        abort();
    }
    return values;
}

SCPDriver::ValidationLevel
BallotProtocol::validateValues(SCPStatement const& st)
{
    ZoneScoped;
    std::set<Value> values;

    values = getStatementValues(st);

    if (values.empty())
    {
        // This shouldn't happen
        return SCPDriver::kInvalidValue;
    }

    SCPDriver::ValidationLevel res = std::accumulate(
        values.begin(), values.end(), SCPDriver::kFullyValidatedValue,
        [&](SCPDriver::ValidationLevel lv, stellar::Value const& v) {
            if (lv > SCPDriver::kInvalidValue)
            {
                auto tr = mSlot.getSCPDriver().validateValue(
                    mSlot.getSlotIndex(), v, false);
                lv = std::min(tr, lv);
            }
            return lv;
        });

    return res;
}

void
BallotProtocol::sendLatestEnvelope()
{
    ZoneScoped;
    // emit current envelope if needed
    if (mCurrentMessageLevel == 0 && mLastEnvelope && mSlot.isFullyValidated())
    {
        if (!mLastEnvelopeEmit || mLastEnvelope != mLastEnvelopeEmit)
        {
            mLastEnvelopeEmit = mLastEnvelope;
            mSlot.getSCPDriver().emitEnvelope(mLastEnvelopeEmit->getEnvelope());
        }
    }
}

std::array<const char*, BallotProtocol::SCP_PHASE_NUM>
    BallotProtocol::phaseNames = std::array{"PREPARE", "FINISH", "EXTERNALIZE"};

Json::Value
BallotProtocol::getJsonInfo()
{
    Json::Value ret;
    ret["heard"] = mHeardFromQuorum;
    ret["ballot"] = ballotToStr(mCurrentBallot);
    ret["phase"] = phaseNames[mPhase];

    ret["state"] = getLocalState();
    return ret;
}

SCP::QuorumInfoNodeState
BallotProtocol::getState(NodeID const& n, bool selfAlreadyMovedOn)
{
    auto state = SCP::QuorumInfoNodeState::AGREE;
    if (n == mSlot.getLocalNode()->getNodeID())
    {
        // always mark myself as AGREE.
        return state;
    }

    auto it = mLatestEnvelopes.find(n);
    if (it == mLatestEnvelopes.end())
    {
        if (mTimerExpCount >= Slot::NUM_TIMEOUTS_THRESHOLD_FOR_REPORTING ||
            selfAlreadyMovedOn)
        {
            state = SCP::QuorumInfoNodeState::MISSING;
        }
        else
        {
            // It's too soon to start calling this node
            // MISSING.
            state = SCP::QuorumInfoNodeState::NO_INFO;
        }
    }
    else if (mLastEnvelopeEmit)
    {
        auto& st = it->second->getStatement();
        auto t = st.pledges.type();
        bool externalized = t == SCPStatementType::SCP_ST_EXTERNALIZE;
        bool confirmedCommit =
            t == SCPStatementType::SCP_ST_CONFIRM &&
            st.pledges.confirm().ballot.counter == UINT32_MAX;
        if (mPhase == SCP_PHASE_EXTERNALIZE &&
            (!externalized || !confirmedCommit) && selfAlreadyMovedOn)
        {
            // We have already externalized and moved onto the next slot.
            // However, this node hasn't externalized yet.
            state = SCP::QuorumInfoNodeState::DELAYED;
        }

        auto selfAcceptedConfirm =
            mPhase == SCP_PHASE_CONFIRM || mPhase == SCP_PHASE_EXTERNALIZE;
        auto otherAcceptedConfirm = t == SCPStatementType::SCP_ST_CONFIRM ||
                                    t == SCPStatementType::SCP_ST_EXTERNALIZE;

        auto const& selfSt = mLastEnvelopeEmit->getStatement();

        if (selfAcceptedConfirm && otherAcceptedConfirm &&
            !areBallotsCompatible(getWorkingBallot(st),
                                  getWorkingBallot(selfSt)))
        {
            // n has accepted to commit a different value than mine!
            // Even if this node has been marked something else,
            // we will overwrite it since this is bad.
            state = SCP::QuorumInfoNodeState::DISAGREE;
        }
    }
    return state;
}

Json::Value
BallotProtocol::getJsonQuorumInfo(NodeID const& id, bool summary, bool fullKeys)
{
    Json::Value ret;
    auto& phase = ret["phase"];

    // find the state of the node `id`
    SCPBallot b;
    Hash qSetHash;

    auto stateit = mLatestEnvelopes.find(id);
    if (stateit == mLatestEnvelopes.end())
    {
        phase = "unknown";
        if (id == mSlot.getLocalNode()->getNodeID())
        {
            qSetHash = mSlot.getLocalNode()->getQuorumSetHash();
        }
    }
    else
    {
        auto const& st = stateit->second->getStatement();

        switch (st.pledges.type())
        {
        case SCPStatementType::SCP_ST_PREPARE:
            phase = "PREPARE";
            b = st.pledges.prepare().ballot;
            break;
        case SCPStatementType::SCP_ST_CONFIRM:
            phase = "CONFIRM";
            b = st.pledges.confirm().ballot;
            break;
        case SCPStatementType::SCP_ST_EXTERNALIZE:
            phase = "EXTERNALIZE";
            b = st.pledges.externalize().commit;
            break;
        default:
            dbgAbort();
        }
        // use the companion set here even for externalize to capture
        // the view of the quorum set during consensus
        qSetHash = mSlot.getCompanionQuorumSetHashFromStatement(st);
    }

    auto qSet = mSlot.getSCPDriver().getQSet(qSetHash);
    if (!qSet)
    {
        phase = "expired";
        return ret;
    }

    auto f = LocalNode::findClosestVBlocking(
        *qSet, mLatestEnvelopes,
        [&](SCPStatement const& st) {
            return areBallotsCompatible(getWorkingBallot(st), b);
        },
        &id);
    ret["fail_at"] = static_cast<int>(f.size());

    if (!summary)
    {
        auto& f_ex = ret["fail_with"];
        for (auto const& n : f)
        {
            f_ex.append(mSlot.getSCPDriver().toStrKey(n, fullKeys));
        }
        ret["value"] = getLocalNode()->toJson(*qSet, fullKeys);
    }

    ret["hash"] = hexAbbrev(qSetHash);

    return ret;
}

std::string
BallotProtocol::getLocalState() const
{
    std::ostringstream oss;

    oss << "i: " << mSlot.getSlotIndex() << " | " << phaseNames[mPhase]
        << " | b: " << ballotToStr(mCurrentBallot)
        << " | p: " << ballotToStr(mPrepared)
        << " | p': " << ballotToStr(mPreparedPrime)
        << " | h: " << ballotToStr(mHighBallot)
        << " | c: " << ballotToStr(mCommit)
        << " | M: " << mLatestEnvelopes.size();
    return oss.str();
}

std::shared_ptr<LocalNode>
BallotProtocol::getLocalNode()
{
    return mSlot.getSCP().getLocalNode();
}

bool
BallotProtocol::federatedAccept(StatementPredicate voted,
                                StatementPredicate accepted)
{
    ZoneScoped;
    return mSlot.federatedAccept(voted, accepted, mLatestEnvelopes);
}

bool
BallotProtocol::federatedRatify(StatementPredicate voted)
{
    ZoneScoped;
    return mSlot.federatedRatify(voted, mLatestEnvelopes);
}

void
BallotProtocol::checkHeardFromQuorum()
{
    // this method is safe to call regardless of the transitions of the other
    // nodes on the network:
    // we guarantee that other nodes can only transition to higher counters
    // (messages are ignored upstream)
    // therefore the local node will not flip flop between "seen" and "not seen"
    // for a given counter on the local node
    if (mCurrentBallot)
    {
        ZoneScoped;
        if (LocalNode::isQuorum(
                getLocalNode()->getQuorumSet(), mLatestEnvelopes,
                std::bind(&Slot::getQuorumSetFromStatement, &mSlot, _1),
                [&](SCPStatement const& st) {
                    bool res;
                    if (st.pledges.type() == SCP_ST_PREPARE)
                    {
                        res = mCurrentBallot->getBallot().counter <=
                              st.pledges.prepare().ballot.counter;
                    }
                    else
                    {
                        res = true;
                    }
                    return res;
                }))
        {
            bool oldHQ = mHeardFromQuorum;
            mHeardFromQuorum = true;
            if (!oldHQ)
            {
                // if we transition from not heard -> heard, we start the timer
                mSlot.getSCPDriver().ballotDidHearFromQuorum(
                    mSlot.getSlotIndex(), mCurrentBallot->getBallot());
                if (mPhase != SCP_PHASE_EXTERNALIZE)
                {
                    startBallotProtocolTimer();
                }
            }
            if (mPhase == SCP_PHASE_EXTERNALIZE)
            {
                stopBallotProtocolTimer();
            }
        }
        else
        {
            mHeardFromQuorum = false;
            stopBallotProtocolTimer();
        }
    }
}

BallotProtocol::SCPBallotWrapperUPtr
BallotProtocol::makeBallot(SCPBallot const& b) const
{
    auto res = std::make_unique<SCPBallotWrapper>(
        b.counter, mSlot.getSCPDriver().wrapValue(b.value));
    return res;
}

BallotProtocol::SCPBallotWrapperUPtr
BallotProtocol::makeBallot(uint32 c, Value const& v) const
{
    return makeBallot(SCPBallot(c, v));
}

std::string
BallotProtocol::ballotToStr(
    BallotProtocol::SCPBallotWrapperUPtr const& ballot) const
{
    std::string res;
    if (ballot)
    {
        res = mSlot.getSCP().ballotToStr(ballot->getBallot());
    }
    else
    {
        res = "(<null_ballot>)";
    }
    return res;
}
}
