//===- Inliner.cpp - Pass to inline function calls ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements a basic inlining algorithm that operates bottom up over
// the Strongly Connect Components(SCCs) of the CallGraph. This enables a more
// incremental propagation of inlining decisions from the leafs to the roots of
// the callgraph.
//
//===----------------------------------------------------------------------===//

#include "PassDetail.h"
#include "mlir/Analysis/CallGraph.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Interfaces/SideEffects.h"
#include "mlir/Transforms/InliningUtils.h"
#include "mlir/Transforms/Passes.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Parallel.h"

#define DEBUG_TYPE "inlining"

using namespace mlir;

static llvm::cl::opt<bool> disableCanonicalization(
    "mlir-disable-inline-simplify",
    llvm::cl::desc("Disable running simplifications during inlining"),
    llvm::cl::ReallyHidden, llvm::cl::init(false));

static llvm::cl::opt<unsigned> maxInliningIterations(
    "mlir-max-inline-iterations",
    llvm::cl::desc("Maximum number of iterations when inlining within an SCC"),
    llvm::cl::ReallyHidden, llvm::cl::init(4));

//===----------------------------------------------------------------------===//
// Symbol Use Tracking
//===----------------------------------------------------------------------===//

/// Returns true if this operation can be discarded if it is a symbol and has no
/// uses. 'allUsesVisible' corresponds to if the parent symbol table is hidden
/// from above.
static bool canDiscardSymbolOnUseEmpty(Operation *op, bool allUsesVisible) {
  if (!SymbolTable::isSymbol(op))
    return false;

  // TODO: This is essentially the same logic from SymbolDCE. Remove this when
  // we have a 'Symbol' interface.
  // Private symbols are always initially considered dead.
  SymbolTable::Visibility visibility = SymbolTable::getSymbolVisibility(op);
  if (visibility == mlir::SymbolTable::Visibility::Private)
    return true;
  // We only include nested visibility here if all uses are visible.
  if (allUsesVisible && visibility == SymbolTable::Visibility::Nested)
    return true;
  // Otherwise, public symbols are never removable.
  return false;
}

/// Walk all of the symbol table operations nested with 'op' along with a
/// boolean signifying if the symbols within can be treated as if all uses are
/// visible. The provided callback is invoked with the symbol table operation,
/// and a boolean signaling if all of the uses within the symbol table are
/// visible.
static void walkSymbolTables(Operation *op, bool allSymUsesVisible,
                             function_ref<void(Operation *, bool)> callback) {
  if (op->hasTrait<OpTrait::SymbolTable>()) {
    allSymUsesVisible = allSymUsesVisible || !SymbolTable::isSymbol(op) ||
                        SymbolTable::getSymbolVisibility(op) ==
                            SymbolTable::Visibility::Private;
    callback(op, allSymUsesVisible);
  } else {
    // Otherwise if 'op' is not a symbol table, any nested symbols are
    // guaranteed to be hidden.
    allSymUsesVisible = true;
  }

  for (Region &region : op->getRegions())
    for (Block &block : region)
      for (Operation &nested : block)
        walkSymbolTables(&nested, allSymUsesVisible, callback);
}

/// Walk all of the used symbol callgraph nodes referenced with the given op.
static void walkReferencedSymbolNodes(
    Operation *op, CallGraph &cg,
    DenseMap<Attribute, CallGraphNode *> &resolvedRefs,
    function_ref<void(CallGraphNode *, Operation *)> callback) {
  auto symbolUses = SymbolTable::getSymbolUses(op);
  assert(symbolUses && "expected uses to be valid");

  Operation *symbolTableOp = op->getParentOp();
  for (const SymbolTable::SymbolUse &use : *symbolUses) {
    auto refIt = resolvedRefs.insert({use.getSymbolRef(), nullptr});
    CallGraphNode *&node = refIt.first->second;

    // If this is the first instance of this reference, try to resolve a
    // callgraph node for it.
    if (refIt.second) {
      auto *symbolOp = SymbolTable::lookupNearestSymbolFrom(symbolTableOp,
                                                            use.getSymbolRef());
      auto callableOp = dyn_cast_or_null<CallableOpInterface>(symbolOp);
      if (!callableOp)
        continue;
      node = cg.lookupNode(callableOp.getCallableRegion());
    }
    if (node)
      callback(node, use.getUser());
  }
}

//===----------------------------------------------------------------------===//
// CGUseList

namespace {
/// This struct tracks the uses of callgraph nodes that can be dropped when
/// use_empty. It directly tracks and manages a use-list for all of the
/// call-graph nodes. This is necessary because many callgraph nodes are
/// referenced by SymbolRefAttr, which has no mechanism akin to the SSA `Use`
/// class.
struct CGUseList {
  /// This struct tracks the uses of callgraph nodes within a specific
  /// operation.
  struct CGUser {
    /// Any nodes referenced in the top-level attribute list of this user. We
    /// use a set here because the number of references does not matter.
    DenseSet<CallGraphNode *> topLevelUses;

    /// Uses of nodes referenced by nested operations.
    DenseMap<CallGraphNode *, int> innerUses;
  };

  CGUseList(Operation *op, CallGraph &cg);

  /// Drop uses of nodes referred to by the given call operation that resides
  /// within 'userNode'.
  void dropCallUses(CallGraphNode *userNode, Operation *callOp, CallGraph &cg);

  /// Remove the given node from the use list.
  void eraseNode(CallGraphNode *node);

  /// Returns true if the given callgraph node has no uses and can be pruned.
  bool isDead(CallGraphNode *node) const;

  /// Returns true if the given callgraph node has a single use and can be
  /// discarded.
  bool hasOneUseAndDiscardable(CallGraphNode *node) const;

  /// Recompute the uses held by the given callgraph node.
  void recomputeUses(CallGraphNode *node, CallGraph &cg);

  /// Merge the uses of 'lhs' with the uses of the 'rhs' after inlining a copy
  /// of 'lhs' into 'rhs'.
  void mergeUsesAfterInlining(CallGraphNode *lhs, CallGraphNode *rhs);

private:
  /// Decrement the uses of discardable nodes referenced by the given user.
  void decrementDiscardableUses(CGUser &uses);

  /// A mapping between a discardable callgraph node (that is a symbol) and the
  /// number of uses for this node.
  DenseMap<CallGraphNode *, int> discardableSymNodeUses;
  /// A mapping between a callgraph node and the symbol callgraph nodes that it
  /// uses.
  DenseMap<CallGraphNode *, CGUser> nodeUses;
};
} // end anonymous namespace

CGUseList::CGUseList(Operation *op, CallGraph &cg) {
  /// A set of callgraph nodes that are always known to be live during inlining.
  DenseMap<Attribute, CallGraphNode *> alwaysLiveNodes;

  // Walk each of the symbol tables looking for discardable callgraph nodes.
  auto walkFn = [&](Operation *symbolTableOp, bool allUsesVisible) {
    for (Block &block : symbolTableOp->getRegion(0)) {
      for (Operation &op : block) {
        // If this is a callgraph operation, check to see if it is discardable.
        if (auto callable = dyn_cast<CallableOpInterface>(&op)) {
          if (auto *node = cg.lookupNode(callable.getCallableRegion())) {
            if (canDiscardSymbolOnUseEmpty(&op, allUsesVisible))
              discardableSymNodeUses.try_emplace(node, 0);
            continue;
          }
        }
        // Otherwise, check for any referenced nodes. These will be always-live.
        walkReferencedSymbolNodes(&op, cg, alwaysLiveNodes,
                                  [](CallGraphNode *, Operation *) {});
      }
    }
  };
  walkSymbolTables(op, /*allSymUsesVisible=*/!op->getBlock(), walkFn);

  // Drop the use information for any discardable nodes that are always live.
  for (auto &it : alwaysLiveNodes)
    discardableSymNodeUses.erase(it.second);

  // Compute the uses for each of the callable nodes in the graph.
  for (CallGraphNode *node : cg)
    recomputeUses(node, cg);
}

void CGUseList::dropCallUses(CallGraphNode *userNode, Operation *callOp,
                             CallGraph &cg) {
  auto &userRefs = nodeUses[userNode].innerUses;
  auto walkFn = [&](CallGraphNode *node, Operation *user) {
    auto parentIt = userRefs.find(node);
    if (parentIt == userRefs.end())
      return;
    --parentIt->second;
    --discardableSymNodeUses[node];
  };
  DenseMap<Attribute, CallGraphNode *> resolvedRefs;
  walkReferencedSymbolNodes(callOp, cg, resolvedRefs, walkFn);
}

void CGUseList::eraseNode(CallGraphNode *node) {
  // Drop all child nodes.
  for (auto &edge : *node)
    if (edge.isChild())
      eraseNode(edge.getTarget());

  // Drop the uses held by this node and erase it.
  auto useIt = nodeUses.find(node);
  assert(useIt != nodeUses.end() && "expected node to be valid");
  decrementDiscardableUses(useIt->getSecond());
  nodeUses.erase(useIt);
  discardableSymNodeUses.erase(node);
}

bool CGUseList::isDead(CallGraphNode *node) const {
  // If the parent operation isn't a symbol, simply check normal SSA deadness.
  Operation *nodeOp = node->getCallableRegion()->getParentOp();
  if (!SymbolTable::isSymbol(nodeOp))
    return MemoryEffectOpInterface::hasNoEffect(nodeOp) && nodeOp->use_empty();

  // Otherwise, check the number of symbol uses.
  auto symbolIt = discardableSymNodeUses.find(node);
  return symbolIt != discardableSymNodeUses.end() && symbolIt->second == 0;
}

bool CGUseList::hasOneUseAndDiscardable(CallGraphNode *node) const {
  // If this isn't a symbol node, check for side-effects and SSA use count.
  Operation *nodeOp = node->getCallableRegion()->getParentOp();
  if (!SymbolTable::isSymbol(nodeOp))
    return MemoryEffectOpInterface::hasNoEffect(nodeOp) && nodeOp->hasOneUse();

  // Otherwise, check the number of symbol uses.
  auto symbolIt = discardableSymNodeUses.find(node);
  return symbolIt != discardableSymNodeUses.end() && symbolIt->second == 1;
}

void CGUseList::recomputeUses(CallGraphNode *node, CallGraph &cg) {
  Operation *parentOp = node->getCallableRegion()->getParentOp();
  CGUser &uses = nodeUses[node];
  decrementDiscardableUses(uses);

  // Collect the new discardable uses within this node.
  uses = CGUser();
  DenseMap<Attribute, CallGraphNode *> resolvedRefs;
  auto walkFn = [&](CallGraphNode *refNode, Operation *user) {
    auto discardSymIt = discardableSymNodeUses.find(refNode);
    if (discardSymIt == discardableSymNodeUses.end())
      return;

    if (user != parentOp)
      ++uses.innerUses[refNode];
    else if (!uses.topLevelUses.insert(refNode).second)
      return;
    ++discardSymIt->second;
  };
  walkReferencedSymbolNodes(parentOp, cg, resolvedRefs, walkFn);
}

void CGUseList::mergeUsesAfterInlining(CallGraphNode *lhs, CallGraphNode *rhs) {
  auto &lhsUses = nodeUses[lhs], &rhsUses = nodeUses[rhs];
  for (auto &useIt : lhsUses.innerUses) {
    rhsUses.innerUses[useIt.first] += useIt.second;
    discardableSymNodeUses[useIt.first] += useIt.second;
  }
}

void CGUseList::decrementDiscardableUses(CGUser &uses) {
  for (CallGraphNode *node : uses.topLevelUses)
    --discardableSymNodeUses[node];
  for (auto &it : uses.innerUses)
    discardableSymNodeUses[it.first] -= it.second;
}

//===----------------------------------------------------------------------===//
// CallGraph traversal
//===----------------------------------------------------------------------===//

/// Run a given transformation over the SCCs of the callgraph in a bottom up
/// traversal.
static void runTransformOnCGSCCs(
    const CallGraph &cg,
    function_ref<void(MutableArrayRef<CallGraphNode *>)> sccTransformer) {
  std::vector<CallGraphNode *> currentSCCVec;
  auto cgi = llvm::scc_begin(&cg);
  while (!cgi.isAtEnd()) {
    // Copy the current SCC and increment so that the transformer can modify the
    // SCC without invalidating our iterator.
    currentSCCVec = *cgi;
    ++cgi;
    sccTransformer(currentSCCVec);
  }
}

namespace {
/// This struct represents a resolved call to a given callgraph node. Given that
/// the call does not actually contain a direct reference to the
/// Region(CallGraphNode) that it is dispatching to, we need to resolve them
/// explicitly.
struct ResolvedCall {
  ResolvedCall(CallOpInterface call, CallGraphNode *sourceNode,
               CallGraphNode *targetNode)
      : call(call), sourceNode(sourceNode), targetNode(targetNode) {}
  CallOpInterface call;
  CallGraphNode *sourceNode, *targetNode;
};
} // end anonymous namespace

/// Collect all of the callable operations within the given range of blocks. If
/// `traverseNestedCGNodes` is true, this will also collect call operations
/// inside of nested callgraph nodes.
static void collectCallOps(iterator_range<Region::iterator> blocks,
                           CallGraphNode *sourceNode, CallGraph &cg,
                           SmallVectorImpl<ResolvedCall> &calls,
                           bool traverseNestedCGNodes) {
  SmallVector<std::pair<Block *, CallGraphNode *>, 8> worklist;
  auto addToWorklist = [&](CallGraphNode *node,
                           iterator_range<Region::iterator> blocks) {
    for (Block &block : blocks)
      worklist.emplace_back(&block, node);
  };

  addToWorklist(sourceNode, blocks);
  while (!worklist.empty()) {
    Block *block;
    std::tie(block, sourceNode) = worklist.pop_back_val();

    for (Operation &op : *block) {
      if (auto call = dyn_cast<CallOpInterface>(op)) {
        // TODO(riverriddle) Support inlining nested call references.
        CallInterfaceCallable callable = call.getCallableForCallee();
        if (SymbolRefAttr symRef = callable.dyn_cast<SymbolRefAttr>()) {
          if (!symRef.isa<FlatSymbolRefAttr>())
            continue;
        }

        CallGraphNode *targetNode = cg.resolveCallable(call);
        if (!targetNode->isExternal())
          calls.emplace_back(call, sourceNode, targetNode);
        continue;
      }

      // If this is not a call, traverse the nested regions. If
      // `traverseNestedCGNodes` is false, then don't traverse nested call graph
      // regions.
      for (auto &nestedRegion : op.getRegions()) {
        CallGraphNode *nestedNode = cg.lookupNode(&nestedRegion);
        if (traverseNestedCGNodes || !nestedNode)
          addToWorklist(nestedNode ? nestedNode : sourceNode, nestedRegion);
      }
    }
  }
}

//===----------------------------------------------------------------------===//
// Inliner
//===----------------------------------------------------------------------===//
namespace {
/// This class provides a specialization of the main inlining interface.
struct Inliner : public InlinerInterface {
  Inliner(MLIRContext *context, CallGraph &cg)
      : InlinerInterface(context), cg(cg) {}

  /// Process a set of blocks that have been inlined. This callback is invoked
  /// *before* inlined terminator operations have been processed.
  void
  processInlinedBlocks(iterator_range<Region::iterator> inlinedBlocks) final {
    // Find the closest callgraph node from the first block.
    CallGraphNode *node;
    Region *region = inlinedBlocks.begin()->getParent();
    while (!(node = cg.lookupNode(region))) {
      region = region->getParentRegion();
      assert(region && "expected valid parent node");
    }

    collectCallOps(inlinedBlocks, node, cg, calls,
                   /*traverseNestedCGNodes=*/true);
  }

  /// The current set of call instructions to consider for inlining.
  SmallVector<ResolvedCall, 8> calls;

  /// The callgraph being operated on.
  CallGraph &cg;
};
} // namespace

/// Returns true if the given call should be inlined.
static bool shouldInline(ResolvedCall &resolvedCall) {
  // Don't allow inlining terminator calls. We currently don't support this
  // case.
  if (resolvedCall.call.getOperation()->isKnownTerminator())
    return false;

  // Don't allow inlining if the target is an ancestor of the call. This
  // prevents inlining recursively.
  if (resolvedCall.targetNode->getCallableRegion()->isAncestor(
          resolvedCall.call.getParentRegion()))
    return false;

  // Otherwise, inline.
  return true;
}

/// Delete the given node and remove it from the current scc and the callgraph.
static void deleteNode(CallGraphNode *node, CGUseList &useList, CallGraph &cg,
                       MutableArrayRef<CallGraphNode *> currentSCC) {
  // Erase the parent operation and remove it from the various lists.
  node->getCallableRegion()->getParentOp()->erase();
  cg.eraseNode(node);

  // Replace this node in the currentSCC with the external node.
  auto it = llvm::find(currentSCC, node);
  if (it != currentSCC.end())
    *it = cg.getExternalNode();
}

/// Attempt to inline calls within the given scc. This function returns
/// success if any calls were inlined, failure otherwise.
static LogicalResult
inlineCallsInSCC(Inliner &inliner, CGUseList &useList,
                 MutableArrayRef<CallGraphNode *> currentSCC) {
  CallGraph &cg = inliner.cg;
  auto &calls = inliner.calls;

  // Collect all of the direct calls within the nodes of the current SCC. We
  // don't traverse nested callgraph nodes, because they are handled separately
  // likely within a different SCC.
  for (CallGraphNode *node : currentSCC) {
    if (node->isExternal())
      continue;

    // If this node is dead, just delete it now.
    if (useList.isDead(node))
      deleteNode(node, useList, cg, currentSCC);
    else
      collectCallOps(*node->getCallableRegion(), node, cg, calls,
                     /*traverseNestedCGNodes=*/false);
  }
  if (calls.empty())
    return failure();

  // A set of dead nodes to remove after inlining.
  SmallVector<CallGraphNode *, 1> deadNodes;

  // Try to inline each of the call operations. Don't cache the end iterator
  // here as more calls may be added during inlining.
  bool inlinedAnyCalls = false;
  for (unsigned i = 0; i != calls.size(); ++i) {
    ResolvedCall &it = calls[i];
    LLVM_DEBUG({
      llvm::dbgs() << "* Considering inlining call: ";
      it.call.dump();
    });
    if (!shouldInline(it))
      continue;
    CallOpInterface call = it.call;
    Region *targetRegion = it.targetNode->getCallableRegion();

    // If this is the last call to the target node and the node is discardable,
    // then inline it in-place and delete the node if successful.
    bool inlineInPlace = useList.hasOneUseAndDiscardable(it.targetNode);

    LogicalResult inlineResult = inlineCall(
        inliner, call, cast<CallableOpInterface>(targetRegion->getParentOp()),
        targetRegion, /*shouldCloneInlinedRegion=*/!inlineInPlace);
    if (failed(inlineResult))
      continue;
    inlinedAnyCalls = true;

    // If the inlining was successful, Merge the new uses into the source node.
    useList.dropCallUses(it.sourceNode, call.getOperation(), cg);
    useList.mergeUsesAfterInlining(it.targetNode, it.sourceNode);

    // then erase the call.
    call.erase();

    // If we inlined in place, mark the node for deletion.
    if (inlineInPlace) {
      useList.eraseNode(it.targetNode);
      deadNodes.push_back(it.targetNode);
    }
  }

  for (CallGraphNode *node : deadNodes)
    deleteNode(node, useList, cg, currentSCC);
  calls.clear();
  return success(inlinedAnyCalls);
}

/// Canonicalize the nodes within the given SCC with the given set of
/// canonicalization patterns.
static void canonicalizeSCC(CallGraph &cg, CGUseList &useList,
                            MutableArrayRef<CallGraphNode *> currentSCC,
                            MLIRContext *context,
                            const OwningRewritePatternList &canonPatterns) {
  // Collect the sets of nodes to canonicalize.
  SmallVector<CallGraphNode *, 4> nodesToCanonicalize;
  for (auto *node : currentSCC) {
    // Don't canonicalize the external node, it has no valid callable region.
    if (node->isExternal())
      continue;

    // Don't canonicalize nodes with children. Nodes with children
    // require special handling as we may remove the node during
    // canonicalization. In the future, we should be able to handle this
    // case with proper node deletion tracking.
    if (node->hasChildren())
      continue;

    // We also won't apply canonicalizations for nodes that are not
    // isolated. This avoids potentially mutating the regions of nodes defined
    // above, this is also a stipulation of the 'applyPatternsGreedily' driver.
    auto *region = node->getCallableRegion();
    if (!region->getParentOp()->isKnownIsolatedFromAbove())
      continue;
    nodesToCanonicalize.push_back(node);
  }
  if (nodesToCanonicalize.empty())
    return;

  // Canonicalize each of the nodes within the SCC in parallel.
  // NOTE: This is simple now, because we don't enable canonicalizing nodes
  // within children. When we remove this restriction, this logic will need to
  // be reworked.
  ParallelDiagnosticHandler canonicalizationHandler(context);
  llvm::parallel::for_each_n(
      llvm::parallel::par, /*Begin=*/size_t(0),
      /*End=*/nodesToCanonicalize.size(), [&](size_t index) {
        // Set the order for this thread so that diagnostics will be properly
        // ordered.
        canonicalizationHandler.setOrderIDForThread(index);

        // Apply the canonicalization patterns to this region.
        auto *node = nodesToCanonicalize[index];
        applyPatternsGreedily(*node->getCallableRegion(), canonPatterns);

        // Make sure to reset the order ID for the diagnostic handler, as this
        // thread may be used in a different context.
        canonicalizationHandler.eraseOrderIDForThread();
      });

  // Recompute the uses held by each of the nodes.
  for (CallGraphNode *node : nodesToCanonicalize)
    useList.recomputeUses(node, cg);
}

/// Attempt to inline calls within the given scc, and run canonicalizations with
/// the given patterns, until a fixed point is reached. This allows for the
/// inlining of newly devirtualized calls.
static void inlineSCC(Inliner &inliner, CGUseList &useList,
                      MutableArrayRef<CallGraphNode *> currentSCC,
                      MLIRContext *context,
                      const OwningRewritePatternList &canonPatterns) {
  // If we successfully inlined any calls, run some simplifications on the
  // nodes of the scc. Continue attempting to inline until we reach a fixed
  // point, or a maximum iteration count. We canonicalize here as it may
  // devirtualize new calls, as well as give us a better cost model.
  unsigned iterationCount = 0;
  while (succeeded(inlineCallsInSCC(inliner, useList, currentSCC))) {
    // If we aren't allowing simplifications or the max iteration count was
    // reached, then bail out early.
    if (disableCanonicalization || ++iterationCount >= maxInliningIterations)
      break;
    canonicalizeSCC(inliner.cg, useList, currentSCC, context, canonPatterns);
  }
}

//===----------------------------------------------------------------------===//
// InlinerPass
//===----------------------------------------------------------------------===//

namespace {
struct InlinerPass : public InlinerBase<InlinerPass> {
  void runOnOperation() override {
    CallGraph &cg = getAnalysis<CallGraph>();
    auto *context = &getContext();

    // The inliner should only be run on operations that define a symbol table,
    // as the callgraph will need to resolve references.
    Operation *op = getOperation();
    if (!op->hasTrait<OpTrait::SymbolTable>()) {
      op->emitOpError() << " was scheduled to run under the inliner, but does "
                           "not define a symbol table";
      return signalPassFailure();
    }

    // Collect a set of canonicalization patterns to use when simplifying
    // callable regions within an SCC.
    OwningRewritePatternList canonPatterns;
    for (auto *op : context->getRegisteredOperations())
      op->getCanonicalizationPatterns(canonPatterns, context);

    // Run the inline transform in post-order over the SCCs in the callgraph.
    Inliner inliner(context, cg);
    CGUseList useList(getOperation(), cg);
    runTransformOnCGSCCs(cg, [&](MutableArrayRef<CallGraphNode *> scc) {
      inlineSCC(inliner, useList, scc, context, canonPatterns);
    });
  }
};
} // end anonymous namespace

std::unique_ptr<Pass> mlir::createInlinerPass() {
  return std::make_unique<InlinerPass>();
}
