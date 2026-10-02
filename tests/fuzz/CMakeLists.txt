include(CheckCXXSourceCompiles)

set(CGRAPH_SAVED_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS}")
set(CGRAPH_SAVED_REQUIRED_LINK_OPTIONS "${CMAKE_REQUIRED_LINK_OPTIONS}")
set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -fsanitize=fuzzer-no-link")
set(CMAKE_REQUIRED_LINK_OPTIONS "${CMAKE_REQUIRED_LINK_OPTIONS};-fsanitize=fuzzer")
check_cxx_source_compiles(
  "extern \"C\" int LLVMFuzzerTestOneInput(const unsigned char*, unsigned long) { return 0; }"
  CGRAPH_HAS_LIBFUZZER_RUNTIME)
set(CMAKE_REQUIRED_FLAGS "${CGRAPH_SAVED_REQUIRED_FLAGS}")
set(CMAKE_REQUIRED_LINK_OPTIONS "${CGRAPH_SAVED_REQUIRED_LINK_OPTIONS}")

if(NOT CGRAPH_HAS_LIBFUZZER_RUNTIME)
  message(FATAL_ERROR
    "CGRAPH_BUILD_FUZZERS requires a Clang toolchain with the libFuzzer runtime. "
    "Apple Command Line Tools Clang may not include libclang_rt.fuzzer; use upstream LLVM/Clang.")
endif()

function(cgraph_enable_libfuzzer target)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "CGRAPH_BUILD_FUZZERS requires Clang or AppleClang for libFuzzer")
  endif()

  target_compile_options(${target} PRIVATE -fsanitize=fuzzer-no-link)
  target_link_options(${target} PRIVATE -fsanitize=fuzzer)
endfunction()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(cgraph_engine PRIVATE -fsanitize=fuzzer-no-link)
endif()

if(TARGET cgraph_tree_sitter AND CMAKE_C_COMPILER_ID MATCHES "Clang")
  target_compile_options(cgraph_tree_sitter PRIVATE -fsanitize=fuzzer-no-link)
endif()

function(cgraph_add_extractor_fuzzer target language source_file)
  add_executable(${target}
    extractor_fuzzer.cpp)

  target_compile_definitions(${target}
    PRIVATE
      "CGRAPH_FUZZ_LANGUAGE=cgraph::DetectedLanguage::${language}"
      "CGRAPH_FUZZ_SOURCE_FILE=\"${source_file}\"")

  target_link_libraries(${target}
    PRIVATE
      cgraph::engine)

  cgraph_set_warnings(${target})
  cgraph_enable_sanitizers(${target})
  cgraph_enable_libfuzzer(${target})
endfunction()

add_executable(cgraph_fuzz_smoke
  fuzz_smoke.cpp)

target_link_libraries(cgraph_fuzz_smoke
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_fuzz_smoke)
cgraph_enable_sanitizers(cgraph_fuzz_smoke)
cgraph_enable_libfuzzer(cgraph_fuzz_smoke)

add_test(NAME cgraph_fuzz_smoke_test COMMAND cgraph_fuzz_smoke -runs=1)

cgraph_add_extractor_fuzzer(cgraph_fuzz_apex Apex fuzz.cls)
cgraph_add_extractor_fuzzer(cgraph_fuzz_c C fuzz.c)
cgraph_add_extractor_fuzzer(cgraph_fuzz_cpp Cpp fuzz.cpp)
cgraph_add_extractor_fuzzer(cgraph_fuzz_delphi Delphi fuzz.dfm)
cgraph_add_extractor_fuzzer(cgraph_fuzz_groovy Groovy fuzz.groovy)
cgraph_add_extractor_fuzzer(cgraph_fuzz_java Java fuzz.java)
cgraph_add_extractor_fuzzer(cgraph_fuzz_javascript JavaScript fuzz.js)
cgraph_add_extractor_fuzzer(cgraph_fuzz_kotlin Kotlin fuzz.kt)
cgraph_add_extractor_fuzzer(cgraph_fuzz_mcp_config McpConfig mcp.json)
cgraph_add_extractor_fuzzer(cgraph_fuzz_msbuild MsBuild fuzz.csproj)
cgraph_add_extractor_fuzzer(cgraph_fuzz_python Python fuzz.py)
cgraph_add_extractor_fuzzer(cgraph_fuzz_ruby Ruby fuzz.rb)
cgraph_add_extractor_fuzzer(cgraph_fuzz_scala Scala fuzz.scala)
cgraph_add_extractor_fuzzer(cgraph_fuzz_spring_config SpringConfig application.yml)
cgraph_add_extractor_fuzzer(cgraph_fuzz_typescript TypeScript fuzz.ts)
cgraph_add_extractor_fuzzer(cgraph_fuzz_tsx Tsx fuzz.tsx)

foreach(target IN ITEMS
    cgraph_fuzz_apex
    cgraph_fuzz_c
    cgraph_fuzz_cpp
    cgraph_fuzz_delphi
    cgraph_fuzz_groovy
    cgraph_fuzz_java
    cgraph_fuzz_javascript
    cgraph_fuzz_kotlin
    cgraph_fuzz_mcp_config
    cgraph_fuzz_msbuild
    cgraph_fuzz_python
    cgraph_fuzz_ruby
    cgraph_fuzz_scala
    cgraph_fuzz_spring_config
    cgraph_fuzz_typescript
    cgraph_fuzz_tsx)
  add_test(NAME ${target}_smoke_test COMMAND ${target} -runs=1 -max_len=4096)
endforeach()
