//===-- CheerpBackend.cpp - Backend wrapper for CheerpWriter---------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// Copyright 2011-2015 Leaning Technologies
//
//===----------------------------------------------------------------------===//

#include "CheerpTargetMachine.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/PassManager.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/Type.h"
#include "llvm/Cheerp/Writer.h"
#include "llvm/Cheerp/WasmWriter.h"
#include "llvm/Cheerp/LinearMemoryHelper.h"
#include "llvm/Cheerp/AllocaMerging.h"
#include "llvm/Cheerp/AllocaLowering.h"
#include "llvm/Cheerp/AllocateArrayLowering.h"
#include "llvm/Cheerp/IdenticalCodeFolding.h"
#include "llvm/Cheerp/PointerPasses.h"
#include "llvm/Cheerp/CFGPasses.h"
#include "llvm/Cheerp/Registerize.h"
#include "llvm/Cheerp/ResolveAliases.h"
#include "llvm/Cheerp/SourceMaps.h"
#include "llvm/Cheerp/CommandLine.h"

using namespace llvm;

extern "C" void LLVMInitializeCheerpBackendTarget() {
  // Register the target.
  RegisterTargetMachine<CheerpTargetMachine> X(TheCheerpBackendTarget);
}

namespace {
  class CheerpWritePass : public ModulePass {
  private:
    formatted_raw_ostream &Out;
    static char ID;
    void getAnalysisUsage(AnalysisUsage& AU) const;
  public:
    explicit CheerpWritePass(formatted_raw_ostream &o) :
      ModulePass(ID), Out(o) { }
    bool runOnModule(Module &M);
    const char *getPassName() const {
	return "CheerpWritePass";
    }
  };
} // end anonymous namespace.

bool CheerpWritePass::runOnModule(Module& M)
{
  cheerp::PointerAnalyzer &PA = getAnalysis<cheerp::PointerAnalyzer>();
  cheerp::GlobalDepsAnalyzer &GDA = getAnalysis<cheerp::GlobalDepsAnalyzer>();
  cheerp::Registerize &registerize = getAnalysis<cheerp::Registerize>();
  cheerp::AllocaStoresExtractor &allocaStoresExtractor = getAnalysis<cheerp::AllocaStoresExtractor>();
  cheerp::LinearMemoryHelper linearHelper(M, cheerp::LinearMemoryHelper::FunctionAddressMode::AsmJS, GDA);
  std::unique_ptr<cheerp::SourceMapGenerator> sourceMapGenerator;
  GDA.forceTypedArrays = ForceTypedArrays;
  if (!SourceMap.empty())
  {
    std::error_code ErrorCode;
    sourceMapGenerator.reset(new cheerp::SourceMapGenerator(SourceMap, SourceMapPrefix, SourceMapStandAlone, M.getContext(), ErrorCode));
    if (ErrorCode)
    {
       // An error occurred opening the source map file, bail out
       llvm::report_fatal_error(ErrorCode.message(), false);
       return false;
    }
  }
  PA.fullResolve();
  PA.computeConstantOffsets(M);
  // Destroy the stores here, we need them to properly compute the pointer kinds, but we want to optimize them away before registerize
  allocaStoresExtractor.destroyStores();
  registerize.assignRegisters(M, PA);

  std::error_code ErrorCode;
  llvm::tool_output_file memFile(AsmJSMemFile, ErrorCode, sys::fs::F_None);
  std::unique_ptr<llvm::formatted_raw_ostream> memOut;
  if (!AsmJSMemFile.empty())
  {
    memOut.reset(new formatted_raw_ostream(memFile.os()));
  }

  cheerp::NameGenerator namegen(M, GDA, registerize, PA, ReservedNames, PrettyCode);
  cheerp::CheerpWriter writer(M, Out, PA, registerize, GDA, linearHelper, namegen, allocaStoresExtractor, memOut.get(), AsmJSMemFile,
          sourceMapGenerator.get(), PrettyCode, MakeModule, NoRegisterize, !NoNativeJavaScriptMath,
          !NoJavaScriptMathImul, !NoJavaScriptMathFround, !NoCredits, MeasureTimeToMain, CheerpHeapSize,
          BoundsCheck, SymbolicGlobalsAsmJS, std::string(), ForceTypedArrays);
  writer.makeJS();
  if (ErrorCode)
  {
    if(!AsmJSMemFile.empty())
    {
      // An error occurred opening the asm.js memory file, bail out
      llvm::report_fatal_error(ErrorCode.message(), false);
    }
    return false;
  }
  memFile.keep();

  return false;
}

void CheerpWritePass::getAnalysisUsage(AnalysisUsage& AU) const
{
  AU.addRequired<cheerp::GlobalDepsAnalyzer>();
  AU.addRequired<cheerp::PointerAnalyzer>();
  AU.addRequired<cheerp::Registerize>();
  AU.addRequired<cheerp::AllocaStoresExtractor>();
}

char CheerpWritePass::ID = 0;

//===----------------------------------------------------------------------===//
//                       External Interface declaration
//===----------------------------------------------------------------------===//

bool CheerpTargetMachine::addPassesToEmitFile(PassManagerBase &PM,
                                           formatted_raw_ostream &o,
                                           CodeGenFileType FileType,
                                           bool DisableVerify,
                                           AnalysisID StartAfter,
                                           AnalysisID StopAfter) {
  PM.add(createAllocaLoweringPass());
  PM.add(createResolveAliasesPass());
  PM.add(createFreeAndDeleteRemovalPass());
  PM.add(cheerp::createGlobalDepsAnalyzerPass());
  if (!CheerpNoICF)
    PM.add(cheerp::createIdenticalCodeFoldingPass());
  PM.add(createPointerArithmeticToArrayIndexingPass());
  PM.add(createPointerToImmutablePHIRemovalPass());
  PM.add(createGEPOptimizerPass());
  PM.add(cheerp::createRegisterizePass(!NoJavaScriptMathFround, NoRegisterize));
  PM.add(cheerp::createPointerAnalyzerPass());
  PM.add(cheerp::createAllocaMergingPass());
  PM.add(createIndirectCallOptimizerPass());
  PM.add(createAllocaArraysPass());
  PM.add(cheerp::createAllocaArraysMergingPass());
  PM.add(createDelayAllocasPass());
  PM.add(createRemoveFwdBlocksPass());
  // Keep this pass last, it is going to remove stores to memory from the LLVM visible code, so further optimizing afterwards will break
  PM.add(cheerp::createAllocaStoresExtractor());
  PM.add(new CheerpWritePass(o));
  return false;
}
