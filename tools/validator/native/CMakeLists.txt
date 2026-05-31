# Validator native lib: a SHARED object the Python validator loads via ctypes.
#
# Lives under tools/ because it's tooling, not production code. The top-level
# CMakeLists.txt only descends here when ECHOBOX_DYNAMIC_PLUGINS=ON, so the
# production build still doesn't touch tools/ (matching the contract spelled
# out in tools/README.md).
#
# Algorithm-agnostic: the .so does NOT link any detector. At init time it
# scans an algorithms directory and dlopens whatever plugins live there
# (same mechanism the production binary uses in dynamic-plugin mode).

add_library(echobox_validator SHARED
    validator_c_api.cpp
    validator_c_api.h
)

target_include_directories(echobox_validator
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(echobox_validator
    PRIVATE
        dsp_interface          # ISweepTracker + TrackerRegistry headers
        ls_dsp_core            # KissFftEngine, BiquadFilter
        ls_logging             # LS_INFO calls from inside the FFT/HPF code
        ${CMAKE_DL_LIBS}       # dlopen / dlsym for plugin loading
)

# Plugin .so files reference symbols (logger macros, libstdc++) that the
# validator .so provides. Exporting our symbols lets the dlopen'd plugins
# resolve back into us at load time, same way ENABLE_EXPORTS on the
# production binary lets its plugins resolve into the executable.
set_target_properties(echobox_validator PROPERTIES ENABLE_EXPORTS ON)

install(TARGETS echobox_validator LIBRARY DESTINATION lib)
