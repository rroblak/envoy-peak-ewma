build_lib_example(
    NAME main
    SOURCE_FILES
        main.cc
    LIBRARIES_TO_LINK
        load-balancer-simulation
        core          # Standard ns-3 modules
        network
        internet
        applications
        csma
        stats
        internet-apps
)
