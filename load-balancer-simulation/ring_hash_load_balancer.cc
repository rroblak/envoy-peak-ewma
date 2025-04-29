#include "ring_hash_load_balancer.h"

#include "ns3/log.h"
#include "ns3/object.h"                 // For NS_OBJECT_ENSURE_REGISTERED
#include "ns3/packet.h"                 // For Ptr<Packet>
#include "ns3/inet-socket-address.h"    // For InetSocketAddress
#include "ns3/ipv4-address.h"           // For Ipv4Address (used in address conversion for logging)
#include "ns3/uinteger.h"               // For UintegerValue
#include "ns3/simulator.h"              // For Simulator::GetContext() (used in fallback)
#include "ns3/random-variable-stream.h" // For fallback random selection (though simple modulo is used)

#include <cmath>     // For std::round, std::max, std::min
#include <vector>
#include <string>
#include <sstream>   // For std::ostringstream, std::to_string
#include <algorithm> // For std::max, std::min
#include <map>       // For std::map (already in .h)

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("RingHashLoadBalancer");
NS_OBJECT_ENSURE_REGISTERED(RingHashLoadBalancer);

// Define static const members
const uint64_t RingHashLoadBalancer::DefaultMinRingSize = 1024;
const uint64_t RingHashLoadBalancer::DefaultMaxRingSize = 8 * 1024 * 1024; // 8M
const uint32_t RingHashLoadBalancer::DefaultHashesPerHost = 100; // Baseline virtual nodes per host

TypeId RingHashLoadBalancer::GetTypeId()
{
    static TypeId tid = TypeId("ns3::RingHashLoadBalancer")
                            .SetParent<LoadBalancerApp>()
                            .SetGroupName("Applications")
                            .AddConstructor<RingHashLoadBalancer>()
                            .AddAttribute("MinRingSize",
                                          "Minimum number of virtual node entries in the hash ring.",
                                          UintegerValue(DefaultMinRingSize),
                                          MakeUintegerAccessor(&RingHashLoadBalancer::m_minRingSize),
                                          MakeUintegerChecker<uint64_t>(1)) // Min ring size must be at least 1
                            .AddAttribute("MaxRingSize",
                                          "Maximum number of virtual node entries in the hash ring.",
                                          UintegerValue(DefaultMaxRingSize),
                                          MakeUintegerAccessor(&RingHashLoadBalancer::m_maxRingSize),
                                          MakeUintegerChecker<uint64_t>(1)); // Max ring size must be at least 1
    return tid;
}

RingHashLoadBalancer::RingHashLoadBalancer()
    : m_minRingSize(DefaultMinRingSize), // Will be set by attribute or default
      m_maxRingSize(DefaultMaxRingSize)  // Will be set by attribute or default
      // m_hasher is default constructed
{
    NS_LOG_FUNCTION(this);
}

RingHashLoadBalancer::~RingHashLoadBalancer()
{
    NS_LOG_FUNCTION(this);
}

void RingHashLoadBalancer::SetBackends(const std::vector<std::pair<InetSocketAddress, uint32_t>>& backends)
{
    NS_LOG_FUNCTION(this);
    LoadBalancerApp::SetBackends(backends);
    RecalculateRing();
}

void RingHashLoadBalancer::AddBackend(const InetSocketAddress& backendAddress, uint32_t weight)
{
    NS_LOG_FUNCTION(this << backendAddress << weight);
    LoadBalancerApp::AddBackend(backendAddress, weight);
    RecalculateRing();
}

void RingHashLoadBalancer::AddBackend(const InetSocketAddress& backendAddress)
{
    NS_LOG_FUNCTION(this << backendAddress);
    LoadBalancerApp::AddBackend(backendAddress); // Base class adds with default weight
    RecalculateRing();
}

void RingHashLoadBalancer::RecalculateRing() {
    NS_LOG_FUNCTION(this);
    m_ring.clear(); // Clear the existing ring before rebuilding

    if (m_backends.empty()) {
        NS_LOG_WARN("RingHash LB: No backends available. Ring remains empty.");
        return;
    }

    // Validate and adjust ring size configuration
    if (m_minRingSize > m_maxRingSize) {
        NS_LOG_ERROR("RingHash LB Config Error: MinRingSize (" << m_minRingSize
                      << ") > MaxRingSize (" << m_maxRingSize << "). Adjusting MinRingSize to MaxRingSize.");
        m_minRingSize = m_maxRingSize;
    }
    if (m_maxRingSize == 0) { // Should be caught by attribute checker (>=1) but defensive
        NS_LOG_ERROR("RingHash LB Config Error: MaxRingSize is 0. Setting to default: " << DefaultMaxRingSize);
        m_maxRingSize = DefaultMaxRingSize;
        if (m_minRingSize > m_maxRingSize) m_minRingSize = m_maxRingSize; // Re-check min
    }
     if (m_minRingSize == 0) { // Should be caught by attribute checker (>=1)
        NS_LOG_ERROR("RingHash LB Config Error: MinRingSize is 0. Setting to 1.");
        m_minRingSize = 1;
     }


    double totalWeight = 0.0;
    uint32_t positiveWeightBackendCount = 0;
    for (const auto& backendInfo : m_backends) {
        if (backendInfo.weight > 0) {
            totalWeight += static_cast<double>(backendInfo.weight);
            positiveWeightBackendCount++;
        }
    }

    if (totalWeight <= 0.0 || positiveWeightBackendCount == 0) {
        NS_LOG_WARN("RingHash LB: All backends have zero or negative total weight. Ring remains empty.");
        return;
    }

    // Determine the target number of total hashes (virtual nodes) for the ring
    // This aims for roughly DefaultHashesPerHost per *average-weighted* host, scaled by total weight,
    // then clamped by min/max ring size.
    // A simpler approach: target_total_hashes = positiveWeightBackendCount * DefaultHashesPerHost;
    // The scaling factor can be complex; using a direct multiplication by positiveWeightBackendCount
    // and then clamping is common. Envoy's approach is more nuanced.
    // Let's use a simpler scaling for now: target proportional to number of hosts.
    uint64_t desiredHashes = positiveWeightBackendCount * DefaultHashesPerHost;
    uint64_t targetTotalHashes = std::max(m_minRingSize, desiredHashes);
    targetTotalHashes = std::min(m_maxRingSize, targetTotalHashes);

    NS_LOG_INFO("RingHash LB: Recalculating Ring. PositiveWeightBackends=" << positiveWeightBackendCount
                  << ", TotalWeight=" << totalWeight << ", TargetTotalVirtualNodes=" << targetTotalHashes
                  << " (Configured MinRing=" << m_minRingSize << ", MaxRing=" << m_maxRingSize << ")");

    uint64_t actualTotalHashesGenerated = 0;
    uint64_t minHashesForAnyHost = targetTotalHashes; // Initialize with a large value
    uint64_t maxHashesForAnyHost = 0;

    for (const auto& backendInfo : m_backends) {
        if (backendInfo.weight == 0) {
            continue; // Skip zero-weight backends
        }

        // Calculate number of virtual nodes for this host based on its weight proportion
        double weightFraction = static_cast<double>(backendInfo.weight) / totalWeight;
        uint64_t numHashesForThisHost = static_cast<uint64_t>(std::round(static_cast<double>(targetTotalHashes) * weightFraction));
        // Ensure at least one virtual node if weight is positive, even if rounding leads to zero
        numHashesForThisHost = std::max(static_cast<uint64_t>(1), numHashesForThisHost);

        std::ostringstream oss;
        oss << backendInfo.address; // Convert InetSocketAddress to string for base key
        std::string baseKey = oss.str();
        uint64_t currentHostHashesCount = 0;

        for (uint64_t i = 0; i < numHashesForThisHost; ++i) {
            // Create a unique key for each virtual node: "IP:Port_virtualNodeIndex"
            std::string virtualNodeKey = baseKey + "_" + std::to_string(i);
            uint64_t hashValue = m_hasher(virtualNodeKey);

            // Insert into the ring (std::map automatically sorts by key, i.e., hashValue)
            // If a hash collision occurs, insert_or_assign will overwrite.
            // While 64-bit hash collisions are rare, they are possible.
            // More advanced implementations might use techniques to handle collisions explicitly
            // or ensure unique hash points, but for simulation, this is often sufficient.
            auto [it, inserted] = m_ring.insert_or_assign(hashValue, backendInfo.address);
            if (!inserted && it->second != backendInfo.address) { // Collision and a different backend was there
                NS_LOG_WARN("RingHash LB: Hash collision for key '" << virtualNodeKey << "' (hash=" << hashValue 
                              << "). Overwrote existing backend " << it->second << " with " << backendInfo.address);
            } else if (!inserted) { // Collision but same backend (e.g. from different virtual node key yielding same hash)
                 NS_LOG_DEBUG("RingHash LB: Hash collision for key '" << virtualNodeKey << "' (hash=" << hashValue 
                               << ") but points to the same backend " << backendInfo.address << ". No change.");
            }
            currentHostHashesCount++;
            actualTotalHashesGenerated++;
        }
        minHashesForAnyHost = std::min(minHashesForAnyHost, currentHostHashesCount);
        maxHashesForAnyHost = std::max(maxHashesForAnyHost, currentHostHashesCount);
    }

    if (m_ring.empty() && positiveWeightBackendCount > 0) {
        NS_LOG_ERROR("RingHash LB: Ring construction resulted in an empty ring despite " 
                       << positiveWeightBackendCount << " positive-weight backends. Check hashing or logic.");
        return;
    }
    
    // If minHashesForAnyHost is still its initial large value, it means no hosts were processed.
    if (minHashesForAnyHost == targetTotalHashes && positiveWeightBackendCount > 0 && !m_ring.empty()) minHashesForAnyHost = 0;


    NS_LOG_INFO("RingHash LB: Ring built. Actual VirtualNodes=" << m_ring.size()
                  << " (Targeted: " << targetTotalHashes << ", Generated before map insertion: " << actualTotalHashesGenerated << ")"
                  << ", MinVirtualNodes/Host=" << (m_ring.empty() ? 0 : minHashesForAnyHost)
                  << ", MaxVirtualNodes/Host=" << maxHashesForAnyHost);
}

bool RingHashLoadBalancer::ChooseBackend(Ptr<Packet> packet [[maybe_unused]],
                                         const Address& fromAddress [[maybe_unused]],
                                         uint64_t l7Identifier,
                                         InetSocketAddress& chosenBackend)
{
    NS_LOG_FUNCTION(this << l7Identifier);

    if (m_ring.empty()) {
        NS_LOG_WARN("RingHash LB: Ring is empty. Cannot choose backend.");
        // Fallback: If backends exist, choose one randomly.
        if (!m_backends.empty()) {
            NS_LOG_WARN("RingHash LB: Ring empty, falling back to random selection from available backends.");
            std::vector<size_t> eligibleIndices;
            for(size_t i=0; i < m_backends.size(); ++i) {
                if (m_backends[i].weight > 0) eligibleIndices.push_back(i);
            }
            if (!eligibleIndices.empty()) {
                uint32_t randIdxPos = Simulator::GetContext() % eligibleIndices.size();
                chosenBackend = m_backends[eligibleIndices[randIdxPos]].address;
                return true;
            }
        }
        NS_LOG_ERROR("RingHash LB: Ring is empty and no fallback backend found.");
        return false;
    }

    std::string keyString = std::to_string(l7Identifier); // Use L7 identifier for hashing
    uint64_t requestHash = m_hasher(keyString);
    NS_LOG_DEBUG("RingHash LB: Hashing L7Id=" << l7Identifier << " (Key='" << keyString << "') -> RequestHash=" << requestHash);

    // Find the first element in the ring whose hash value is >= requestHash
    auto it = m_ring.lower_bound(requestHash);

    // If lower_bound reaches the end, it means the requestHash is greater than all
    // keys in the ring, so wrap around to the first element of the ring.
    if (it == m_ring.end()) {
        it = m_ring.begin();
        NS_LOG_DEBUG("RingHash LB: RequestHash " << requestHash << " wrapped around to the beginning of the ring.");
    }

    // This check should ideally not be needed if m_ring.empty() is handled above,
    // but as a safeguard:
    if (it == m_ring.end()) { // Still end after wrap, implies ring is actually empty
        NS_LOG_ERROR("RingHash LB: Ring is unexpectedly empty after lookup and wrap-around attempt for L7Id=" << l7Identifier);
        return false;
    }

    chosenBackend = it->second; // The value (backend address) associated with the found hash point

    NS_LOG_INFO("RingHash LB: L7Id=" << l7Identifier << " (RequestHash=" << requestHash 
                  << ") mapped to RingPoint=" << it->first << ", ChosenBackend=" << chosenBackend);
    return true;
}

void RingHashLoadBalancer::RecordBackendLatency(InetSocketAddress backendAddress [[maybe_unused]], Time rtt [[maybe_unused]])
{
    NS_LOG_DEBUG("RingHash LB: RecordBackendLatency for " << backendAddress << " (not used).");
}

void RingHashLoadBalancer::NotifyRequestSent(InetSocketAddress backendAddress [[maybe_unused]])
{
    NS_LOG_DEBUG("RingHash LB: NotifyRequestSent for " << backendAddress << " (not used).");
}

void RingHashLoadBalancer::NotifyRequestFinished(InetSocketAddress backendAddress [[maybe_unused]])
{
    NS_LOG_DEBUG("RingHash LB: NotifyRequestFinished for " << backendAddress << " (not used).");
}

} // namespace ns3
