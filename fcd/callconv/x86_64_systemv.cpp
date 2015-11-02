//
// x86_64_systemv.cpp
// Copyright (C) 2015 Félix Cloutier.
// All Rights Reserved.
//
// This file is part of fcd.
// 
// fcd is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// fcd is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with fcd.  If not, see <http://www.gnu.org/licenses/>.
//

// About the x86_64 SystemV calling convention:
// http://x86-64.org/documentation/abi.pdf pp 20-22
// In short, for arguments:
// - Aggregates are passed in registers, unless one of the fields is a floating-point field, in which case it goes to
//		memory; or unless not enough integer registers are available, in which case it also goes to the stack.
// - Integral arguments are passed in rdi-rsi-rdx-rcx-r8-r9.
// - Floating-point arguments are passed in [xyz]mm0-[xyz]mm7
// - Anything else/left remaining goes to the stack.
// For return values:
// - Integral values go to rax-rdx.
// - Floating-point values go to xmm0-xmm1.
// - Large return values may be written to *rdi, and rax will contain rdi (in which case it's indistinguishible from
//		a function accepting the output destination as a first parameter).
// The relative parameter order of values of different classes is not preserved.

#include "cc_common.h"
#include "x86_64_systemv.h"

SILENCE_LLVM_WARNINGS_BEGIN()
#include <llvm/IR/PatternMatch.h>
#include "MemorySSA.h"
SILENCE_LLVM_WARNINGS_END()

#include <unordered_map>

using namespace llvm;
using namespace llvm::PatternMatch;
using namespace std;

namespace
{
	RegisterCallingConvention<CallingConvention_x86_64_systemv> registerSysV;
	
	const char* returnRegisters[] = {"rax", "rdx"};
	const char* parameterRegisters[] = { "rdi", "rsi", "rdx", "rcx", "r8", "r9" };
	
	// only handles integer types
	template<unsigned N>
	bool addEntriesForType(TargetInfo& targetInfo, llvm::SmallVector<ValueInformation, N>& into, const char**& regIter, const char** end, Type* type)
	{
		unsigned pointerSize = targetInfo.getPointerSize();
		if (isa<PointerType>(type))
		{
			type = IntegerType::get(type->getContext(), pointerSize);
		}
		
		if (auto intType = dyn_cast<IntegerType>(type))
		{
			unsigned bitSize = intType->getIntegerBitWidth();
			while (regIter != end && bitSize != 0)
			{
				into.emplace_back(ValueInformation::IntegerRegister, targetInfo.registerNamed(*regIter));
				regIter++;
				bitSize -= min<unsigned>(bitSize, 64);
			}
			return bitSize == 0;
		}
		
		return type == Type::getVoidTy(type->getContext());
	}
}

bool CallingConvention_x86_64_systemv::matches(TargetInfo &target, Executable &executable) const
{
	return target.targetName().substr(3) == "x86" && executable.getExecutableType().substr(6) == "ELF 64";
}

const char* CallingConvention_x86_64_systemv::getName() const
{
	return "x86_64/SystemV";
}

void CallingConvention_x86_64_systemv::analyzeFunction(ParameterRegistry &registry, CallInformation &callInfo, llvm::Function &function)
{
	TargetInfo& targetInfo = registry.getAnalysis<TargetInfo>();
	
	// Identify register GEPs.
	// (assume x86 regs as first parameter)
	assert(function.arg_size() == 1);
	Argument* regs = function.arg_begin();
	auto pointerType = dyn_cast<PointerType>(regs->getType());
	assert(pointerType != nullptr && pointerType->getTypeAtIndex(int(0))->getStructName() == "struct.x86_regs");
	
	unordered_multimap<const TargetRegisterInfo*, GetElementPtrInst*> geps;
	for (auto& use : regs->uses())
	{
		if (GetElementPtrInst* gep = dyn_cast<GetElementPtrInst>(use.getUser()))
		if (const TargetRegisterInfo* regName = targetInfo.registerInfo(*gep))
		{
			geps.insert({regName, gep});
		}
	}
	
	// Look at temporary registers that are read before they are written
	MemorySSA& mssa = *registry.getMemorySSA(function);
	for (const char* name : parameterRegisters)
	{
		const TargetRegisterInfo* smallReg = targetInfo.registerNamed(name);
		const TargetRegisterInfo* regInfo = targetInfo.largestOverlappingRegister(*smallReg);
		auto range = geps.equal_range(regInfo);
		for (auto iter = range.first; iter != range.second; ++iter)
		{
			for (auto& use : iter->second->uses())
			{
				if (auto load = dyn_cast<LoadInst>(use.getUser()))
				{
					MemoryAccess* parent = mssa.getMemoryAccess(load)->getDefiningAccess();
					if (mssa.isLiveOnEntryDef(parent))
					{
						// register argument!
						callInfo.parameters.emplace_back(ValueInformation::IntegerRegister, regInfo);
					}
				}
			}
		}
	}
	
	// Does the function refer to values at an offset above the initial rsp value?
	// Assume that rsp is known to be preserved.
	auto spRange = geps.equal_range(targetInfo.getStackPointer());
	for (auto iter = spRange.first; iter != spRange.second; ++iter)
	{
		auto* gep = iter->second;
		// Find all uses of reference to sp register
		for (auto& use : gep->uses())
		{
			if (auto load = dyn_cast<LoadInst>(use.getUser()))
			{
				// Find uses above +8 (since +0 is the return address)
				for (auto& use : load->uses())
				{
					ConstantInt* offset = nullptr;
					if (match(use.get(), m_Add(m_Value(), m_ConstantInt(offset))))
					{
						make_signed<decltype(offset->getLimitedValue())>::type intOffset = offset->getLimitedValue();
						if (intOffset > 8)
						{
							// memory argument!
							callInfo.parameters.emplace_back(ValueInformation::Stack, intOffset);
						}
					}
				}
			}
		}
	}
	
	// Are we using return registers?
	vector<const TargetRegisterInfo*> usedReturns;
	usedReturns.reserve(2);
	
	for (const char* name : returnRegisters)
	{
		const TargetRegisterInfo* regInfo = targetInfo.registerNamed(name);
		auto range = geps.equal_range(regInfo);
		for (auto iter = range.first; iter != range.second; ++iter)
		{
			for (auto& use : iter->second->uses())
			{
				if (isa<StoreInst>(use.getUser()))
				{
					usedReturns.push_back(regInfo);
				}
			}
		}
	}
	
	for (const TargetRegisterInfo* reg : ipaFindUsedReturns(registry, function, usedReturns))
	{
		// return value!
		callInfo.returnValues.emplace_back(ValueInformation::IntegerRegister, reg);
	}
	
	// TODO: Look at called functions to find hidden parameters/return values
}

bool CallingConvention_x86_64_systemv::analyzeFunctionType(ParameterRegistry& registry, CallInformation& fillOut, FunctionType& type)
{
	TargetInfo& targetInfo = registry.getAnalysis<TargetInfo>();
	auto iter = begin(returnRegisters);
	if (!addEntriesForType(targetInfo, fillOut.returnValues, iter, end(returnRegisters), type.getReturnType()))
	{
		return false;
	}
	
	iter = begin(parameterRegisters);
	for (Type* t : type.params())
	{
		if (!addEntriesForType(targetInfo, fillOut.parameters, iter, end(parameterRegisters), t))
		{
			return false;
		}
	}
	
	return true;
}
