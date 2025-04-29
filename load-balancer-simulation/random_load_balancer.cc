#include "random_load_balancer.h"

#include "ns3/log.h"
#include "ns3/object.h"                 // For NS_OBJECT_ENSURE_REGISTERED
#include "ns3/packet.h"                 // For Ptr<Packet>
#include "ns3/inet-socket-address.h"    // For InetSocketAddress
#include "ns3/simulator.h"              // For Simulator::GetContext()
#include "ns3/random-variable-stream.h" // For UniformRandomVariable (already in .h)

// Standard Library Includes (none strictly needed beyond what ns-3 headers provide for this simple class)

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("RandomLoadBalancer");
NS_OBJECT_ENSURE_REGISTERED(RandomLoadBalancer);

TypeId RandomLoadBalancer::GetTypeId()
{
    static TypeId tid = TypeId("ns3::RandomLoadBalancer")
                            .SetParent<LoadBalancerApp>()
                            .SetGroupName("Applications")
                            .AddConstructor<RandomLoadBalancer>();
    // No specific attributes are needed for this basic RandomLoadBalancer.
    return tid;
}

RandomLoadBalancer::RandomLoadBalancer()
    : m_randomGenerator(CreateObject<UniformRandomVariable>())
{
    NS_LOG_FUNCTION(this);
    // Seed the random number generator stream using the simulation context
    // to ensure different behavior across different runs or nodes if desired.
    m_randomGenerator->SetStream(Simulator::GetContext());
}

RandomLoadBalancer::~RandomLoadBalancer()
{
    NS_LOG_FUNCTION(this);
}

bool RandomLoadBalancer::ChooseBackend(Ptr<Packet> packet [[maybe_unused]],
                                       const Address& fromAddress [[maybe_unused]],
                                       uint64_t l7Identifier [[maybe_unused]],
                                       InetSocketAddress& chosenBackend)
{
    NS_LOG_FUNCTION(this);

    // m_backends is a protected member of the base class LoadBalancerApp
    if (m_backends.empty())
    {
        NS_LOG_WARN("Random LB: No backends available to choose from.");
        return false;
    }

    // Select a random index. GetInteger(min, max) is inclusive.
    uint32_t randomIndex = m_randomGenerator->GetInteger(0, m_backends.size() - 1);

    // The ns3::UniformRandomVariable should guarantee the index is within bounds [0, m_backends.size() - 1].
    // A check like `if (randomIndex >= m_backends.size())` would typically indicate an issue with the RNG or logic.
    // However, given the contract of GetInteger, it's generally safe.

    chosenBackend = m_backends[randomIndex].address;

    NS_LOG_INFO("Random LB: Selected backend at index " << randomIndex
                  << " [" << chosenBackend << "]");

    return true;
}

void RandomLoadBalancer::RecordBackendLatency(InetSocketAddress backendAddress [[maybe_unused]], Time rtt [[maybe_unused]])
{
    // The RandomLoadBalancer does not use latency information for its decisions.
    NS_LOG_DEBUG("Random LB: RecordBackendLatency called for " << backendAddress << " (not used).");
}

void RandomLoadBalancer::NotifyRequestSent(InetSocketAddress backendAddress [[maybe_unused]])
{
    // The RandomLoadBalancer does not need to track active requests for its selection logic.
    NS_LOG_DEBUG("Random LB: NotifyRequestSent for " << backendAddress << " (not used).");
}

void RandomLoadBalancer::NotifyRequestFinished(InetSocketAddress backendAddress [[maybe_unused]])
{
    // The RandomLoadBalancer does not need to track finished requests for its selection logic.
    NS_LOG_DEBUG("Random LB: NotifyRequestFinished for " << backendAddress << " (not used).");
}

} // namespace ns3
