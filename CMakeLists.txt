cmake_minimum_required(VERSION 3.10)

project(fm95 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED YES)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

file(GLOB SRC_FILES "src/*.c")

file(GLOB FILTER_FILES "filter/*.c")
file(GLOB LIB_FILES "lib/*.c")

file(GLOB INIH_FILES "inih/*.c")

add_library(inih OBJECT ${INIH_FILES})
target_include_directories(inih PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/inih)

add_library(libfmfilter OBJECT ${FILTER_FILES})
add_library(libfm OBJECT ${LIB_FILES})

target_include_directories(libfmfilter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/filter)
target_include_directories(libfm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/lib)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(libfmfilter PRIVATE DEBUG=1)
    target_compile_definitions(libfm PRIVATE DEBUG=1)
endif()

foreach(SRC_FILE ${SRC_FILES})
    get_filename_component(EXEC_NAME ${SRC_FILE} NAME_WE)

    add_executable(${EXEC_NAME} ${SRC_FILE})
    target_compile_options(${EXEC_NAME} PRIVATE -O2 -Wall -Wextra -Werror -Wno-unused-parameter)
    
    if(EXEC_NAME STREQUAL "fm95")
        target_link_libraries(${EXEC_NAME} PRIVATE m inih pulse pulse-simple liquid pthread libfmfilter libfm)
    elseif(EXEC_NAME STREQUAL "chimer95" OR EXEC_NAME STREQUAL "sca95")
        target_link_libraries(${EXEC_NAME} PRIVATE m inih pulse pulse-simple liquid pthread libfm)
    elseif(EXEC_NAME STREQUAL "vban95")
        target_link_libraries(${EXEC_NAME} PRIVATE m pulse pulse-simple liquid pthread libfm)
    else()
        message(FATAL_ERROR "How do I link this? ${EXEC_NAME}")
    endif()

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_definitions(${EXEC_NAME} PRIVATE DEBUG=1)
    endif()

    install(TARGETS ${EXEC_NAME}
        DESTINATION /usr/bin
        PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
                    GROUP_EXECUTE GROUP_WRITE GROUP_READ
                    WORLD_EXECUTE WORLD_READ)
endforeach()
