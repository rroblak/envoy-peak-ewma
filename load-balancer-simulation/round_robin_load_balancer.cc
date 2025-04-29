#include "round_robin_load_balancer.h"

#include "ns3/log.h"
#include "ns3/object.h"                 // For NS_OBJECT_ENSURE_REGISTERED
#include "ns3/packet.h"                 // For Ptr<Packet>
#include "ns3/inet-socket-address.h"    // For InetSocketAddress
// #include "ns3/uinteger.h"            // Not directly used for attributes here
#include "ns3/core-module.h"            // For Simulator (though not directly used here, often included)

#include <numeric>   // For std::gcd (C++17 standard)
#include <vector>    // For std::vector (already in .h context)
#include <limits>    // For std::numeric_limits (not strictly needed here but good for general numeric code)
#include <algorithm> // For std::max

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("WeightedRoundRobinLoadBalancer");
NS_OBJECT_ENSURE_REGISTERED(WeightedRoundRobinLoadBalancer);

TypeId WeightedRoundRobinLoadBalancer::GetTypeId()
{
    static TypeId tid = TypeId("ns3::WeightedRoundRobinLoadBalancer")
                            .SetParent<LoadBalancerApp>()
                            .SetGroupName("Applications")
                            .AddConstructor<WeightedRoundRobinLoadBalancer>();
    // No specific attributes for basic WRR beyond what LoadBalancerApp provides.
    return tid;
}

WeightedRoundRobinLoadBalancer::WeightedRoundRobinLoadBalancer()
    : m_currentIndex(0),    // Initialize to start before the first backend conceptually
      m_currentWeight(0),   // Current weight marker starts at 0
      m_maxWeight(0),       // Will be calculated
      m_gcdWeight(0)        // Will be calculated
{
    NS_LOG_FUNCTION(this);
}

WeightedRoundRobinLoadBalancer::~WeightedRoundRobinLoadBalancer()
{
    NS_LOG_FUNCTION(this);
}

void WeightedRoundRobinLoadBalancer::SetBackends(const std::vector<std::pair<InetSocketAddress, uint32_t>>& backends)
{
    NS_LOG_FUNCTION(this);
    LoadBalancerApp::SetBackends(backends); // Base class updates m_backends
    RecalculateWrrState();                  // Update WRR specific state
}

void WeightedRoundRobinLoadBalancer::AddBackend(const InetSocketAddress& backendAddress, uint32_t weight)
{
    NS_LOG_FUNCTION(this << backendAddress << weight);
    LoadBalancerApp::AddBackend(backendAddress, weight);
    RecalculateWrrState();
}

void WeightedRoundRobinLoadBalancer::AddBackend(const InetSocketAddress& backendAddress)
{
    NS_LOG_FUNCTION(this << backendAddress);
    LoadBalancerApp::AddBackend(backendAddress); // Base class adds with default weight
    RecalculateWrrState();
}

bool WeightedRoundRobinLoadBalancer::ChooseBackend(Ptr<Packet> packet [[maybe_unused]],
                                                 const Address& fromAddress [[maybe_unused]],
                                                 uint64_t l7Identifier [[maybe_unused]],
                                                 InetSocketAddress& chosenBackend)
{
    NS_LOG_FUNCTION(this);

    // m_backends is a protected member of LoadBalancerApp
    if (m_backends.empty())
    {
        NS_LOG_WARN("WRR LB: No backends available.");
        return false;
    }

    // If state indicates no valid backends (e.g., all had zero weight)
    if (m_maxWeight == 0) {
        // This implies RecalculateWrrState found no backends with positive weight.
        // We could try to recalculate, but if m_backends is not empty and m_maxWeight is 0,
        // it means all configured backends have weight 0.
        NS_LOG_WARN("WRR LB: No backends with positive weight available (MaxWeight is 0).");
        // Fallback: if there's at least one backend (even if 0 weight), pick the first one.
        // This behavior might be debatable for WRR (should 0-weight ever be picked?).
        // For now, to avoid returning false if list isn't empty:
        if (!m_backends.empty()) {
            NS_LOG_WARN("WRR LB: Falling back to selecting the first backend due to all zero weights.");
            chosenBackend = m_backends[0].address;
            return true;
        }
        return false; // No backends at all, or recalculation failed to find any.
    }

    // Nginx-style Weighted Round Robin algorithm
    while (true)
    {
        m_currentIndex = (m_currentIndex + 1) % m_backends.size();

        if (m_currentIndex == 0) // Completed a full cycle through backends
        {
            m_currentWeight = m_currentWeight - m_gcdWeight;
            if (m_currentWeight <= 0)
            {
                m_currentWeight = m_maxWeight;
                if (m_maxWeight == 0) { // Should have been caught by the initial m_maxWeight check
                    NS_LOG_ERROR("WRR LB: Max weight is zero during selection loop. Inconsistent state.");
                    return false; // Prevent infinite loop or division by zero if gcdWeight was also 0
                }
            }
        }
        
        // Ensure m_currentIndex is valid before accessing m_backends
        // This check is more of a safeguard; modulo arithmetic should keep it in bounds.
        if (m_currentIndex >= m_backends.size()) {
             NS_LOG_ERROR("WRR LB: m_currentIndex (" << m_currentIndex 
                           << ") is out of bounds for m_backends.size() (" << m_backends.size() 
                           << "). This indicates a logical error.");
             return false; // Critical error
        }


        // Select backend if its weight is sufficient and positive
        if (m_backends[m_currentIndex].weight > 0 &&
            m_backends[m_currentIndex].weight >= static_cast<uint32_t>(m_currentWeight))
        {
            chosenBackend = m_backends[m_currentIndex].address;
            NS_LOG_INFO("WRR LB: Choosing backend at index " << m_currentIndex
                          << " [" << chosenBackend << "]"
                          << " (Weight: " << m_backends[m_currentIndex].weight 
                          << ", CurrentMarkerWeight: " << m_currentWeight << ")");
            return true;
        }
        // If current backend is not chosen, loop continues to find the next suitable one.
    }
    // Loop should always find a backend if m_maxWeight > 0.
    // return false; // Logically unreachable if m_maxWeight > 0
}

void WeightedRoundRobinLoadBalancer::RecalculateWrrState()
{
    NS_LOG_FUNCTION(this);
    // m_backends is a protected member of LoadBalancerApp

    if (m_backends.empty()) {
        m_maxWeight = 0;
        m_gcdWeight = 0;
        m_currentIndex = 0; // Or some other sentinel like -1 or m_backends.size()
        m_currentWeight = 0;
        NS_LOG_INFO("WRR State: No backends. MaxWeight=0, GcdWeight=0.");
        return;
    }

    m_maxWeight = 0;
    uint32_t calculatedGcd = 0; // Initialize GCD for calculation
    bool firstPositiveWeightFound = false;
    uint32_t positiveWeightCount = 0;

    for (const auto& backendInfo : m_backends) {
        if (backendInfo.weight > 0) {
            positiveWeightCount++;
            m_maxWeight = std::max(m_maxWeight, backendInfo.weight);
            if (!firstPositiveWeightFound) {
                calculatedGcd = backendInfo.weight;
                firstPositiveWeightFound = true;
            } else {
                calculatedGcd = std::gcd(calculatedGcd, backendInfo.weight);
            }
        } else {
             NS_LOG_DEBUG("WRR State: Backend " << backendInfo.address << " has zero weight, ignored for GCD/MaxW calculation.");
        }
    }

    if (positiveWeightCount == 0) { // All backends have zero weight
        m_maxWeight = 0;
        m_gcdWeight = 0; // Or 1, depending on desired behavior if all are 0. 0 makes sense.
        NS_LOG_WARN("WRR State: All configured backends have zero weight. MaxWeight=0, GcdWeight=0.");
    } else {
        m_gcdWeight = (calculatedGcd == 0 && m_maxWeight > 0) ? m_maxWeight : calculatedGcd; // If GCD is 0 but maxW >0 (e.g. single backend), GCD is maxW.
        if (m_gcdWeight == 0) { // Should only happen if all positive weights were somehow 0, or single 0 weight.
             NS_LOG_WARN("WRR State: Calculated GCD is 0 despite positive weights. Setting GCD to 1 as a fallback.");
             m_gcdWeight = 1; // Fallback to 1 if GCD calculation leads to 0 with positive weights.
        }
    }

    // Reset current index and weight for the selection algorithm.
    // Start currentIndex at the conceptual end to make the first pick m_backends[0] after (idx+1)%size.
    m_currentIndex = m_backends.empty() ? 0 : (m_backends.size() - 1); 
    m_currentWeight = 0; // Start current weight marker at 0

    NS_LOG_INFO("WRR State Recalculated: MaxWeight=" << m_maxWeight << ", GcdWeight=" << m_gcdWeight
                  << ", NumBackends=" << m_backends.size() 
                  << ", PositiveWeightBackends=" << positiveWeightCount);
}

void WeightedRoundRobinLoadBalancer::RecordBackendLatency(InetSocketAddress backendAddress [[maybe_unused]], Time rtt [[maybe_unused]])
{
    NS_LOG_DEBUG("WRR LB: RecordBackendLatency for " << backendAddress << " (not used).");
}

void WeightedRoundRobinLoadBalancer::NotifyRequestSent(InetSocketAddress backendAddress [[maybe_unused]])
{
    NS_LOG_DEBUG("WRR LB: NotifyRequestSent for " << backendAddress << " (not used).");
}

void WeightedRoundRobinLoadBalancer::NotifyRequestFinished(InetSocketAddress backendAddress [[maybe_unused]])
{
    NS_LOG_DEBUG("WRR LB: NotifyRequestFinished for " << backendAddress << " (not used).");
}

} // namespace ns3
