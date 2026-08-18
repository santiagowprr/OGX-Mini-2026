function(init_git_submodules EXTERNAL_DIR)
    get_filename_component(REPO_DIR "${EXTERNAL_DIR}/../.." ABSOLUTE)
    message(STATUS "Initializing submodules in ${REPO_DIR}")

    execute_process(
        COMMAND git submodule update --init --recursive
        WORKING_DIRECTORY ${REPO_DIR}
        RESULT_VARIABLE INIT_SUBMODULE_RESULT
        OUTPUT_VARIABLE INIT_SUBMODULE_OUTPUT
        ERROR_VARIABLE INIT_SUBMODULE_ERROR
    )

    if(INIT_SUBMODULE_RESULT EQUAL 0)
        message(STATUS "Submodules initialized successfully.")
    else()
        message(FATAL_ERROR "Failed to initialize submodules: ${INIT_SUBMODULE_ERROR}")
    endif()
endfunction()
