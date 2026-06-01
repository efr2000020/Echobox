# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

find_package(PkgConfig REQUIRED)
pkg_check_modules(SNDFILE REQUIRED sndfile)
pkg_check_modules(ALSA    REQUIRED alsa)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Module libraries (order: leaves first, then aggregators).
add_subdirectory(common)
add_subdirectory(logging)
add_subdirectory(recorder)
add_subdirectory(audio)
add_subdirectory(dsp)
add_subdirectory(app)
# In development mode, build the native validator library for Python interop.
if(ECHOBOX_DYNAMIC_PLUGINS)
    add_subdirectory(${CMAKE_SOURCE_DIR}/tools/validator/native validator_native)
endif()

add_executable(Echobox main.cpp)
target_link_libraries(Echobox PRIVATE
    ls_app
    ls_common
    ls_audio
    ls_dsp_pipeline
    ls_dsp_core
    ls_recorder
    ls_logging
    dsp_interface
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

# In dynamic-plugin (dev) builds, export symbols so dlopen'd algorithm plugins
# can resolve back into the main binary (shared logger, etc). Not needed when
# the detector is statically linked.
if(ECHOBOX_DYNAMIC_PLUGINS)
    set_target_properties(Echobox PROPERTIES ENABLE_EXPORTS ON)
endif()
