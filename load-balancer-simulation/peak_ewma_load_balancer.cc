#include "peak_ewma_load_balancer.h"

#include "ns3/log.h"
#include "ns3/object.h"                 // For NS_OBJECT_ENSURE_REGISTERED
#include "ns3/packet.h"                 // For Ptr<Packet>
#include "ns3/inet-socket-address.h"    // For InetSocketAddress
#include "ns3/uinteger.h"               // For UintegerValue
#include "ns3/double.h"                 // For DoubleValue (though not directly used for attributes here)
#include "ns3/simulator.h"              // For Simulator::GetContext(), Simulator::Now()
#include "ns3/random-variable-stream.h" // For UniformRandomVariable (already in .h but good for context)
#include "ns3/core-module.h"            // For Time, Seconds (often includes Simulator and RVS)


#include <vector>
#include <utility>   // For std::pair, std::piecewise_construct, std::forward_as_tuple
#include <limits>    // For std::numeric_limits
#include <algorithm> // For std::find_if (not directly used here, but good for context of map operations)
#include <map>       // For std::map (already in .h)

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("PeakEwmaLoadBalancer");
NS_OBJECT_ENSURE_REGISTERED(PeakEwmaLoadBalancer);

TypeId PeakEwmaLoadBalancer::GetTypeId()
{
    static TypeId tid = TypeId("ns3::PeakEwmaLoadBalancer")
                            .SetParent<LoadBalancerApp>()
                            .SetGroupName("Applications")
                            .AddConstructor<PeakEwmaLoadBalancer>()
                            .AddAttribute("DecayTime",
                                          "The time window for EWMA decay (e.g., '10s'). "
                                          "Determines how quickly the EWMA adapts to new latency measurements.",
                                          TimeValue(Seconds(10.0)), // Default decay window
                                          MakeTimeAccessor(&PeakEwmaLoadBalancer::m_decayTime),
                                          MakeTimeChecker(Time(MilliSeconds(1)))); // Ensure decay time is positive
    return tid;
}

PeakEwmaLoadBalancer::PeakEwmaLoadBalancer()
    : m_decayTime(Seconds(10.0)), // Default, will be overridden by attribute
      m_randomGenerator(CreateObject<UniformRandomVariable>())
{
    NS_LOG_FUNCTION(this);
    // It's good practice to seed the RNG stream if it's not done by default or if specific behavior is needed.
    // Using Simulator::GetContext() can provide a unique seed per simulation run context.
    m_randomGenerator->SetStream(Simulator::GetContext());
}

PeakEwmaLoadBalancer::~PeakEwmaLoadBalancer()
{
    NS_LOG_FUNCTION(this);
}

void PeakEwmaLoadBalancer::SetBackends(const std::vector<std::pair<InetSocketAddress, uint32_t>>& backends)
{
    NS_LOG_FUNCTION(this);
    LoadBalancerApp::SetBackends(backends); // Call base to update m_backends

    m_backendMetrics.clear(); // Remove metrics for any backends that are no longer present
    for (const auto& backendInfoPair : m_backends) // Iterate through the updated m_backends from base class
    {
        // Use emplace to construct EwmaMetric in place
        m_backendMetrics.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(backendInfoPair.address), // Key for the map
            std::forward_as_tuple(m_decayTime)              // Arguments for EwmaMetric constructor
        );
        NS_LOG_DEBUG("PeakEWMA: Initialized EwmaMetric for backend " << backendInfoPair.address 
                     << " with decay time " << m_decayTime);
    }
    NS_LOG_INFO("PeakEWMA: Backend metrics map rebuilt. Size: " << m_backendMetrics.size());
}

void PeakEwmaLoadBalancer::AddBackend(const InetSocketAddress& backendAddress, uint32_t weight)
{
    NS_LOG_FUNCTION(this << backendAddress << weight);

    bool metricExisted = (m_backendMetrics.count(backendAddress) > 0);

    LoadBalancerApp::AddBackend(backendAddress, weight);

    if (!metricExisted) {
        // Only add a new metric if one didn't exist. If it existed, its metric state is preserved.
        m_backendMetrics.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(backendAddress),
            std::forward_as_tuple(m_decayTime)
        );
        NS_LOG_INFO("PeakEWMA: Added new EwmaMetric for backend " << backendAddress
                    << " with decay time " << m_decayTime);
    } else {
        NS_LOG_INFO("PeakEWMA: Backend " << backendAddress << " updated (or re-added). "
                    "Existing EwmaMetric will be used. If decay time changed globally, "
                    "existing metrics do not automatically update their decay time; "
                    "a full SetBackends or manual reset would be needed for that.");
        // Note: If m_decayTime attribute itself changes, existing EwmaMetric objects
        // won't automatically pick up the new decayTime. They are constructed with the
        // m_decayTime value at the point of their creation.
    }
}

void PeakEwmaLoadBalancer::AddBackend(const InetSocketAddress& backendAddress)
{
    NS_LOG_FUNCTION(this << backendAddress);
    bool metricExisted = (m_backendMetrics.count(backendAddress) > 0);

    LoadBalancerApp::AddBackend(backendAddress);

    if (!metricExisted) {
        m_backendMetrics.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(backendAddress),
            std::forward_as_tuple(m_decayTime)
        );
        NS_LOG_INFO("PeakEWMA: Added new EwmaMetric for backend " << backendAddress
                    << " (default weight) with decay time " << m_decayTime);
    } else {
        NS_LOG_INFO("PeakEWMA: Backend " << backendAddress
                    << " updated (default weight). Existing EwmaMetric will be used.");
    }
}

bool PeakEwmaLoadBalancer::ChooseBackend(Ptr<Packet> packet [[maybe_unused]],
                                         const Address& fromAddress [[maybe_unused]],
                                         uint64_t l7Identifier [[maybe_unused]],
                                         InetSocketAddress& chosenBackend)
{
    NS_LOG_FUNCTION(this);

    if (m_backends.empty())
    {
        NS_LOG_WARN("PeakEWMA LB: No backends available to choose from.");
        return false;
    }

    // P2C (Power of Two Choices) Selection Strategy
    if (m_backends.size() == 1) {
        chosenBackend = m_backends[0].address;
        double load = 0.0;
        auto metric_it = m_backendMetrics.find(chosenBackend);
        if(metric_it != m_backendMetrics.end()) {
            load = metric_it->second.GetLoad();
        } else {
            NS_LOG_WARN("PeakEWMA LB: Metric not found for single backend " << chosenBackend << ". Load assumed high.");
            load = std::numeric_limits<double>::max();
        }
        NS_LOG_INFO("PeakEWMA LB: Only one backend [" << chosenBackend << "], selecting it (Load: " << load << ").");
        return true;
    }

    // Select two distinct random indices from m_backends
    uint32_t idx1 = m_randomGenerator->GetInteger(0, m_backends.size() - 1);
    uint32_t idx2 = idx1;
    int attempts = 0;
    const int maxAttempts = 10; // Safeguard for small number of backends

    while (idx2 == idx1 && attempts < maxAttempts) { // Ensure two distinct choices if possible
        idx2 = m_randomGenerator->GetInteger(0, m_backends.size() - 1);
        attempts++;
    }

    if (idx1 == idx2) { // Failed to get two distinct indices
        NS_LOG_DEBUG("PeakEWMA LB (P2C): Could not get two distinct indices (Attempts: " << attempts
                     << "). Picking index " << idx1 << " by default.");
        chosenBackend = m_backends[idx1].address;
        // Log load for chosen backend
        double load = 0.0;
        auto metric_it = m_backendMetrics.find(chosenBackend);
        if(metric_it != m_backendMetrics.end()) {
            load = metric_it->second.GetLoad();
        } else {
             NS_LOG_WARN("PeakEWMA LB: Metric not found for P2C fallback backend " << chosenBackend);
             load = std::numeric_limits<double>::max();
        }
        NS_LOG_INFO("PeakEWMA LB (P2C Fallback/SingleChoice): Selected index " << idx1 << " [" << chosenBackend << "] (Load: " << load << ")");
        return true;
    }

    // Get load scores for the two chosen backends
    double load1 = std::numeric_limits<double>::max();
    double load2 = std::numeric_limits<double>::max();
    const InetSocketAddress& addr1 = m_backends[idx1].address;
    const InetSocketAddress& addr2 = m_backends[idx2].address;

    auto metric_it1 = m_backendMetrics.find(addr1);
    if (metric_it1 != m_backendMetrics.end()) {
        load1 = metric_it1->second.GetLoad();
    } else { // Tie-breaking: randomly choose
        NS_LOG_WARN("PeakEWMA LB: Metric not found for P2C candidate backend " << addr1 << " (index " << idx1 << "). Load assumed high.");
    }

    auto metric_it2 = m_backendMetrics.find(addr2);
    if (metric_it2 != m_backendMetrics.end()) {
        load2 = metric_it2->second.GetLoad();
    } else {
        NS_LOG_WARN("PeakEWMA LB: Metric not found for P2C candidate backend " << addr2 << " (index " << idx2 << "). Load assumed high.");
    }

    uint32_t chosenIdx;
    if (load1 < load2) {
        chosenIdx = idx1;
    } else if (load2 < load1) {
        chosenIdx = idx2;
    } else { // Tie-breaking: randomly choose
        chosenIdx = (m_randomGenerator->GetValue() < 0.5) ? idx1 : idx2;
        NS_LOG_DEBUG("PeakEWMA LB (P2C): Tie between index " << idx1 << " (Load: " << load1
                     << ") and " << idx2 << " (Load: " << load2 << "). Randomly choosing index " << chosenIdx);
    }
    chosenBackend = m_backends[chosenIdx].address;

    NS_LOG_INFO("PeakEWMA LB (P2C): Chose between Idx " << idx1 << " (Addr: " << addr1 << ", Load: " << load1
                << ") and Idx " << idx2 << " (Addr: " << addr2 << ", Load: " << load2
                << "). Selected Idx " << chosenIdx << " [" << chosenBackend << "].");
    return true;
}

void PeakEwmaLoadBalancer::RecordBackendLatency(InetSocketAddress backendAddress, Time rtt)
{
    NS_LOG_FUNCTION(this << backendAddress << rtt.GetMilliSeconds() << "ms");

    auto metric_it = m_backendMetrics.find(backendAddress);
    if (metric_it != m_backendMetrics.end())
    {
        metric_it->second.Observe(rtt.GetNanoSeconds());
        NS_LOG_DEBUG("PeakEWMA: Recorded RTT " << rtt.GetMilliSeconds() << "ms for backend " << backendAddress
                     << ". New cost: " << metric_it->second.GetCurrentCostNs() / 1e6 << "ms, Pending: " << metric_it->second.GetPendingRequests());
    }
    else
    {
        NS_LOG_WARN("PeakEWMA LB: Cannot record latency for unknown backend " << backendAddress);
    }
}

void PeakEwmaLoadBalancer::NotifyRequestSent(InetSocketAddress backendAddress)
{
    NS_LOG_FUNCTION(this << backendAddress);

    auto metric_it = m_backendMetrics.find(backendAddress);
    if (metric_it != m_backendMetrics.end())
    {
        metric_it->second.IncrementPending();
        NS_LOG_DEBUG("PeakEWMA: Incremented pending for backend " << backendAddress
                     << ". New pending: " << metric_it->second.GetPendingRequests());
    }
    else
    {
        NS_LOG_WARN("PeakEWMA LB: Cannot notify request sent for unknown backend " << backendAddress);
    }
}

void PeakEwmaLoadBalancer::NotifyRequestFinished(InetSocketAddress backendAddress)
{
    NS_LOG_FUNCTION(this << backendAddress);

    auto metric_it = m_backendMetrics.find(backendAddress);
    if (metric_it != m_backendMetrics.end())
    {
        metric_it->second.DecrementPending();
        NS_LOG_DEBUG("PeakEWMA: Decremented pending for backend " << backendAddress
                     << ". New pending: " << metric_it->second.GetPendingRequests());
    }
    else
    {
        NS_LOG_WARN("PeakEWMA LB: Cannot notify request finished for unknown backend " << backendAddress);
    }
}

} // namespace ns3
