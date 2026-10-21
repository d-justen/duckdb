if(NOT SUN AND BUILD_SHELL)
  add_subdirectory(shell)
endif()

if(BUILD_SHELL)
  option(
    BUILD_DIVA_BENCHMARK
    "Build Diva support on Linux x86_64 and AArch64"
    ON)
  option(BUILD_DIVA_SVE2_BENCHMARK "Also build ARM SVE2 bit-permutation benchmarks" ON)
  option(BUILD_GRAFITE_BENCHMARK "Build Grafite support for filter_benchmark"
         ON)

  add_executable(plan_serializer utils/plan_serializer.cpp)
  target_link_libraries(plan_serializer duckdb_static
                        ${DUCKDB_EXTRA_LINK_FLAGS})
  link_extension_libraries(plan_serializer "")

  set(DUCKDB_FILTER_BENCHMARK_DIVA_ENABLED FALSE)
  if(BUILD_DIVA_BENCHMARK
     AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
     AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|aarch64|arm64)$")
    function(configure_diva target architecture)
      add_library(
        ${target} STATIC
        diva_benchmark_adapter.cpp ../third_party/diva/wormhole/kv.c
        ../third_party/diva/wormhole/lib.c ../third_party/diva/wormhole/wh.c
        ../third_party/diva/wormhole/wh_int.c)
      target_include_directories(${target} PUBLIC ../third_party/diva)
      target_compile_options(
        ${target}
        PRIVATE ${architecture} -pthread -fno-stack-protector -fno-builtin-memcpy
                -fno-builtin-memmove -fno-builtin-memcmp)
      # Fatal errors must reach the runner, not Wormhole's interactive GDB wait.
      target_compile_definitions(${target} PRIVATE NOSIGNAL)
      target_link_libraries(${target} PRIVATE Threads::Threads)
      set_target_properties(${target} PROPERTIES LINKER_LANGUAGE CXX)
    endfunction()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
      configure_diva(duckdb_diva_benchmark -march=armv8.2-a+crc)
      if(BUILD_DIVA_SVE2_BENCHMARK)
        configure_diva(duckdb_diva_benchmark_sve2 -march=armv8.2-a+sve2-bitperm+crc)
      endif()
    else()
      configure_diva(duckdb_diva_benchmark -march=native)
    endif()
    set(DUCKDB_FILTER_BENCHMARK_DIVA_ENABLED TRUE)
  endif()

  set(DUCKDB_FILTER_BENCHMARK_GRAFITE_ENABLED FALSE)
  if(BUILD_GRAFITE_BENCHMARK AND CMAKE_SYSTEM_PROCESSOR MATCHES
                                 "^(x86_64|amd64|AMD64|aarch64|arm64)$")
    add_library(duckdb_grafite_benchmark STATIC grafite_benchmark_adapter.cpp)
    target_compile_definitions(duckdb_grafite_benchmark PUBLIC SUCCINCT_LIB_SUX)
    target_include_directories(
      duckdb_grafite_benchmark PUBLIC ../third_party/grafite/include
                                      ../third_party/grafite_sux)
    set_target_properties(duckdb_grafite_benchmark PROPERTIES LINKER_LANGUAGE
                                                              CXX)
    set(DUCKDB_FILTER_BENCHMARK_GRAFITE_ENABLED TRUE)
  endif()

  add_executable(filter_benchmark filter_benchmark.cpp)
  target_link_libraries(filter_benchmark duckdb_static
                        ${DUCKDB_EXTRA_LINK_FLAGS})
  link_extension_libraries(filter_benchmark "")
  if(DUCKDB_FILTER_BENCHMARK_GRAFITE_ENABLED)
    target_link_libraries(filter_benchmark duckdb_grafite_benchmark)
    target_compile_definitions(filter_benchmark
                               PRIVATE DUCKDB_FILTER_BENCHMARK_HAS_GRAFITE=1)
  endif()
  if(DUCKDB_FILTER_BENCHMARK_DIVA_ENABLED)
    target_link_libraries(filter_benchmark duckdb_diva_benchmark)
    target_compile_definitions(filter_benchmark
                               PRIVATE DUCKDB_FILTER_BENCHMARK_HAS_DIVA=1)
  endif()
  duckdb_codesign_for_debugging(filter_benchmark)

  add_executable(range_benchmark range_benchmark.cpp)
  target_link_libraries(range_benchmark duckdb_static
                        ${DUCKDB_EXTRA_LINK_FLAGS})
  link_extension_libraries(range_benchmark "")
  if(DUCKDB_FILTER_BENCHMARK_GRAFITE_ENABLED)
    target_link_libraries(range_benchmark duckdb_grafite_benchmark)
    target_compile_definitions(range_benchmark
                               PRIVATE DUCKDB_FILTER_BENCHMARK_HAS_GRAFITE=1)
  endif()
  if(DUCKDB_FILTER_BENCHMARK_DIVA_ENABLED)
    target_link_libraries(range_benchmark duckdb_diva_benchmark)
    target_compile_definitions(range_benchmark
                               PRIVATE DUCKDB_FILTER_BENCHMARK_HAS_DIVA=1)
  endif()
  duckdb_codesign_for_debugging(range_benchmark)

  add_executable(join_pruning_benchmark join_pruning_benchmark.cpp)
  target_link_libraries(join_pruning_benchmark duckdb_static
                        ${DUCKDB_EXTRA_LINK_FLAGS})
  link_extension_libraries(join_pruning_benchmark "")
  if(DUCKDB_FILTER_BENCHMARK_GRAFITE_ENABLED)
    target_link_libraries(join_pruning_benchmark duckdb_grafite_benchmark)
    target_compile_definitions(join_pruning_benchmark
                               PRIVATE DUCKDB_FILTER_BENCHMARK_HAS_GRAFITE=1)
  endif()
  if(DUCKDB_FILTER_BENCHMARK_DIVA_ENABLED)
    target_link_libraries(join_pruning_benchmark duckdb_diva_benchmark)
    target_compile_definitions(join_pruning_benchmark
                               PRIVATE DUCKDB_FILTER_BENCHMARK_HAS_DIVA=1)
  endif()
  duckdb_codesign_for_debugging(join_pruning_benchmark)

  add_executable(filter_benchmark_tests filter_benchmark_tests.cpp)
  target_link_libraries(filter_benchmark_tests duckdb_static
                        ${DUCKDB_EXTRA_LINK_FLAGS})
  link_extension_libraries(filter_benchmark_tests "")
  if(DUCKDB_FILTER_BENCHMARK_GRAFITE_ENABLED)
    target_link_libraries(filter_benchmark_tests duckdb_grafite_benchmark)
    target_compile_definitions(filter_benchmark_tests
                               PRIVATE DUCKDB_FILTER_BENCHMARK_HAS_GRAFITE=1)
  endif()
  if(DUCKDB_FILTER_BENCHMARK_DIVA_ENABLED)
    target_link_libraries(filter_benchmark_tests duckdb_diva_benchmark)
    target_compile_definitions(filter_benchmark_tests
                               PRIVATE DUCKDB_FILTER_BENCHMARK_HAS_DIVA=1)
  endif()
  duckdb_codesign_for_debugging(filter_benchmark_tests)
  # Adapter-only validation also runs without rebuilding DuckDB itself.
  add_executable(range_filter_adapter_tests range_filter_adapter_tests.cpp)
  if(DUCKDB_FILTER_BENCHMARK_GRAFITE_ENABLED)
    target_link_libraries(range_filter_adapter_tests duckdb_grafite_benchmark)
    target_compile_definitions(range_filter_adapter_tests PRIVATE DUCKDB_FILTER_BENCHMARK_HAS_GRAFITE=1)
  endif()
  if(DUCKDB_FILTER_BENCHMARK_DIVA_ENABLED)
    target_link_libraries(range_filter_adapter_tests duckdb_diva_benchmark)
    target_compile_definitions(range_filter_adapter_tests PRIVATE DUCKDB_FILTER_BENCHMARK_HAS_DIVA=1)
    add_executable(diva_bit_tests diva_bit_tests.cpp)
    target_include_directories(diva_bit_tests PRIVATE ../third_party/diva)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
      target_compile_options(diva_bit_tests PRIVATE -march=native)
    endif()
  endif()
  if(TARGET duckdb_diva_benchmark_sve2)
    # Only Diva's adapter/dependency use SVE. The benchmark driver, PRF, Grafite
    # and DuckDB have exactly the same compilation options in both executables.
    foreach(base filter_benchmark range_benchmark join_pruning_benchmark range_filter_adapter_tests)
      add_executable(${base}_sve2 ${base}.cpp)
      if(NOT base STREQUAL "range_filter_adapter_tests")
        target_link_libraries(${base}_sve2 duckdb_static ${DUCKDB_EXTRA_LINK_FLAGS})
        link_extension_libraries(${base}_sve2 "")
      endif()
      target_link_libraries(${base}_sve2 duckdb_diva_benchmark_sve2)
      target_compile_definitions(${base}_sve2 PRIVATE DUCKDB_FILTER_BENCHMARK_HAS_DIVA=1)
      if(DUCKDB_FILTER_BENCHMARK_GRAFITE_ENABLED)
        target_link_libraries(${base}_sve2 duckdb_grafite_benchmark)
        target_compile_definitions(${base}_sve2 PRIVATE DUCKDB_FILTER_BENCHMARK_HAS_GRAFITE=1)
      endif()
    endforeach()
    add_executable(diva_bit_tests_sve2 diva_bit_tests.cpp)
    target_include_directories(diva_bit_tests_sve2 PRIVATE ../third_party/diva)
    target_compile_options(diva_bit_tests_sve2 PRIVATE -march=armv8.2-a+sve2-bitperm+crc)
  endif()
endif()
