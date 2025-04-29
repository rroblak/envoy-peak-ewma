#ifndef MAGLEV_LOAD_BALANCER_H
#define MAGLEV_LOAD_BALANCER_H

// Base class
#include "load_balancer.h"

// NS-3 Includes (none strictly needed here beyond base class if Ptr types are from base)
#include "ns3/nstime.h" // For Time in overridden virtual function signatures

// Standard Library Includes
#include <vector>
#include <string>     // For std::string (used by std::hash<std::string>)
#include <functional> // For std::hash
#include <cstdint>    // For uint32_t, uint64_t
#include <utility>    // For std::pair (used in base class method signatures)

namespace ns3 {

// Forward declarations (if base class doesn't cover them for overridden methods)
class Packet;
class Address;
class InetSocketAddress;

/**
 * @brief Implements Maglev consistent hashing load balancing.
 *
 * Maglev is a consistent hashing algorithm that generates a lookup table.
 * Each entry in the table points to a backend server. The table is populated
 * such that the number of entries for each backend is proportional to its weight.
 * Client requests (identified by an L7 identifier) are hashed to an entry
 * in this table to determine the backend server.
 * This approach provides good load distribution and minimizes disruptions
 * when the set of backend servers changes.
 *
 * @see Google's Maglev paper: "Maglev: A Fast and Reliable Software Network Load Balancer"
 */
class MaglevLoadBalancer : public LoadBalancerApp
{
  public:
    /**
     * @brief Gets the TypeId for this application.
     * @return The object TypeId.
     */
    static TypeId GetTypeId();

    MaglevLoadBalancer();
    virtual ~MaglevLoadBalancer() override;

    // Override backend management methods to trigger lookup table rebuild
    virtual void SetBackends(const std::vector<std::pair<InetSocketAddress, uint32_t>>& backends) override;
    virtual void AddBackend(const InetSocketAddress& backendAddress, uint32_t weight) override;
    virtual void AddBackend(const InetSocketAddress& backendAddress) override; // Adds with default weight

  protected:
    /**
     * @brief Chooses a backend using the Maglev lookup table.
     * The L7 identifier from the request is hashed to find an index in the table.
     * @param packet The incoming request packet (content not directly used by Maglev selection).
     * @param fromAddress The source address of the request (not directly used by Maglev selection).
     * @param l7Identifier The Layer 7 identifier from the request, used for hashing.
     * @param[out] chosenBackend The InetSocketAddress of the selected backend server.
     * @return True if a backend was successfully chosen, false otherwise (e.g., table not built).
     */
    virtual bool ChooseBackend(Ptr<Packet> packet,
                               const Address& fromAddress,
                               uint64_t l7Identifier,
                               InetSocketAddress& chosenBackend) override;

    /**
     * @brief Records latency for a backend. No-op for MaglevLoadBalancer.
     * Maglev's primary selection mechanism does not use active latency feedback.
     * @param backendAddress The address of the backend.
     * @param rtt The recorded round-trip time.
     */
    virtual void RecordBackendLatency(InetSocketAddress backendAddress, Time rtt) override;

    /**
     * @brief Notification that a request has been sent to a backend. No-op for MaglevLoadBalancer.
     * Maglev does not use active request counts for its selection logic.
     * @param backendAddress The address of the backend the request was sent to.
     */
    virtual void NotifyRequestSent(InetSocketAddress backendAddress) override;

    /**
     * @brief Notification that a request to a backend has finished. No-op for MaglevLoadBalancer.
     * Maglev does not use active request counts for its selection logic.
     * @param backendAddress The address of the backend whose request finished.
     */
    virtual void NotifyRequestFinished(InetSocketAddress backendAddress) override;

  private:
    /**
     * @brief Rebuilds the Maglev lookup table based on the current set of backend servers and their weights.
     * This method should be called whenever the backend configuration changes.
     */
    void BuildTable();

    /**
     * @brief A simple primality test. Maglev tables perform best with prime sizes.
     * @param n The number to test for primality.
     * @return True if n is prime, false otherwise.
     */
    bool IsPrime(uint64_t n);

    // --- Maglev Specific Member Variables ---
    uint64_t m_tableSize; //!< Size of the Maglev lookup table (attribute, should ideally be prime).
    
    /**
     * @brief Default size for the Maglev lookup table, chosen as a common prime number.
     * (e.g., Google paper section 5.3 & Envoy default use 65537).
     */
    static const uint64_t DefaultTableSize;

    //! The Maglev lookup table, mapping hash indices to backend server addresses.
    std::vector<InetSocketAddress> m_lookupTable;

    std::hash<std::string> m_hasher; //!< Standard string hasher used for generating permutations.
    bool m_tableBuilt;               //!< Flag indicating if the lookup table has been successfully built.
};

} // namespace ns3
#endif // MAGLEV_LOAD_BALANCER_H
