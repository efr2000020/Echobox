add_library(ls_audio STATIC
    AlsaAudioSource.cpp
)
target_include_directories(ls_audio
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/..
    PRIVATE ${ALSA_INCLUDE_DIRS}
)
target_link_libraries(ls_audio
    PUBLIC  ls_logging
    PRIVATE ${ALSA_LIBRARIES}
)
target_sources(ls_audio PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/IAudioSource.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/AlsaAudioSource.hpp
)
