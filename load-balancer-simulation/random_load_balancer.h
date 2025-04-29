#ifndef RANDOM_LOAD_BALANCER_H
#define RANDOM_LOAD_BALANCER_H

// Base class
#include "load_balancer.h"

// NS-3 Includes
#include "ns3/random-variable-stream.h" // For Ptr<UniformRandomVariable>
#include "ns3/nstime.h"                 // For Time (used in overridden virtual function signatures)

// Standard Library Includes
#include <vector>  // For std::vector (used in base class method signatures)
#include <utility> // For std::pair (used in base class method signatures)
#include <cstdint> // For uint32_t, uint64_t

namespace ns3 {

// Forward declarations (if base class doesn't cover them for overridden methods)
class Packet;
class Address;
class InetSocketAddress;

/**
 * @brief Implements a simple Random load balancing algorithm.
 *
 * This load balancer selects a backend server purely at random from the list
 * of available backends. It does not consider backend weights, active requests,
 * or latency metrics for its selection decision.
 */
class RandomLoadBalancer : public LoadBalancerApp
{
  public:
    /**
     * @brief Gets the TypeId for this application.
     * @return The object TypeId.
     */
    static TypeId GetTypeId();

    RandomLoadBalancer();
    virtual ~RandomLoadBalancer() override;

    // Note: For a basic RandomLoadBalancer, overriding SetBackends/AddBackend
    // is not strictly necessary as it doesn't maintain additional state that
    // needs complex recalculation when backends change (unlike Maglev or PeakEWMA).
    // The base class versions are sufficient for managing m_backends.

  protected:
    /**
     * @brief Chooses a backend server randomly from the configured list.
     *
     * @param packet The incoming request packet (content not used by this algorithm).
     * @param fromAddress The source address of the request (not used by this algorithm).
     * @param l7Identifier The L7 identifier from the request (not used by this algorithm).
     * @param[out] chosenBackend The InetSocketAddress of the randomly selected backend server.
     * @return True if a backend was successfully chosen, false otherwise (e.g., no backends available).
     */
    virtual bool ChooseBackend(Ptr<Packet> packet,
                               const Address& fromAddress,
                               uint64_t l7Identifier,
                               InetSocketAddress& chosenBackend) override;

    /**
     * @brief Records latency for a backend. No-op for RandomLoadBalancer.
     * @param backendAddress The address of the backend.
     * @param rtt The recorded round-trip time.
     */
    virtual void RecordBackendLatency(InetSocketAddress backendAddress, Time rtt) override;

    /**
     * @brief Notification that a request has been sent to a backend. No-op for RandomLoadBalancer.
     * @param backendAddress The address of the backend the request was sent to.
     */
    virtual void NotifyRequestSent(InetSocketAddress backendAddress) override;

    /**
     * @brief Notification that a request to a backend has finished. No-op for RandomLoadBalancer.
     * @param backendAddress The address of the backend whose request finished.
     */
    virtual void NotifyRequestFinished(InetSocketAddress backendAddress) override;

  private:
    Ptr<UniformRandomVariable> m_randomGenerator; //!< Random number generator for selecting backends.
};

} // namespace ns3
#endif // RANDOM_LOAD_BALANCER_H
