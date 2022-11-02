// SpanTracePass: inserts __st_enter(id) at function entry and __st_exit(id)
// before every return, and emits a per-module table mapping ids to names.
//
//   clang -O2 -fpass-plugin=libSpanTracePass.so foo.c -lspantrace
//   opt -load-pass-plugin=libSpanTracePass.so -passes=spantrace in.ll -S
//
// Options (pass through clang with -mllvm; clang also needs
// -Xclang -load -Xclang libSpanTracePass.so so they're registered in time):
//   -spantrace-filter=<regex>   only instrument functions whose demangled name matches
//   -spantrace-min-size=<N>     skip functions with fewer than N IR instructions
#include <cstdint>
#include <string>
#include <vector>

#include "llvm/ADT/StringRef.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Regex.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace llvm;

static cl::opt<std::string> FilterOpt(
    "spantrace-filter", cl::init(""),
    cl::desc("Only instrument functions whose demangled name matches this regex"));

static cl::opt<unsigned> MinSizeOpt(
    "spantrace-min-size", cl::init(0),
    cl::desc("Skip functions with fewer than this many IR instructions"));

namespace {

// FNV-1a. Stable across runs and compilers, which matters because ids from
// different translation units end up in the same trace.
uint64_t Fnv1a(StringRef s, uint64_t h = 0xcbf29ce484222325ULL) {
  for (unsigned char c : s) {
    h ^= c;
    h *= 0x100000001b3ULL;
  }
  return h;
}

class SpanTracePass : public PassInfoMixin<SpanTracePass> {
 public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
    if (!FilterOpt.empty()) {
      std::string err;
      Regex re(FilterOpt);
      if (!re.isValid(err))
        report_fatal_error(Twine("spantrace: bad -spantrace-filter: ") + err);
    }

    LLVMContext &Ctx = M.getContext();
    Type *VoidTy = Type::getVoidTy(Ctx);
    Type *I64 = Type::getInt64Ty(Ctx);
    PointerType *Ptr = PointerType::getUnqual(Ctx);

    Regex filter(FilterOpt);
    std::vector<Function *> targets;
    for (Function &F : M)
      if (shouldInstrument(F, filter)) targets.push_back(&F);
    if (targets.empty()) return PreservedAnalyses::all();

    FunctionCallee Enter = M.getOrInsertFunction("__st_enter", VoidTy, I64);
    FunctionCallee Exit = M.getOrInsertFunction("__st_exit", VoidTy, I64);
    struct Entry {
      uint64_t id;
      std::string name;
    };
    std::vector<Entry> table;
    for (Function *F : targets) {
      const uint64_t id = functionId(M, *F);
      instrument(*F, Enter, Exit, ConstantInt::get(I64, id));
      table.push_back({id, F->getName().str()});
    }

    emitNameTable(M, table.size(), [&](size_t i) { return table[i]; }, I64, Ptr, VoidTy);
    return PreservedAnalyses::none();
  }

  static bool isRequired() { return true; }

 private:
  static bool shouldInstrument(Function &F, Regex &filter) {
    if (F.isDeclaration() || F.hasAvailableExternallyLinkage()) return false;
    StringRef name = F.getName();
    if (name.starts_with("__st_") || name.starts_with("llvm.")) return false;
    if (F.hasFnAttribute(Attribute::Naked)) return false;
    if (MinSizeOpt > 0 && F.getInstructionCount() < MinSizeOpt) return false;
    if (!FilterOpt.empty() && !filter.match(demangle(name.str()))) return false;
    return true;
  }

  // Externally visible functions hash by name alone so every TU agrees on the
  // id. Internal ones (static functions, anonymous namespaces) also mix in the
  // source file, since two files can each have their own `static helper()`.
  static uint64_t functionId(Module &M, Function &F) {
    uint64_t h = Fnv1a(F.getName());
    if (F.hasLocalLinkage()) h = Fnv1a(M.getSourceFileName(), h ^ 0x9e3779b97f4a7c15ULL);
    return h;
  }

  static void instrument(Function &F, FunctionCallee Enter, FunctionCallee Exit, Constant *Id) {
    // Enter goes after the entry block's allocas so they stay together at the
    // top, which keeps mem2reg/SROA happy.
    BasicBlock &EntryBB = F.getEntryBlock();
    BasicBlock::iterator IP = EntryBB.getFirstInsertionPt();
    while (IP != EntryBB.end() && isa<AllocaInst>(*IP)) ++IP;
    IRBuilder<> B(&EntryBB, IP);
    B.CreateCall(Enter, {Id});

    for (BasicBlock &BB : F) {
      auto *Ret = dyn_cast<ReturnInst>(BB.getTerminator());
      if (!Ret) continue;
      // Nothing may sit between a musttail call and its ret, so exit first.
      Instruction *Before = Ret;
      if (CallInst *Tail = BB.getTerminatingMustTailCall()) Before = Tail;
      IRBuilder<> RB(Before);
      RB.CreateCall(Exit, {Id});
    }
  }

  template <class GetEntry>
  static void emitNameTable(Module &M, size_t n, GetEntry get, Type *I64, PointerType *Ptr,
                            Type *VoidTy) {
    LLVMContext &Ctx = M.getContext();
    StructType *EntryTy = StructType::get(Ctx, {I64, Ptr});
    std::vector<Constant *> rows;
    rows.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      auto e = get(i);
      Constant *Str = ConstantDataArray::getString(Ctx, e.name, /*AddNull=*/true);
      auto *NameGV = new GlobalVariable(M, Str->getType(), /*isConstant=*/true,
                                        GlobalValue::PrivateLinkage, Str, ".st.name");
      NameGV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
      rows.push_back(ConstantStruct::get(EntryTy, {ConstantInt::get(I64, e.id), NameGV}));
    }
    ArrayType *TableTy = ArrayType::get(EntryTy, n);
    auto *Table = new GlobalVariable(M, TableTy, /*isConstant=*/true,
                                     GlobalValue::PrivateLinkage,
                                     ConstantArray::get(TableTy, rows), ".st.names");

    FunctionCallee Register =
        M.getOrInsertFunction("__st_register_names", VoidTy, Ptr, I64);
    Function *Ctor = Function::Create(FunctionType::get(VoidTy, false),
                                      GlobalValue::InternalLinkage, "__st_module_ctor", M);
    BasicBlock *BB = BasicBlock::Create(Ctx, "entry", Ctor);
    IRBuilder<> B(BB);
    B.CreateCall(Register, {Table, ConstantInt::get(I64, n)});
    B.CreateRetVoid();
    // Early priority so names are in place before ordinary static constructors
    // start calling traced code. Ids are hashes, so even events recorded before
    // this runs resolve correctly at dump time.
    appendToGlobalCtors(M, Ctor, /*Priority=*/101);
  }
};

}  // namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "SpanTrace", LLVM_VERSION_STRING, [](PassBuilder &PB) {
            // `opt -passes=spantrace`
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name != "spantrace") return false;
                  MPM.addPass(SpanTracePass());
                  return true;
                });
            // `clang -fpass-plugin=...`: run after the optimizer so we trace what
            // actually survived inlining (and don't pay for tracing what didn't).
            PB.registerOptimizerLastEPCallback(
                [](ModulePassManager &MPM, OptimizationLevel) {
                  MPM.addPass(SpanTracePass());
                });
          }};
}
