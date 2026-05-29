find_program(CTAGS ctags)

if(CTAGS)
    if(TARGET libfyaml-ctags AND DEFINED libfyaml_BINARY_DIR)
        set(_fyai_libfyaml_tags "${libfyaml_BINARY_DIR}/tags")
    else()
        set(_fyai_libfyaml_tags "")
    endif()

    # Name of the build directory (for exclusion)
    get_filename_component(_build_dir_name "${CMAKE_BINARY_DIR}" NAME)

    add_custom_target(fyai-ctags-local
        COMMAND ${CTAGS}
            -R
            --exclude=${_build_dir_name}
            --exclude=.git
            --exclude=.cache
            --extra=+q
            --c-kinds=+lpx
            --fields=afikmsSzt
            -f "${CMAKE_BINARY_DIR}/tags.fyai"
            "${CMAKE_CURRENT_SOURCE_DIR}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        VERBATIM
    )

    if(_fyai_libfyaml_tags)
        add_custom_target(fyai-ctags
            COMMAND ${CMAKE_COMMAND}
                "-DPRIMARY=${_fyai_libfyaml_tags}"
                "-DSECONDARY=${CMAKE_BINARY_DIR}/tags.fyai"
                "-DOUTPUT=${CMAKE_BINARY_DIR}/tags"
                -P "${CMAKE_CURRENT_LIST_DIR}/merge-tags.cmake"
            DEPENDS libfyaml-ctags fyai-ctags-local
            VERBATIM
        )
    else()
        add_custom_target(fyai-ctags
            COMMAND ${CMAKE_COMMAND} -E copy
                "${CMAKE_BINARY_DIR}/tags.fyai"
                "${CMAKE_BINARY_DIR}/tags"
            DEPENDS fyai-ctags-local
            VERBATIM
        )
    endif()

    if(NOT TARGET ctags)
        add_custom_target(ctags DEPENDS fyai-ctags)
    endif()
endif()
