// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "mkldnn_graph_dumper.h"
#include <ie_ngraph_utils.hpp>
#include "exec_graph_info.hpp"
#include "ie_common.h"
#include "mkldnn_debug.h"
#include <ngraph/variant.hpp>
#include "ngraph/ngraph.hpp"
#include "utils/debug_capabilities.h"

#include <vector>
#include <string>
#include <memory>
#include <map>

using namespace InferenceEngine;

namespace MKLDNNPlugin {

void serializeToCout(const MKLDNNGraph &graph);
void serializeToXML(const MKLDNNGraph &graph, const std::string& path);

namespace {

std::map<std::string, std::string> extract_node_metadata(const MKLDNNNodePtr &node) {
    std::map<std::string, std::string> serialization_info;

    if (node->getType() == Input && node->isConstant()) {
        // We need to separate Input and Const layers
        serialization_info[ExecGraphInfoSerialization::LAYER_TYPE] = "Const";
    } else if (node->getType() == Generic) {
        // Path to print actual name for extension layers
        serialization_info[ExecGraphInfoSerialization::LAYER_TYPE] = node->getTypeStr();
    } else {
        serialization_info[ExecGraphInfoSerialization::LAYER_TYPE] = NameFromType(node->getType());
    }

    // Original layers
    serialization_info[ExecGraphInfoSerialization::ORIGINAL_NAMES] = node->getOriginalLayers();

    // Implementation type name
    serialization_info[ExecGraphInfoSerialization::IMPL_TYPE] = node->getPrimitiveDescriptorType();

    std::string outputPrecisionsStr;
    if (!node->getChildEdges().empty()) {
        outputPrecisionsStr = node->getChildEdgeAt(0)->getDesc().getPrecision().name();

        bool isAllEqual = true;
        for (size_t i = 1; i < node->getChildEdges().size(); i++) {
            if (node->getChildEdgeAt(i-1)->getDesc().getPrecision() != node->getChildEdgeAt(i)->getDesc().getPrecision()) {
                isAllEqual = false;
                break;
            }
        }

        // If all output precisions are the same, we store the name only once
        if (!isAllEqual) {
            for (size_t i = 1; i < node->getChildEdges().size(); i++)
                outputPrecisionsStr += "," + std::string(node->getChildEdgeAt(i)->getDesc().getPrecision().name());
        }
    } else {
        // Branch to correctly handle output nodes
        if (!node->getParentEdges().empty()) {
            outputPrecisionsStr = node->getParentEdgeAt(0)->getDesc().getPrecision().name();
        }
    }
    serialization_info[ExecGraphInfoSerialization::OUTPUT_PRECISIONS] = outputPrecisionsStr;

    std::string outputLayoutsStr;
    auto outDescs = node->getSelectedPrimitiveDescriptor()->getConfig().outConfs;

    if (!outDescs.empty()) {
        auto fmt0 = MKLDNNMemoryDesc(outDescs[0].desc).getFormat();
        outputLayoutsStr = mkldnn::utils::fmt2str(fmt0);

        bool isAllEqual = true;
        for (size_t i = 1; i < outDescs.size(); i++) {
            if (MKLDNNMemoryDesc(outDescs[i - 1].desc).getFormat() != MKLDNNMemoryDesc(outDescs[i].desc).getFormat()) {
                isAllEqual = false;
                break;
            }
        }

        // If all output layouts are the same, we store the name only once
        if (!isAllEqual) {
            for (size_t i = 1; i < outDescs.size(); i++) {
                auto fmt = MKLDNNMemoryDesc(outDescs[i].desc).getFormat();
                outputLayoutsStr += "," + std::string(mkldnn::utils::fmt2str(fmt));
            }
        }
    } else {
        outputLayoutsStr = mkldnn::utils::fmt2str(mkldnn::memory::format_tag::undef);
    }
    serialization_info[ExecGraphInfoSerialization::OUTPUT_LAYOUTS] = outputLayoutsStr;

    // Performance
    if (node->PerfCounter().avg() != 0) {
        serialization_info[ExecGraphInfoSerialization::PERF_COUNTER] = std::to_string(node->PerfCounter().avg());
    } else {
        serialization_info[ExecGraphInfoSerialization::PERF_COUNTER] = "not_executed";  // it means it was not calculated yet
    }

    serialization_info[ExecGraphInfoSerialization::EXECUTION_ORDER] = std::to_string(node->getExecIndex());

    serialization_info[ExecGraphInfoSerialization::RUNTIME_PRECISION] = node->getRuntimePrecision().name();

    return serialization_info;
}

}  // namespace

InferenceEngine::CNNNetwork dump_graph_as_ie_ngraph_net(const MKLDNNGraph &graph) {
    std::map<MKLDNNNodePtr, std::shared_ptr<ngraph::Node> > node2layer;

    ngraph::ResultVector results;
    ngraph::ParameterVector params;
    ngraph::NodeVector to_hold;

    auto get_inputs = [&] (const MKLDNNNodePtr & node) {
        auto pr_edges = node->getParentEdges();
        ngraph::OutputVector inputs(pr_edges.size());

        for (int i = 0; i < pr_edges.size(); i++) {
            auto edge = node->getParentEdgeAt(i);
            int pr_port = edge->getInputNum();
            int ch_port = edge->getOutputNum();
            auto pr_node = edge->getParent();

            IE_ASSERT(node2layer.count(pr_node) == 1);
            auto pr = node2layer[pr_node];

            inputs[ch_port] = pr->output(pr_port);
        }

        return inputs;
    };

    auto create_ngraph_node = [&](const MKLDNNNodePtr &node) {
        bool is_input = false, is_output = false, should_be_hold = false;
        for (auto && kvp : graph.inputNodesMap) {
            if (kvp.second == node) {
                is_input = true;
                break;
            }
        }

        for (auto && kvp : graph.outputNodesMap) {
            if (kvp.second == node) {
                is_output = true;
                break;
            }
        }

        if (!is_output && node->getChildEdges().empty()) {
            // The node has no consumer and is not an output.
            // Should be hold in other irregular way.
            should_be_hold = true;
        }

        auto meta_data = extract_node_metadata(node);
        std::shared_ptr<ngraph::Node> return_node;
        if (is_input) {
            auto desc = node->getChildEdgeAt(0)->getDesc();
            auto param = std::make_shared<ngraph::op::Parameter>(
                details::convertPrecision(desc.getPrecision()),
                ngraph::PartialShape(desc.getDims()));
            return_node = param;
            params.push_back(param);
        } else if (is_output) {
            results.emplace_back(std::make_shared<ngraph::op::Result>(get_inputs(node).back()));
            return_node = results.back();
        } else {
            return_node = std::make_shared<ExecGraphInfoSerialization::ExecutionNode>(
                get_inputs(node), node->getSelectedPrimitiveDescriptor()->getConfig().outConfs.size());

            for (size_t port = 0; port < return_node->get_output_size(); ++port) {
                auto desc = node->getChildEdgeAt(port)->getDesc();
                return_node->set_output_type(port,
                    details::convertPrecision(desc.getPrecision()),
                    ngraph::PartialShape(desc.getDims()));
            }
        }

        if (should_be_hold) {
            to_hold.push_back(return_node);
        }

        for (auto && kvp : meta_data)
            return_node->get_rt_info()[kvp.first] = std::make_shared<::ngraph::VariantWrapper<std::string>>(kvp.second);
        return_node->set_friendly_name(node->getName());

        return return_node;
    };

    ngraph::NodeVector nodes;
    nodes.reserve(graph.graphNodes.size());
    for (auto &node : graph.graphNodes) {  // important: graph.graphNodes are in topological order
        nodes.emplace_back(create_ngraph_node(node));
        node2layer[node] = nodes.back();
    }

    auto holder = results[0];
    for (auto &node : to_hold) {
        holder->add_control_dependency(node);
    }

    auto function = std::make_shared<ngraph::Function>(results, params, graph._name);
    InferenceEngine::CNNNetwork net(function);
    return net;
}

#ifdef CPU_DEBUG_CAPS
void serialize(const MKLDNNGraph &graph) {
    const std::string& path = graph.getConfig().debugCaps.execGraphPath;

    if (path.empty())
        return;

    if (path == "cout")
        serializeToCout(graph);
    else if (!path.compare(path.size() - 4, 4, ".xml"))
        serializeToXML(graph, path);
    else
        IE_THROW() << "Unknown serialize format. Should be either 'cout' or '*.xml'. Got " << path;
}

void serializeToXML(const MKLDNNGraph &graph, const std::string& path) {
    if (path.empty())
        return;

    graph.dump().serialize(path);
}

void serializeToCout(const MKLDNNGraph &graph) {
    for (const auto& node : graph.GetNodes()) {
        std::cout << "name: " << node->getName() << " [ ";
        if (!node->getParentEdges().empty()) {
            const auto& parentEdge = *(node->getParentEdges()[0].lock());
            const auto& prnt_out_desc = parentEdge.getOutputDescRO();
            std::cout << "in: " << prnt_out_desc.getPrecision().name()
                      << "/l=" << prnt_out_desc.getLayout()
                      << "; ";
        }
        if (!node->getChildEdges().empty()) {
            const auto& childEdge = *(node->getChildEdges()[0].lock());
            const auto& chld_in_desc = childEdge.getInputDescRO();
            std::cout << "out: " << chld_in_desc.getPrecision().name()
                      << "/l=" << chld_in_desc.getLayout();
        }
        std::cout << " ]"  << std::endl;
    }
}
#endif
}  // namespace MKLDNNPlugin
