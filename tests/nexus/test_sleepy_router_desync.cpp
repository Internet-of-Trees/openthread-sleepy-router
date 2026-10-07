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

// Regression test for the "residual purge sub-case" tracked in docs/sleepy-router/DESIGN_LOG.md
// (§6, point 2) and docs/sleepy-router/ROADMAP.md (§7.3): unlike bug #12 (a Router-peer leaving
// `RouterTable` entirely, fixed by `IndirectSender::ClearAllMessagesForSleepyRouter()` -- see
// test_sleepy_router_purge.cpp), this covers a Router-peer that stays a *valid* neighbor but stops
// being CSL-synchronized.
//
// This is reachable through an entirely ordinary, pre-existing (non-fork) code path: after
// `kMaxCslTriggeredTxAttempts` (default 4) consecutive CSL NoAck failures to the same neighbor,
// `CslTxScheduler::HandleSentFrame()` calls `aCslNeighbor.SetCslSynchronized(false)` to mark it
// desynchronized -- generalized by this fork from Child-only to also apply to Router/Parent via the
// shared `CslNeighbor` base (see docs/sleepy-router/DESIGN_LOG.md, Tappa A). Critically, on this
// failure path `CslTxScheduler::HandleSentFrame()` returns *without* ever calling into
// `IndirectSender` (see the early `ExitNow()` before the call to `HandleSentFrameToCslNeighbor()`),
// so nobody ever calls `IndirectSender::RequestMessageUpdate(Router&)` to re-evaluate the Router's
// cursor (`GetIndirectMessage()`) now that it can no longer be serviced. From that point on,
// `CslTxScheduler::RescheduleCslTx()` permanently skips this Router (`!IsCslSynchronized()` ->
// `continue`), so the message sits in `MeshForwarder`'s send queue forever: never delivered, never
// dropped, never freed -- even though the Router never left `RouterTable` and the existing bug #12
// fix never triggers.
//
// Unlike test_sleepy_router_purge.cpp (which removes the peer via `Mle::RemoveNeighbor()` after the
// ~100s neighbor-aging timeout), this test keeps the peer's Thread stack disabled for only long
// enough to exhaust the CSL retry budget (a few hundred ms), well under that timeout, specifically
// to keep the Router entry valid and prove the two scenarios are distinct.

#include <stdio.h>

#include "platform/nexus_core.hpp"
#include "platform/nexus_node.hpp"

namespace ot {
namespace Nexus {

static constexpr uint32_t kFormNetworkTime    = 13 * 1000;
static constexpr uint32_t kAttachToRouterTime = 200 * 1000;
static constexpr uint32_t kCslSyncTime        = 5 * 1000;

/**
 * Step size and budget used to poll for the Router-peer's CSL desync, after enough consecutive CSL
 * NoAck failures accumulate. `kDefaultSleepyRouterCslPeriod` is documented elsewhere in this test
 * suite as ~160ms of real time per CSL window (see test_sleepy_router_chain.cpp); exhausting
 * `OPENTHREAD_CONFIG_MAC_MAX_TX_ATTEMPTS_INDIRECT_POLLS` (default 4) attempts therefore needs at
 * least ~640ms. The budget below is generous (3s) to absorb phase-alignment slack on the very first
 * window without ever getting close to `kNeighborAgingTimeout`.
 */
static constexpr uint16_t kDesyncPollSteps  = 150;
static constexpr uint32_t kDesyncPollStepMs = 20;

/**
 * Time to advance, after desync is confirmed, to prove the stuck message never gets cleaned up on
 * its own -- comfortably below `Mle::kMaxNeighborAge` (100s) so the Router-peer stays a valid
 * neighbor throughout (i.e. this is *not* the bug #12 scenario in disguise).
 */
static constexpr uint32_t kPostDesyncObservationTime = 30 * 1000;

/**
 * Time to advance past `Mle::kMaxNeighborAge` (100s), after which Router_2 is no longer a valid
 * neighbor and the queued message must have been cleaned up by `RemoveNeighbor()` (bug #12).
 */
static constexpr uint32_t kPastNeighborAgingTime = 90 * 1000;

/** Distance beyond which the Nexus radio model drops every frame (RSSI below -100 dBm). */
static constexpr float kUnreachableDistance = 2000.0f;

void TestSleepyRouterDesync(void)
{
    /**
     * Topology:
     *   Router_1 (Leader) ---- Router_2 (Sleepy Router, later goes silent just long enough to desync)
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
    Log("Step 2: Router_2 joins and upgrades to a genuine Router");

    router2.Join(router1);
    nexus.AdvanceTime(kAttachToRouterTime);
    VerifyOrQuit(router2.Get<Mle::Mle>().IsRouter());

    Log("---------------------------------------------------------------------------------------");
    Log("Step 3: Router_2 enables Sleepy Router mode; Router_1 learns its CSL schedule");

    router2.Get<Mle::Mle>().SetSleepyRouterMode(true);
    nexus.AdvanceTime(kCslSyncTime);

    {
        Ip6::Address router1Mleid = router1.Get<Mle::Mle>().GetMeshLocalEid();
        Ip6::Address router2Mleid = router2.Get<Mle::Mle>().GetMeshLocalEid();

        // Same deterministic trick as test_sleepy_router_peer.cpp: a unicast frame FROM the Sleepy
        // Router carries its CSL IE regardless of destination role, so this guarantees Router_1
        // learns the schedule instead of waiting on a Trickle timer.
        nexus.SendAndVerifyEchoRequest(router2, router2Mleid, router1Mleid);
    }

    Router *peer = router1.Get<RouterTable>().FindRouterByRloc16(router2.Get<Mle::Mle>().GetRloc16());
    VerifyOrQuit(peer != nullptr);
    VerifyOrQuit(peer->IsCslSynchronized());

    Log("---------------------------------------------------------------------------------------");
    Log("Step 4: queue a message for Router_2 indirectly, then make it go silent in the same");
    Log("        instant -- no `AdvanceTime()` in between, so the message is guaranteed to still");
    Log("        be queued (never transmitted) when Router_2 stops acknowledging");

    // Same reasoning as test_sleepy_router_purge.cpp: Nexus uses a heap-backed message pool, so
    // counting `MeshForwarder`'s own send queue is the precise (and only) way to observe the leak.
    PriorityQueue::Info sendQueueInfo;
    MessageQueue::Info  reassemblyQueueInfo;

    router1.Get<MeshForwarder>().GetQueueInfo(sendQueueInfo, reassemblyQueueInfo);
    uint16_t sendQueueBaseline = sendQueueInfo.mNumMessages;
    Log("Router_1 baseline send-queue message count: %u", sendQueueBaseline);

    {
        Ip6::Address router1Mleid = router1.Get<Mle::Mle>().GetMeshLocalEid();
        Ip6::Address router2Mleid = router2.Get<Mle::Mle>().GetMeshLocalEid();

        router1.SendEchoRequest(router2Mleid, 0, 0, 64, &router1Mleid);
    }

    // `Icmp6::SendEchoRequest()` posts the actual enqueue onto a tasklet rather than calling
    // `MeshForwarder::SendMessage()` synchronously -- `AdvanceTime(0)` pumps pending tasklets without
    // letting any simulated time (and therefore any real transmission attempt) elapse, preserving
    // the "still queued, never transmitted" guarantee this test relies on.
    nexus.AdvanceTime(0);

    router1.Get<MeshForwarder>().GetQueueInfo(sendQueueInfo, reassemblyQueueInfo);
    Log("Router_1 send-queue message count right after queueing: %u", sendQueueInfo.mNumMessages);
    VerifyOrQuit(sendQueueInfo.mNumMessages > sendQueueBaseline);

    // Router_2 goes silent (stops acknowledging), but -- unlike test_sleepy_router_purge.cpp -- only
    // for long enough to exhaust the CSL retry budget, never anywhere near the neighbor-aging
    // timeout. It stays a *valid* entry in Router_1's `RouterTable` throughout this test.
    //
    // `Core::SetNodeEnabled(false)` does not silence the Nexus radio (it still ACKs before any MAC-level
    // check), so instead move Router_2 out of range: the radio model drops anything below -100 dBm,
    // which with its path-loss parameters happens beyond ~1000 distance units.
    router2.SetPosition(kUnreachableDistance, 0);

    Log("---------------------------------------------------------------------------------------");
    Log("Step 5: poll until Router_1 gives up on CSL sync for Router_2 (consecutive NoAck exhausts");
    Log("        the retry budget), while Router_2 stays a valid (non-removed) neighbor");

    bool desynced = false;

    for (uint16_t step = 0; step < kDesyncPollSteps; step++)
    {
        nexus.AdvanceTime(kDesyncPollStepMs);

        if (!peer->IsCslSynchronized())
        {
            desynced = true;
            Log("Router_2 desynchronized after %u ms", (step + 1) * kDesyncPollStepMs);
            break;
        }
    }

    VerifyOrQuit(desynced);
    VerifyOrQuit(peer->IsStateValid());

    Log("---------------------------------------------------------------------------------------");
    Log("Step 6: confirm the queued message does not silently leak forever -- it should eventually");
    Log("        leave the send queue (delivered, converted to direct, or cleanly dropped) even");
    Log("        though Router_2 was never removed from `RouterTable`");

    nexus.AdvanceTime(kPostDesyncObservationTime);

    router1.Get<MeshForwarder>().GetQueueInfo(sendQueueInfo, reassemblyQueueInfo);
    // Informational only: while Router_2 is still a valid neighbor, the message is expected to wait
    // (a de-synchronized Router-peer is not retried until it speaks again).
    Log("Router_1 send-queue message count %u ms after desync: %u", kPostDesyncObservationTime,
        sendQueueInfo.mNumMessages);

    nexus.AdvanceTime(kPastNeighborAgingTime);

    router1.Get<MeshForwarder>().GetQueueInfo(sendQueueInfo, reassemblyQueueInfo);
    Log("Router_1 send-queue message count %u ms after desync: %u", kPostDesyncObservationTime + kPastNeighborAgingTime,
        sendQueueInfo.mNumMessages);
    Log("Router_2 still a valid neighbor of Router_1: %d", peer->IsStateValid());
    VerifyOrQuit(sendQueueInfo.mNumMessages == sendQueueBaseline);

    nexus.SaveTestInfo("test_sleepy_router_desync.json");
}

} // namespace Nexus
} // namespace ot

int main(void)
{
    ot::Nexus::TestSleepyRouterDesync();
    printf("All tests passed\n");
    return 0;
}
