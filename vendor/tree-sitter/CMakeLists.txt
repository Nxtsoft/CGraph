add_library(cgraph_tree_sitter STATIC
  core/lib/src/lib.c
  grammars/c/src/parser.c
  grammars/cpp/src/parser.c
  grammars/cpp/src/scanner.c
  grammars/csharp/src/parser.c
  grammars/csharp/src/scanner.c
  grammars/go/src/parser.c
  grammars/groovy/src/parser.c
  grammars/java/src/parser.c
  grammars/javascript/src/parser.c
  grammars/javascript/src/scanner.c
  grammars/kotlin/src/parser.c
  grammars/kotlin/src/scanner.c
  grammars/python/src/parser.c
  grammars/python/src/scanner.c
  grammars/ruby/src/parser.c
  grammars/ruby/src/scanner.c
  grammars/scala/src/parser.c
  grammars/scala/src/scanner.c
  grammars/typescript/typescript/src/parser.c
  grammars/typescript/typescript/src/scanner.c
  grammars/typescript/tsx/src/parser.c
  grammars/typescript/tsx/src/scanner.c)

add_library(cgraph::tree_sitter ALIAS cgraph_tree_sitter)

target_include_directories(cgraph_tree_sitter
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/core/lib/include"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/core/lib/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/grammars/c/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/grammars/cpp/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/grammars/csharp/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/grammars/go/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/grammars/groovy/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/grammars/java/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/grammars/javascript/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/grammars/kotlin/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/grammars/python/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/grammars/ruby/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/grammars/scala/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/grammars/typescript/typescript/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/grammars/typescript/tsx/src")

if(MSVC)
  target_compile_options(cgraph_tree_sitter PRIVATE /wd4244 /wd4267)
else()
  target_compile_options(cgraph_tree_sitter PRIVATE -Wno-conversion -Wno-sign-conversion)
endif()
