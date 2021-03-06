//===- TestBufferPlacement.cpp - Test for buffer placement 0----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements logic for testing buffer placement including its
// utility converters.
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/Linalg/IR/LinalgOps.h"
#include "mlir/IR/Function.h"
#include "mlir/IR/Operation.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Transforms/BufferPlacement.h"

using namespace mlir;

namespace {
/// This pass tests the computeAllocPosition helper method and two provided
/// operation converters, FunctionAndBlockSignatureConverter and
/// BufferAssignmentReturnOpConverter. Furthermore, this pass converts linalg
/// operations on tensors to linalg operations on buffers to prepare them for
/// the BufferPlacement pass that can be applied afterwards.
struct TestBufferPlacementPreparationPass
    : mlir::PassWrapper<TestBufferPlacementPreparationPass,
                        OperationPass<ModuleOp>> {

  /// Converts tensor-type generic linalg operations to memref ones using buffer
  /// assignment.
  class GenericOpConverter
      : public BufferAssignmentOpConversionPattern<linalg::GenericOp> {
  public:
    using BufferAssignmentOpConversionPattern<
        linalg::GenericOp>::BufferAssignmentOpConversionPattern;

    LogicalResult
    matchAndRewrite(linalg::GenericOp op, ArrayRef<Value> operands,
                    ConversionPatternRewriter &rewriter) const final {
      Location loc = op.getLoc();
      ResultRange results = op.getOperation()->getResults();
      SmallVector<Value, 2> newArgs, newResults;
      newArgs.reserve(operands.size() + results.size());
      newArgs.append(operands.begin(), operands.end());
      newResults.reserve(results.size());

      // Update all types to memref types.
      for (auto result : results) {
        ShapedType type = result.getType().cast<ShapedType>();
        assert(type && "Generic operations with non-shaped typed results are "
                       "not currently supported.");
        if (!type.hasStaticShape())
          return rewriter.notifyMatchFailure(
              op, "dynamic shapes not currently supported");
        auto memrefType =
            MemRefType::get(type.getShape(), type.getElementType());

        // Compute alloc position and insert a custom allocation node.
        OpBuilder::InsertionGuard guard(rewriter);
        rewriter.restoreInsertionPoint(
            bufferAssignment->computeAllocPosition(result));
        auto alloc = rewriter.create<AllocOp>(loc, memrefType);
        newArgs.push_back(alloc);
        newResults.push_back(alloc);
      }

      // Generate a new linalg operation that works on buffers.
      auto linalgOp = rewriter.create<linalg::GenericOp>(
          loc, llvm::None, newArgs, rewriter.getI64IntegerAttr(operands.size()),
          rewriter.getI64IntegerAttr(results.size()), op.indexing_maps(),
          op.iterator_types(), op.docAttr(), op.library_callAttr());

      // Create a new block in the region of the new Generic Op.
      Block &oldBlock = op.getRegion().front();
      Region &newRegion = linalgOp.region();
      Block *newBlock = rewriter.createBlock(&newRegion, newRegion.begin(),
                                             oldBlock.getArgumentTypes());

      // Map the old block arguments to the new ones.
      BlockAndValueMapping mapping;
      mapping.map(oldBlock.getArguments(), newBlock->getArguments());

      // Add the result arguments to the new block.
      for (auto result : newResults)
        newBlock->addArgument(
            result.getType().cast<ShapedType>().getElementType());

      // Clone the body of the old block to the new block.
      rewriter.setInsertionPointToEnd(newBlock);
      for (auto &op : oldBlock.getOperations())
        rewriter.clone(op, mapping);

      // Replace the results of the old Generic Op with the results of the new
      // one.
      rewriter.replaceOp(op, newResults);
      return success();
    }
  };

  void populateTensorLinalgToBufferLinalgConversionPattern(
      MLIRContext *context, BufferAssignmentPlacer *placer,
      TypeConverter *converter, OwningRewritePatternList *patterns) {
    // clang-format off
    patterns->insert<
                   FunctionAndBlockSignatureConverter,
                   GenericOpConverter,
                   BufferAssignmentReturnOpConverter<
                      ReturnOp, ReturnOp, linalg::CopyOp>
    >(context, placer, converter);
    // clang-format on
  }

  void runOnOperation() override {
    MLIRContext &context = getContext();
    ConversionTarget target(context);
    BufferAssignmentTypeConverter converter;

    // Mark all Standard operations legal.
    target.addLegalDialect<StandardOpsDialect>();

    // Mark all Linalg operations illegal as long as they work on tensors.
    auto isIllegalType = [&](Type type) { return !converter.isLegal(type); };
    auto isLegalOperation = [&](Operation *op) {
      return llvm::none_of(op->getOperandTypes(), isIllegalType) &&
             llvm::none_of(op->getResultTypes(), isIllegalType);
    };
    target.addDynamicallyLegalDialect<linalg::LinalgDialect>(
        Optional<ConversionTarget::DynamicLegalityCallbackFn>(
            isLegalOperation));

    // Mark Standard Return operations illegal as long as one operand is tensor.
    target.addDynamicallyLegalOp<mlir::ReturnOp>([&](mlir::ReturnOp returnOp) {
      return llvm::none_of(returnOp.getOperandTypes(), isIllegalType);
    });

    // Mark the function whose arguments are in tensor-type illegal.
    target.addDynamicallyLegalOp<FuncOp>([&](FuncOp funcOp) {
      return converter.isSignatureLegal(funcOp.getType());
    });

    // Walk over all the functions to apply buffer assignment.
    getOperation().walk([&](FuncOp function) -> WalkResult {
      OwningRewritePatternList patterns;
      BufferAssignmentPlacer placer(function);
      populateTensorLinalgToBufferLinalgConversionPattern(
          &context, &placer, &converter, &patterns);

      // Applying full conversion
      return applyFullConversion(function, target, patterns, &converter);
    });
  };
};
} // end anonymous namespace

namespace mlir {
void registerTestBufferPlacementPreparationPass() {
  PassRegistration<TestBufferPlacementPreparationPass>(
      "test-buffer-placement-preparation",
      "Tests buffer placement helper methods including its "
      "operation-conversion patterns");
}
} // end namespace mlir