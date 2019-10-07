/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/mlir/tensorflow/translate/import_model.h"

#include <iterator>
#include <tuple>

#include "absl/algorithm/container.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/inlined_vector.h"
#include "absl/strings/escaping.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/raw_ostream.h"
#include "mlir/Analysis/Verifier.h"  // TF:local_config_mlir
#include "mlir/Dialect/StandardOps/Ops.h"  // TF:local_config_mlir
#include "mlir/IR/Attributes.h"  // TF:local_config_mlir
#include "mlir/IR/Builders.h"  // TF:local_config_mlir
#include "mlir/IR/Function.h"  // TF:local_config_mlir
#include "mlir/IR/Identifier.h"  // TF:local_config_mlir
#include "mlir/IR/Location.h"  // TF:local_config_mlir
#include "mlir/IR/MLIRContext.h"  // TF:local_config_mlir
#include "mlir/IR/Module.h"  // TF:local_config_mlir
#include "mlir/IR/Types.h"  // TF:local_config_mlir
#include "tensorflow/compiler/jit/shape_inference_helpers.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/control_flow_ops.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_executor.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_saved_model.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_types.h"
#include "tensorflow/compiler/mlir/tensorflow/translate/mlir_roundtrip_flags.h"
#include "tensorflow/compiler/mlir/tensorflow/utils/convert_tensor.h"
#include "tensorflow/compiler/mlir/tensorflow/utils/convert_type.h"
#include "tensorflow/compiler/mlir/tensorflow/utils/mangling_util.h"
#include "tensorflow/compiler/tf2xla/functionalize_control_flow.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/core/common_runtime/function.h"
#include "tensorflow/core/common_runtime/shape_refiner.h"
#include "tensorflow/core/framework/attr_value.pb.h"
#include "tensorflow/core/framework/function.pb.h"
#include "tensorflow/core/framework/graph.pb.h"
#include "tensorflow/core/framework/graph_to_functiondef.h"
#include "tensorflow/core/framework/node_def.pb.h"
#include "tensorflow/core/framework/node_def_util.h"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/shape_inference.h"
#include "tensorflow/core/framework/tensor.pb.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/framework/versions.pb.h"
#include "tensorflow/core/graph/algorithm.h"
#include "tensorflow/core/graph/graph.h"
#include "tensorflow/core/graph/graph_constructor.h"
#include "tensorflow/core/graph/node_builder.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/strings/str_util.h"
#include "tensorflow/core/platform/protobuf.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/protobuf/graph_debug_info.pb.h"
#include "tensorflow/core/protobuf/meta_graph.pb.h"
#include "tensorflow/core/protobuf/saved_object_graph.pb.h"
#include "tensorflow/core/protobuf/trackable_object_graph.pb.h"

static inline absl::string_view StringRefToView(llvm::StringRef ref) {
  return {ref.data(), ref.size()};
}

namespace tensorflow {
using stream_executor::port::StatusOr;

namespace {

// Stateful helper class to import a TensorFlow model into an MLIR Module.
//
// This is the base class that contains common utilties shared between the
// GraphDef importer and SavedModel importer.
//
// A subclass is expected to call `PrepareConvert` first to perform necessary
// preparation over the graph and also certain internal bookkeeping data.
// Afterwards the other protected methods can be called.
class ImporterBase {
 protected:
  explicit ImporterBase(
      const FunctionLibraryDefinition& flib, const GraphDebugInfo& debug_info,
      const GraphImportConfig& specs, mlir::ModuleOp module,
      std::unordered_map<std::string, std::string>* tf_name_to_mlir_name)
      : builder_(module.getContext()),
        module_(module),
        context_(module.getContext()),
        tf_name_to_mlir_name_(tf_name_to_mlir_name),
        graph_flib_(flib),
        specs_(specs),
        debug_info_(debug_info) {}

  // Returns the inferred function signature of the given function body. Input
  // types are unranked tensor of the respective datatype in the function and
  // result types are inferred by the shape_refiner_. Result types need not be
  // unranked tensors and could be ranked tensors in cases where result type
  // depends on an op with static output shape like tf.Const.
  StatusOr<mlir::FunctionType> InferLibFunctionType(const FunctionBody& fbody);

  // Extracts arg and ret nodes from FunctionBody.
  void GetArgsAndRetsFromFunctionBody(
      const FunctionBody& fbody,
      absl::InlinedVector<OutputTensor, 4>* arg_nodes,
      absl::InlinedVector<OutputTensor, 4>* ret_nodes,
      absl::InlinedVector<Node*, 4>* control_ret_nodes);

  // Prepares converting the graph to an MLIR module. This step removes the
  // backedges of the graph, orders the nodes and infers the shapes.
  Status PrepareConvert(const Graph& graph);

  // Converts the prepared graph to a Function and adds it to the module. A set
  // of nodes from the graph are given to converted to the arguments and returns
  // of the function.
  Status Convert(llvm::StringRef func_name, mlir::FunctionType func_type,
                 const absl::InlinedVector<OutputTensor, 4>& arg_nodes,
                 const absl::InlinedVector<OutputTensor, 4>& ret_nodes,
                 const absl::InlinedVector<Node*, 4>& control_ret_nodes,
                 llvm::ArrayRef<mlir::NamedAttribute> attrs);

  // Finds out the function definition for the given function name from the
  // graph and converts it to a function of the module. This method is called
  // on demand because the graph flib_def does not provide an iterator
  // interface.
  Status ConvertLibFunction(llvm::StringRef func_name);

  // Returns the list of nodes in the graph. Nodes are presented in the reverse
  // order of a post-order depth-first visit starting from the graph's source
  // nodes.
  llvm::ArrayRef<Node*> GetOrderedNodes() const { return ordered_nodes_; }

  // Returns the inferred output type at index `idx` of the `node` in the
  // context.
  StatusOr<mlir::TensorType> InferOutputType(const Node& node, int idx,
                                             mlir::Builder builder);

 private:
  // Most types with subtypes have only one subtype.
  using ElementSubtypes = llvm::SmallVector<mlir::TensorType, 1>;

  // Adds all the ordered_nodes to the shape refiner shape_refiner_. Then all
  // data type and shape information is maintained by the shape_refiner_.
  Status AddNodesToShapeRefiner();

  // Returns the inferred input type at index `idx` of the `node` in the
  // context.
  StatusOr<mlir::TensorType> InferInputType(const Node& node, int idx,
                                            mlir::Builder builder);

  // Converts the inferred shape referred to by 'handle' in 'context', with
  // given element type, and returns an MLIR tensor type.
  StatusOr<mlir::TensorType> ConvertDataTypeAndShape(
      DataType dtype, const shape_inference::ShapeHandle& handle,
      const std::vector<shape_inference::ShapeAndType>* handle_subtypes,
      shape_inference::InferenceContext* context, mlir::Builder builder);

  // Converts the inferred shape referred to by 'handle' in 'context', with
  // given element type, and returns an MLIR tensor type.
  StatusOr<mlir::TensorType> ConvertElementTypeAndShape(
      mlir::Type element_type, const shape_inference::ShapeHandle& handle,
      shape_inference::InferenceContext* context, mlir::Builder builder);

  // Converts the inferred subtypes for an element type to corresponding MLIR
  // types in 'context'.
  StatusOr<ElementSubtypes> ConvertSubtypes(
      const std::vector<shape_inference::ShapeAndType>* handle_subtypes,
      shape_inference::InferenceContext* context, mlir::Builder builder);

  // Converts the tensor proto into an MLIR elements attribute.
  StatusOr<mlir::ElementsAttr> ConvertTensorProto(const TensorProto& value) {
    return ::tensorflow::ConvertTensorProto(value, &builder_);
  }

  // Converts func name in graphdef to mlir::SymbolRefAttribute.
  StatusOr<mlir::SymbolRefAttr> ConvertFunctionCallName(
      const std::string& func_name);

  // Converts the given non-function-call AttrValue to an MLIR Attribute.
  StatusOr<mlir::Attribute> ConvertAttributeValue(const AttrValue& value);

  // Converts the given function-call AttrValue to MLIR Attributes and pushes
  // them to the given attributes list. For example, if there is a kFunc
  // AttrValue {name : foo, attrs : {k1 : bar, k2 : rfc}}, it will convert it to
  // a list of MLIR Attributes: [{base_name : foo}, {base_name.k1 : bar},
  // {base_name.k2 : rfc}}.
  Status ConvertFunctionCallAttribute(
      const std::string& base_name, const AttrValue& value,
      llvm::SmallVector<mlir::NamedAttribute, 4>* attributes);

  // Helper to create either a tf_executor operation or a TF operation wrapped
  // in an island.
  mlir::Operation* createOperation(
      const Node& node, llvm::StringRef op_name,
      const mlir::OperationState& result,
      const llvm::SmallVectorImpl<mlir::Value*>& control_operands);

  // Converts one NodeDef from the input GraphDef into an Operation and
  // inserts it into the MLIR module using builder_.
  Status ConvertNode(const Node& node);

  // If the input graph represents a while-loop, the edges pointing from a
  // "NextIteration" node to a "Merge" node add cyclic dependencies and make the
  // topological sorting impossible. We need to remove these edges from the
  // input graph to infer shapes and construct a Function. For each
  // "NextIteration" node, there are two operations, "NextIteration.source"
  // and "NextIteration.sink" are added to the MLIR module.
  using BackEdge = BackEdgeHelper::BackEdge;

  // Removes backedges from the input graph. The removed edges are added back to
  // to OpBuilder after the remaining graph is converted to the Function.
  Status RemoveBackedges(const Graph& graph);

  // Restores backedges removed during shape inference to the final Function.
  Status AddBackedges();

  // Restores a single backedge in the Function by adding a replicated
  // operation before the dst operation.
  Status AddBackedge(mlir::Operation* sink, mlir::Operation* dst,
                     int dst_input);

  // Adds the input arguments and return operation to the function. The
  // arguments are added as basic block argument. Also the argument types and
  // the id of the nodes from the input graph needs to be specified.
  Status ConvertFunctionArgAndRets(
      mlir::Block* bb, mlir::tf_executor::GraphOp graph_op,
      llvm::ArrayRef<mlir::Type> arg_types,
      const absl::InlinedVector<OutputTensor, 4>& arg_nodes,
      const absl::InlinedVector<OutputTensor, 4>& ret_nodes,
      const absl::InlinedVector<Node*, 4>& control_ret_nodes);

  // Gets the location information of the given node. It uses the
  // "original_node_name" in the NodeDef to get the corresponding file location
  // (FileLineColLoc) from the input DebugInfo and returns an CallSiteLoc. If
  // there are multiple "original_node_names", a FusedLoc is returned. If the
  // node name couldn't be found in the input DebugInfo, a NameLoc is used as
  // the location.
  mlir::Location GetLocation(const NodeDef& node);

  // Gets the location information string for the given node.
  std::string GetLocationStr(const Node& node, bool includeNodeName = false);

  // Inserts a placeholder node in the graph to replace the input node. Replaces
  // all the output edges of the input_node with the placeholder node, and
  // removes the input_node from the graph. The new node has the same name as
  // the input_node, so Nodespecs do not need any modification.
  // Note: This modifies the graph, and so any list of ordered nodes needs to be
  // reconstructed.
  StatusOr<Node*> ReplaceWithPlaceholderNode(const TensorShapeProto& shape,
                                             DataType dtype, Node* input_node);

  // Gets the input and output nodes corresponding to the specified input and
  // output nodes in specs_. If there are no input or output nodes specified,
  // nodes will be empty
  Status GetInputOutputNodes(std::unordered_set<const Node*>* nodes);

  // The input graph with backedges removed. The removed backedges are stored
  // in the back_edge_helper.
  BackEdgeHelper back_edge_helper_;
  // A map between node and output index, for each backedge.
  absl::flat_hash_map<const Node*, int> back_edge_node_output_;
  absl::flat_hash_map<const Node*, BackEdge> back_edge_dst_inputs_;
  // A map between sink and source operation of NextIteration
  absl::flat_hash_map<mlir::Operation*, mlir::Operation*>
      next_iteration_sink_source_;

  // All nodes and version information about the (copied) imported graph.
  std::unique_ptr<Graph> graph_;
  std::vector<Node*> ordered_nodes_;

  // Maps from a Node ID to a MLIR value.
  using NodeValueMap = absl::flat_hash_map<int, mlir::Operation*>;

  mlir::OpBuilder builder_;
  mlir::ModuleOp module_;
  mlir::MLIRContext* context_;
  std::unordered_map<std::string, std::string>* tf_name_to_mlir_name_;
  const FunctionLibraryDefinition& graph_flib_;
  const GraphImportConfig& specs_;
  const GraphDebugInfo& debug_info_;
  NodeValueMap node_values_;
  std::unique_ptr<ShapeRefiner> shape_refiner_;
};

// Returns true if the node with given name has a non primary output that is
// used by some other node as an input. Returns false if no outputs are in use
// or only the first output is in use.
bool HasNonPrimaryOutputInUse(const GraphDef& graph_def,
                              const std::string& node) {
  for (const auto& node_def : graph_def.node()) {
    for (const auto& input : node_def.input()) {
      if (absl::StartsWith(input, node + ":") && input != node + ":0") {
        return true;
      }
    }
  }
  return false;
}

// Updates the given LegacyFedInput node with Placeholder node if it is one of
// the inputs. Returns an error if non primary output of the LegacyFedInput node
// is in use and therefore can not be replaced by the Placeholder node that only
// has a single output.
Status UpdateLegacyFedInputNode(const GraphDef& graph_def,
                                const GraphImportConfig::InputArrays& inputs,
                                NodeDef* node) {
  const std::string& node_name = node->name();
  auto it = inputs.find(node_name);

  // Node is not an input.
  if (it == inputs.end()) return Status::OK();

  if (HasNonPrimaryOutputInUse(graph_def, node_name)) {
    return errors::InvalidArgument(
        "LegacyFedInput node ", node->name(),
        " has non primary output in use and can not be replaced with "
        "Placeholder node");
  }

  // Update op name, drop inputs and set attributes required by the Placeholder
  // op.
  *node->mutable_op() = "Placeholder";
  node->clear_attr();
  node->clear_input();
  AddNodeAttr("dtype", it->second.imported_dtype, node);
  AddNodeAttr("shape", it->second.shape, node);
  return Status::OK();
}

// Preprocesses GraphDef before it can be converted to Graph by,
// - Adding the default attributes to each node def if they are missing from
//   the GraphDef.
// - Replacing LegacyFedInput nodes with Placeholder nodes if
//   convert_legacy_fed_inputs option is enabled.
Status PreprocessGraphDef(const GraphImportConfig* specs, GraphDef* graph_def) {
  const tensorflow::OpRegistrationData* op_reg_data;
  for (auto& node_def : *graph_def->mutable_node()) {
    // TODO(hinsu): Completely deprecate support for LegacyFedInput ops. One
    // solution could be have a tool to let users upgrade old serialized graphs.
    if (specs && specs->convert_legacy_fed_inputs &&
        node_def.op() == "LegacyFedInput") {
      TF_RETURN_IF_ERROR(
          UpdateLegacyFedInputNode(*graph_def, specs->inputs, &node_def));
    }

    auto status =
        tensorflow::OpRegistry::Global()->LookUp(node_def.op(), &op_reg_data);
    if (!status.ok()) {
      // This is likely a function call node, so we should continue.
      VLOG(1) << status.ToString();
      continue;
    }
    ::tensorflow::AddDefaultsToNodeDef(op_reg_data->op_def, &node_def);
  }
  return Status::OK();
}

Status ImporterBase::RemoveBackedges(const Graph& graph) {
  // TODO(fengliuai): Converting to GraphDef and back is the easiest way to
  // clone a graph.
  // TODO(fengliuai): clone the graph without going to graph_def first.
  GraphDef graph_def;
  graph.ToGraphDef(&graph_def);
  graph_ = absl::make_unique<Graph>(graph.flib_def());
  GraphConstructorOptions opts;
  opts.allow_internal_ops = true;
  opts.add_default_attributes = false;
  TF_RETURN_IF_ERROR(::tensorflow::ConvertGraphDefToGraph(
      opts, std::move(graph_def), graph_.get()));

  // Remove all the backedges. So the nodes can be added to the shape refiner.
  TF_RETURN_IF_ERROR(back_edge_helper_.Remove(graph_.get()));
  VLOG(1) << "Found " << (back_edge_helper_.RemovedEdges().size())
          << " backedges.";

  // Creates a map for quickly identifying whether a node output is a backedge.
  for (const auto& edge : back_edge_helper_.RemovedEdges()) {
    if (back_edge_node_output_.find(edge.src) != back_edge_node_output_.end() &&
        back_edge_node_output_[edge.src] != edge.src_output) {
      return errors::FailedPrecondition(
          "More than one of the src node outputs are backedges!");
    }
    back_edge_node_output_[edge.src] = edge.src_output;
    // We expect a merge to receive a single backedge (multiple NextIteration
    // nodes feeding into the same merge is unexpected here).
    DCHECK(!back_edge_dst_inputs_.contains(edge.dst));
    back_edge_dst_inputs_[edge.dst] = edge;
  }

  // Obtains a RPO ordering, using node names as a tiebreak for stable sorting.
  GetReversePostOrder(
      *graph_, &ordered_nodes_,
      [](const Node* n1, const Node* n2) { return n1->name() < n2->name(); });

  return Status::OK();
}

StatusOr<Node*> ImporterBase::ReplaceWithPlaceholderNode(
    const TensorShapeProto& shape, DataType dtype, Node* input_node) {
  Node* placeholder_node;
  NodeBuilder builder(input_node->name(), "Placeholder");
  builder.Attr("shape", shape);
  builder.Attr("dtype", dtype);
  TF_RETURN_IF_ERROR(builder.Finalize(graph_.get(), &placeholder_node));

  while (!input_node->out_edges().empty()) {
    const Edge* oe = *input_node->out_edges().begin();
    TF_RETURN_IF_ERROR(graph_->UpdateEdge(
        placeholder_node,
        oe->src_output() == Graph::kControlSlot ? Graph::kControlSlot : 0,
        oe->dst(), oe->dst_input()));
  }

  graph_->RemoveNode(input_node);

  return placeholder_node;
}

Status ImporterBase::GetInputOutputNodes(
    std::unordered_set<const Node*>* nodes) {
  auto node_name_map = graph_->BuildNodeNameIndex();
  auto add_node = [&](const string& name) {
    auto it = node_name_map.find(name);
    if (it == node_name_map.end()) {
      return errors::FailedPrecondition(
          absl::StrCat("Graph does not contain node :", name));
    }
    nodes->insert(it->second);
    return Status::OK();
  };

  for (const auto& input : specs_.inputs) {
    TF_RETURN_IF_ERROR(add_node(input.first));
  }

  for (const auto& output_node_name : specs_.output_arrays) {
    TF_RETURN_IF_ERROR(add_node(output_node_name));
  }

  return Status::OK();
}

// TODO(fengliuai): Replace the iterative algorithm by an one pass propagation
Status ImporterBase::AddNodesToShapeRefiner() {
  shape_refiner_ = absl::make_unique<ShapeRefiner>(graph_->versions(),
                                                   graph_->op_registry());
  // Some operations (for example "TPUExecute") don't have shape inference
  // function defined, so we should set this to false for adding nodes with
  // these types of operations.
  shape_refiner_->set_require_shape_inference_fns(false);
  shape_refiner_->set_function_library_for_shape_inference(&graph_flib_);

  // First add all nodes to the refiner.
  for (Node* node : ordered_nodes_) {
    // We need to use a TensorFlow node to teach the shape refiner that user
    // specifies certain data type and shape for the inputs in the `specs_`.
    // This node shouldn't have any inputs, only have one output and its
    // output type/shape is only determined by its "named" attributes. (The
    // attributes should have fixed names so we can use the info from `specs_`
    // to set the value of them.) `Placeholder` satisfies these constraints.
    //
    // Therefore, if the input node isn't a `Placeholder`, we create one and use
    // it to replace the original input node, so the shape refiner can
    // successfully propagate the user's input type and shape to the rest of the
    // graph.
    auto it = specs_.inputs.find(node->name());
    if (it != specs_.inputs.end()) {
      auto node_name = node->op_def().name();
      if (node_name != "Placeholder" && node_name != "LegacyFedInput" &&
          node_name != "_Arg") {
        // We do not handle the case where the input node has multple outputs
        if (node->num_outputs() > 1) {
          return errors::FailedPrecondition(absl::StrCat(
              "Input arrays can only have op with single output. Node op:",
              node_name));
        }
        // For single output nodes, replace them with Placeholder node
        TF_ASSIGN_OR_RETURN(
            node, ReplaceWithPlaceholderNode(it->second.shape,
                                             it->second.imported_dtype, node));
      } else {
        node->AddAttr("shape", it->second.shape);
        node->AddAttr("dtype", it->second.imported_dtype);
      }
    }
    // Adds the node to the shape refiner.
    TF_RETURN_WITH_CONTEXT_IF_ERROR(shape_refiner_->AddNode(node),
                                    GetLocationStr(*node));

    // We currently have no other way to get shapes from ReadVariableOp's.
    // Some graphs seem to have _output_shapes attributes on them, so use that
    // if possible.
    // TODO(silvasean): Ideally, we would do this in a separate shape inference
    // pass to avoid adding complexity to the importer. But right now, we don't
    // have an MLIR-native shape inference pass, so we need to do this while we
    // still have the Graph around, i.e. here, in the importer.
    if (node->op_def().name() == "ReadVariableOp") {
      // TODO(silvasean): In some graphs, this seems to be annotated on every
      // node. Why and by whom?
      // TODO(b/140588338): We should ideally incorporate that information for
      // all nodes, but right now, this can result in e.g. an Identity node with
      // signature such as
      // `(tensor<?x?xf32>) -> tensor<?x9216xf32>` which fails the verifier
      // (which checks for exact type equality; _output_shapes results in
      // us shoehorning in the more-precise type on the output).
      if (const AttrValue* attr = node->attrs().Find("_output_shapes")) {
        auto& list = attr->list();
        for (auto shape : llvm::enumerate(list.shape())) {
          auto* node_context = shape_refiner_->GetContext(node);
          shape_inference::ShapeHandle handle;
          TF_RETURN_WITH_CONTEXT_IF_ERROR(
              node_context->MakeShapeFromShapeProto(shape.value(), &handle),
              GetLocationStr(*node));
          node_context->set_output(shape.index(), handle);
        }
      }
    }

    // If it is the argument node, the shape handle is set explicitly, so it
    // can be propagated to the body nodes of the function.
    if (StringPiece(node->type_string()) == FunctionLibraryDefinition::kArgOp) {
      auto* node_context = shape_refiner_->GetContext(node);
      DCHECK(node_context != nullptr);
      auto it = node->def().attr().find("shape");
      if (it != node->def().attr().end()) {
        shape_inference::ShapeHandle handle;
        TF_RETURN_WITH_CONTEXT_IF_ERROR(
            node_context->MakeShapeFromShapeProto(it->second.shape(), &handle),
            GetLocationStr(*node));
        node_context->set_output(0, handle);
      } else {
        node_context->set_output(0, node_context->UnknownShape());
      }
    }
  }

  // Since we might have inserted and removed nodes from the graph, fix
  // source/sink edges and reconstruct the RPO ordering of nodes
  FixupSourceAndSinkEdges(graph_.get());

  // Prune nodes in the graph that are not reachable from the output.
  if (specs_.prune_unused_nodes) {
    std::unordered_set<const Node*> prune_start;
    TF_RETURN_IF_ERROR(GetInputOutputNodes(&prune_start));
    if (!prune_start.empty()) {
      if (PruneForReverseReachability(graph_.get(), prune_start)) {
        VLOG(1) << "Pruned unused nodes in graphdef";
      } else {
        VLOG(1) << "No unused nodes in graphdef to prune";
      }
    } else {
      VLOG(1) << "No output nodes specified, skipping pruning";
    }
  } else {
    VLOG(1) << "Pruning unused nodes in graphdef is disabled";
  }

  // Re-initialize ordered_nodes_ since we might have modified the graph.
  GetReversePostOrder(
      *graph_, &ordered_nodes_,
      [](const Node* n1, const Node* n2) { return n1->name() < n2->name(); });

  VLOG(1) << "Inferring graph shapes to fixpoint";

  // The "changed" information from UpdateNode can give false positives, so we
  // create a dedicated method to verify the shapes are not changed before and
  // after the shape refine.
  auto same_inferred_shape = [](shape_inference::InferenceContext* c,
                                shape_inference::ShapeHandle s0,
                                shape_inference::ShapeHandle s1) -> bool {
    if (s0.SameHandle(s1) || (!c->RankKnown(s0) && !c->RankKnown(s1))) {
      return true;
    }
    if (c->Rank(s0) != c->Rank(s1)) {
      return false;
    }
    for (int i = 0; i < c->Rank(s0); ++i) {
      if (!c->Dim(s0, i).SameHandle(c->Dim(s1, i))) {
        int64 val0 = c->Value(c->Dim(s0, i));
        int64 val1 = c->Value(c->Dim(s1, i));
        // Negative value is treated as unknown so all negative values indicate
        // the same dimension.
        if (val0 >= 0 && val1 >= 0 && val0 != val1) return false;
      }
    }
    return true;
  };

  bool changed = true;
  int i = 0;
  const int kMaxIterationCount = 2;
  while (changed && i != kMaxIterationCount) {
    changed = false;
    for (const Node* node : ordered_nodes_) {
      auto* shape_context = shape_refiner_->GetContext(node);
      DCHECK(shape_context != nullptr);
      absl::InlinedVector<shape_inference::ShapeHandle, 4> existing;
      existing.reserve(shape_context->num_outputs());
      for (int o = 0; o < shape_context->num_outputs(); ++o) {
        existing.push_back(shape_context->output(o));
      }
      bool inferred = false;
      TF_RETURN_WITH_CONTEXT_IF_ERROR(
          shape_refiner_->UpdateNode(node, /*relax=*/false, &inferred),
          GetLocationStr(*node));
      for (int o = 0; o < shape_context->num_outputs(); ++o) {
        if (!same_inferred_shape(shape_context, shape_context->output(o),
                                 existing[o])) {
          changed = true;
          break;
        }
      }
    }
    ++i;
  }
  if (i >= kMaxIterationCount) {
    LOG(WARNING) << "Graph shapes did not converge to a fixpoint within "
                 << kMaxIterationCount
                 << " iterations. Graph shapes may be conservative.";
  }
  VLOG(1) << "Graph shapes were inferred with " << (i - 1)
          << " extra rounds of analysis to reach a fixpoint.";
  return Status::OK();
}

StatusOr<mlir::TensorType> ImporterBase::InferInputType(const Node& node,
                                                        int idx,
                                                        mlir::Builder builder) {
  ExtendedInferenceContext* shape_context =
      shape_refiner_->GetExtendedContext(&node);
  DataType dtype = shape_context->input_type(idx);
  auto* context = shape_context->get_context();
  return ConvertDataTypeAndShape(dtype, context->input(idx),
                                 context->input_handle_shapes_and_types(idx),
                                 context, builder);
}

StatusOr<mlir::TensorType> ImporterBase::InferOutputType(
    const Node& node, int idx, mlir::Builder builder) {
  ExtendedInferenceContext* shape_context =
      shape_refiner_->GetExtendedContext(&node);
  DataType dtype = shape_context->output_type(idx);
  auto* context = shape_context->get_context();
  return ConvertDataTypeAndShape(dtype, context->output(idx),
                                 context->output_handle_shapes_and_types(idx),
                                 context, builder);
}

StatusOr<mlir::TensorType> ImporterBase::ConvertDataTypeAndShape(
    DataType dtype, const shape_inference::ShapeHandle& handle,
    const std::vector<shape_inference::ShapeAndType>* handle_subtypes,
    shape_inference::InferenceContext* context, mlir::Builder builder) {
  TF_ASSIGN_OR_RETURN(auto subtypes,
                      ConvertSubtypes(handle_subtypes, context, builder));

  // TODO(hinsu): Store subtypes information for DT_RESOURCE element type as
  // well.
  mlir::Type element_type;
  if (dtype == DT_VARIANT) {
    element_type = mlir::TF::VariantType::get(subtypes, context_);
  } else {
    TF_RETURN_IF_ERROR(
        ::tensorflow::ConvertDataType(dtype, builder, &element_type));
  }
  return ConvertElementTypeAndShape(element_type, handle, context, builder);
}

StatusOr<mlir::TensorType> ImporterBase::ConvertElementTypeAndShape(
    mlir::Type element_type, const shape_inference::ShapeHandle& handle,
    shape_inference::InferenceContext* context, mlir::Builder builder) {
  if (!context->RankKnown(handle)) {
    return builder.getTensorType(element_type);
  }

  // Sentinel for an unknown dimension size. getTensorType interprets any
  // negative value as an unknown dimension.
  // TODO(jmolloy): Ideally this shouldn't be a local sentinel.
  const int64_t kUnknownDim = -1;

  absl::InlinedVector<int64_t, 4> dimensions;
  int32 rank = context->Rank(handle);
  dimensions.reserve(rank);
  for (int i = 0; i < rank; ++i) {
    auto dim_handle = context->Dim(handle, i);
    if (!context->ValueKnown(dim_handle))
      dimensions.push_back(kUnknownDim);
    else
      dimensions.push_back(context->Value(dim_handle));
  }

  return builder.getTensorType(
      llvm::makeArrayRef(dimensions.begin(), dimensions.end()), element_type);
}

StatusOr<ImporterBase::ElementSubtypes> ImporterBase::ConvertSubtypes(
    const std::vector<shape_inference::ShapeAndType>* handle_subtypes,
    shape_inference::InferenceContext* context, mlir::Builder builder) {
  ElementSubtypes subtypes;
  if (!handle_subtypes) return subtypes;

  subtypes.reserve(handle_subtypes->size());
  for (const auto& subtype : *handle_subtypes) {
    mlir::Type element_type;
    TF_RETURN_IF_ERROR(
        ::tensorflow::ConvertDataType(subtype.dtype, builder, &element_type));
    TF_ASSIGN_OR_RETURN(mlir::TensorType type,
                        ConvertElementTypeAndShape(element_type, subtype.shape,
                                                   context, builder));
    subtypes.push_back(type);
  }
  return subtypes;
}

Status ImporterBase::ConvertFunctionCallAttribute(
    const std::string& base_name, const AttrValue& value,
    llvm::SmallVector<mlir::NamedAttribute, 4>* attributes) {
  TF_ASSIGN_OR_RETURN(auto func_attr,
                      ConvertFunctionCallName(value.func().name()));
  attributes->push_back(builder_.getNamedAttr(base_name, func_attr));

  for (const auto& it : value.func().attr()) {
    auto name = absl::StrCat(base_name, ".", it.first);
    TF_ASSIGN_OR_RETURN(auto value, ConvertAttributeValue(it.second));
    attributes->push_back(builder_.getNamedAttr(name, value));
  }
  return Status::OK();
}

StatusOr<mlir::SymbolRefAttr> ImporterBase::ConvertFunctionCallName(
    const std::string& func_name) {
  TF_RETURN_IF_ERROR(ConvertLibFunction(func_name));
  auto mlir_func_name = (*tf_name_to_mlir_name_)[func_name];
  auto func = module_.lookupSymbol<mlir::FuncOp>(mlir_func_name);
  return builder_.getSymbolRefAttr(func);
}

StatusOr<mlir::Attribute> ImporterBase::ConvertAttributeValue(
    const AttrValue& value) {
  switch (value.value_case()) {
    case AttrValue::kI:
      return builder_.getI64IntegerAttr(value.i());
    case AttrValue::kS:
      return builder_.getStringAttr(value.s());
    case AttrValue::kF:
      return builder_.getFloatAttr(builder_.getF32Type(), value.f());
    case AttrValue::kB:
      return builder_.getBoolAttr(value.b());
    case AttrValue::kType:
      return builder_.getStringAttr(
          mangling_util::MangleDataType(value.type()));
    case AttrValue::kShape:
      return builder_.getStringAttr(mangling_util::MangleShape(value.shape()));
    case AttrValue::kTensor:
      return ConvertTensorProto(value.tensor());
    case AttrValue::kList: {
      absl::InlinedVector<mlir::Attribute, 8> attrs;
      for (const auto& item : value.list().i())
        attrs.push_back(builder_.getI64IntegerAttr(item));
      for (const auto& item : value.list().s())
        attrs.push_back(builder_.getStringAttr(item));
      for (const auto& item : value.list().f())
        attrs.push_back(builder_.getFloatAttr(builder_.getF32Type(), item));
      for (const auto& item : value.list().b())
        attrs.push_back(builder_.getBoolAttr(item));
      for (const auto& item : value.list().type()) {
        attrs.push_back(builder_.getStringAttr(
            mangling_util::MangleDataType(static_cast<DataType>(item))));
      }
      for (const auto& item : value.list().shape()) {
        attrs.push_back(
            builder_.getStringAttr(mangling_util::MangleShape(item)));
      }
      for (const auto& item : value.list().tensor()) {
        TF_ASSIGN_OR_RETURN(auto attr, ConvertTensorProto(item));
        attrs.push_back(attr);
      }
      for (const auto& item : value.list().func()) {
        TF_ASSIGN_OR_RETURN(auto attr, ConvertFunctionCallName(item.name()));
        if (item.attr_size() != 0)
          return errors::Unimplemented(
              "func attributes with non-zero attr.size()");
        attrs.push_back(attr);
      }
      return builder_.getArrayAttr(
          llvm::makeArrayRef(attrs.begin(), attrs.end()));
    }
    case AttrValue::kFunc:
      return errors::Unknown("kFunc type should be handled separately!");
    case AttrValue::VALUE_NOT_SET:
      return builder_.getUnitAttr();
    // kPlaceholder is not implemented.
    default:
      return errors::Unimplemented(
          absl::StrCat("Attribute ", value.DebugString()));
  }
}

void ImporterBase::GetArgsAndRetsFromFunctionBody(
    const FunctionBody& fbody, absl::InlinedVector<OutputTensor, 4>* arg_nodes,
    absl::InlinedVector<OutputTensor, 4>* ret_nodes,
    absl::InlinedVector<Node*, 4>* control_ret_nodes) {
  arg_nodes->reserve(fbody.arg_nodes.size());
  ret_nodes->reserve(fbody.ret_nodes.size());
  for (auto arg : fbody.arg_nodes) {
    arg_nodes->emplace_back(arg, 0);
  }
  for (auto ret : fbody.ret_nodes) {
    ret_nodes->emplace_back(ret, 0);
  }
  *control_ret_nodes = fbody.control_ret_nodes;
}

Status ImporterBase::ConvertLibFunction(llvm::StringRef func_name) {
  // If the library function has been converted already, nothing needs to be
  // done.
  if (tf_name_to_mlir_name_->find(func_name) != tf_name_to_mlir_name_->end())
    return Status::OK();

  std::string mlir_func_name =
      graph_flib_.UniqueFunctionName(StringRefToView(func_name));
  (*tf_name_to_mlir_name_)[func_name] = mlir_func_name;

  const auto& func_lib = graph_flib_;
  const auto* func_def = func_lib.Find(func_name);
  if (func_def == nullptr) {
    return errors::FailedPrecondition(
        absl::StrCat("Failed to find function '", StringRefToView(func_name),
                     "'. The imported TensorFlow GraphDef is ill-formed."));
  }

  // Converts the function definition to a graph.
  std::unique_ptr<FunctionBody> fbody;
  TF_RETURN_IF_ERROR(
      FunctionDefToBodyHelper(*func_def, AttrSlice(), &func_lib, &fbody));

  // Converts the argument and return types to mlir types.
  absl::InlinedVector<mlir::NamedAttribute, 8> attributes;
  attributes.reserve(func_def->attr_size());
  for (const auto& name_and_value : func_def->attr()) {
    // This is a function definition attribute, so it shouldn't contain
    // kFunc attribute and it is treated as normal one.
    TF_ASSIGN_OR_RETURN(auto attr,
                        ConvertAttributeValue(name_and_value.second));
    std::string attr_name =
        mangling_util::MangleAttributeName(name_and_value.first);
    attributes.push_back(builder_.getNamedAttr(attr_name, attr));
  }

  // Checks opdef stateful attribute and import that as Function Attribute
  if (func_def->signature().is_stateful()) {
    auto stateful_str = mlir::TF::TensorFlowDialect::GetStatefulAttrName();
    attributes.push_back(
        builder_.getNamedAttr(stateful_str, builder_.getUnitAttr()));
  }

  // Checks for an associated custom gradient function. Adds it to the attribute
  // list of this function.
  auto grad_func_name = func_lib.FindGradient(func_name);
  if (!grad_func_name.empty()) {
    TF_RETURN_IF_ERROR(ConvertLibFunction(grad_func_name));
    auto mlir_grad_func_name = (*tf_name_to_mlir_name_)[grad_func_name];
    auto grad_func = module_.lookupSymbol<mlir::FuncOp>(mlir_grad_func_name);
    auto gradient_attr = builder_.getSymbolRefAttr(grad_func);
    auto grad_string = mlir::TF::TensorFlowDialect::GetGradientAttrName();
    attributes.push_back(builder_.getNamedAttr(grad_string, gradient_attr));
  }

  // Converts the graph to a MLIR function and adds it to the module.
  // We populate the NodeSpec so that all the _Arg ops get their shape
  // added correctly.
  GraphImportConfig specs;
  for (const auto& name_and_value : func_def->attr()) {
    if (name_and_value.first == "_input_shapes") {
      auto& list = name_and_value.second.list();
      auto& signature = func_def->signature();
      DCHECK_EQ(list.shape_size(), signature.input_arg_size());
      for (int i = 0; i < list.shape_size(); i++) {
        auto& input_arg = signature.input_arg(i);
        auto& array_info = specs.inputs[input_arg.name()];
        array_info.imported_dtype = input_arg.type();
        array_info.shape = list.shape(i);
      }
    }
  }

  ImporterBase child_importer(graph_flib_, debug_info_, specs, module_,
                              tf_name_to_mlir_name_);
  TF_RETURN_IF_ERROR(child_importer.PrepareConvert(*fbody->graph));

  TF_ASSIGN_OR_RETURN(auto func_type,
                      child_importer.InferLibFunctionType(*fbody));

  absl::InlinedVector<OutputTensor, 4> arg_nodes;
  absl::InlinedVector<OutputTensor, 4> ret_nodes;
  absl::InlinedVector<Node*, 4> control_ret_nodes;
  GetArgsAndRetsFromFunctionBody(*fbody, &arg_nodes, &ret_nodes,
                                 &control_ret_nodes);

  TF_RETURN_IF_ERROR(child_importer.Convert(
      mlir_func_name, func_type, arg_nodes, ret_nodes, control_ret_nodes,
      llvm::makeArrayRef(attributes.begin(), attributes.end())));
  return Status::OK();
}

Status ImporterBase::PrepareConvert(const Graph& graph) {
  TF_RETURN_IF_ERROR(RemoveBackedges(graph));
  TF_RETURN_IF_ERROR(AddNodesToShapeRefiner());
  return Status::OK();
}

Status ImporterBase::Convert(
    llvm::StringRef func_name, mlir::FunctionType func_type,
    const absl::InlinedVector<OutputTensor, 4>& arg_nodes,
    const absl::InlinedVector<OutputTensor, 4>& ret_nodes,
    const absl::InlinedVector<Node*, 4>& control_ret_nodes,
    llvm::ArrayRef<mlir::NamedAttribute> attrs) {
  // TODO(b/122040776): Uses debug info for FunctionDef.
  auto function = mlir::FuncOp::create(mlir::UnknownLoc::get(context_),
                                       func_name, func_type, attrs);

  module_.push_back(function);
  // Seeds the builder with an initial block.
  function.addEntryBlock();
  builder_ = mlir::OpBuilder(function.getBody());
  auto* bb = &function.front();

  // Create the graph operation in which we will convert the individual nodes.
  auto graph = builder_.create<mlir::tf_executor::GraphOp>(
      function.getLoc(), func_type.getResults());
  builder_.createBlock(&graph.body());

  for (const Node* node : ordered_nodes_) {
    TF_RETURN_IF_ERROR(ConvertNode(*node));
  }

  // Adds the backedges back to the function by creating the source and sink
  // pairs.
  TF_RETURN_IF_ERROR(AddBackedges());

  return ConvertFunctionArgAndRets(bb, graph, func_type.getInputs(), arg_nodes,
                                   ret_nodes, control_ret_nodes);
}

Status ImporterBase::ConvertFunctionArgAndRets(
    mlir::Block* bb, mlir::tf_executor::GraphOp graph_op,
    llvm::ArrayRef<mlir::Type> arg_types,
    const absl::InlinedVector<OutputTensor, 4>& arg_nodes,
    const absl::InlinedVector<OutputTensor, 4>& ret_nodes,
    const absl::InlinedVector<Node*, 4>& control_ret_nodes) {
  for (int i = 0, e = arg_types.size(); i < e; ++i) {
    // The lookup can't fail here: otherwise some nodes in the function haven't
    // be converted to mlir operations and don't have a mapping.
    mlir::Operation* island =
        node_values_.find(arg_nodes[i].node->id())->second;
    // We are looking for the instruction inside the island
    mlir::Block& body = island->getRegion(0).front();
    mlir::Operation* inst = &body.front();

    auto* bb_arg = bb->getArgument(i);
    mlir::Value* arg_def = bb_arg;

    // If this is an arg node, just forward the entry block argument
    if (arg_nodes[i].node->IsArg()) {
      island->getResult(0)->replaceAllUsesWith(arg_def);
      island->dropAllReferences();
      island->erase();
      continue;
    }

    // This is an input node, we'll create a new input operation by suffixing
    // the existing one with .input.
    auto inst_name = inst->getName().getStringRef();
    mlir::OperationState state(inst->getLoc(),
                               inst_name.str().append(".input"));
    state.attributes.append(inst->getAttrs().begin(), inst->getAttrs().end());

    for (auto* r : inst->getResults()) state.types.push_back(r->getType());

    state.operands.append(inst->getOperands().begin(),
                          inst->getOperands().end());
    state.operands.push_back(bb_arg);
    builder_.setInsertionPoint(inst);
    auto* input = builder_.createOperation(state);
    arg_def = input->getResult(arg_nodes[i].index);

    for (auto index = 0; index < inst->getNumResults(); index++) {
      inst->getResult(index)->replaceAllUsesWith(arg_def);
    }
    inst->dropAllReferences();
    inst->erase();
  }

  llvm::SmallVector<mlir::Value*, 8> inst_to_return;
  for (const auto& ret : ret_nodes) {
    auto* inst = node_values_[ret.node->id()];
    auto op = absl::string_view(ret.node->type_string());
    if (op == FunctionLibraryDefinition::kRetOp ||
        op == FunctionLibraryDefinition::kDeviceRetOp) {
      // Lookup the instruction inside the island
      auto island_op = llvm::cast<mlir::tf_executor::IslandOp>(inst);
      mlir::Operation* inner_op = &island_op.GetBody().front();
      // Remove kRetOp or kDeviceRetOp operation and return its operand.
      // kRetOp and kDeviceRetOp should have just one operand unless they have
      // control dependencies.
      if (inner_op->getNumOperands() != 1)
        return errors::Unimplemented("Return node with multiple inputs.");
      inst_to_return.push_back(inner_op->getOperand(0));
      inst->dropAllReferences();
      inst->erase();
    } else {
      inst_to_return.push_back(inst->getResult(ret.index));
    }
  }

  for (Node* control_ret : control_ret_nodes) {
    auto* inst = node_values_[control_ret->id()];
    inst_to_return.push_back(*std::prev(inst->result_end()));
  }

  // Terminate the function by adding a Fetch operation to terminate the graph
  // and a return operation to return the Graph results.
  builder_.setInsertionPointToEnd(&graph_op.body().front());
  builder_.create<mlir::tf_executor::FetchOp>(graph_op.getLoc(),
                                              inst_to_return);
  inst_to_return.assign(graph_op.getResults().begin(),
                        graph_op.getResults().end());
  builder_.setInsertionPointToEnd(bb);
  builder_.create<mlir::ReturnOp>(mlir::UnknownLoc::get(context_),
                                  inst_to_return);
  return Status::OK();
}

mlir::Location ImporterBase::GetLocation(const NodeDef& node_def) {
  const auto& debug_info = debug_info_.traces();

  // Get the CallSiteLoc for a node name.
  // - If the debug info of the node couldn't be found, the caller of the
  //   returned CallSiteLoc is set to an UnknownLoc;
  // - If the debug info of the node is found, the caller of the returned
  //   CallSiteLoc is set to a call stack which is formed by the debug info.
  auto node_name_to_call_site = [&](const std::string& name) -> mlir::Location {
    auto name_id = mlir::Identifier::get(name, context_);
    const auto& location_it = debug_info.find(name);
    if (location_it == debug_info.end()) {
      // Only the node name is stored if the location is unknown.
      return mlir::NameLoc::get(name_id, context_);
    }

    // Convert the stack trace to a chain of mlir::CallSiteLocs.
    const auto& trace = location_it->second;
    llvm::SmallVector<mlir::Location, 4> locations;
    locations.reserve(trace.file_line_cols_size());
    for (const auto& location : trace.file_line_cols()) {
      const auto& file = debug_info_.files(location.file_index());
      auto file_name = mlir::Identifier::get(file, context_);
      auto file_line_loc = mlir::FileLineColLoc::get(file_name, location.line(),
                                                     location.col(), context_);
      locations.push_back(file_line_loc);
    }
    // Handle empty location vector.
    if (locations.empty()) return mlir::NameLoc::get(name_id, context_);

    // Use the front FileLineColLoc to generate a NameLoc.
    mlir::Location node_name_loc =
        mlir::NameLoc::get(name_id, locations.front());

    // If there are more locations then generate a stack trace, otherwise just
    // return the name loc.
    auto callsite_locs = llvm::makeArrayRef(locations).drop_front();
    return callsite_locs.empty()
               ? node_name_loc
               : mlir::CallSiteLoc::get(node_name_loc, callsite_locs);
  };

  // For NextIteration nodes, location is used to pair source and sink nodes.
  // Hence, we use node name as location to keep it unique.
  // TODO(prakalps): In future the plan is to use tokens to pair source/sink
  // nodes. Then NextIteration nodes would not need to be handled seprately.
  if (node_def.op() == "NextIteration")
    return node_name_to_call_site(node_def.name());

  auto original_nodes =
      node_def.experimental_debug_info().original_node_names();
  auto original_funcs =
      node_def.experimental_debug_info().original_func_names();

  if (original_nodes.empty()) {
    // If the original nodes are not defined in the node def, but the current
    // node name is contained in the debug info file, then we fall back to use
    // the current node name to get the location info. Otherwise, use a
    // NameLoc with node name as in a TensorFlow graph the node name is unique.
    auto& curr_node_name = node_def.name();
    if (debug_info.find(curr_node_name) == debug_info.end()) {
      return mlir::NameLoc::get(mlir::Identifier::get(curr_node_name, context_),
                                context_);
    } else {
      return node_name_to_call_site(curr_node_name);
    }
  } else {
    // If the original nodes are defined, then we use them to get a list of
    // call sites, and then fuse them to a single fused location.
    llvm::SmallVector<mlir::Location, 4> node_call_sites;
    node_call_sites.reserve(original_nodes.size());
    for (int i = 0, e = original_nodes.size(); i != e; ++i) {
      auto node_name = original_nodes[i];
      auto func_name = (i < original_funcs.size()) ? original_funcs[i] : "";
      // Use the catenation of function and node names as the lookup key.
      // This matches the way that the key is formed on the python side.
      std::string key = node_name + "@" + func_name;
      node_call_sites.push_back(node_name_to_call_site(key));
    }
    return mlir::FusedLoc::get(node_call_sites, context_);
  }
}

std::string ImporterBase::GetLocationStr(const Node& node,
                                         bool includeNodeName) {
  const auto location = GetLocation(node.def());
  std::string s;
  llvm::raw_string_ostream ss(s);
  location.print(ss);
  ss.flush();
  // Removes the node name prefix if it exists.
  if (!s.empty() && s[0] == '\"' && s.find_first_of(node.name()) == 1) {
    return s.replace(0, node.name().size() + 3, "");
  }
  return s;
}

mlir::Operation* ImporterBase::createOperation(
    const Node& node, llvm::StringRef op_name,
    const mlir::OperationState& result,
    const llvm::SmallVectorImpl<mlir::Value*>& control_operands) {
  // For the tf.executor specific operations (not wrapped in an island), we
  // have an extra returned value for the control result, and we concatenate
  // control and non-control operands.
  mlir::SmallVector<mlir::Type, 4> types(result.types);
  types.push_back(mlir::tf_executor::ControlType::get(builder_.getContext()));
  mlir::SmallVector<mlir::Value*, 4> operands(result.operands);
  operands.append(control_operands.begin(), control_operands.end());

  auto loc = result.location;
  // Dispatch based on the name and create the appropriate operation.
  if (node.IsSwitch()) {
    // Switch and _SwitchN both are in switch class, differentiate based on
    // op name.
    if (node.op_def().name() == "_SwitchN") {
      return builder_.create<mlir::tf_executor::SwitchNOp>(loc, types, operands,
                                                           result.attributes);
    }
    return builder_.create<mlir::tf_executor::SwitchOp>(loc, types, operands,
                                                        result.attributes);
  }
  if (node.IsMerge()) {
    return builder_.create<mlir::tf_executor::MergeOp>(loc, types, operands,
                                                       result.attributes);
  }
  if (node.IsNextIteration()) {
    // NextIteration is a bit special, we create a pair of operations that are
    // linked together through a token returned by the source.
    // We make use of a separate builder to insert the source at the top of
    // the block.
    mlir::OpBuilder builder_at_begin(builder_.getBlock(),
                                     builder_.getBlock()->begin());
    auto source_op =
        builder_at_begin.create<mlir::tf_executor::NextIterationSourceOp>(
            loc, operands[0]->getType(), result.attributes);
    return builder_.create<mlir::tf_executor::NextIterationSinkOp>(
        loc, source_op.token(), operands, result.attributes);
  }
  if (node.IsLoopCond()) {
    return builder_.create<mlir::tf_executor::LoopCondOp>(loc, types, operands,
                                                          result.attributes);
  }
  if (node.IsEnter()) {
    return builder_.create<mlir::tf_executor::EnterOp>(loc, types, operands,
                                                       result.attributes);
  }
  if (node.IsExit()) {
    return builder_.create<mlir::tf_executor::ExitOp>(loc, types, operands,
                                                      result.attributes);
  }
  if (node.IsControlTrigger()) {
    return builder_.create<mlir::tf_executor::ControlTriggerOp>(
        loc, operands, result.attributes);
  }
  // Regular TensorFlow operation are wrapped in a tf_executor.island.
  auto island = builder_.create<mlir::tf_executor::IslandOp>(
      result.location, types, control_operands,
      mlir::ArrayRef<mlir::NamedAttribute>{});
  island.body().push_back(new mlir::Block);
  mlir::OpBuilder island_builder(&island.GetBody());

  // Create the operation inside the island now.
  mlir::Operation* inner_op = island_builder.createOperation(result);

  // Add the terminator for the island
  mlir::SmallVector<mlir::Value*, 8> ret_vals(inner_op->getResults());
  island_builder.create<mlir::tf_executor::YieldOp>(result.location, ret_vals);
  return island.getOperation();
}

Status ImporterBase::ConvertNode(const Node& node) {
  if (!node.IsOp()) {
    // Don't import the pseudo-nodes _SOURCE or _SINK. These are added by
    // Graph and don't exist in GraphDef.
    return Status::OK();
  }

  // If it is a custom OP, its definition should be found in the library. We
  // create the MLIR function and insert it to the module if it doesn't exist.
  std::string node_type_name = node.type_string();
  const auto* func_def = graph_flib_.Find(node_type_name);
  if (func_def) {
    TF_RETURN_IF_ERROR(ConvertLibFunction(node_type_name));
    node_type_name = (*tf_name_to_mlir_name_)[node_type_name];
  }

  auto get_full_op_name = [&](const std::string& op_name) {
    const char* kTfPrefix = "tf.";
    return kTfPrefix + op_name;
  };

  std::string op_name = get_full_op_name(node_type_name);
  if (back_edge_node_output_.contains(&node)) {
    op_name = op_name + ".sink";
  }

  const auto& node_def = node.def();
  mlir::OperationState result(GetLocation(node_def), op_name);

  for (int i = 0; i < node.num_outputs(); ++i) {
    // The backedge has been removed, so we shouldn't count the corresponding
    // output from the src node when converting to an operation.
    if (back_edge_node_output_.contains(&node) &&
        back_edge_node_output_[&node] == i) {
      continue;
    }
    TF_ASSIGN_OR_RETURN(auto type, InferOutputType(node, i, builder_));
    result.types.push_back(type);
  }

  // Surprisingly input edges can be nondeterministically ordered. This
  // particularly seems to be the case for the control edges between _SOURCE
  // and _SINK that the Graph constructor inserts. Copy the input edges and
  // sort the edges, but only the control edges, not data edges!
  // TODO(jmolloy): We should probably just ignore _SOURCE and _SINK nodes.
  // They'll break roundtripping anyway unless we strip them when converting
  // back to graphdef.
  absl::InlinedVector<const Edge*, 8> in_edges(node.in_edges().size());
  absl::c_copy(node.in_edges(), in_edges.begin());
  absl::c_stable_sort(in_edges, [](const Edge* e1, const Edge* e2) {
    if (e1->IsControlEdge() && !e2->IsControlEdge()) return false;
    if (!e1->IsControlEdge() && e2->IsControlEdge()) return true;
    return e1->dst_input() < e2->dst_input();
  });

  result.operands.reserve(in_edges.size());

  // Collect the control operands separately, they will be held by the island.
  mlir::SmallVector<mlir::Value*, 8> control_operands;

  for (const auto* input_edge : in_edges) {
    const Node& input_node = *input_edge->src();
    if (input_node.IsSource()) {
      if (in_edges.size() != 1) {
        return errors::FailedPrecondition(
            "The node has other inputs besides the _Source node");
      }
      // We don't import the _SOURCE node.
      continue;
    }
    if (input_node.IsArg() && input_edge->IsControlEdge()) {
      // Currently we have not reached consensus as to what TF function
      // semantics are (b/133509504). Here we assume that all arguments to a
      // function should be available before we start execution of any internal
      // node. This makes the control dependencies between function arguments
      // and internal nodes redundant, and so we do not import them. The TF
      // inliner however assumes no such dependency between function args and
      // internal nodes exists, unless explicitly stated. Since we drop control
      // dependencies here, it leads to loss of information. If the function is
      // inlined later, the inliner would not know of these explicit control
      // dependencies present in the original graph.
      continue;
    }
    if (node_values_.find(input_node.id()) == node_values_.end())
      return errors::FailedPrecondition(
          "Graph not traversed in reverse post order; use seen before def!");
    mlir::Operation* inst = node_values_[input_node.id()];
    if (input_edge->IsControlEdge())
      control_operands.push_back(inst->getResult(inst->getNumResults() - 1));
    else
      result.operands.push_back(inst->getResult(input_edge->src_output()));
  }

  using FuncPairType = std::pair<const std::string*, const AttrValue*>;
  std::vector<FuncPairType> funcs;
  result.attributes.reserve(node.attrs().size() + 2);
  for (const auto& name_and_value : node.attrs()) {
    const auto& attr_name = name_and_value.first;
    const AttrValue& attr_value = name_and_value.second;
    if (attr_value.value_case() == AttrValue::kFunc) {
      // Attribute iteration order is not defined for protocol buffer Map.
      // Process function attributes separately in the lexicographical order to
      // have deterministic order of functions in the constructed IR.
      funcs.emplace_back(&attr_name, &attr_value);
    } else {
      TF_ASSIGN_OR_RETURN(auto attr, ConvertAttributeValue(attr_value));
      result.attributes.push_back(builder_.getNamedAttr(attr_name, attr));
    }
  }

  auto comparator = [](const FuncPairType& a, const FuncPairType& b) {
    return *a.first < *b.first;
  };
  std::sort(funcs.begin(), funcs.end(), comparator);
  for (const auto& func : funcs) {
    TF_RETURN_IF_ERROR(ConvertFunctionCallAttribute(*func.first, *func.second,
                                                    &result.attributes));
  }

  result.attributes.push_back(builder_.getNamedAttr(
      "name", builder_.getStringAttr(std::string(node.name()))));
  result.attributes.push_back(builder_.getNamedAttr(
      "device", builder_.getStringAttr(std::string(node_def.device()))));

  // Map If and StatelessIf op in TensorFlow to the common If op in MLIR and add
  // the differentiating attribute.
  if (node.IsIfNode()) {
    result.name = mlir::OperationName(get_full_op_name("If"), context_);
    mlir::BoolAttr val = builder_.getBoolAttr(node_type_name == "StatelessIf");
    result.attributes.push_back(builder_.getNamedAttr("is_stateless", val));
  }

  // Map While and StatelessWhile op in TensorFlow to the common While op in
  // MLIR and add the differentiating attribute.
  if (node.IsWhileNode()) {
    result.name = mlir::OperationName(get_full_op_name("While"), context_);
    mlir::BoolAttr val =
        builder_.getBoolAttr(node_type_name == "StatelessWhile");
    result.attributes.push_back(builder_.getNamedAttr("is_stateless", val));
  }

  // Register the mapping between the TF node and the newly created operation.
  node_values_[node.id()] =
      createOperation(node, op_name, result, control_operands);

  return Status::OK();
}

// Add the backedges to the CFG. Given a backedge, we replace the original
// source and destination operations by two new operations. Most of the
// fields of the replacements are copied from the original operations.
// However,
// - for the src operation, one output is inserted to the front of the output
//   list. The type of the output is set to the type of the non-control result
//   of the dst operation, and
// - for the dst operation, one operand is inserted to the front of the
//   operand list. This operand is using the first result of the src
//   operation.
// TODO(fengliuai): Preserve the order of the results and operands if
// necessary.
Status ImporterBase::AddBackedges() {
  for (auto it : back_edge_dst_inputs_) {
    BackEdge& edge = it.second;
    if (!edge.src->IsNextIteration() || !edge.dst->IsMerge()) {
      return errors::FailedPrecondition(
          "Invalid backedge; should be from NextIteration to Merge!");
    }
    auto* sink = node_values_[edge.src->id()];
    auto* dst = node_values_[edge.dst->id()];
    TF_RETURN_IF_ERROR(AddBackedge(sink, dst, edge.dst_input));
  }
  return Status::OK();
}

Status ImporterBase::AddBackedge(mlir::Operation* sink, mlir::Operation* dst,
                                 int dst_input) {
  // Get the NextIteration.Source operation from the token operand of the sink.
  mlir::Operation* source = sink->getOperand(0)->getDefiningOp();

  // Adds the "source" to the operands of the dst by creating a new dst
  // operation.
  mlir::OperationState state(dst->getLoc(), dst->getName());
  auto num_operands = dst->getNumOperands();
  state.operands.reserve(num_operands + 1);
  for (int input = 0, e = num_operands + 1; input != e; ++input) {
    if (input < dst_input) {
      state.operands.push_back(dst->getOperand(input));
    } else if (input == dst_input) {
      state.operands.push_back(source->getResult(0));
    } else {
      state.operands.push_back(dst->getOperand(input - 1));
    }
  }
  state.attributes.assign(dst->getAttrs().begin(), dst->getAttrs().end());
  state.types.assign(dst->getResultTypes().begin(),
                     dst->getResultTypes().end());
  builder_.setInsertionPoint(dst);
  auto* new_dst = builder_.createOperation(state);

  // Replaces the output uses of the old operation by the corresponding
  // result of the new operation, and deletes the old operation.
  for (unsigned i = 0, e = dst->getNumResults(); i != e; ++i) {
    auto* new_output = new_dst->getResult(i);
    dst->getResult(i)->replaceAllUsesWith(new_output);
  }
  dst->dropAllReferences();
  dst->erase();
  return Status::OK();
}

StatusOr<mlir::FunctionType> ImporterBase::InferLibFunctionType(
    const FunctionBody& fbody) {
  mlir::Builder builder(context_);

  // The FunctionBody contains a graph with a single-output _Arg node for each
  // function argument and a single-input _Retval node for each function return
  // value.
  //
  // We already populated the ShapeRefiner with all the information about the
  // shapes of these graph edges, so we just query it to build the corresponding
  // MLIR function type signature.

  llvm::SmallVector<mlir::Type, 4> arg_types;
  arg_types.reserve(fbody.arg_types.size());
  for (auto arg : fbody.arg_nodes) {
    // Find node in the graph using the node id instead of using `arg` directly
    // because the graph has been cloned.
    auto* node = graph_->FindNodeId(arg->id());
    TF_ASSIGN_OR_RETURN(auto type, InferOutputType(*node, /*idx=*/0, builder));
    arg_types.push_back(type);
  }

  llvm::SmallVector<mlir::Type, 4> ret_types;
  ret_types.reserve(fbody.ret_types.size());
  for (auto ret : fbody.ret_nodes) {
    // Find node in the graph using the node id instead of using `ret` directly
    // because the graph has been cloned.
    auto* node = graph_->FindNodeId(ret->id());
    TF_ASSIGN_OR_RETURN(auto type, InferInputType(*node, /*idx=*/0, builder));
    ret_types.push_back(type);
  }

  return builder.getFunctionType(arg_types, ret_types);
}

// Stateful helper class to import a TensorFlow model expressed in GraphDef into
// an MLIR Module.
//
// The nodes defined in the graph is converted to a function called "main". All
// the library function definitions are converted to MLIR functions in the
// module.
class GraphDefImporter : public ImporterBase {
 public:
  // Main entry point: converts the given graph to an MLIR Module.
  static StatusOr<mlir::OwningModuleRef> Convert(
      mlir::MLIRContext* context, const Graph& graph,
      const GraphDebugInfo& debug_info,
      const FunctionLibraryDefinition& flib_def,
      const GraphImportConfig& specs);

 private:
  explicit GraphDefImporter(
      const FunctionLibraryDefinition& flib, const GraphDebugInfo& debug_info,
      const GraphImportConfig& specs, mlir::ModuleOp module,
      std::unordered_map<std::string, std::string>* tf_name_to_mlir_name)
      : ImporterBase(flib, debug_info, specs, module, tf_name_to_mlir_name) {}

  // Returns the function signature of the main function of converted MLIR
  // module, the input nodes and output nodes. The type and shape information
  // for the function arguments are read from `specs`, but the type and shape
  // information for the function returns are inferred by the shape refiner in
  // ImporterBase.
  StatusOr<mlir::FunctionType> InferMainFunctionType(
      const GraphImportConfig& specs, mlir::MLIRContext* context,
      absl::InlinedVector<OutputTensor, 4>* arg_nodes,
      absl::InlinedVector<OutputTensor, 4>* ret_nodes);
};

StatusOr<mlir::OwningModuleRef> GraphDefImporter::Convert(
    mlir::MLIRContext* context, const Graph& graph,
    const GraphDebugInfo& debug_info, const FunctionLibraryDefinition& flib_def,
    const GraphImportConfig& specs) {
  mlir::OwningModuleRef module =
      mlir::ModuleOp::create(mlir::UnknownLoc::get(context));
  std::unordered_map<std::string, std::string> tf_name_to_mlir_name;

  GraphDefImporter importer(flib_def, debug_info, specs, module.get(),
                            &tf_name_to_mlir_name);

  mlir::FunctionType func_type;
  absl::InlinedVector<OutputTensor, 4> arg_nodes;
  absl::InlinedVector<OutputTensor, 4> ret_nodes;
  absl::InlinedVector<Node*, 4> control_ret_nodes;
  llvm::SmallVector<mlir::NamedAttribute, 1> attrs;
  std::unique_ptr<FunctionBody> graph_fbody;
  if (specs.graph_as_function) {
    if (specs.prune_unused_nodes || !specs.inputs.empty() ||
        !specs.output_arrays.empty() || !specs.output_arrays_order.empty())
      return errors::InvalidArgument(
          "Pruning of graph is currently unsupported when the main graph is "
          "converted to a function.");
    // Converts graph into a FunctionDef.
    FunctionDef graph_fdef;
    TF_RETURN_IF_ERROR(GraphToFunctionDef(graph, "main", &graph_fdef));

    // Converts FunctionDef into a FunctionBody.
    TF_RETURN_IF_ERROR(FunctionDefToBodyHelper(graph_fdef, AttrSlice(),
                                               &flib_def, &graph_fbody));

    TF_RETURN_IF_ERROR(importer.PrepareConvert(*graph_fbody->graph));
    TF_ASSIGN_OR_RETURN(func_type, importer.InferLibFunctionType(*graph_fbody));
    importer.GetArgsAndRetsFromFunctionBody(*graph_fbody, &arg_nodes,
                                            &ret_nodes, &control_ret_nodes);

    if (!arg_nodes.empty() || !ret_nodes.empty()) {
      mlir::Builder b(context);
      std::string s;
      llvm::raw_string_ostream ss(s);
      auto node_name = [&](const Node* node) { ss << node->name(); };
      mlir::interleave(graph_fbody->arg_nodes, ss, node_name, ",");
      auto inputs = b.getNamedAttr("inputs", b.getStringAttr(ss.str()));
      s.clear();
      mlir::interleave(graph_fbody->ret_nodes, ss, node_name, ",");
      auto outputs = b.getNamedAttr("outputs", b.getStringAttr(ss.str()));

      attrs.push_back(b.getNamedAttr("tf.entry_function",
                                     b.getDictionaryAttr({inputs, outputs})));
    }
  } else {
    TF_RETURN_IF_ERROR(importer.PrepareConvert(graph));

    // Collects the argument and return nodes by looking up the node names
    // specified by the user.
    TF_ASSIGN_OR_RETURN(func_type, importer.InferMainFunctionType(
                                       specs, context, &arg_nodes, &ret_nodes));

    // TODO(prakalps): Refactor to keep attribute strings (tf.entry_function,
    // tf.versions) shared by importer and exporter in a centralized place.
    // Record the input and output mapping.
    if (!specs.inputs.empty() || !specs.output_arrays.empty()) {
      mlir::Builder b(context);
      std::string s;
      llvm::raw_string_ostream ss(s);
      mlir::interleave(
          specs.inputs, ss,
          [&](const std::pair<std::string, ArrayInfo>& v) { ss << v.first; },
          ",");
      auto inputs = b.getNamedAttr("inputs", b.getStringAttr(ss.str()));
      s.clear();
      mlir::interleave(specs.output_arrays, ss, ",");
      auto outputs = b.getNamedAttr("outputs", b.getStringAttr(ss.str()));

      attrs.push_back(b.getNamedAttr("tf.entry_function",
                                     b.getDictionaryAttr({inputs, outputs})));
    }
  }

  // Record version info.
  const auto& graph_versions = graph.versions();
  mlir::Builder b(context);
  auto producer = b.getNamedAttr(
      "producer", b.getI32IntegerAttr(graph_versions.producer()));
  auto min_consumer = b.getNamedAttr(
      "min_consumer", b.getI32IntegerAttr(graph_versions.min_consumer()));
  auto bad_consumers = b.getNamedAttr(
      "bad_consumers", b.getI32ArrayAttr(llvm::ArrayRef<int32_t>(
                           graph_versions.bad_consumers().begin(),
                           graph_versions.bad_consumers().end())));
  module->setAttr("tf.versions",
                  b.getDictionaryAttr(llvm::ArrayRef<mlir::NamedAttribute>(
                      {producer, min_consumer, bad_consumers})));

  TF_RETURN_IF_ERROR(importer.ImporterBase::Convert(
      "main", func_type, arg_nodes, ret_nodes, control_ret_nodes, attrs));
  return module;
}

StatusOr<mlir::FunctionType> GraphDefImporter::InferMainFunctionType(
    const GraphImportConfig& specs, mlir::MLIRContext* context,
    absl::InlinedVector<OutputTensor, 4>* arg_nodes,
    absl::InlinedVector<OutputTensor, 4>* ret_nodes) {
  // Finds out all the input nodes and output nodes.
  if (!specs.inputs.empty() || !specs.output_arrays.empty()) {
    arg_nodes->resize(specs.inputs.size());
    ret_nodes->resize(specs.output_arrays_order.size());

    for (Node* n : GetOrderedNodes()) {
      // Handle inputs/arguments.
      auto input_it = specs.inputs.find(n->name());
      if (input_it != specs.inputs.end()) {
        (*arg_nodes)[std::distance(specs.inputs.begin(), input_it)] = {n, 0};
      }

      // Handle outputs/returns.
      if (specs.output_arrays.find(n->name()) != specs.output_arrays.end()) {
        for (int i = 0, e = specs.output_arrays_order.size(); i != e; ++i) {
          std::pair<std::string, std::string> name_and_port =
              absl::StrSplit(specs.output_arrays_order[i], ':');
          auto name = name_and_port.first;
          if (name != n->name()) continue;
          int port = 0;
          if (!name_and_port.second.empty() &&
              !absl::SimpleAtoi(name_and_port.second, &port)) {
            return errors::InvalidArgument("Invalid port specification: ",
                                           specs.output_arrays_order[i]);
          }
          (*ret_nodes)[i] = {n, port};
        }
      }
    }
  }

  int i = 0;
  for (auto it : specs.inputs) {
    if (arg_nodes->at(i++).node == nullptr) {
      return errors::InvalidArgument("Input ", it.first,
                                     " was not found in graph");
    }
  }
  for (int i = 0, e = specs.output_arrays_order.size(); i != e; ++i) {
    if (ret_nodes->at(i).node == nullptr) {
      return errors::InvalidArgument("Output ", specs.output_arrays_order[i],
                                     " was not found in graph");
    }
  }

  // Starts to construct the function type.
  llvm::SmallVector<mlir::Type, 4> arg_types;
  llvm::SmallVector<mlir::Type, 4> ret_types;
  arg_types.reserve(specs.inputs.size());
  ret_types.reserve(specs.output_arrays.size());
  mlir::Builder builder(context);

  // Input nodes as function arguments.
  for (const auto& input : specs.inputs) {
    mlir::Type element_type;
    const auto& node_info = input.second;
    TF_RETURN_IF_ERROR(::tensorflow::ConvertDataType(node_info.imported_dtype,
                                                     builder, &element_type));
    llvm::SmallVector<int64_t, 4> shape;
    TF_RETURN_IF_ERROR(ConvertToMlirShape(node_info.shape, &shape));
    arg_types.push_back(builder.getTensorType(shape, element_type));
  }

  // Output nodes as function returns.
  for (const auto& ret : *ret_nodes) {
    if (ret.node->num_outputs() <= ret.index) {
      return errors::InvalidArgument("Invalid output index ", ret.index,
                                     " specified for node: ", ret.node->name());
    }
    TF_ASSIGN_OR_RETURN(auto type,
                        InferOutputType(*ret.node, ret.index, builder));
    ret_types.push_back(type);
  }

  return builder.getFunctionType(arg_types, ret_types);
}

// Stateful helper class to import a TensorFlow model expressed in SavedModel
// into an MLIR Module.
class SavedModelImporter : public ImporterBase {
 public:
  // Main entry point: converts all functions in the given meta graph to an MLIR
  // Module.
  static StatusOr<mlir::OwningModuleRef> Convert(
      const SavedModelBundle& saved_model, const GraphDebugInfo& debug_info,
      mlir::MLIRContext* context, absl::Span<std::string> exported_names,
      bool add_default_attributes);

 private:
  explicit SavedModelImporter(
      const FunctionLibraryDefinition& flib, const GraphDebugInfo& debug_info,
      const GraphImportConfig& specs, mlir::ModuleOp module,
      std::unordered_map<std::string, std::string>* tf_name_to_mlir_name)
      : ImporterBase(flib, debug_info, specs, module, tf_name_to_mlir_name) {}
};

// Determines the names used to reference objects in the SavedObjectGraph.
class ObjectNames {
 public:
  explicit ObjectNames(const SavedObjectGraph& object_graph,
                       absl::Span<std::string> exported_names);

  // Gets the names that external users of the SavedModel can use to refer to
  // this node.
  llvm::ArrayRef<llvm::StringRef> GetExportedNames(int node_id) const;

  // Gets the name in the module symbol table for this node.
  // This name is only used for internal IR references.
  llvm::StringRef GetSymbolTableName(int node_id) const;

 private:
  // In the absence of any other information, use this name as the symbol table
  // name for this node.
  std::string GetDefaultSymbolTableName(int node_id) const;
  // Determines if a name is exported.
  bool IsExported(const std::string& name);
  // Main object graph traversal function.
  void RecursivelyVisitObjectGraph(int node_id);
  // Gets a stable StringRef from a std::string.
  llvm::StringRef SaveString(const std::string& s) const;

  // The object graph we are traversing.
  const SavedObjectGraph& object_graph_;
  // The set of names to export. Empty means "export all".
  std::unordered_set<std::string> names_to_export_;

  // When we recursively follow the object graph tree structure from the root,
  // we track its path in the object graph by pushing and popping from here
  // during traversal.
  llvm::SmallVector<std::string, 8> path_segments_;
  // The set of node_id's that are on the current DFS stack.
  // For cyclic object graphs, this prevents infinite recursion.
  std::unordered_set<int> on_stack_nodes_;

  // Key: node_id.
  // Value: all object names that node_id appears as.
  // Each object name corresponds to a unique path from the root of the object
  // graph.
  // The common intuitive case is when there is only one name for a given
  // object, which corresponds to the object graph being a tree.
  //
  // But, there cases where the object graph is a general graph. For
  // example, this happens commonly in Keras models, where `foo.bar` is
  // also reachable via the name `keras_api.foo.bar`.
  // Cycles are possible too.
  absl::flat_hash_map<int, std::vector<std::string>> object_names_;

  // Key: node_id
  // Value: all names that this object is exported as
  absl::flat_hash_map<int, llvm::SmallVector<llvm::StringRef, 1>>
      exported_names_;
  // Key: node_id
  // Value: pretty symbol table name to use for internal references to this
  // object.
  absl::flat_hash_map<int, llvm::StringRef> pretty_symbol_table_name_;

  // Stable strings we can take StringRef's into. Used only by the SaveString
  // method.
  mutable std::unordered_set<std::string> saved_strings_;
};

ObjectNames::ObjectNames(const SavedObjectGraph& object_graph,
                         absl::Span<std::string> exported_names)
    : object_graph_(object_graph),
      names_to_export_(exported_names.begin(), exported_names.end()) {
  // Visit all reachable nodes from the root of the object graph.
  // This builds up object_names_ to contain all names like `foo.bar` that a
  // particular node in the graph can be reached from.
  RecursivelyVisitObjectGraph(/*node_id=*/0);

  // Populate the exported_names_ map.
  // TODO(silvasean): Diagnose typos in exported names?
  for (auto& kv : object_names_) {
    // Make object names map independent of our particular choice of object
    // graph traversal.
    std::sort(kv.second.begin(), kv.second.end(),
              [](absl::string_view a, absl::string_view b) {
                // The sort order here influences the "pretty name" we assign
                // below. We want the most debuggable name to be first.
                //
                // Debuggability heuristics:
                // 1. Names that end in digits are likely to be internal aliases
                // to the "real" names.
                // 2. Longer names are more likely to be internal aliases.
                //
                // Example set of object names created by Keras for the weight
                // matrix of a fully connected layer on a trivial FC mnist
                // model:
                // - `model.layer-1.kernel` (this is the "best" name)
                // - `model.keras_api.layers.1.kernel`
                // - `model.variables.0`
                // - `model.keras_api.layers.1.keras_api.trainable_variables.0`
                // - ... 10 more long aliases ending in digits ...
                return std::make_tuple(isdigit(a.back()), a.size(), a) <
                       std::make_tuple(isdigit(b.back()), b.size(), b);
              });
    for (const std::string& name : kv.second) {
      if (IsExported(name)) {
        exported_names_[kv.first].push_back(SaveString(name));
      }
    }
  }
  // Create "pretty" symbol table names for nodes where that is applicable.
  // We could make all symbol table names use the default, which is basically
  // just the node id. But for debugging purposes, it's nicer if we can mix in
  // a recognizable object name if we have the information to do so.
  for (auto& kv : object_names_) {
    int node_id = kv.first;
    std::string internal_name =
        absl::StrCat(GetDefaultSymbolTableName(node_id), "__");
    // If the object has an exported name, we prefer that since it is probably
    // the most recognizable. Otherwise, we grab some non-exported name of the
    // object.
    if (exported_names_.find(node_id) != exported_names_.end()) {
      internal_name += exported_names_[node_id][0].str();
    } else {
      internal_name += object_names_[node_id][0];
    }
    pretty_symbol_table_name_[node_id] = SaveString(internal_name);
  }
}

llvm::ArrayRef<llvm::StringRef> ObjectNames::GetExportedNames(
    int node_id) const {
  auto it = exported_names_.find(node_id);
  if (it != exported_names_.end()) {
    return it->second;
  }
  return {};
}

llvm::StringRef ObjectNames::GetSymbolTableName(int node_id) const {
  auto it = pretty_symbol_table_name_.find(node_id);
  if (it != pretty_symbol_table_name_.end()) {
    return it->second;
  }
  return SaveString(GetDefaultSymbolTableName(node_id));
}

std::string ObjectNames::GetDefaultSymbolTableName(int node_id) const {
  return absl::StrCat("__sm_node", node_id);
}

bool ObjectNames::IsExported(const std::string& name) {
  if (names_to_export_.empty()) {
    return true;
  }
  return names_to_export_.find(name) != names_to_export_.end();
}

void ObjectNames::RecursivelyVisitObjectGraph(int node_id) {
  const SavedObject& object = object_graph_.nodes(node_id);

  switch (object.kind_case()) {
    case SavedObject::kConstant:
    case SavedObject::kFunction:
    case SavedObject::kVariable: {
      object_names_[node_id].push_back(absl::StrJoin(path_segments_, "."));
      break;
    }
    default:
      break;
  }

  for (const auto& child_ref : object.children()) {
    bool on_stack = !on_stack_nodes_.insert(child_ref.node_id()).second;
    if (on_stack) {
      // This is a backedge. Don't traverse it.
      continue;
    }

    path_segments_.push_back(child_ref.local_name());
    RecursivelyVisitObjectGraph(child_ref.node_id());
    path_segments_.pop_back();

    on_stack_nodes_.erase(child_ref.node_id());
  }
}

llvm::StringRef ObjectNames::SaveString(const std::string& s) const {
  return llvm::StringRef(*saved_strings_.insert(s).first);
}

StatusOr<Tensor> GetTensorFromSession(Session* session, std::string name) {
  std::vector<Tensor> outputs;
  TF_RETURN_IF_ERROR(session->Run(/*inputs=*/{}, /*output_tensor_names=*/{name},
                                  /*target_node_names=*/{}, &outputs));
  return outputs[0];
}

// Variable ops return resource types, but we want to read their contents.
// We need to find a "ReadVariableOp" that reads a given variable to get out a
// tensor value. These seem to always be present in the GraphDef's main graph.
// TODO(silvasean): Find a better way to do this.
StatusOr<Tensor> ReadVariableFromSession(const SavedModelBundle& saved_model,
                                         std::string variable_name) {
  const GraphDef& graph_def = saved_model.meta_graph_def.graph_def();
  // TODO(silvasean): Don't do linear search.
  for (const NodeDef& node : graph_def.node()) {
    if (node.op() == "ReadVariableOp" && node.input_size() == 1 &&
        node.input(0) == variable_name) {
      return GetTensorFromSession(saved_model.session.get(), node.name());
    }
  }
  return errors::InvalidArgument("Could not find ReadVariableOp reading '",
                                 variable_name, "'");
}

Status DiagnoseMultipleConcreteFunctions(const SavedObjectGraph& object_graph,
                                         const ObjectNames& object_names) {
  for (int node_id = 0; node_id < object_graph.nodes_size(); node_id++) {
    const SavedObject& object = object_graph.nodes(node_id);
    if (object_names.GetExportedNames(node_id).empty()) {
      continue;
    }
    if (object.kind_case() == SavedObject::kFunction) {
      // We only allow a single input signature to each SavedFunction.
      // This assumption means we have a 1:1 correspondence between
      // tf.function <=> SavedFunction <=> SavedConcreteFunction <=> FunctionDef
      // This makes defining the ABI easier (or even well-defined at all).
      // TODO(silvasean): How to detect a function that doesn't have an
      // explicitly user-provided input signature, but happens to have been
      // traced exactly once?
      if (object.function().concrete_functions_size() != 1) {
        llvm::SmallVector<std::string, 4> names;
        for (llvm::StringRef s : object_names.GetExportedNames(node_id)) {
          names.push_back(s.str());
        }
        return errors::InvalidArgument(
            "Exported function '", absl::StrJoin(names, ","),
            "' with multiple concrete functions. Check if you have "
            "@tf.function(input_signature=[...]) on this function.");
      }
    }
  }
  return Status::OK();
}

Status CreateSavedModelIR(
    const ObjectNames& object_names, mlir::ModuleOp module,
    const SavedObjectGraph& object_graph,
    const std::unordered_map<std::string, std::string>& tf_name_to_mlir_name,
    const SavedModelBundle& saved_model) {
  mlir::OpBuilder builder(module.getBodyRegion());
  mlir::SymbolTable symbol_table(module);
  for (int node_id = 0; node_id < object_graph.nodes_size(); node_id++) {
    const SavedObject& object = object_graph.nodes(node_id);
    // For correctness, we cannot import functions that don't have exported
    // names, since they don't necessarily have a well-defined ABI (diagnosed
    // earlier).
    //
    // For variables/constants, pruning them is purely an optimization,
    // and more complicated since it requires use-def analysis of which
    // functions use which variables/constants, so we don't do anything
    // special for them here as part of our initial IR construction.
    if (object.kind_case() == SavedObject::kFunction) {
      if (object_names.GetExportedNames(node_id).empty()) {
        continue;
      }
      const SavedFunction& function = object.function();
      auto func = symbol_table.lookup<mlir::FuncOp>(
          tf_name_to_mlir_name.find(function.concrete_functions(0))->second);
      func.setAttr(
          "tf_saved_model.exported_names",
          builder.getStrArrayAttr(object_names.GetExportedNames(node_id)));
      const SavedConcreteFunction& concrete_function =
          object_graph.concrete_functions().at(function.concrete_functions(0));

      int bound_input_base =
          func.getNumArguments() - concrete_function.bound_inputs_size();

      for (auto& bound_input :
           llvm::enumerate(concrete_function.bound_inputs())) {
        int arg_index = bound_input_base + bound_input.index();
        auto symbol_ref = builder.getSymbolRefAttr(
            object_names.GetSymbolTableName(bound_input.value()));
        func.setArgAttr(arg_index, "tf_saved_model.bound_input", symbol_ref);
      }
    } else if (object.kind_case() == SavedObject::kVariable) {
      const SavedVariable& variable = object.variable();
      TF_ASSIGN_OR_RETURN(
          Tensor value, ReadVariableFromSession(saved_model, variable.name()));
      TF_ASSIGN_OR_RETURN(auto value_attr, ConvertTensor(value, &builder));

      auto op = builder.create<mlir::tf_saved_model::GlobalTensorOp>(
          builder.getUnknownLoc(),
          builder.getStringAttr(object_names.GetSymbolTableName(node_id)),
          value_attr,
          /*is_mutable=*/builder.getUnitAttr());
      op.setAttr(
          "tf_saved_model.exported_names",
          builder.getStrArrayAttr(object_names.GetExportedNames(node_id)));
    } else if (object.kind_case() == SavedObject::kConstant) {
      const SavedConstant& constant = object.constant();
      TF_ASSIGN_OR_RETURN(Tensor value,
                          GetTensorFromSession(saved_model.session.get(),
                                               constant.operation()));
      TF_ASSIGN_OR_RETURN(auto value_attr, ConvertTensor(value, &builder));
      auto op = builder.create<mlir::tf_saved_model::GlobalTensorOp>(
          builder.getUnknownLoc(),
          builder.getStringAttr(object_names.GetSymbolTableName(node_id)),
          value_attr,
          /*is_mutable=*/nullptr);
      op.setAttr(
          "tf_saved_model.exported_names",
          builder.getStrArrayAttr(object_names.GetExportedNames(node_id)));
    }
  }
  module.setAttr("tf_saved_model.semantics", builder.getUnitAttr());
  return Status::OK();
}

StatusOr<mlir::OwningModuleRef> SavedModelImporter::Convert(
    const SavedModelBundle& saved_model, const GraphDebugInfo& debug_info,
    mlir::MLIRContext* context, absl::Span<std::string> exported_names,
    bool add_default_attributes) {
  GraphImportConfig specs;
  mlir::OwningModuleRef module =
      mlir::ModuleOp::create(mlir::UnknownLoc::get(context));
  std::unordered_map<std::string, std::string> tf_name_to_mlir_name;

  const auto& graphdef = saved_model.meta_graph_def.graph_def();
  GraphConstructorOptions options;
  options.allow_internal_ops = true;
  options.add_default_attributes = add_default_attributes;
  Graph graph(OpRegistry::Global());

  GraphDef preprocessed_graphdef(graphdef);
  if (add_default_attributes) {
    TF_RETURN_IF_ERROR(PreprocessGraphDef(nullptr, &preprocessed_graphdef));
  }

  TF_RETURN_IF_ERROR(
      ConvertGraphDefToGraph(options, preprocessed_graphdef, &graph));

  SavedModelImporter importer(graph.flib_def(), debug_info, specs, module.get(),
                              &tf_name_to_mlir_name);

  auto fn_names = graph.flib_def().ListFunctionNames();
  for (const auto& fn_name : fn_names) {
    TF_RETURN_IF_ERROR(importer.ConvertLibFunction(fn_name));
  }

  if (!saved_model.meta_graph_def.has_object_graph_def()) {
    return errors::InvalidArgument(
        "SavedModel does not have an object graph. Please use TF2.");
  }
  auto& object_graph = saved_model.meta_graph_def.object_graph_def();
  ObjectNames object_names(object_graph, exported_names);

  // Clean up a couple func's that always seem to be present when importing a
  // SavedModel. This is not strictly needed, as there is a separate pass that
  // will clean them up, but this makes staring at the raw IR of minimal
  // examples quite a bit nicer.
  for (auto func : llvm::make_early_inc_range(module->getOps<mlir::FuncOp>())) {
    if (func.getName().startswith("__inference__traced_save_") ||
        func.getName().startswith("__inference__traced_restore_") ||
        func.getName().startswith("__inference_signature_wrapper_")) {
      func.erase();
    }
  }

  // Diagnose SavedFunction's with multiple input signatures.
  TF_RETURN_IF_ERROR(
      DiagnoseMultipleConcreteFunctions(object_graph, object_names));

  // Construct the SavedModel IR.
  TF_RETURN_IF_ERROR(CreateSavedModelIR(object_names, module.get(),
                                        object_graph, tf_name_to_mlir_name,
                                        saved_model));
  assert(mlir::succeeded(mlir::verify(module.get())));

  return module;
}

}  // namespace

Status UpgradeLegacyGraph(Graph* graph, FunctionLibraryDefinition* flib_def) {
  return FunctionalizeControlFlow(graph, flib_def);
}

StatusOr<mlir::OwningModuleRef> ConvertGraphdefToMlir(
    const GraphDef& graphdef, const GraphDebugInfo& debug_info,
    const GraphImportConfig& specs, mlir::MLIRContext* context,
    bool add_default_attributes) {
  GraphConstructorOptions options;
  options.allow_internal_ops = true;
  options.add_default_attributes = add_default_attributes;
  Graph graph(OpRegistry::Global());

  GraphDef preprocessed_graphdef(graphdef);
  if (add_default_attributes) {
    TF_RETURN_IF_ERROR(PreprocessGraphDef(&specs, &preprocessed_graphdef));
  }
  TF_RETURN_IF_ERROR(ConvertGraphDefToGraph(
      options, std::move(preprocessed_graphdef), &graph));
  return ConvertGraphToMlir(graph, debug_info, graph.flib_def(), specs,
                            context);
}

StatusOr<mlir::OwningModuleRef> ConvertGraphToMlir(
    const Graph& graph, const GraphDebugInfo& debug_info,
    const FunctionLibraryDefinition& flib_def, const GraphImportConfig& specs,
    mlir::MLIRContext* context) {
  // TODO(jpienaar): Remove need to const_cast.
  if (specs.upgrade_legacy) {
    TF_RETURN_IF_ERROR(
        UpgradeLegacyGraph(const_cast<Graph*>(&graph),
                           const_cast<FunctionLibraryDefinition*>(&flib_def)));
  }
  return GraphDefImporter::Convert(context, graph, debug_info, flib_def, specs);
}

StatusOr<mlir::OwningModuleRef> ConvertSavedModelToMlir(
    const SavedModelBundle& saved_model, const GraphDebugInfo& debug_info,
    mlir::MLIRContext* context, absl::Span<std::string> exported_names,
    bool add_default_attributes) {
  return SavedModelImporter::Convert(saved_model, debug_info, context,
                                     exported_names, add_default_attributes);
}

std::string MlirModuleToString(mlir::ModuleOp module) {
  std::string txt_module;
  {
    llvm::raw_string_ostream os{txt_module};
    module.print(os);
  }
  return txt_module;
}

}  // namespace tensorflow
