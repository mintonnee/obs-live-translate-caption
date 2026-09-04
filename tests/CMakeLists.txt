FetchContent_Declare(Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.5.2)
FetchContent_MakeAvailable(Catch2)

include(CTest)
include(Catch)

add_executable(unit-tests
  test-main.cpp
  test-audio-convert.cpp
  test-base64.cpp
  test-backoff.cpp
  test-ring-buffer.cpp
  test-smoke.cpp
  test-caption-protocol.cpp
  test-translate-protocol.cpp
  test-caption-composer.cpp
  test-caption-wrap.cpp
  ${CMAKE_SOURCE_DIR}/src/audio-convert.cpp
  ${CMAKE_SOURCE_DIR}/src/backoff.cpp
  ${CMAKE_SOURCE_DIR}/src/base64.cpp
  ${CMAKE_SOURCE_DIR}/src/ring-buffer.cpp
  ${CMAKE_SOURCE_DIR}/src/caption-protocol.cpp
  ${CMAKE_SOURCE_DIR}/src/translate-protocol.cpp
  ${CMAKE_SOURCE_DIR}/src/caption-composer.cpp
  ${CMAKE_SOURCE_DIR}/src/caption-wrap.cpp
)
target_include_directories(unit-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(unit-tests PRIVATE Catch2::Catch2 nlohmann_json::nlohmann_json)
catch_discover_tests(unit-tests)

# Per-module test binaries for the caption pipeline (docs/specs/001 §4.8). Each
# compiles only its own slice so parallel slice work can build and run its tests
# without depending on the state of sibling slices. `unit-tests` above remains
# the full suite.
function(add_module_test name)
  add_executable(${name}
    test-main.cpp
    test-${name}.cpp
    ${ARGN}
  )
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(${name} PRIVATE Catch2::Catch2 nlohmann_json::nlohmann_json)
  catch_discover_tests(${name} TEST_PREFIX "${name}/")
endfunction()

add_module_test(caption-protocol
  ${CMAKE_SOURCE_DIR}/src/base64.cpp
  ${CMAKE_SOURCE_DIR}/src/caption-protocol.cpp
)
add_module_test(translate-protocol
  ${CMAKE_SOURCE_DIR}/src/translate-protocol.cpp
)
add_module_test(caption-composer
  ${CMAKE_SOURCE_DIR}/src/caption-wrap.cpp
  ${CMAKE_SOURCE_DIR}/src/caption-composer.cpp
)
add_module_test(caption-wrap
  ${CMAKE_SOURCE_DIR}/src/caption-wrap.cpp
)
