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
#include "clang/Lex/Lexer.h"

#include <string>

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

// Rewrites the raw integer<->pointer double casts "(T *)(uintptr_t)e" and
// "(intT)(uintptr_t)e" to ptr_cast(T, e) / addr_cast(intT, e) (see
// src/libc/include/wasmos_cast.h). Fixit-driven: run with clang-tidy --fix.
class ReinterpretCastCheck : public ClangTidyCheck {
public:
  ReinterpretCastCheck(StringRef Name, ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context) {}

  void registerMatchers(MatchFinder *Finder) override {
    // Outer C-style cast whose operand is another C-style cast; the inner one
    // being to uintptr_t is verified in check().
    Finder->addMatcher(
        cStyleCastExpr(hasSourceExpression(cStyleCastExpr().bind("inner")))
            .bind("outer"),
        this);
  }

  void check(const MatchFinder::MatchResult &Result) override {
    const auto *Outer = Result.Nodes.getNodeAs<CStyleCastExpr>("outer");
    const auto *Inner = Result.Nodes.getNodeAs<CStyleCastExpr>("inner");
    if (!Outer || !Inner)
      return;
    // Inner cast must be to uintptr_t (written spelling, not the canonical
    // unsigned long/long long).
    if (Inner->getTypeAsWritten().getAsString() != "uintptr_t")
      return;

    // Bail on anything touching a macro; source-text rewriting there is unsafe.
    const SourceManager &SM = *Result.SourceManager;
    const LangOptions &LO = Result.Context->getLangOpts();
    const Expr *Operand = Inner->getSubExprAsWritten();
    if (Outer->getBeginLoc().isMacroID() || Outer->getEndLoc().isMacroID() ||
        Operand->getBeginLoc().isMacroID() || Operand->getEndLoc().isMacroID())
      return;

    // Use the *written* type text, not the canonicalized type, so typedefs stay
    // as spelled. Only rewrite plain "T *" pointers and integer types; skip
    // typedef'd or function pointers (their written form isn't "T *", and
    // ptr_cast's "type *" reconstruction wouldn't be valid).
    StringRef WrittenTy = Lexer::getSourceText(
        CharSourceRange::getTokenRange(
            Outer->getTypeInfoAsWritten()->getTypeLoc().getSourceRange()),
        SM, LO);
    std::string TyStr = WrittenTy.trim().str();
    QualType OuterTy = Outer->getTypeAsWritten();
    std::string Macro, TypeArg;
    if (OuterTy->isPointerType()) {
      if (TyStr.empty() || TyStr.back() != '*' ||
          OuterTy->getPointeeType()->isFunctionType())
        return;
      TyStr.pop_back(); // drop the '*'
      while (!TyStr.empty() && (TyStr.back() == ' ' || TyStr.back() == '\t'))
        TyStr.pop_back();
      if (TyStr.empty())
        return;
      Macro = "ptr_cast";
      TypeArg = TyStr;
    } else if (OuterTy->isIntegerType() && !TyStr.empty()) {
      Macro = "addr_cast";
      TypeArg = TyStr;
    } else {
      return;
    }

    StringRef OperandText = Lexer::getSourceText(
        CharSourceRange::getTokenRange(Operand->getSourceRange()), SM, LO);
    if (OperandText.empty())
      return;

    std::string Repl =
        Macro + "(" + TypeArg + ", " + OperandText.str() + ")";
    diag(Outer->getBeginLoc(),
         "use %0() instead of a raw (%1)(uintptr_t) reinterpret cast")
        << Macro << TypeArg
        << FixItHint::CreateReplacement(
               CharSourceRange::getTokenRange(Outer->getSourceRange()), Repl);
  }
};

class WasmosModule : public ClangTidyModule {
public:
  void addCheckFactories(ClangTidyCheckFactories &Factories) override {
    Factories.registerCheck<StandaloneBlockCheck>("wasmos-standalone-block");
    Factories.registerCheck<ReinterpretCastCheck>("wasmos-reinterpret-cast");
  }
};

// Registering the module runs on dlopen via clang-tidy --load.
static ClangTidyModuleRegistry::Add<WasmosModule>
    X("wasmos-module", "wasmos-specific clang-tidy checks.");

} // namespace clang::tidy::wasmos
