# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Replay tool (echobox-replay). Off by default; enabled via the top-level
# option(ECHOBOX_BUILD_REPLAY ...). When OFF, none of these files are
# compiled and the shipping binary is byte-identical to a build without
# the replay directory present at all — the entire target is created inside
# an `if(ECHOBOX_BUILD_REPLAY)` guard at the top-level.

# pkg-config / threads are found in src/CMakeLists.txt for the shipping
# build; tools/ is included at the top level so we re-find here rather
# than depending on cache side-effects. find_package(Threads) is safe to
# call twice; pkg_check_modules is guarded by the SNDFILE_FOUND cache.
find_package(PkgConfig REQUIRED)
if(NOT SNDFILE_FOUND)
    pkg_check_modules(SNDFILE REQUIRED sndfile)
endif()
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Small library wrapping the file-backed IAudioSource + the replay-tool
# session-header / manifest helpers, so a unit test can link them without
# pulling in main().
add_library(ls_replay_wav STATIC
    WavFileAudioSource.cpp
    SessionHeader.cpp
    RunManifest.cpp
)
target_include_directories(ls_replay_wav
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${SNDFILE_INCLUDE_DIRS}
)
target_link_libraries(ls_replay_wav
    PUBLIC  ls_audio ls_app ls_logging
    PRIVATE ${SNDFILE_LIBRARIES}
)
target_sources(ls_replay_wav PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/WavFileAudioSource.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SessionHeader.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/RunManifest.hpp
)

add_executable(echobox-replay
    main.cpp
)

# Stamp the build type into the manifest so a scorer can tell an -O3/-ffast-
# math run from a Debug run. Applied to echobox-replay (which compiles
# main.cpp, where the define is read), not to ls_replay_wav.
target_compile_definitions(echobox-replay PRIVATE
    ECHOBOX_REPLAY_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
)
target_link_libraries(echobox-replay PRIVATE
    ls_replay_wav
    ls_app
    ls_common
    ls_dsp_pipeline
    ls_dsp_core
    ls_recorder
    ls_logging
    dsp_interface
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

# Match the shipping binary's plugin-loading mode. When dynamic plugins are
# on, main.cpp uses TrackerRegistry::scanPlugins(); when off, it registers
# a statically-linked detector via extern "C" entry points and must link
# BandEnergyDetector directly.
if(ECHOBOX_DYNAMIC_PLUGINS)
    target_compile_definitions(echobox-replay PRIVATE ECHOBOX_DYNAMIC_PLUGINS=1)
    # Plugins dlopen'd at runtime need to resolve back into the logger; the
    # shipping binary flips ENABLE_EXPORTS for the same reason.
    set_target_properties(echobox-replay PROPERTIES ENABLE_EXPORTS ON)
else()
    target_link_libraries(echobox-replay PRIVATE BandEnergyDetector)
endif()

install(TARGETS echobox-replay RUNTIME DESTINATION bin)
