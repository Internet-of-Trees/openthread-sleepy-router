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

// First automated (Nexus) regression test for the "Sleepy Router" fork: a Router that
// duty-cycles its radio via CSL while remaining a full routing peer, and a peer Router
// that reaches it indirectly via CSL instead of direct transmission. See
// docs/sleepy-router/DESIGN_LOG.md for the full design rationale and the bug catalog this
// pipeline exposed along the way -- this test exercises the same end-to-end scenario
// that was validated manually on OTNS (0% packet loss, radio genuinely sleeping),
// as a whitebox, deterministic, CI-friendly regression test.

#include <stdio.h>

#include "platform/nexus_core.hpp"
#include "platform/nexus_node.hpp"

namespace ot {
namespace Nexus {

/**
 * Time to advance for a node to form a network and become leader, in milliseconds.
 */
static constexpr uint32_t kFormNetworkTime = 13 * 1000;

/**
 * Time to advance for a node to join and upgrade to a router, in milliseconds.
 */
static constexpr uint32_t kAttachToRouterTime = 200 * 1000;

/**
 * Time to advance for the Sleepy Router's CSL schedule to be advertised and learned
 * by its peer (`Mac::ProcessCsl()`), in milliseconds.
 */
static constexpr uint32_t kCslSyncTime = 5 * 1000;

/**
 * Number of `kCslPollStep`-sized steps to poll the Sleepy Router's radio state over,
 * looking for at least one `kStateSleep` sample. Comfortably more than one full CSL
 * period at the default Sleepy Router CSL period (~160ms).
 */
static constexpr uint16_t kCslPollSteps = 60;
static constexpr uint32_t kCslPollStepMs = 10;

void TestSleepyRouterPeer(void)
{
    /**
     * Topology:
     *   Router_1 (Leader) ---- Router_2 (Sleepy Router)
     */

    Core nexus;

    Node &router1 = nexus.CreateNode();
    Node &router2 = nexus.CreateNode();

    router1.SetName("Router_1");
    router2.SetName("Router_2");

    nexus.AdvanceTime(0);

    Log("---------------------------------------------------------------------------------------");
    Log("Step 1: Form network, Router_1 becomes Leader");

    router1.Form();
    nexus.AdvanceTime(kFormNetworkTime);
    VerifyOrQuit(router1.Get<Mle::Mle>().IsLeader());

    Log("---------------------------------------------------------------------------------------");
    Log("Step 2: Router_2 joins and upgrades to a genuine Router (not just a Child)");

    router2.Join(router1);
    nexus.AdvanceTime(kAttachToRouterTime);
    VerifyOrQuit(router2.Get<Mle::Mle>().IsRouter());

    Log("---------------------------------------------------------------------------------------");
    Log("Step 3: Router_2 enables Sleepy Router mode");

    // `IsRxOnWhenIdle()` deliberately stays true throughout (Router eligibility is never
    // touched by Sleepy Router mode -- see docs/sleepy-router/DESIGN_LOG.md section 3, Tappa F).
    VerifyOrQuit(router2.Get<Mle::Mle>().IsRxOnWhenIdle());
    router2.Get<Mle::Mle>().SetSleepyRouterMode(true);
    VerifyOrQuit(router2.Get<Mle::Mle>().IsSleepyRouterMode());
    VerifyOrQuit(router2.Get<Mle::Mle>().IsRxOnWhenIdle());

    nexus.AdvanceTime(kCslSyncTime);

    Log("---------------------------------------------------------------------------------------");
    Log("Step 4: Router_1 learns Router_2's CSL schedule via the CSL IE on its outgoing frames");

    // Waiting for a naturally-occurring broadcast (MLE Advertisement, on a Trickle timer) to
    // carry the CSL IE would make this test's timing depend on Trickle's randomized interval --
    // exactly the kind of "hope the timing lines up" this test is meant to avoid (see the chat
    // log around 2026-09-29 on why the purge-scenario test was written as deterministic OTNS
    // Python instead of manual CLI timing). Force it deterministically instead: once
    // `Mac::IsCslEnabled()` is true for Router_2 (a direct consequence of the Tappa F fix chain),
    // `MessageFramer::PrepareMacHeaders()`'s pre-existing `else if (Get<Mac::Mac>().IsCslEnabled())`
    // branch appends the CSL IE to *every* outgoing unicast frame from Router_2, not just frames
    // to a Child -- so a single ping FROM Router_2 is enough to carry it to Router_1.
    {
        Ip6::Address router1Mleid = router1.Get<Mle::Mle>().GetMeshLocalEid();
        Ip6::Address router2Mleid = router2.Get<Mle::Mle>().GetMeshLocalEid();

        nexus.SendAndVerifyEchoRequest(router2, router2Mleid, router1Mleid);
    }

    {
        Router *peer = router1.Get<RouterTable>().FindRouterByRloc16(router2.Get<Mle::Mle>().GetRloc16());

        VerifyOrQuit(peer != nullptr);
        VerifyOrQuit(peer->IsCslSynchronized());
        VerifyOrQuit(peer->GetCslPeriod() > 0);
    }

    Log("---------------------------------------------------------------------------------------");
    Log("Step 5: verify Router_2's radio genuinely duty-cycles (physically sleeps), not just");
    Log("        advertises a CSL schedule it never honors itself (bug #9 in the design log)");

    {
        bool observedSleep = false;

        for (uint16_t i = 0; i < kCslPollSteps; i++)
        {
            nexus.AdvanceTime(kCslPollStepMs);

            if (router2.mRadio.mState == Radio::kStateSleep)
            {
                observedSleep = true;
                break;
            }
        }

        VerifyOrQuit(observedSleep);
    }

    Log("---------------------------------------------------------------------------------------");
    Log("Step 6: Router_1 still reaches Router_2 end to end, delivered indirectly via CSL instead");
    Log("        of direct transmission (which would simply fail while Router_2 is asleep)");

    {
        Ip6::Address router1Mleid = router1.Get<Mle::Mle>().GetMeshLocalEid();
        Ip6::Address router2Mleid = router2.Get<Mle::Mle>().GetMeshLocalEid();

        nexus.SendAndVerifyEchoRequest(router1, router1Mleid, router2Mleid);
    }

    nexus.SaveTestInfo("test_sleepy_router_peer.json");
}

} // namespace Nexus
} // namespace ot

int main(void)
{
    ot::Nexus::TestSleepyRouterPeer();
    printf("All tests passed\n");
    return 0;
}
