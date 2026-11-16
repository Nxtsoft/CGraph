#pragma once

namespace cgraph {

// How deep a recursive reader of one expression may go before it gives up on
// that expression. A URL, a `use` tree or a macro body that a person wrote
// nests a handful of levels; generated code nests tens of thousands, and a
// reader that takes one stack frame per level overflows the extraction
// thread's stack, which no try/catch can stop.
//
// Kept small on purpose: several of these readers run from inside walk_node,
// which may itself be 200 frames down on a 512 KB thread stack, and their
// frames are over a kilobyte each in a debug build.
inline constexpr int kMaxExpressionNesting = 64;

// Counts the live frames of one recursive function. Give each function its own
// counter (a `thread_local int`, or for a recursive lambda an `int` beside it)
// and construct a guard at the top:
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
