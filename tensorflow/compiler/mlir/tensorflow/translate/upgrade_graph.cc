/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/mlir/tensorflow/translate/upgrade_graph.h"

#include "llvm/ADT/StringSet.h"
#include "tensorflow/core/common_runtime/device.h"
#include "tensorflow/core/common_runtime/device_factory.h"
#include "tensorflow/core/common_runtime/device_mgr.h"
#include "tensorflow/core/grappler/clusters/virtual_cluster.h"
#include "tensorflow/core/grappler/grappler_item.h"
#include "tensorflow/core/grappler/grappler_item_builder.h"
#include "tensorflow/core/grappler/optimizers/meta_optimizer.h"
#include "tensorflow/core/protobuf/meta_graph.pb.h"

namespace tensorflow {

// Returns the set of ops that we want to generate shared_names for them if
// empty.
const llvm::StringSet<>& GetSharedNameGenerationCompatibleOps() {
  static auto* const ops = new llvm::StringSet<>({"VariableV2", "Variable"});
  return *ops;
}

Status GenerateResourceSharedNameIfEmpty(Graph& graph,
                                         FunctionLibraryDefinition& flib_def) {
  auto is_resource_op_with_empty_shared_name = [](const NodeDef& node_def,
                                                  const OpDef& op_def) {
    if (!GetSharedNameGenerationCompatibleOps().contains(op_def.name())) {
      // If this op is not in the allowlist, then it is likely a custom op.
      // Currently for these ops, we are relying on its "use_node_name_sharing"
      // to decide whether it is valid to generate shared_names. If the OpDef
      // has "use_node_name_sharing" field, then it is valid to use node names
      // as shared names.
      if (!std::any_of(op_def.attr().begin(), op_def.attr().end(),
                       [](const auto& attr_def) {
                         return attr_def.name() == "use_node_name_sharing" &&
                                attr_def.type() == "bool";
                       }))
        return false;
    }

    if (!std::any_of(op_def.attr().begin(), op_def.attr().end(),
                     [](const auto& attr_def) {
                       return attr_def.name() == "shared_name" &&
                              attr_def.type() == "string";
                     }))
      return false;

    auto iter = node_def.attr().find("shared_name");
    if (iter == node_def.attr().end()) return true;
    return iter->second.s().empty();
  };

  // Upgrade nodes in the graph.
  for (auto* node : graph.nodes()) {
    if (is_resource_op_with_empty_shared_name(node->def(), node->op_def())) {
      node->AddAttr("shared_name", node->name());
    }
  }

  // Upgrade nodes in the functions.
  auto func_names = flib_def.ListFunctionNames();
  for (const auto& func_name : func_names) {
    const FunctionDef* orig = flib_def.Find(func_name);
    DCHECK(orig);
    auto copy = *orig;
    for (auto& node_def : *copy.mutable_node_def()) {
      const OpDef* op_def = nullptr;
      TF_RETURN_IF_ERROR(flib_def.LookUpOpDef(node_def.op(), &op_def));
      if (is_resource_op_with_empty_shared_name(node_def, *op_def)) {
        // Use the concat of function name and node name for such ops in a
        // function as the shared_name. "@" is used as the separator because it
        // is not allowed in the function name or the node name.
        (*node_def.mutable_attr())["shared_name"].set_s(
            absl::StrCat(node_def.name(), "@", func_name));
      }
    }
    TF_RETURN_IF_ERROR(flib_def.ReplaceFunction(func_name, copy));
  }

  return tensorflow::Status::OK();
}

Status RunGrappler(MetaGraphDef* meta_graph_def) {
  std::vector<std::unique_ptr<Device>> devices;
  // Only CPU device is used so instead of calling DeviceFactory::AddDevices()
  // with dummy session config, which will conflict with user defined options
  // and create unwanted devices, call cpu_factory->CreateDevices() to get CPU
  // only devices.
  DeviceFactory* cpu_factory = DeviceFactory::GetFactory("CPU");
  SessionOptions options;
  TF_RETURN_IF_ERROR(cpu_factory->CreateDevices(
      options, "/job:localhost/replica:0/task:0", &devices));
  Device* cpu_device = devices[0].get();
  auto device_mgr = absl::make_unique<StaticDeviceMgr>(std::move(devices));

  DeviceSet dev_set;
  for (auto d : device_mgr->ListDevices()) dev_set.AddDevice(d);

  ConfigProto config_proto;
  // Avoid grappler logic that lowers to v1 control flow.
  config_proto.mutable_experimental()->set_use_tfrt(true);
  config_proto.mutable_graph_options()
      ->mutable_optimizer_options()
      ->set_do_function_inlining(true);
  // Do not skip grappler optimization even for small graphs.
  config_proto.mutable_graph_options()
      ->mutable_rewrite_options()
      ->set_min_graph_nodes(-1);

  std::unique_ptr<grappler::GrapplerItem> item =
      grappler::GrapplerItemFromMetaGraphDef("graph", *meta_graph_def,
                                             grappler::ItemConfig());

  grappler::VirtualCluster cluster(&dev_set);
  return grappler::RunMetaOptimizer(std::move(*item), config_proto, cpu_device,
                                    &cluster,
                                    meta_graph_def->mutable_graph_def());
}

}  // namespace tensorflow
