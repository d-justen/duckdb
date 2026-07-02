if(NOT SUN AND BUILD_SHELL)
  add_subdirectory(shell)
endif()

if(BUILD_SHELL)
  option(BUILD_DIVA_BENCHMARK "Build Diva support for filter_benchmark on supported Linux x86_64 platforms" ON)

  add_executable(plan_serializer utils/plan_serializer.cpp)
  target_link_libraries(plan_serializer duckdb_static
                        ${DUCKDB_EXTRA_LINK_FLAGS})
  link_extension_libraries(plan_serializer "")

  set(DUCKDB_FILTER_BENCHMARK_DIVA_ENABLED FALSE)
  if(BUILD_DIVA_BENCHMARK AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    add_library(duckdb_diva_benchmark STATIC
      diva_benchmark_adapter.cpp
      ../third_party/diva/wormhole/kv.c
      ../third_party/diva/wormhole/lib.c
      ../third_party/diva/wormhole/wh.c
      ../third_party/diva/wormhole/wh_int.c)
    target_include_directories(duckdb_diva_benchmark PUBLIC ../third_party/diva)
    target_compile_options(duckdb_diva_benchmark PRIVATE -march=native -pthread -fno-stack-protector
                                                         -fno-builtin-memcpy -fno-builtin-memmove -fno-builtin-memcmp)
    target_link_libraries(duckdb_diva_benchmark PRIVATE Threads::Threads)
    set_target_properties(duckdb_diva_benchmark PROPERTIES LINKER_LANGUAGE CXX)
    set(DUCKDB_FILTER_BENCHMARK_DIVA_ENABLED TRUE)
  endif()

  add_executable(filter_benchmark filter_benchmark.cpp)
  target_link_libraries(filter_benchmark duckdb_static ${DUCKDB_EXTRA_LINK_FLAGS})
  link_extension_libraries(filter_benchmark "")
  if(DUCKDB_FILTER_BENCHMARK_DIVA_ENABLED)
    target_link_libraries(filter_benchmark duckdb_diva_benchmark)
    target_compile_definitions(filter_benchmark PRIVATE DUCKDB_FILTER_BENCHMARK_HAS_DIVA=1)
  endif()
  duckdb_codesign_for_debugging(filter_benchmark)

  add_executable(filter_benchmark_tests filter_benchmark_tests.cpp)
  target_link_libraries(filter_benchmark_tests duckdb_static ${DUCKDB_EXTRA_LINK_FLAGS})
  link_extension_libraries(filter_benchmark_tests "")
  duckdb_codesign_for_debugging(filter_benchmark_tests)
endif()
