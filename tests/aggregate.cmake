execute_process(
  COMMAND "${REPLAY}" --aggregate "${EXAMPLES}/example.dbc" "${EXAMPLES}/energy-replay.log"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE errors
  TIMEOUT 3
)
set(expected [=[500 004 SOC.SOCavg=20
500 006 BatteryCurrent.RawBattCurrent=20
500 006 BatteryCurrent.SmoothBattCurrent=30
500 006 BatteryCurrent.BattVoltage=110
1000 004 SOC.SOCavg=30
1000 006 BatteryCurrent.RawBattCurrent=50
1000 006 BatteryCurrent.SmoothBattCurrent=50
1000 006 BatteryCurrent.BattVoltage=130
2500 004 SOC.SOCavg=50
2500 006 BatteryCurrent.RawBattCurrent=60
2500 006 BatteryCurrent.SmoothBattCurrent=60
2500 006 BatteryCurrent.BattVoltage=140
]=])

execute_process(
  COMMAND "${REPLAY}" --aggregate "${EXAMPLES}/example.dbc" -
  INPUT_FILE "${EXAMPLES}/energy-replay.log"
  RESULT_VARIABLE stdin_result
  OUTPUT_VARIABLE stdin_output
  ERROR_VARIABLE stdin_errors
  TIMEOUT 3
)
if(NOT stdin_result STREQUAL "0" OR NOT stdin_output STREQUAL expected OR NOT stdin_errors STREQUAL "")
  message(FATAL_ERROR "Aggregate replay from stdin exit=${stdin_result}\nExpected:\n${expected}\nActual:\n${stdin_output}\nStderr:\n${stdin_errors}")
endif()

if(NOT result STREQUAL "0" OR NOT output STREQUAL expected OR NOT errors STREQUAL "")
  message(FATAL_ERROR "Aggregate replay exit=${result}\nExpected:\n${expected}\nActual:\n${output}\nStderr:\n${errors}")
endif()
