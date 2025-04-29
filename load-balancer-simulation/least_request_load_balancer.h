#ifndef LEAST_REQUEST_LOAD_BALANCER_H
#define LEAST_REQUEST_LOAD_BALANCER_H

#include "load_balancer.h" // Base class
#include "ns3/random-variable-stream.h" // For Ptr<UniformRandomVariable>
#include "ns3/nstime.h" // For Time (used in overridden virtual functions)

#include <vector>
#include <utility> // For std::pair
#include <cstdint> // For uint32_t, uint64_t

namespace ns3 {

// Forward declarations
class Packet;
class Address;
class InetSocketAddress;

/**
 * @brief A load balancer implementing the Least Request algorithm.
 *
 * This load balancer selects a backend server based on the number of active
 * Layer 7 (L7) requests currently being handled by each server.
 * - If all backend servers have equal weights, it uses the "Power of Two Choices" (P2C)
 * method: two servers are chosen randomly, and the one with fewer active
 * requests is selected.
 * - If backend servers have different weights, it uses a dynamic weighted least
 * request algorithm. The effective weight of each server is calculated as
 * `nominal_weight / (active_requests + 1)^bias`. The `activeRequestBias`
 * attribute controls the impact of active requests on this calculation.
 *
 * The active request counts are managed by the base `LoadBalancerApp` class.
 */
class LeastRequestLoadBalancer : public LoadBalancerApp
{
  public:
    /**
     * @brief Gets the TypeId for this application.
     * @return The object TypeId.
     */
    static TypeId GetTypeId();

    LeastRequestLoadBalancer();
    virtual ~LeastRequestLoadBalancer() override;

    // Override backend management methods to update internal state (m_weightsAreEqual)
    virtual void SetBackends(const std::vector<std::pair<InetSocketAddress, uint32_t>>& backends) override;
    virtual void AddBackend(const InetSocketAddress& backendAddress, uint32_t weight) override;
    virtual void AddBackend(const InetSocketAddress& backendAddress) override; // Adds with default weight

  protected:
    /**
     * @brief Core logic for choosing a backend based on the Least Request principle.
     *
     * @param packet The incoming request packet (content not used by this algorithm).
     * @param fromAddress The source address of the request (not used by this algorithm).
     * @param l7Identifier The L7 identifier from the request (not used by this algorithm).
     * @param[out] chosenBackend The InetSocketAddress of the selected backend server.
     * @return True if a backend was successfully chosen, false otherwise (e.g., no backends available).
     */
    virtual bool ChooseBackend(Ptr<Packet> packet,
                               const Address& fromAddress,
                               uint64_t l7Identifier,
                               InetSocketAddress& chosenBackend) override;

    /**
     * @brief Records latency for a backend. No-op for LeastRequestLoadBalancer.
     * @param backendAddress The address of the backend.
     * @param rtt The recorded round-trip time.
     */
    virtual void RecordBackendLatency(InetSocketAddress backendAddress, Time rtt) override;

    /**
     * @brief Notification that a request has been sent to a backend. No-op for LeastRequestLoadBalancer.
     * Active request counts are handled by the base class.
     * @param backendAddress The address of the backend the request was sent to.
     */
    virtual void NotifyRequestSent(InetSocketAddress backendAddress) override;

    /**
     * @brief Notification that a request to a backend has finished (response received or error).
     * No-op for LeastRequestLoadBalancer. Active request counts are handled by the base class.
     * @param backendAddress The address of the backend whose request finished.
     */
    virtual void NotifyRequestFinished(InetSocketAddress backendAddress) override;

  private:
    /**
     * @brief Checks if all configured backend servers have identical weights.
     * Updates the m_weightsAreEqual member flag, which determines whether
     * to use P2C or weighted least request logic.
     */
    void CheckIfWeightsAreEqual();

    // Member Variables specific to Least Request logic
    bool m_weightsAreEqual;                     //!< True if all backend weights are equal, enabling P2C.
    Ptr<UniformRandomVariable> m_randomGenerator; //!< Random number generator for P2C and weighted selection.
    double m_activeRequestBias;                 //!< Bias factor for active requests in weighted calculation (attribute).
};

} // namespace ns3
#endif // LEAST_REQUEST_LOAD_BALANCER_H
