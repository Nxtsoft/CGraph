#include "cgraph/normalize.hpp"

#include <string_view>

namespace {

struct Case {
  std::string_view input;
  std::string_view expected;
};

constexpr Case kCases[] = {
    {"Hello, World!", "hello_world"},
    {"  Foo---Bar  ", "foo_bar"},
    {"Crème Brûlée", "crème_brûlée"},
    {"Café", "café"},
    {"東京.Service", "東京_service"},
    {"Привет мир", "привет_мир"},
    {"Class::Method", "class_method"},
    {"naïve_user42", "naïve_user42"},
    {"① Service", "1_service"},
    {"___Already__ID___", "already_id"},
    {"", ""},
};

}  // namespace

int main() {
  for (const auto& test_case : kCases) {
    if (cgraph::make_id(test_case.input) != test_case.expected) {
      return 1;
    }
  }

  // relative_import_stub: the namespace carries what make_id would erase.
  struct StubCase {
    std::string_view joined;
    std::string_view expected;
  };
  constexpr StubCase kStubCases[] = {
      {"pkg/service", "import-relative-module:pkg/service"},
      {"./util", "import-relative-module:util"},
      {"pkg/../util", "import-relative-module:util"},     // interior ".." folds away
      {"", "import-relative-module:"},                    // the root itself
      {"pkg/..", "import-relative-module:"},
      {"..", "import-relative-up-1-module:"},
      {"../../x/y", "import-relative-up-2-module:x/y"},   // depth survives, not clamped at the root
      {"pkg/../../../x", "import-relative-up-2-module:x"},
  };
  for (const auto& test_case : kStubCases) {
    if (cgraph::relative_import_stub("module", test_case.joined) != test_case.expected) {
      return 2;
    }
  }
  // Every distinct stub above must stay distinct after make_id, and none may
  // equal the bare-package id of the same path.
  if (cgraph::make_id(cgraph::relative_import_stub("module", "..")) ==
          cgraph::make_id(cgraph::relative_import_stub("module", "")) ||
      cgraph::make_id(cgraph::relative_import_stub("module", "../x")) ==
          cgraph::make_id(cgraph::relative_import_stub("module", "x")) ||
      cgraph::make_id(cgraph::relative_import_stub("module", "x")) == cgraph::make_id("import-module:x")) {
    return 3;
  }
  return 0;
}
