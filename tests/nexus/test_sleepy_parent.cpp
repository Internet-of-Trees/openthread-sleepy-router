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

// Covers the *original* Sleepy Router scenario (Tappe A-D in docs/sleepy-router/DESIGN_LOG.md,
// before the Tappa E generalization to arbitrary Router-peers): a Router enables Sleepy
// Router mode while acting as Parent to an always-on Child (a MED -- rx-on-when-idle, not
// itself CSL-capable, exactly the topology used in every manual OTNS test of this pipeline,
// `add router` + `add med`). The Child must still reach its now-duty-cycling Parent, which
// needs the Child -> sleepy-Parent indirect CSL queue (`AddMessageForSleepyParent`) -- the
// reverse of stock OpenThread's only built-in CSL direction (Parent -> sleepy Child).
//
// This is also the scenario behind the still-not-fully-explained §7.1 packet loss history
// (design log section 5): a manual OTNS run once showed ~85% loss on this exact path,
// re-tested clean after the Tappa F fixes without ever isolating which specific fix
// resolved it. This test exists so that regression, if it ever reappears, is caught
// automatically instead of rediscovered by chance in a future manual run.

#include <stdio.h>

#include "platform/nexus_core.hpp"
#include "platform/nexus_node.hpp"

namespace ot {
namespace Nexus {

static constexpr uint32_t kFormNetworkTime = 13 * 1000;
static constexpr uint32_t kAttachToParentTime = 30 * 1000;
static constexpr uint32_t kCslSyncTime = 5 * 1000;

static constexpr uint16_t kCslPollSteps   = 60;
static constexpr uint32_t kCslPollStepMs = 10;

void TestSleepyParent(void)
{
    /**
     * Topology:
     *   Router_1 (Leader, becomes Sleepy Router) ---- Child (MED, always rx-on-when-idle)
     */

    Core nexus;

    Node &router1 = nexus.CreateNode();
    Node &child   = nexus.CreateNode();

    router1.SetName("Router_1");
    child.SetName("Child");

    nexus.AdvanceTime(0);

    Log("---------------------------------------------------------------------------------------");
    Log("Step 1: Form network, Router_1 becomes Leader");

    router1.Form();
    nexus.AdvanceTime(kFormNetworkTime);
    VerifyOrQuit(router1.Get<Mle::Mle>().IsLeader());

    Log("---------------------------------------------------------------------------------------");
    Log("Step 2: Child joins as a MED (rx-on-when-idle, not itself CSL-capable)");

    child.Join(router1, Node::kAsMed);
    nexus.AdvanceTime(kAttachToParentTime);
    VerifyOrQuit(child.Get<Mle::Mle>().IsChild());
    VerifyOrQuit(child.Get<Mle::Mle>().IsRxOnWhenIdle());

    Log("---------------------------------------------------------------------------------------");
    Log("Step 3: Router_1 (the Child's Parent) enables Sleepy Router mode");

    VerifyOrQuit(router1.Get<Mle::Mle>().IsRxOnWhenIdle());
    router1.Get<Mle::Mle>().SetSleepyRouterMode(true);
    VerifyOrQuit(router1.Get<Mle::Mle>().IsRxOnWhenIdle());

    nexus.AdvanceTime(kCslSyncTime);

    Log("---------------------------------------------------------------------------------------");
    Log("Step 4: the Child learns its Parent's CSL schedule");

    // As in test_sleepy_router_peer.cpp: waiting for naturally-occurring traffic to carry the CSL
    // IE would make this test's timing depend on whatever housekeeping traffic happens to fire --
    // force it deterministically instead. `MessageFramer::PrepareMacHeaders()` appends the CSL IE
    // to every unicast frame Router_1 sends once its own `Mac::IsCslEnabled()` is true (a direct
    // consequence of the Tappa F fix chain), so a single ping FROM the Parent is enough.
    {
        Ip6::Address router1Mleid = router1.Get<Mle::Mle>().GetMeshLocalEid();
        Ip6::Address childMleid   = child.Get<Mle::Mle>().GetMeshLocalEid();

        nexus.SendAndVerifyEchoRequest(router1, router1Mleid, childMleid);
    }

    VerifyOrQuit(child.Get<Mle::Mle>().GetParent().IsCslSynchronized());
    VerifyOrQuit(child.Get<Mle::Mle>().GetParent().GetCslPeriod() > 0);

    Log("---------------------------------------------------------------------------------------");
    Log("Step 5: verify Router_1's radio genuinely duty-cycles while still parenting an always-on");
    Log("        Child (not just when it has no children at all)");

    {
        bool observedSleep = false;

        for (uint16_t i = 0; i < kCslPollSteps; i++)
        {
            nexus.AdvanceTime(kCslPollStepMs);

            if (router1.mRadio.mState == Radio::kStateSleep)
            {
                observedSleep = true;
                break;
            }
        }

        VerifyOrQuit(observedSleep);
    }

    Log("---------------------------------------------------------------------------------------");
    Log("Step 6: Child reaches its sleepy Parent (needs the Child -> sleepy-Parent indirect CSL");
    Log("        queue -- the direction stock OpenThread does not implement on its own)");

    {
        Ip6::Address router1Mleid = router1.Get<Mle::Mle>().GetMeshLocalEid();
        Ip6::Address childMleid   = child.Get<Mle::Mle>().GetMeshLocalEid();

        nexus.SendAndVerifyEchoRequest(child, childMleid, router1Mleid);
    }

    Log("---------------------------------------------------------------------------------------");
    Log("Step 7: the sleepy Parent still reaches its Child directly after all that indirect CSL");
    Log("        traffic (stock direction, must not have regressed)");

    {
        Ip6::Address router1Mleid = router1.Get<Mle::Mle>().GetMeshLocalEid();
        Ip6::Address childMleid   = child.Get<Mle::Mle>().GetMeshLocalEid();

        nexus.SendAndVerifyEchoRequest(router1, router1Mleid, childMleid);
    }

    nexus.SaveTestInfo("test_sleepy_parent.json");
}

} // namespace Nexus
} // namespace ot

int main(void)
{
    ot::Nexus::TestSleepyParent();
    printf("All tests passed\n");
    return 0;
}
