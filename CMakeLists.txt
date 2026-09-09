if(CONFIG_SHIELD_PROSPECTOR_ADAPTER)
    target_sources(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/behavior_prospector_brightness.c
    )
endif()
