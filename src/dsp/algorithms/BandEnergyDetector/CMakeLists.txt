if(ECHOBOX_DYNAMIC_PLUGINS)
    # Dev: hot-swappable shared object, dlopen'd from ./algorithms at runtime.
    add_library(BandEnergyDetector MODULE
        BandEnergyDetector.cpp
        BandEnergyDetector.hpp
    )
    target_link_libraries(BandEnergyDetector PRIVATE dsp_interface ls_logging)
    target_include_directories(BandEnergyDetector PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    set_target_properties(BandEnergyDetector PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/src/algorithms
        PREFIX ""
    )

    install(TARGETS BandEnergyDetector
            LIBRARY DESTINATION bin/algorithms)
else()
    # Release: statically linked into the single Echobox binary. No dlopen,
    # no algorithms/ directory to ship.
    add_library(BandEnergyDetector STATIC
        BandEnergyDetector.cpp
        BandEnergyDetector.hpp
    )
    target_link_libraries(BandEnergyDetector PRIVATE dsp_interface ls_logging)
    target_include_directories(BandEnergyDetector PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
endif()
