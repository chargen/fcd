/*
 * Copyright (c) 2018 Trail of Bits, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fcd/ast_remill/SimplifyConditions.h"

namespace fcd {

char SimplifyConditions::ID = 0;

SimplifyConditions::SimplifyConditions(clang::CompilerInstance &ins,
                                       fcd::IRToASTVisitor &ast_gen)
    : ModulePass(SimplifyConditions::ID),
      ast_ctx(&ins.getASTContext()),
      ast_gen(&ast_gen),
      z3_ctx(new z3::context()),
      z3_gen(new fcd::Z3ConvVisitor(ast_ctx, z3_ctx.get())) {}

bool SimplifyConditions::VisitIfStmt(clang::IfStmt *stmt) {
  z3_gen->GetOrCreateZ3Expr(stmt->getCond());
  return true;
}

bool SimplifyConditions::runOnModule(llvm::Module &module) {
  TraverseDecl(ast_ctx->getTranslationUnitDecl());
  return true;
}

llvm::ModulePass *createSimplifyConditionsPass(clang::CompilerInstance &ins,
                                               fcd::IRToASTVisitor &gen) {
  return new SimplifyConditions(ins, gen);
}
}