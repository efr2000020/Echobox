# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

# Collection overlay: validation-only data collection firmware. Kill-switched
# by CollectionConfig::enabled; see DATA_COLLECTION_IMPL_VALIDATION_PLAN.

add_library(ls_collection STATIC
    Session.cpp
)

target_include_directories(ls_collection
    PUBLIC ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(ls_collection
    PUBLIC
        ls_logging
    PRIVATE
        Threads::Threads
)
