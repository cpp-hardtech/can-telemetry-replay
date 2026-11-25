execute_process(
  COMMAND "${REPLAY}" --physical "${CMAKE_CURRENT_LIST_DIR}/extended-multiplex.dbc"
    "${CMAKE_CURRENT_LIST_DIR}/extended-multiplex.log"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE errors
  TIMEOUT 3
)
set(expected [=[1 00000123 Status.Mode=1
1 00000123 Status.Flags=2748
1 00000123 Status.Runtime=25
2 00000123 Status.Mode=2
2 00000123 Status.Detail=4
2 00000123 Status.Offset=-7 A
2 00000123 Status.Runtime=26
3 00000123 Status.Mode=3
3 00000123 Status.Detail=6
3 00000123 Status.Offset=-7 A
3 00000123 Status.Temperature=1 C
3 00000123 Status.Runtime=27
4 00000123 Status.Mode=4
4 00000123 Status.Runtime=28
5 00000123 Status.Mode=6
5 00000123 Status.Flags=2748
5 00000123 Status.Runtime=29
6 00000123 Status.Mode=3
6 00000123 Status.Detail=7
6 00000123 Status.Runtime=30
7 00000123 Status.Mode=3
7 00000123 Status.Detail=8
7 00000123 Status.Offset=-7 A
7 00000123 Status.Runtime=31
]=])
if(NOT result STREQUAL "0" OR NOT output STREQUAL expected OR NOT errors STREQUAL "")
  message(FATAL_ERROR "Extended multiplex replay exit=${result}\nExpected:\n${expected}\nActual:\n${output}\nStderr:\n${errors}")
endif()
