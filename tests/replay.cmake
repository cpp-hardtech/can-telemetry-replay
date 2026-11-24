execute_process(
  COMMAND "${REPLAY}" "${EXAMPLES}/example.dbc" "${EXAMPLES}/replay.log"
  WORKING_DIRECTORY "${EXAMPLES}/.."
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE errors
  TIMEOUT 3
)
set(expected "0 003 GPSAltitude.GPSAltitude=-2\n500 003 GPSAltitude.GPSAltitude=5\n500 007 PowerState.PowerState=2\n")
if(NOT result STREQUAL "0" OR NOT output STREQUAL expected OR NOT errors STREQUAL "")
  message(FATAL_ERROR "Replay exit=${result}\nExpected:\n${expected}\nActual:\n${output}\nStderr:\n${errors}")
endif()

execute_process(
  COMMAND "${REPLAY}" "${CMAKE_CURRENT_LIST_DIR}/multiplex.dbc" "${CMAKE_CURRENT_LIST_DIR}/multiplex.log"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE errors
  TIMEOUT 3
)
set(expected "1 00000123 Status.Selector=1\n1 00000123 Status.Flags=2748\n2 00000123 Status.Selector=2\n2 00000123 Status.Offset=-16\n")
if(NOT result STREQUAL "0" OR NOT output STREQUAL expected OR NOT errors STREQUAL "")
  message(FATAL_ERROR "Multiplexed replay exit=${result}\nExpected:\n${expected}\nActual:\n${output}\nStderr:\n${errors}")
endif()

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/short-frame.log" "0 003 FE\n")
execute_process(
  COMMAND "${REPLAY}" "${EXAMPLES}/example.dbc" "${CMAKE_CURRENT_BINARY_DIR}/short-frame.log"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE errors
  TIMEOUT 3
)
if(NOT result STREQUAL "1" OR NOT output STREQUAL "" OR NOT errors MATCHES "short-frame[.]log:1: expected 2 payload bytes")
  message(FATAL_ERROR "Short frame exit=${result}\nStdout:\n${output}\nStderr:\n${errors}")
endif()
