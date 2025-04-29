#include "maglev_load_balancer.h"

#include "ns3/log.h"
#include "ns3/object.h"              // For NS_OBJECT_ENSURE_REGISTERED
#include "ns3/packet.h"              // For Ptr<Packet>
#include "ns3/inet-socket-address.h" // For InetSocketAddress
#include "ns3/ipv4-address.h"        // For Ipv4Address::GetAny()
#include "ns3/uinteger.h"            // For UintegerValue
#include "ns3/simulator.h"           // For Simulator::GetContext() in fallback

#include <cmath>     // For std::sqrt in IsPrime, std::max
#include <algorithm> // For std::max, std::min, std::sort
#include <vector>
#include <string>
#include <sstream>   // For std::ostringstream in MaglevBuildEntry comparison

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("MaglevLoadBalancer");
NS_OBJECT_ENSURE_REGISTERED(MaglevLoadBalancer);

// Define the static const member
const uint64_t MaglevLoadBalancer::DefaultTableSize = 65537; // Common prime for Maglev

TypeId MaglevLoadBalancer::GetTypeId()
{
    static TypeId tid = TypeId("ns3::MaglevLoadBalancer")
                            .SetParent<LoadBalancerApp>()
                            .SetGroupName("Applications")
                            .AddConstructor<MaglevLoadBalancer>()
                            .AddAttribute("TableSize",
                                          "Size of the Maglev lookup table (should ideally be a prime number).",
                                          UintegerValue(DefaultTableSize),
                                          MakeUintegerAccessor(&MaglevLoadBalancer::m_tableSize),
                                          MakeUintegerChecker<uint64_t>(1)); // Table size must be at least 1
    return tid;
}

MaglevLoadBalancer::MaglevLoadBalancer()
    : m_tableSize(DefaultTableSize), // Initialized by attribute or default
      m_tableBuilt(false)
      // m_hasher is default constructed
{
    NS_LOG_FUNCTION(this);
}

MaglevLoadBalancer::~MaglevLoadBalancer()
{
    NS_LOG_FUNCTION(this);
}

void MaglevLoadBalancer::SetBackends(const std::vector<std::pair<InetSocketAddress, uint32_t>>& backends)
{
    NS_LOG_FUNCTION(this);
    LoadBalancerApp::SetBackends(backends);
    BuildTable();
}

void MaglevLoadBalancer::AddBackend(const InetSocketAddress& backendAddress, uint32_t weight)
{
    NS_LOG_FUNCTION(this << backendAddress << weight);
    LoadBalancerApp::AddBackend(backendAddress, weight);
    BuildTable();
}

void MaglevLoadBalancer::AddBackend(const InetSocketAddress& backendAddress)
{
    NS_LOG_FUNCTION(this << backendAddress);
    LoadBalancerApp::AddBackend(backendAddress); // Base class adds with default weight
    BuildTable();
}

bool MaglevLoadBalancer::IsPrime(uint64_t n) {
    if (n <= 1) return false;
    if (n <= 3) return true;
    if (n % 2 == 0 || n % 3 == 0) return false;
    // Check divisibility from 5 onwards with a step of 6 (5, 7, 11, 13, ...)
    for (uint64_t i = 5; i * i <= n; i = i + 6) {
        if (n % i == 0 || n % (i + 2) == 0)
            return false;
    }
    return true;
}

void MaglevLoadBalancer::BuildTable() {
    NS_LOG_FUNCTION(this);
    m_tableBuilt = false; // Invalidate table until successfully built
    m_lookupTable.clear();

    if (m_backends.empty()) {
        NS_LOG_WARN("Maglev LB: No backends available. Lookup table cannot be built.");
        return;
    }
    if (m_tableSize == 0) {
        NS_LOG_ERROR("Maglev LB: TableSize is 0. Cannot build lookup table.");
        return;
    }

    if (!IsPrime(m_tableSize)) {
        NS_LOG_WARN("Maglev LB: Configured TableSize (" << m_tableSize
                      << ") is not prime. Maglev's properties are best with prime table sizes.");
    }

    // Internal structure for calculating permutations and weights during table build
    struct MaglevBuildEntry {
        InetSocketAddress address;
        uint32_t weight;    // Nominal weight
        uint64_t offset;    // Initial offset in the permutation sequence
        uint64_t skip;      // Step size in the permutation sequence
        uint64_t nextIndexInPermutation = 0; // Tracks current position in this backend's permutation
        double targetWeightScore = 0.0; // Score used for weighted filling passes
        uint64_t slotsFilledCount = 0;   // For debugging: count of slots assigned to this backend

        MaglevBuildEntry(InetSocketAddress a, uint32_t w, uint64_t off, uint64_t sk)
          : address(a), weight(w), offset(off), skip(sk) {}

        // Comparator for stable sorting of build entries
        bool operator<(const MaglevBuildEntry& other) const {
            if (offset != other.offset) return offset < other.offset;
            if (skip != other.skip) return skip < other.skip;
            // Tie-break using string representation of address for consistent ordering
            std::ostringstream oss_this, oss_other;
            oss_this << address;
            oss_other << other.address;
            return oss_this.str() < oss_other.str();
        }
    };

    std::vector<MaglevBuildEntry> buildEntries;
    buildEntries.reserve(m_backends.size());
    uint32_t maxWeight = 0;
    uint64_t positiveWeightBackendCount = 0;

    // Prepare build entries: calculate hashes for offset/skip and identify max weight
    for (const auto& backendInfo : m_backends) {
        if (backendInfo.weight == 0) {
            NS_LOG_DEBUG("Maglev LB: Skipping backend " << backendInfo.address << " (weight=0).");
            continue; // Zero-weight backends do not participate
        }
        positiveWeightBackendCount++;
        maxWeight = std::max(maxWeight, backendInfo.weight);

        std::ostringstream oss;
        oss << backendInfo.address; // Consistent string key for hashing
        std::string keyBase = oss.str();
        std::string keySkip = keyBase + "_skip"; // Ensure distinct hash inputs

        uint64_t offset = m_hasher(keyBase) % m_tableSize;
        uint64_t skip = (m_hasher(keySkip) % (m_tableSize - 1)) + 1; // skip > 0

        buildEntries.emplace_back(backendInfo.address, backendInfo.weight, offset, skip);
    }

    if (positiveWeightBackendCount == 0) {
        NS_LOG_WARN("Maglev LB: No backends with positive weight. Lookup table cannot be built.");
        return;
    }
    if (positiveWeightBackendCount > m_tableSize) {
        NS_LOG_WARN("Maglev LB: Number of backends with positive weight (" << positiveWeightBackendCount
                      << ") exceeds table size (" << m_tableSize
                      << "). Some backends may not get any slots in the lookup table.");
    }

    // Sort entries to ensure deterministic table generation if backend order varies but properties are same
    std::sort(buildEntries.begin(), buildEntries.end());

    m_lookupTable.assign(m_tableSize, InetSocketAddress(Ipv4Address::GetAny(), 0)); // Initialize with sentinel

    NS_LOG_INFO("Maglev LB: Building Lookup Table. Valid Backends=" << positiveWeightBackendCount
                  << ", MaxWeight=" << maxWeight << ", TableSize=" << m_tableSize);

    uint64_t filledSlotsCount = 0;
    uint32_t currentPass = 1; // Iteration counter for weighted distribution

    // Iteratively fill the lookup table
    while (filledSlotsCount < m_tableSize) {
        for (auto& entry : buildEntries) {
            // This weighting approach gives more chances to higher-weighted backends in earlier passes.
            if (static_cast<double>(currentPass) * entry.weight < entry.targetWeightScore) {
                continue;
            }
            entry.targetWeightScore += static_cast<double>(maxWeight); // Update score for next consideration

            // Find the next available slot for this backend using its permutation
            uint64_t currentPermutationValue = (entry.offset + (entry.skip * entry.nextIndexInPermutation)) % m_tableSize;
            while (m_lookupTable[currentPermutationValue].GetPort() != 0) { // Slot already filled
                entry.nextIndexInPermutation++;
                currentPermutationValue = (entry.offset + (entry.skip * entry.nextIndexInPermutation)) % m_tableSize;
            }

            m_lookupTable[currentPermutationValue] = entry.address;
            entry.nextIndexInPermutation++;
            entry.slotsFilledCount++;
            filledSlotsCount++;

            if (filledSlotsCount == m_tableSize) {
                break; // Table is full
            }
        }
        currentPass++;
        // Safety break to prevent excessively long loops (e.g., if table size is small relative to weights)
        if (currentPass > m_tableSize * 2 && filledSlotsCount < m_tableSize) { // Heuristic limit
            NS_LOG_ERROR("Maglev LB: BuildTable exceeded safety iteration limit (" << currentPass 
                         << ") but table not full (" << filledSlotsCount << "/" << m_tableSize 
                         << "). Aborting build. Check weights and table size.");
            m_lookupTable.clear(); // Invalidate the partially built table
            return;
        }
    }

    // Log statistics about table population for diagnostics
    uint64_t minSlots = m_tableSize;
    uint64_t maxSlots = 0;
    if (!buildEntries.empty()) { // Avoid issues if buildEntries was empty (though positiveWeightBackendCount check should prevent)
        for(const auto& entry : buildEntries) {
            minSlots = std::min(minSlots, entry.slotsFilledCount);
            maxSlots = std::max(maxSlots, entry.slotsFilledCount);
        }
    } else {
        minSlots = 0; // No entries, so min is 0
    }


    NS_LOG_INFO("Maglev LB: Lookup Table built. Size=" << m_lookupTable.size()
                  << ", MinSlots/Backend=" << minSlots
                  << ", MaxSlots/Backend=" << maxSlots);
    m_tableBuilt = true;
}

bool MaglevLoadBalancer::ChooseBackend(Ptr<Packet> packet [[maybe_unused]],
                                       const Address& fromAddress [[maybe_unused]],
                                       uint64_t l7Identifier,
                                       InetSocketAddress& chosenBackend)
{
    NS_LOG_FUNCTION(this << l7Identifier);

    if (!m_tableBuilt || m_lookupTable.empty()) {
        NS_LOG_WARN("Maglev LB: Lookup table not built or empty. Attempting fallback.");
        // Fallback: Simple random selection among available backends with positive weight
        if (!m_backends.empty()) {
            std::vector<size_t> eligibleFallbackIndices;
            for(size_t i = 0; i < m_backends.size(); ++i) {
                if (m_backends[i].weight > 0) {
                    eligibleFallbackIndices.push_back(i);
                }
            }
            if (!eligibleFallbackIndices.empty()) {
                // Note: Simulator::GetContext() provides some variability but isn't a strong RNG.
                // For a robust fallback, a Ptr<RandomVariableStream> member would be better.
                uint32_t randomIndexIntoEligible = Simulator::GetContext() % eligibleFallbackIndices.size();
                chosenBackend = m_backends[eligibleFallbackIndices[randomIndexIntoEligible]].address;
                NS_LOG_WARN("Maglev LB: Fallback selected backend " << chosenBackend << " randomly.");
                return true;
            } else {
                 NS_LOG_WARN("Maglev LB: Fallback failed, no backend with positive weight found.");
            }
        }
        NS_LOG_ERROR("Maglev LB: Cannot choose backend (table not built and no fallback possible).");
        return false;
    }

    // Use L7 identifier for hashing
    std::string keyString = std::to_string(l7Identifier);
    uint64_t requestHash = m_hasher(keyString);
    uint64_t tableIndex = requestHash % m_tableSize;

    chosenBackend = m_lookupTable[tableIndex];

    // Validate chosen backend (should not be the sentinel value if table built correctly)
    if (chosenBackend.GetPort() == 0 && chosenBackend.GetIpv4() == Ipv4Address::GetAny()) {
        NS_LOG_ERROR("Maglev LB: Lookup table returned an uninitialized/sentinel backend for L7Id=" << l7Identifier
                      << " (Hash=" << requestHash << ", Index=" << tableIndex 
                      << "). This indicates a flaw in table construction or an empty slot. Fallback needed.");
        // Implement a more robust fallback or error handling if this case occurs.
        // For now, indicate failure.
        return false;
    }

    NS_LOG_INFO("Maglev LB: L7Id=" << l7Identifier << " (Key='" << keyString << "', Hash=" << requestHash 
                  << ") -> TableIndex=" << tableIndex << ", ChosenBackend=" << chosenBackend);
    return true;
}

void MaglevLoadBalancer::RecordBackendLatency(InetSocketAddress backendAddress [[maybe_unused]], Time rtt [[maybe_unused]])
{
    // Maglev algorithm, in its standard form, does not use latency feedback for selection.
    NS_LOG_DEBUG("Maglev LB: RecordBackendLatency called for " << backendAddress << " (not used by Maglev).");
}

void MaglevLoadBalancer::NotifyRequestSent(InetSocketAddress backendAddress [[maybe_unused]])
{
    // Maglev does not track active requests for its primary selection logic.
    // The base class might track active requests for general stats, but Maglev itself doesn't use it.
    NS_LOG_DEBUG("Maglev LB: NotifyRequestSent for " << backendAddress << " (not directly used by Maglev selection).");
}

void MaglevLoadBalancer::NotifyRequestFinished(InetSocketAddress backendAddress [[maybe_unused]])
{
    // Similar to NotifyRequestSent, Maglev's core logic doesn't rely on this.
    NS_LOG_DEBUG("Maglev LB: NotifyRequestFinished for " << backendAddress << " (not directly used by Maglev selection).");
}

} // namespace ns3
