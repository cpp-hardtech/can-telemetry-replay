function(expect_replay scenario expected_result expected_output error_pattern)
  execute_process(
    COMMAND "${REPLAY}" ${ARGN}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
    TIMEOUT 3
  )
  if(NOT result STREQUAL expected_result OR NOT output STREQUAL expected_output OR
      NOT errors MATCHES "${error_pattern}")
    message(FATAL_ERROR "${scenario} exit=${result}, expected ${expected_result}\nExpected stdout:\n${expected_output}\nActual stdout:\n${output}\nExpected stderr pattern: ${error_pattern}\nStderr:\n${errors}")
  endif()
endfunction()

set(dbc "${CMAKE_CURRENT_BINARY_DIR}/id-filter.dbc")
file(WRITE "${dbc}" [=[
VERSION "0.1"

NS_ :

BS_:

BU_: Sender Receiver

BO_ 291 StandardReading: 1 Sender
 SG_ Value : 0|8@1+ (2,0) [0|510] "V" Receiver
BO_ 2147483939 ExtendedReading: 1 Sender
 SG_ Value : 0|8@1+ (0.5,1) [1|128.5] "V" Receiver
BO_ 292 OtherReading: 2 Sender
 SG_ Value : 0|8@1+ (1,0) [0|255] "V" Receiver
]=])

set(log "${CMAKE_CURRENT_BINARY_DIR}/id-filter.log")
file(WRITE "${log}" "0 123 05\n1 124 0100\n2 00000123 07\n3 123 09\n")
set(first_standard "0 123 StandardReading.Value=5\n")
set(last_standard "3 123 StandardReading.Value=9\n")
set(extended "2 00000123 ExtendedReading.Value=7\n")

expect_replay("Standard ID" 0 "${first_standard}${last_standard}" "^$"
  --id 123 "${dbc}" "${log}")
expect_replay("Extended ID with same numeric value" 0 "${extended}" "^$"
  --id 00000123 "${dbc}" "${log}")
expect_replay("Repeated IDs in recorded order" 0 "${first_standard}${extended}${last_standard}" "^$"
  --id 00000123 --id 123 --id 123 "${dbc}" "${log}")

set(candump_log "${CMAKE_CURRENT_BINARY_DIR}/id-filter-candump.log")
file(WRITE "${candump_log}"
  "(2.000000) can0 123#05\n(2.000001) can1 00000123#07\n(2.000001) can1 123#09\n(2.000002) can2 123#0B\n")
set(csv_header "timestamp,timestamp_unit,interface,can_id,message,signal,value,unit\n")
expect_replay("Candump ID filter with physical CSV output" 0
  "${csv_header}\"2.000001\",\"s\",\"can1\",\"00000123\",\"ExtendedReading\",\"Value\",4.5,\"V\"\n" "^$"
  --candump --id 00000123 --physical --csv "${dbc}" "${candump_log}")
expect_replay("One candump interface" 0
  "2.000001 can1 00000123 ExtendedReading.Value=7\n2.000001 can1 123 StandardReading.Value=9\n" "^$"
  --interface can1 --candump "${dbc}" "${candump_log}")
expect_replay("Repeated interfaces intersect CAN ID selection in recorded order" 0
  "2.000000 can0 123 StandardReading.Value=5\n2.000001 can1 123 StandardReading.Value=9\n" "^$"
  --candump --interface can1 --interface can0 --interface can1 --id 123 "${dbc}" "${candump_log}")
expect_replay("Candump interface and ID filters with physical CSV output" 0
  "${csv_header}\"2.000001\",\"s\",\"can1\",\"123\",\"StandardReading\",\"Value\",18,\"V\"\n" "^$"
  --candump --interface can1 --id 123 --physical --csv "${dbc}" "${candump_log}")
expect_replay("CSV without matching interfaces" 0 "${csv_header}" "^$"
  --candump --interface can9 --csv "${dbc}" "${candump_log}")

set(candump_short_log "${CMAKE_CURRENT_BINARY_DIR}/interface-filter-short-frame.log")
file(WRITE "${candump_short_log}" "(2.000000) can2 124#01\n")
expect_replay("Invalid payload length on an unselected interface" 1 ""
  "interface-filter-short-frame[.]log:1: expected 2 payload bytes"
  --candump --interface can1 "${dbc}" "${candump_short_log}")
expect_replay("Interface requires candump input" 2 "" "--interface requires --candump"
  --interface can1 "${dbc}" "${log}")
expect_replay("Interface name must be one token" 2 "" "invalid interface name for --interface"
  --candump --interface "can 1" "${dbc}" "${candump_log}")
expect_replay("Missing interface operand" 2 "" "." --candump --interface)

expect_replay("CSV without matching frames" 0 "${csv_header}" "^$"
  --csv --id 125 "${dbc}" "${log}")

expect_replay("Aggregate ID filter keeps group inputs and drains at EOF" 0
  "500 004 SOC.SOCavg=20\n1000 004 SOC.SOCavg=30\n2500 004 SOC.SOCavg=50\n" "^$"
  --aggregate --id 004 "${EXAMPLES}/example.dbc" "${EXAMPLES}/energy-replay.log")

set(short_log "${CMAKE_CURRENT_BINARY_DIR}/id-filter-short-frame.log")
file(WRITE "${short_log}" "# OtherReading has a two-byte payload\n0 124 01\n")
expect_replay("Short frame with an unselected ID" 1 ""
  "id-filter-short-frame[.]log:2: expected 2 payload bytes"
  --id 123 "${dbc}" "${short_log}")

foreach(invalid_id IN ITEMS 23 800 20000000 GGG)
  expect_replay("Invalid ID ${invalid_id}" 2 "" "." --id "${invalid_id}" "${dbc}" "${log}")
endforeach()
expect_replay("Missing ID operand" 2 "" "." --id)
