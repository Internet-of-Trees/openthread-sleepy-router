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

// Nexus port of the "purge scenario" regression test already covered on OTNS
// (pylibs/unittests/test_sleepy_router_purge.py). See docs/sleepy-router/DESIGN_LOG.md,
// bug #12: a message queued indirectly for a Sleepy Router (`Message::IsPendingForRouter()`)
// used to leak forever if that Router disappeared from the network before the message
// was ever transmitted, because `Mle::RemoveNeighbor()` had no equivalent of
// `IndirectSender::ClearAllMessagesForSleepyChild()` for a removed Router. Fixed by
// `IndirectSender::ClearAllMessagesForSleepyRouter()`.
//
// Unlike the OTNS version, this uses Nexus's whitebox C++ API instead of CLI/log
// scraping: `Core::SetNodeEnabled(id, false)` stops the peer's Thread stack in the same
// simulated instant as the fire-and-forget `SendEchoRequest()` call (no `AdvanceTime()`
// in between), guaranteeing the message is still queued when the peer goes silent, and
// `MessagePool::GetFreeBufferCount()` gives a precise before/after buffer count instead
// of parsing `bufferinfo` CLI output.

#include <stdio.h>

#include "platform/nexus_core.hpp"
#include "platform/nexus_node.hpp"

namespace ot {
namespace Nexus {

static constexpr uint32_t kFormNetworkTime   = 13 * 1000;
static constexpr uint32_t kAttachToRouterTime = 200 * 1000;
static constexpr uint32_t kCslSyncTime        = 5 * 1000;

/**
 * Time to advance past the Leader's neighbor-aging timeout for a Router
 * (`Mle::kMaxNeighborAge` = 100s) plus the Link Request retries and Link Accept
 * timeout that follow it, so `Mle::RemoveNeighbor()` fires for the now-silent peer.
 */
static constexpr uint32_t kNeighborAgingTimeout = 180 * 1000;

void TestSleepyRouterPurge(void)
{
    /**
     * Topology:
     *   Router_1 (Leader) ---- Router_2 (Sleepy Router, later goes silent)
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

        // Same deterministic trick as test_sleepy_router_peer.cpp: a unicast frame FROM
        // the Sleepy Router carries its CSL IE regardless of destination role, so this
        // guarantees Router_1 learns the schedule instead of waiting on a Trickle timer.
        nexus.SendAndVerifyEchoRequest(router2, router2Mleid, router1Mleid);
    }

    Router *peer = router1.Get<RouterTable>().FindRouterByRloc16(router2.Get<Mle::Mle>().GetRloc16());
    VerifyOrQuit(peer != nullptr);
    VerifyOrQuit(peer->IsCslSynchronized());

    Log("---------------------------------------------------------------------------------------");
    Log("Step 4: queue a message for Router_2 indirectly, then make it disappear in the same");
    Log("        instant -- no `AdvanceTime()` in between, so the message is guaranteed to");
    Log("        still be queued (never transmitted) when Router_2 goes silent");

    // Nexus configures `OPENTHREAD_CONFIG_MESSAGE_USE_HEAP_ENABLE` + `_HEAP_EXTERNAL_ENABLE` (plain
    // malloc/free, no fixed buffer pool), so `MessagePool::GetFreeBufferCount()` is a dead end here
    // (always returns the "unlimited" sentinel, `UINT16_MAX`) -- unlike on `ot-rfsim`, where the CLI
    // `bufferinfo` command's `free:` count is exactly what the OTNS Python version of this test
    // (pylibs/unittests/test_sleepy_router_purge.py) uses. Count messages in `MeshForwarder`'s own
    // send queue directly instead -- more precise anyway, since that is exactly where the leaked
    // message would sit.
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
    // `MeshForwarder::SendMessage()` synchronously -- `AdvanceTime(0)` pumps pending tasklets
    // without letting any simulated time (and therefore any real transmission attempt) elapse,
    // preserving the "still queued, never transmitted" guarantee this test relies on.
    nexus.AdvanceTime(0);

    nexus.SetNodeEnabled(router2.GetId(), false);

    router1.Get<MeshForwarder>().GetQueueInfo(sendQueueInfo, reassemblyQueueInfo);
    Log("Router_1 send-queue message count right after Router_2 goes silent: %u", sendQueueInfo.mNumMessages);
    VerifyOrQuit(sendQueueInfo.mNumMessages > sendQueueBaseline);

    Log("---------------------------------------------------------------------------------------");
    Log("Step 5: advance past Router_1's neighbor-aging timeout so `Mle::RemoveNeighbor()` fires");
    Log("        for Router_2, and verify the queued message was released, not leaked");

    nexus.AdvanceTime(kNeighborAgingTimeout);

    {
        const Router *removedPeer = router1.Get<RouterTable>().FindRouterByRloc16(router2.Get<Mle::Mle>().GetRloc16());

        VerifyOrQuit((removedPeer == nullptr) || !removedPeer->IsStateValid());
    }

    router1.Get<MeshForwarder>().GetQueueInfo(sendQueueInfo, reassemblyQueueInfo);
    Log("Router_1 send-queue message count after neighbor-aging timeout: %u", sendQueueInfo.mNumMessages);
    VerifyOrQuit(sendQueueInfo.mNumMessages == sendQueueBaseline);

    nexus.SaveTestInfo("test_sleepy_router_purge.json");
}

} // namespace Nexus
} // namespace ot

int main(void)
{
    ot::Nexus::TestSleepyRouterPurge();
    printf("All tests passed\n");
    return 0;
}
