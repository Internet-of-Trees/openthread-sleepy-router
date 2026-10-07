/*
 *  Copyright (c) 2026, The OpenThread Authors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

// Scale regression test for the "Sleepy Router" fork (see docs/sleepy-router/DESIGN_LOG.md):
// the three existing Nexus tests (`test_sleepy_router_peer.cpp`, `test_sleepy_parent.cpp`,
// `test_sleepy_router_purge.cpp`) only ever place a single Sleepy Router next to an always-on
// neighbor (Leader or MED Child). This test chains *three consecutive* Sleepy Routers between
// a Leader and an always-on relay, which introduces something none of those tests exercise:
// Sleepy<->Sleepy adjacency, where both sides of a link may be physically asleep at once.
//
// Why that is not just "the same thing three times": `Radio::CanReceiveOnChannel()`
// (tests/nexus/platform/nexus_radio.cpp) only returns true while `mState` is `kStateReceive` or
// `kStateTransmit` -- a node whose radio is genuinely in `kStateSleep` does not receive a frame
// sent to it at all, deterministically, not probabilistically. So the very first time two
// adjacent Sleepy Routers must learn *each other's* CSL schedule, one direction of that exchange
// requires sending to a neighbor that may already be physically asleep (it was enabled earlier in
// the chain-building loop and may have long since settled into real duty-cycling), unlike every
// prior test where the "student" of a forced sync ping was always a node that never sleeps.
//
// The ordering below sidesteps that by processing the chain pair-by-pair, left to right, and
// doing both priming pings for a pair *immediately* after enabling the new node's Sleepy Router
// mode -- before any further `AdvanceTime()` gives the earlier node in the pair a chance to
// settle into physical sleep. This is a timing assumption, not a proof: each priming step is
// followed by an explicit `VerifyOrQuit` on the learned schedule, so if the assumption is wrong
// for some chain length/config, the test fails exactly at the pair where it breaks, not with a
// generic timeout later. See docs/sleepy-router/DESIGN_LOG.md, Tappa G, for the full reasoning
// and the empirical confirmation that this ordering holds.

#include <stdio.h>

#include "platform/nexus_core.hpp"
#include "platform/nexus_node.hpp"

namespace ot {
namespace Nexus {

/**
 * Number of consecutive Sleepy Routers chained between the always-on relay and the far end
 * of the path. Three is the minimum chain long enough to show whether per-hop CSL latency
 * compounds linearly or pathologically, without making the test unwieldy to read/debug.
 */
static constexpr uint16_t kNumSleepyRouters = 3;

static constexpr uint32_t kFormNetworkTime    = 13 * 1000;
static constexpr uint32_t kAttachToRouterTime = 200 * 1000;

/**
 * Buffer advanced once after all priming pings for the whole chain are done, before polling
 * for genuine physical sleep. Generous on purpose (unlike the priming pings themselves, which
 * are deliberately done with minimal elapsed time -- see the file header comment) because by
 * this point every pairwise schedule is already learned, so there is no more race to avoid.
 */
static constexpr uint32_t kCslSettleTime = 5 * 1000;

static constexpr uint16_t kCslPollSteps  = 60;
static constexpr uint32_t kCslPollStepMs = 10;

/**
 * End-to-end response timeout for the final echo test, which crosses all `kNumSleepyRouters`
 * sleepy hops in one direction. Each sleepy hop can add up to one full CSL period before the
 * queued frame is actually sent (`kDefaultSleepyRouterCslPeriod`, documented as ~160ms in
 * test_sleepy_router_peer.cpp); a round trip crosses the chain twice. Sized generously rather
 * than tightly, since the point of this test is to catch outright delivery failures, not to
 * measure the latency itself -- that is a separate, still-open measurement (see
 * docs/sleepy-router/ROADMAP.md).
 */
static constexpr uint32_t kChainResponseTimeout = 5 * 1000;

/**
 * Confirms `aObserver` has learned `aSubject`'s CSL schedule (announced via the CSL IE on
 * `aSubject`'s outgoing frames once it is a Sleepy Router -- see
 * docs/sleepy-router/DESIGN_LOG.md section 8, `message_framer.cpp`). Factored out because the
 * chain needs this same check after every priming ping, once per adjacent pair.
 */
void VerifyLearnedCslSchedule(Node &aObserver, Node &aSubject)
{
    Router *subjectAsSeenByObserver =
        aObserver.Get<RouterTable>().FindRouterByRloc16(aSubject.Get<Mle::Mle>().GetRloc16());

    VerifyOrQuit(subjectAsSeenByObserver != nullptr);
    VerifyOrQuit(subjectAsSeenByObserver->IsCslSynchronized());
    VerifyOrQuit(subjectAsSeenByObserver->GetCslPeriod() > 0);
}

void TestSleepyRouterChain(void)
{
    /**
     * Topology:
     *   Leader ---- Relay ---- Sleepy_1 ---- Sleepy_2 ---- Sleepy_3
     *
     * `AllowLinkBetween()` restricts the radio layer to these links only, so the Leader can
     * only ever reach Sleepy_3 by routing through Relay, Sleepy_1 and Sleepy_2 -- a real
     * multi-hop path, not just multiple nodes that happen to all hear each other directly.
     */

    Core  nexus;
    Node &leader = nexus.CreateNode();
    Node &relay  = nexus.CreateNode();
    Node *sleepyRouters[kNumSleepyRouters];

    leader.SetName("Leader");
    relay.SetName("Relay");

    nexus.AdvanceTime(0);

    Log("---------------------------------------------------------------------------------------");
    Log("Step 1: Form network, Leader becomes Leader");

    leader.Form();
    nexus.AdvanceTime(kFormNetworkTime);
    VerifyOrQuit(leader.Get<Mle::Mle>().IsLeader());

    Log("---------------------------------------------------------------------------------------");
    Log("Step 2: Relay (always-on) joins and upgrades to Router");

    AllowLinkBetween(leader, relay);
    relay.Join(leader, Node::kAsFtd);
    nexus.AdvanceTime(kAttachToRouterTime);
    VerifyOrQuit(relay.Get<Mle::Mle>().IsRouter());

    Log("---------------------------------------------------------------------------------------");
    Log("Step 3: build the rest of the chain -- three Routers, each joining the previous one --");
    Log("        all still fully awake at this point, no Sleepy Router mode enabled yet. Joining");
    Log("        while awake sidesteps the harder problem of attaching *through* a node that is");
    Log("        already asleep, which this test does not attempt to solve.");

    for (uint16_t i = 0; i < kNumSleepyRouters; i++)
    {
        Node &prev = (i == 0) ? relay : *sleepyRouters[i - 1];

        sleepyRouters[i] = &nexus.CreateNode();
        sleepyRouters[i]->SetName("Sleepy", static_cast<uint16_t>(i + 1));

        AllowLinkBetween(prev, *sleepyRouters[i]);
        sleepyRouters[i]->Join(prev, Node::kAsFtd);
        nexus.AdvanceTime(kAttachToRouterTime);

        VerifyOrQuit(sleepyRouters[i]->Get<Mle::Mle>().IsRouter());
    }

    Log("---------------------------------------------------------------------------------------");
    Log("Step 4: enable Sleepy Router mode and prime CSL learning, one pair at a time");
    Log("        (see file header comment for why the ordering here matters)");

    for (uint16_t i = 0; i < kNumSleepyRouters; i++)
    {
        Node &prev = (i == 0) ? relay : *sleepyRouters[i - 1];
        Node &cur  = *sleepyRouters[i];

        VerifyOrQuit(cur.Get<Mle::Mle>().IsRxOnWhenIdle());
        cur.Get<Mle::Mle>().SetSleepyRouterMode(true);
        VerifyOrQuit(cur.Get<Mle::Mle>().IsSleepyRouterMode());
        VerifyOrQuit(cur.Get<Mle::Mle>().IsRxOnWhenIdle());

        // Risky direction first, done with minimal elapsed time since `prev` was enabled (or,
        // for i == 0, `prev` is Relay and never sleeps, so this is unconditionally safe): `cur`
        // teaches `prev` its new schedule.
        {
            Ip6::Address curMleid  = cur.Get<Mle::Mle>().GetMeshLocalEid();
            Ip6::Address prevMleid = prev.Get<Mle::Mle>().GetMeshLocalEid();

            nexus.SendAndVerifyEchoRequest(cur, curMleid, prevMleid);
        }
        VerifyLearnedCslSchedule(/* aObserver */ prev, /* aSubject */ cur);

        // Safe direction: `prev` teaches `cur` its own schedule, if `prev` is itself a Sleepy
        // Router (not Relay). `cur` was only just created/enabled, so it is certainly still
        // awake no matter how long this takes.
        if (i > 0)
        {
            Ip6::Address prevMleid = prev.Get<Mle::Mle>().GetMeshLocalEid();
            Ip6::Address curMleid  = cur.Get<Mle::Mle>().GetMeshLocalEid();

            nexus.SendAndVerifyEchoRequest(prev, prevMleid, curMleid);
            VerifyLearnedCslSchedule(/* aObserver */ cur, /* aSubject */ prev);
        }
    }

    Log("---------------------------------------------------------------------------------------");
    Log("Step 5: let the whole chain settle, then verify every Sleepy Router genuinely duty-cycles");
    Log("        its radio (not just advertises a schedule it never honors -- bug #9 in the");
    Log("        design log)");

    nexus.AdvanceTime(kCslSettleTime);

    for (uint16_t i = 0; i < kNumSleepyRouters; i++)
    {
        bool  observedSleep = false;
        Node &cur           = *sleepyRouters[i];

        for (uint16_t step = 0; step < kCslPollSteps; step++)
        {
            nexus.AdvanceTime(kCslPollStepMs);

            if (cur.mRadio.mState == Radio::kStateSleep)
            {
                observedSleep = true;
                break;
            }
        }

        VerifyOrQuit(observedSleep);
    }

    Log("---------------------------------------------------------------------------------------");
    Log("Step 6: end-to-end delivery across all four hops, both directions, through three");
    Log("        consecutive Sleepy Routers");

    {
        Ip6::Address leaderMleid = leader.Get<Mle::Mle>().GetMeshLocalEid();
        Ip6::Address farMleid    = sleepyRouters[kNumSleepyRouters - 1]->Get<Mle::Mle>().GetMeshLocalEid();

        nexus.SendAndVerifyEchoRequest(leader, leaderMleid, farMleid, /* aPayloadSize */ 0,
                                       /* aHopLimit */ Ip6::kDefaultHopLimit, kChainResponseTimeout);
        nexus.SendAndVerifyEchoRequest(*sleepyRouters[kNumSleepyRouters - 1], farMleid, leaderMleid,
                                       /* aPayloadSize */ 0, /* aHopLimit */ Ip6::kDefaultHopLimit,
                                       kChainResponseTimeout);
    }

    nexus.SaveTestInfo("test_sleepy_router_chain.json");
}

} // namespace Nexus
} // namespace ot

int main(void)
{
    ot::Nexus::TestSleepyRouterChain();
    printf("All tests passed\n");
    return 0;
}
