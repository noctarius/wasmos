//===- WasmosTidyModule.cpp - wasmos-specific clang-tidy checks ----------===//
//
// A loadable clang-tidy module ("wasmos-*") holding project-specific checks.
// Built against the LLVM the gate runs (see the wasmos_tidy_plugin target in
// the top-level CMakeLists) and loaded via `clang-tidy --load`. Register new
// checks in WasmosModule::addCheckFactories.
//
// Checks:
//   wasmos-standalone-block  - flags a bare "{ ... }" compound statement that
//     exists only to nest, not to scope. Such blocks add indentation without
//     buying anything; unwrap them (or, in the rare case the scope is truly
//     needed for variable-lifetime/name reuse, add // NOLINT(wasmos-standalone-
//     block) with a reason). A block that is the body of if/for/while/switch or
//     a function is NOT matched (its parent is the control statement / decl),
//     so only genuine standalone blocks are reported.
//
//===----------------------------------------------------------------------===//

#include "clang-tidy/ClangTidyCheck.h"
#include "clang-tidy/ClangTidyModule.h"
// ClangTidyModuleRegistry has its own header through ~LLVM 23; it was folded
// into ClangTidyModule.h afterward (standalone header deprecated, then removed
// in LLVM 24). Include it only where it still exists so the plugin builds
// against both older (CI: LLVM 20) and newer (Homebrew) toolchains.
#if __has_include("clang-tidy/ClangTidyModuleRegistry.h")
#include "clang-tidy/ClangTidyModuleRegistry.h"
#endif
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"

using namespace clang::ast_matchers;

namespace clang::tidy::wasmos {

class StandaloneBlockCheck : public ClangTidyCheck {
public:
  StandaloneBlockCheck(StringRef Name, ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context) {}

  void registerMatchers(MatchFinder *Finder) override {
    // A CompoundStmt whose immediate parent is another CompoundStmt is a
    // standalone block among statements; control-flow/function bodies parent
    // to their own statement/decl and are excluded.
    Finder->addMatcher(
        compoundStmt(hasParent(compoundStmt())).bind("block"), this);
  }

  void check(const MatchFinder::MatchResult &Result) override {
    const auto *Block = Result.Nodes.getNodeAs<CompoundStmt>("block");
    if (!Block || Block->getLBracLoc().isInvalid() ||
        Block->getRBracLoc().isInvalid())
      return;
    // Skip blocks whose braces come from a macro expansion. Vendored runtime
    // macros (e.g. wasm3's m3ApiRawFunction / m3ApiReturn) expand to compound
    // statements; those are not hand-written standalone blocks and cannot be
    // unwrapped in source.
    if (Block->getLBracLoc().isMacroID() || Block->getRBracLoc().isMacroID())
      return;
    // The fixit removes just the two brace tokens (AST-precise); clang-format
    // then re-indents the freed body. Collisions from now-merged scopes surface
    // as compile errors and are resolved by hand.
    diag(Block->getLBracLoc(),
         "standalone brace block only adds nesting; unwrap it (or add "
         "// NOLINT(wasmos-standalone-block) if the scope is genuinely needed)")
        << FixItHint::CreateRemoval(SourceRange(Block->getLBracLoc()))
        << FixItHint::CreateRemoval(SourceRange(Block->getRBracLoc()));
  }
};

class WasmosModule : public ClangTidyModule {
public:
  void addCheckFactories(ClangTidyCheckFactories &Factories) override {
    Factories.registerCheck<StandaloneBlockCheck>("wasmos-standalone-block");
  }
};

// Registering the module runs on dlopen via clang-tidy --load.
static ClangTidyModuleRegistry::Add<WasmosModule>
    X("wasmos-module", "wasmos-specific clang-tidy checks.");

} // namespace clang::tidy::wasmos
