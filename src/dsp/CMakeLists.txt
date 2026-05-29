# Interface + plugin registry
add_library(dsp_interface INTERFACE)
target_include_directories(dsp_interface INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_sources(dsp_interface INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/ISweepTracker.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TrackerRegistry.hpp
)

# FFT engine + biquad (the "core" reusable DSP primitives).
add_library(ls_dsp_core STATIC
    core/KissFftEngine.cpp
)
target_include_directories(ls_dsp_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core
)
target_link_libraries(ls_dsp_core PUBLIC kissfft)
target_sources(ls_dsp_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/core/IFftEngine.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/KissFftEngine.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/BiquadFilter.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Window.hpp
)

# The DSP thread (production runtime).
add_library(ls_dsp_pipeline STATIC
    DspPipeline.cpp
)
target_include_directories(ls_dsp_pipeline PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ls_dsp_pipeline
    PUBLIC  ls_dsp_core dsp_interface ls_logging ls_common ls_recorder
    PRIVATE ${CMAKE_DL_LIBS} Threads::Threads
)

# Detector plugins.
add_subdirectory(algorithms/BandEnergyDetector)
