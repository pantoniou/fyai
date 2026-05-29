if(NOT DEFINED PRIMARY OR NOT DEFINED SECONDARY OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "PRIMARY, SECONDARY, and OUTPUT are required")
endif()

if(NOT EXISTS "${PRIMARY}")
    message(FATAL_ERROR "Primary tags file does not exist: ${PRIMARY}")
endif()

if(NOT EXISTS "${SECONDARY}")
    message(FATAL_ERROR "Secondary tags file does not exist: ${SECONDARY}")
endif()

file(READ "${PRIMARY}" _primary)
file(READ "${SECONDARY}" _secondary)

string(REGEX REPLACE "(^|\n)!_TAG[^\n]*\n" "\\1" _secondary "${_secondary}")

file(WRITE "${OUTPUT}" "${_primary}")
file(APPEND "${OUTPUT}" "${_secondary}")
