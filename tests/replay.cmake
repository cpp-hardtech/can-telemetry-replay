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

set(physical_log "${CMAKE_CURRENT_BINARY_DIR}/physical-values.log")
file(WRITE "${physical_log}" "0 003 FE3F\n5 006 7B00F4FF28000000\n5 007 0080\n")
execute_process(
  COMMAND "${REPLAY}" --physical "${EXAMPLES}/example.dbc" "${physical_log}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE errors
  TIMEOUT 3
)
set(expected "0 003 GPSAltitude.GPSAltitude=-2 M\n5 006 BatteryCurrent.RawBattCurrent=-798 A\n5 006 BatteryCurrent.SmoothBattCurrent=6 A\n5 006 BatteryCurrent.BattVoltage=12.3 V\n5 007 PowerState.PowerState=2\n")
if(NOT result STREQUAL "0" OR NOT output STREQUAL expected OR NOT errors STREQUAL "")
  message(FATAL_ERROR "Physical replay exit=${result}\nExpected:\n${expected}\nActual:\n${output}\nStderr:\n${errors}")
endif()

set(candump_log "${CMAKE_CURRENT_BINARY_DIR}/candump.log")
file(WRITE "${candump_log}"
  "(0.000001) vcan0 003#FE3F R\n(0.000001001) vcan0 003#0500\n(0.000002) vcan1 007#0080\n")
execute_process(
  COMMAND "${REPLAY}" --candump "${EXAMPLES}/example.dbc" "${candump_log}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE errors
  TIMEOUT 3
)
set(expected "0.000001 vcan0 003 GPSAltitude.GPSAltitude=-2\n0.000001001 vcan0 003 GPSAltitude.GPSAltitude=5\n0.000002 vcan1 007 PowerState.PowerState=2\n")
if(NOT result STREQUAL "0" OR NOT output STREQUAL expected OR NOT errors STREQUAL "")
  message(FATAL_ERROR "Candump replay exit=${result}\nExpected:\n${expected}\nActual:\n${output}\nStderr:\n${errors}")
endif()

set(csv_dbc "${CMAKE_CURRENT_BINARY_DIR}/csv-signal.dbc")
set(csv_log "${CMAKE_CURRENT_BINARY_DIR}/csv-signal.log")
file(WRITE "${csv_dbc}" [=[
VERSION "0.1"

NS_ :

BS_:

BU_: Sender Receiver

BO_ 257 Counter: 8 Sender
 SG_ Ticks : 0|64@1+ (1,0) [0|18446744073709551615] "m,s" Receiver
]=])
file(WRITE "${csv_log}" "0 101 FFFFFFFFFFFFFFFF\n")
execute_process(
  COMMAND "${REPLAY}" --csv "${csv_dbc}" "${csv_log}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE errors
  TIMEOUT 3
)
set(csv_header "timestamp,timestamp_unit,interface,can_id,message,signal,value,unit\n")
set(expected "${csv_header}\"0\",\"ms\",\"\",\"101\",\"Counter\",\"Ticks\",18446744073709551615,\"\"\n")
if(NOT result STREQUAL "0" OR NOT output STREQUAL expected OR NOT errors STREQUAL "")
  message(FATAL_ERROR "Raw CSV replay exit=${result}\nExpected:\n${expected}\nActual:\n${output}\nStderr:\n${errors}")
endif()

file(WRITE "${csv_log}" "(1.234567) vcan0 101#0500000000000000\n")
execute_process(
  COMMAND "${REPLAY}" --csv --candump --physical "${csv_dbc}" "${csv_log}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE errors
  TIMEOUT 3
)
set(expected "${csv_header}\"1.234567\",\"s\",\"vcan0\",\"101\",\"Counter\",\"Ticks\",5,\"m,s\"\n")
if(NOT result STREQUAL "0" OR NOT output STREQUAL expected OR NOT errors STREQUAL "")
  message(FATAL_ERROR "Physical CSV replay exit=${result}\nExpected:\n${expected}\nActual:\n${output}\nStderr:\n${errors}")
endif()

if(EXISTS "/dev/full")
  execute_process(
    COMMAND "${REPLAY}" --csv "${EXAMPLES}/example.dbc" "${EXAMPLES}/replay.log"
    OUTPUT_FILE "/dev/full"
    RESULT_VARIABLE result
    ERROR_VARIABLE errors
    TIMEOUT 3
  )
  if(NOT result STREQUAL "1" OR NOT errors MATCHES "could not write replay output")
    message(FATAL_ERROR "Replay with a full output device exited ${result}\nStderr:\n${errors}")
  endif()
endif()
