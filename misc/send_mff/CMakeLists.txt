cmake_minimum_required(VERSION 3.16)
project(measurement_comm_server C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

add_compile_options($<$<COMPILE_LANGUAGE:C>:-Wall>)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
add_compile_options($<$<AND:$<COMPILE_LANGUAGE:C>,$<CONFIG:Release,RelWithDebInfo>>:-O3>)

add_executable(send_measurement_frame_c
  send_measurement_frame.c
)
target_link_libraries(send_measurement_frame_c PRIVATE m)
