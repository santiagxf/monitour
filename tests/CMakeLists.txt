find_package(Catch2 3 CONFIG QUIET)

if(NOT Catch2_FOUND)
    message(STATUS "Catch2 not found via vcpkg — tests will not be built. "
                   "Install with: vcpkg install catch2")
    return()
endif()

add_executable(Monitour_tests
    PassiveCalibrator_tests.cpp
)

target_include_directories(Monitour_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_sources(Monitour_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/calibration/PassiveCalibrator.cpp
    ${CMAKE_SOURCE_DIR}/src/util/Logging.cpp
)

target_link_libraries(Monitour_tests PRIVATE Catch2::Catch2WithMain)

include(CTest)
include(Catch)
catch_discover_tests(Monitour_tests)
