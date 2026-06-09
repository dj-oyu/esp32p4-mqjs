if(NOT DEFINED STD_TOOL)
    message(FATAL_ERROR "STD_TOOL is not set")
endif()

if(NOT DEFINED ATOM_HEADER)
    message(FATAL_ERROR "ATOM_HEADER is not set")
endif()

if(NOT DEFINED STDLIB_HEADER)
    message(FATAL_ERROR "STDLIB_HEADER is not set")
endif()

execute_process(
    COMMAND "${STD_TOOL}" -a -m32
    OUTPUT_FILE "${ATOM_HEADER}"
    RESULT_VARIABLE atom_result
)
if(NOT atom_result EQUAL 0)
    message(FATAL_ERROR "Failed generating ${ATOM_HEADER} (exit ${atom_result})")
endif()

execute_process(
    COMMAND "${STD_TOOL}" -m32
    OUTPUT_FILE "${STDLIB_HEADER}"
    RESULT_VARIABLE stdlib_result
)
if(NOT stdlib_result EQUAL 0)
    message(FATAL_ERROR "Failed generating ${STDLIB_HEADER} (exit ${stdlib_result})")
endif()
