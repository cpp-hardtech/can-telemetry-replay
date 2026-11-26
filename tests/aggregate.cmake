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
if(NOT result STREQUAL "0" OR NOT output STREQUAL expected OR NOT errors STREQUAL "")
  message(FATAL_ERROR "Aggregate replay exit=${result}\nExpected:\n${expected}\nActual:\n${output}\nStderr:\n${errors}")
endif()

set(mux_dbc "${CMAKE_CURRENT_BINARY_DIR}/aggregate-mux-arrival.dbc")
file(WRITE "${mux_dbc}" [=[BS_:
BU_: Sender Receiver
BO_ 291 Status: 3 Sender
 SG_ Selector M : 0|8@1+ (1,0) [0|255] "" Receiver
 SG_ A m1 : 8|8@1+ (1,0) [0|255] "" Receiver
 SG_ B m2 : 8|8@1+ (1,0) [0|255] "" Receiver
 SG_ Shared : 16|8@1+ (1,0) [0|255] "" Receiver
EV_ V2CTxTime: 0 [0|60000] "ms" 100 1 DUMMY_NODE_VECTOR1 Receiver;
EV_ StatusGroupTxFreq: 0 [0|60000] "ms" 100 2 DUMMY_NODE_VECTOR1 Receiver;
BA_DEF_ BO_ "TxGroupFreq" STRING;
BA_ "TxGroupFreq" BO_ 291 "StatusGroupTxFreq";
]=])

function(check_mux_arrival order frames expected)
  set(log "${CMAKE_CURRENT_BINARY_DIR}/aggregate-mux-${order}.log")
  file(WRITE "${log}" "${frames}100 7FF -\n")
  execute_process(
    COMMAND "${REPLAY}" --aggregate "${mux_dbc}" "${log}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
    TIMEOUT 3
  )
  if(NOT result STREQUAL "0" OR NOT output STREQUAL expected OR NOT errors STREQUAL "")
    message(FATAL_ERROR "Aggregate mux replay (${order}) exit=${result}\nExpected:\n${expected}\nActual:\n${output}\nStderr:\n${errors}")
  endif()
endfunction()

check_mux_arrival(forward [=[0 123 010A0A
0 123 021414
]=] [=[100 123 Status.Selector=2
100 123 Status.B=20
100 123 Status.Shared=20
100 123 Status.Selector=1
100 123 Status.A=10
]=])

check_mux_arrival(reversed [=[0 123 021414
0 123 010A0A
]=] [=[100 123 Status.Selector=1
100 123 Status.A=10
100 123 Status.Shared=10
100 123 Status.Selector=2
100 123 Status.B=20
]=])
