#pragma once

namespace cgraph {

// How deep a recursive reader of one expression may go before it gives up on
// that expression. A URL, a `use` tree or a macro body that a person wrote
// nests a handful of levels; generated code nests tens of thousands, and a
// reader that takes one stack frame per level overflows the extraction
// thread's stack, which no try/catch can stop.
inline constexpr int kMaxExpressionNesting = 256;

// Counts the live frames of one recursive function. Give each function its own
// `thread_local int` and construct a guard at the top:
//
//   thread_local int nesting = 0;
//   const NestingGuard guard(nesting);
//   if (guard.too_deep()) { /* answer as for an expression it cannot read */ }
class NestingGuard {
 public:
  explicit NestingGuard(int& depth) : depth_(depth) { ++depth_; }
  ~NestingGuard() { --depth_; }
  NestingGuard(const NestingGuard&) = delete;
  NestingGuard& operator=(const NestingGuard&) = delete;

  [[nodiscard]] bool too_deep() const { return depth_ > kMaxExpressionNesting; }

 private:
  int& depth_;
};

}  // namespace cgraph
