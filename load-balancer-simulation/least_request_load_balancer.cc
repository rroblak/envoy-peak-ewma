#include "least_request_load_balancer.h"

#include "ns3/log.h"
#include "ns3/object.h" // For NS_OBJECT_ENSURE_REGISTERED
#include "ns3/packet.h" // For Ptr<Packet>
#include "ns3/inet-socket-address.h" // For InetSocketAddress
#include "ns3/random-variable-stream.h" // For UniformRandomVariable
#include "ns3/boolean.h" // For BooleanValue, though not directly used here, good for context with attributes
#include "ns3/double.h" // For DoubleValue

#include <vector>
#include <cmath>    // For std::pow, std::max
#include <limits>   // For std::numeric_limits
#include <algorithm> // For std::max if not in cmath for some compilers

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("LeastRequestLoadBalancer");
NS_OBJECT_ENSURE_REGISTERED(LeastRequestLoadBalancer); // Ensures TypeId system registration

TypeId LeastRequestLoadBalancer::GetTypeId()
{
    static TypeId tid = TypeId("ns3::LeastRequestLoadBalancer")
                            .SetParent<LoadBalancerApp>()
                            .SetGroupName("Applications")
                            .AddConstructor<LeastRequestLoadBalancer>()
                            .AddAttribute("ActiveRequestBias",
                                          "Bias for active requests in weight calculation (>= 0.0). "
                                          "Higher values give more penalty to active requests.",
                                          DoubleValue(1.0), // Default matches Envoy's default behavior
                                          MakeDoubleAccessor(&LeastRequestLoadBalancer::m_activeRequestBias),
                                          MakeDoubleChecker<double>(0.0)); // Bias must be non-negative
    return tid;
}

LeastRequestLoadBalancer::LeastRequestLoadBalancer()
    : m_weightsAreEqual(true), // Assume equal until backends are set
      m_randomGenerator(CreateObject<UniformRandomVariable>()),
      m_activeRequestBias(1.0) // Default, will be overridden by attribute if set
{
    NS_LOG_FUNCTION(this);
}

LeastRequestLoadBalancer::~LeastRequestLoadBalancer()
{
    NS_LOG_FUNCTION(this);
}

void LeastRequestLoadBalancer::SetBackends(const std::vector<std::pair<InetSocketAddress, uint32_t>>& backends)
{
    NS_LOG_FUNCTION(this);
    LoadBalancerApp::SetBackends(backends); // Call base class method
    CheckIfWeightsAreEqual(); // Update internal state based on new backends
}

void LeastRequestLoadBalancer::AddBackend(const InetSocketAddress& backendAddress, uint32_t weight)
{
    NS_LOG_FUNCTION(this << backendAddress << weight);
    LoadBalancerApp::AddBackend(backendAddress, weight); // Call base class method
    CheckIfWeightsAreEqual(); // Update internal state
}

void LeastRequestLoadBalancer::AddBackend(const InetSocketAddress& backendAddress)
{
    NS_LOG_FUNCTION(this << backendAddress);
    // This overload in base class typically adds with a default weight (e.g., 1)
    LoadBalancerApp::AddBackend(backendAddress); // Call base class method
    CheckIfWeightsAreEqual(); // Update internal state
}

void LeastRequestLoadBalancer::CheckIfWeightsAreEqual() {
    NS_LOG_FUNCTION(this);
    // m_backends is a protected member of LoadBalancerApp
    if (m_backends.size() <= 1) {
        m_weightsAreEqual = true;
        NS_LOG_DEBUG("Weights considered equal (backends count: " << m_backends.size() << ").");
        return;
    }

    const uint32_t firstWeight = m_backends[0].weight;
    for (size_t i = 1; i < m_backends.size(); ++i) {
        if (m_backends[i].weight != firstWeight) {
            m_weightsAreEqual = false;
            NS_LOG_INFO("Backend weights are NOT equal. Using Dynamic Weighted Least Request.");
            return;
        }
    }

    m_weightsAreEqual = true;
    NS_LOG_INFO("Backend weights ARE equal. Using Random Choice (Power of Two Choices) for selection.");
}


bool LeastRequestLoadBalancer::ChooseBackend(Ptr<Packet> packet [[maybe_unused]],
                                             const Address& fromAddress [[maybe_unused]],
                                             uint64_t l7Identifier [[maybe_unused]],
                                             InetSocketAddress& chosenBackend)
{
    NS_LOG_FUNCTION(this);

    if (m_backends.empty())
    {
        NS_LOG_WARN("LR LB: No backends available to choose from.");
        return false;
    }

    if (m_weightsAreEqual) {
        // --- Power of Two Choices (P2C) Logic ---
        NS_LOG_DEBUG("LR LB (Equal Weights): Using P2C selection.");
        if (m_backends.size() == 1) {
            chosenBackend = m_backends[0].address;
            NS_LOG_INFO("LR LB (P2C): Only one backend [" << chosenBackend << "], ActiveReq: "
                        << m_backends[0].activeRequests << ". Selecting it.");
            return true;
        }

        // Select two distinct random indices
        uint32_t idx1 = m_randomGenerator->GetInteger(0, m_backends.size() - 1);
        uint32_t idx2 = idx1;
        int attempts = 0;
        const int maxAttempts = 10; // Safeguard for small number of backends
        
        // Ensure two distinct choices if possible
        while (idx2 == idx1 && m_backends.size() > 1 && attempts < maxAttempts) {
            idx2 = m_randomGenerator->GetInteger(0, m_backends.size() - 1);
            attempts++;
        }
        
        if (idx1 == idx2) { // Failed to get two distinct indices (e.g., only 2 backends, picked same twice, or maxAttempts)
            NS_LOG_DEBUG("LR LB (P2C): Could not get two distinct indices (Attempts: " << attempts 
                         << "). Picking index " << idx1 << " by default.");
            chosenBackend = m_backends[idx1].address;
            return true;
        }

        uint32_t requests1 = m_backends[idx1].activeRequests;
        uint32_t requests2 = m_backends[idx2].activeRequests;
        uint32_t chosenIdx;

        if (requests1 < requests2) {
            chosenIdx = idx1;
        } else if (requests2 < requests1) {
            chosenIdx = idx2;
        } else { // Tie-breaking: randomly choose
            chosenIdx = (m_randomGenerator->GetValue() < 0.5) ? idx1 : idx2;
        }
        chosenBackend = m_backends[chosenIdx].address;

        NS_LOG_INFO("LR LB (P2C): Chose between Idx " << idx1 << " (Addr: " << m_backends[idx1].address << ", Req: " << requests1 
                    << ") and Idx " << idx2 << " (Addr: " << m_backends[idx2].address << ", Req: " << requests2 
                    << "). Selected Idx " << chosenIdx << " [" << chosenBackend << "].");
        return true;

    } else {
        // --- Dynamic Weighted Least Request Logic ---
        NS_LOG_DEBUG("LR LB (Unequal Weights): Using Dynamic Weighted Least Request (Bias: " << m_activeRequestBias << ").");

        double totalEffectiveWeight = 0.0;
        std::vector<double> backendEffectiveWeights(m_backends.size(), 0.0); // Store effective weight for each backend
        std::vector<size_t> eligibleIndices; // Indices of backends with weight > 0
        eligibleIndices.reserve(m_backends.size());

        for (size_t i = 0; i < m_backends.size(); ++i) {
            const auto& backend = m_backends[i];
            if (backend.weight == 0) { // Skip backends with zero weight
                NS_LOG_DEBUG("  Backend " << backend.address << " (Idx:" << i << ") skipped (Weight=0).");
                continue; // effective weight remains 0.0
            }

            // Denominator: (active_requests + 1)^bias. Adding 1 avoids issues with 0 active requests.
            double denominator = std::pow(static_cast<double>(backend.activeRequests) + 1.0, m_activeRequestBias);
            double effectiveWeight = 0.0;

            if (denominator > std::numeric_limits<double>::epsilon()) { // Avoid division by zero or very small numbers
                effectiveWeight = static_cast<double>(backend.weight) / denominator;
            } else { // Should be rare with +1 and bias >= 0
                effectiveWeight = static_cast<double>(backend.weight); // Effectively ignore bias
                NS_LOG_WARN("  Backend " << backend.address << " (Idx:" << i << ") denominator near zero. Effective weight approx. nominal weight.");
            }
            
            // Ensure non-negative effective weight (should be guaranteed if bias >=0 and weight >=0)
            backendEffectiveWeights[i] = std::max(0.0, effectiveWeight);
            totalEffectiveWeight += backendEffectiveWeights[i];
            eligibleIndices.push_back(i);

            NS_LOG_DEBUG("  Backend " << backend.address << " (Idx:" << i << ", Nom.W:" << backend.weight
                         << ", Req:" << backend.activeRequests
                         << ") -> Eff.W: " << backendEffectiveWeights[i]);
        }

        if (eligibleIndices.empty()) {
            NS_LOG_WARN("LR LB (Unequal Weights): No eligible backends (weight > 0) found.");
            return false;
        }

        // Fallback to P2C among eligible backends if total effective weight is too low
        if (totalEffectiveWeight <= std::numeric_limits<double>::epsilon()) {
            NS_LOG_WARN("LR LB (Unequal Weights): Total effective weight is zero or near-zero. "
                        "Falling back to P2C on " << eligibleIndices.size() << " eligible backend(s).");
            if (eligibleIndices.size() == 1) {
                chosenBackend = m_backends[eligibleIndices[0]].address;
                NS_LOG_INFO("LR LB Fallback (P2C): Only one eligible backend [" << chosenBackend << "], selecting it.");
                return true;
            }

            uint32_t eligiblePos1 = m_randomGenerator->GetInteger(0, eligibleIndices.size() - 1);
            uint32_t eligiblePos2 = eligiblePos1;
            int attempts = 0;
            const int maxAttemptsP2C = 10;
            while (eligiblePos2 == eligiblePos1 && eligibleIndices.size() > 1 && attempts < maxAttemptsP2C) {
                eligiblePos2 = m_randomGenerator->GetInteger(0, eligibleIndices.size() - 1);
                attempts++;
            }
            if (eligiblePos1 == eligiblePos2) {
                 NS_LOG_DEBUG("LR LB Fallback (P2C): Could not get two distinct eligible indices. Picking first.");
                 chosenBackend = m_backends[eligibleIndices[eligiblePos1]].address;
                 return true;
            }

            uint32_t actualIdx1 = eligibleIndices[eligiblePos1];
            uint32_t actualIdx2 = eligibleIndices[eligiblePos2];
            uint32_t requests1 = m_backends[actualIdx1].activeRequests;
            uint32_t requests2 = m_backends[actualIdx2].activeRequests;
            uint32_t chosenActualIdx;

            if (requests1 < requests2) chosenActualIdx = actualIdx1;
            else if (requests2 < requests1) chosenActualIdx = actualIdx2;
            else chosenActualIdx = (m_randomGenerator->GetValue() < 0.5) ? actualIdx1 : actualIdx2;

            chosenBackend = m_backends[chosenActualIdx].address;
            NS_LOG_INFO("LR LB Fallback (P2C): Chose between eligible Idx " << actualIdx1 << " (Req: " << requests1
                        << ") and Idx " << actualIdx2 << " (Req: " << requests2
                        << "). Selected Idx " << chosenActualIdx << " [" << chosenBackend << "].");
            return true;
        }

        // Perform weighted random selection
        double randomPick = m_randomGenerator->GetValue(0.0, totalEffectiveWeight);
        double currentSum = 0.0;

        for (size_t eligibleIdxPos = 0; eligibleIdxPos < eligibleIndices.size(); ++eligibleIdxPos) {
            size_t actualBackendIdx = eligibleIndices[eligibleIdxPos];
            currentSum += backendEffectiveWeights[actualBackendIdx];

            if (randomPick <= currentSum) {
                chosenBackend = m_backends[actualBackendIdx].address;
                NS_LOG_INFO("LR LB (Unequal Weights): Selected Idx " << actualBackendIdx << " [" << chosenBackend << "]"
                            << " (Eff.W: " << backendEffectiveWeights[actualBackendIdx] 
                            << ", TotalEff.W: " << totalEffectiveWeight
                            << ", Pick: " << randomPick << ", CumulativeSum: " << currentSum << ")");
                return true;
            }
        }

        // Fallback: Should ideally not be reached if logic is correct and totalEffectiveWeight > 0.
        // Could happen due to floating point inaccuracies if randomPick is extremely close to totalEffectiveWeight.
        size_t fallbackActualIdx = eligibleIndices.back(); // Pick the last eligible one
        chosenBackend = m_backends[fallbackActualIdx].address;
        NS_LOG_ERROR("LR LB (Unequal Weights): Weighted selection loop failed (RandomPick=" << randomPick
                     << ", TotalEff.W=" << totalEffectiveWeight << "). Picking last eligible backend Idx " 
                     << fallbackActualIdx << " [" << chosenBackend << "] as fallback.");
        return true;
    }
}

void LeastRequestLoadBalancer::RecordBackendLatency(InetSocketAddress backendAddress [[maybe_unused]], Time rtt [[maybe_unused]])
{
    NS_LOG_DEBUG("LR LB: RecordBackendLatency called for " << backendAddress << " with RTT " << rtt << " (not used by LR).");
}

void LeastRequestLoadBalancer::NotifyRequestSent(InetSocketAddress backendAddress)
{
    BackendInfo* info = FindBackendInfo(backendAddress);
    if (info) {
        info->activeRequests++;
        NS_LOG_DEBUG("LR LB: Incremented active requests for " << backendAddress << ". New count: " << info->activeRequests);
    } else {
        NS_LOG_WARN("LR LB: NotifyRequestSent for unknown backend " << backendAddress);
    }
}

void LeastRequestLoadBalancer::NotifyRequestFinished(InetSocketAddress backendAddress)
{
    BackendInfo* info = FindBackendInfo(backendAddress);
    if (info) {
        if (info->activeRequests > 0) {
            info->activeRequests--;
        } else {
            NS_LOG_WARN("LR LB: Attempted to decrement active requests below zero for " << backendAddress);
        }
        NS_LOG_DEBUG("LR LB: Decremented active requests for " << backendAddress << ". New count: " << info->activeRequests);
    } else {
        NS_LOG_WARN("LR LB: NotifyRequestFinished for unknown backend " << backendAddress);
    }
}

} // namespace ns3
