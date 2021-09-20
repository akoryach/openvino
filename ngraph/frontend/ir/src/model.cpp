// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "ir_frontend/model.hpp"

#include <xml_parse_utils.h>

#include <ie_ngraph_utils.hpp>
#include <ngraph_ops/framework_node.hpp>

using namespace ngraph;
using namespace InferenceEngine;

namespace {

struct GenericLayerParams {
    struct LayerPortData {
        size_t portId;
        std::vector<ngraph::Dimension> dims;
        ngraph::element::Type_t precision;
        std::unordered_set<std::string> names;
    };
    size_t layerId;
    std::string version;
    std::string name;
    std::string type;
    std::vector<LayerPortData> inputPorts;
    std::vector<LayerPortData> outputPorts;

    size_t getRealInputPortId(size_t id) const {
        size_t real_id = 0;
        for (auto& it : inputPorts) {
            if (it.portId == id) {
                return real_id;
            }
            ++real_id;
        }
        IE_THROW() << "Can not find input port with id " << id << " in layer " << name;
    }

    size_t getRealOutputPortId(size_t id) const {
        size_t real_id = 0;
        for (auto& it : outputPorts) {
            if (it.portId == id) {
                return real_id;
            }
            ++real_id;
        }
        IE_THROW() << "Can not find output port with id " << id << " in layer " << name;
    }
};

void operator>>(const std::stringstream& in, ngraph::element::Type& type) {
    type = details::convertPrecision(ngraph::trim(in.str()));
}

bool getStrAttribute(const pugi::xml_node& node, const std::string& name, std::string& value) {
    if (!node)
        return false;

    auto attr = node.attribute(name.c_str());
    if (attr.empty())
        return false;
    value = std::string(attr.value());
    return true;
}

template <class T>
bool getParameters(const pugi::xml_node& node, const std::string& name, std::vector<T>& value) {
    std::string param;
    if (!getStrAttribute(node, name, param))
        return false;
    std::stringstream ss(param);
    std::string field;
    while (getline(ss, field, ',')) {
        if (field.empty())
            IE_THROW() << "Cannot get vector of parameters! \"" << param << "\" is incorrect";
        std::stringstream fs(field);
        T val;
        fs >> val;
        value.emplace_back(val);
    }
    return true;
}

template <class T>
T stringToType(const std::string& valStr) {
    T ret{0};
    std::istringstream ss(valStr);
    if (!ss.eof()) {
        ss >> ret;
    }
    return ret;
}

class XmlDeserializer : public ngraph::AttributeVisitor {
public:
    explicit XmlDeserializer(const pugi::xml_node& node,
                             const Blob::CPtr& weights,
                             const std::unordered_map<std::string, ngraph::OpSet>& opsets,
                             std::unordered_map<std::string, std::shared_ptr<ngraph::Variable>>& variables)
        : m_node(node),
          m_weights(weights),
          m_opsets(opsets),
          m_variables(variables) {}

    void on_adapter(const std::string& name, ngraph::ValueAccessor<std::string>& value) override {
        std::string val;
        if (!getStrAttribute(m_node.child("data"), name, val))
            return;
        value.set(val);
    }
    void on_adapter(const std::string& name, ngraph::ValueAccessor<bool>& value) override {
        std::string val;
        if (!getStrAttribute(m_node.child("data"), name, val))
            return;
        std::transform(val.begin(), val.end(), val.begin(), [](char ch) {
            return std::tolower(static_cast<unsigned char>(ch));
        });
        std::set<std::string> true_names{"true", "1"};
        std::set<std::string> false_names{"false", "0"};

        bool is_true = true_names.find(val) != true_names.end();
        bool is_false = false_names.find(val) != false_names.end();

        if (!is_true && !is_false)
            return;
        value.set(is_true);
    }
    void on_adapter(const std::string& name, ngraph::ValueAccessor<void>& adapter) override;

    void on_adapter(const std::string& name, ngraph::ValueAccessor<double>& adapter) override {
        std::string val;
        if (!getStrAttribute(m_node.child("data"), name, val))
            return;
        adapter.set(stringToType<double>(val));
    }
    void on_adapter(const std::string& name, ngraph::ValueAccessor<int64_t>& adapter) override {
        std::string val;
        if (!getStrAttribute(m_node.child("data"), name, val))
            return;
        adapter.set(stringToType<int64_t>(val));
    }

    void on_adapter(const std::string& name,
                    ngraph::ValueAccessor<std::shared_ptr<ngraph::Function>>& adapter) override;

    void on_adapter(const std::string& name, ngraph::ValueAccessor<std::vector<int32_t>>& adapter) override {
        std::vector<int32_t> value;
        if (!getParameters<int32_t>(m_node.child("data"), name, value))
            return;
        adapter.set(value);
    }

    void on_adapter(const std::string& name, ngraph::ValueAccessor<std::vector<int64_t>>& adapter) override {
        std::vector<int64_t> value;
        if (!getParameters<int64_t>(m_node.child("data"), name, value))
            return;
        adapter.set(value);
    }

    void on_adapter(const std::string& name, ngraph::ValueAccessor<std::vector<float>>& adapter) override {
        std::vector<float> value;
        if (!getParameters<float>(m_node.child("data"), name, value))
            return;
        adapter.set(value);
    }

    void on_adapter(const std::string& name, ngraph::ValueAccessor<std::vector<std::string>>& adapter) override {
        std::vector<std::string> value;
        if (!getParameters<std::string>(m_node.child("data"), name, value))
            return;
        adapter.set(value);
    }

    void use_framework_node(bool flag) {
        m_use_framework_node = flag;
    }

private:
    struct IoMap {
        using NodeIdToIoIndex = std::unordered_map<size_t /*xml node id*/, uint64_t /*body io index*/>;
        NodeIdToIoIndex inputs;
        NodeIdToIoIndex outputs;
    };

    /// \brief Traverses port_map in order to create vector of InputDescription shared_ptrs.
    /// Shall be used only for ops which have port_map attribute.
    /// \param node xml op representation
    std::vector<std::shared_ptr<ngraph::op::util::SubGraphOp::InputDescription>> parseInputDescription(
        const pugi::xml_node& node);
    /// \brief Traverses port_map in order to create vector of OutputDescription shared_ptrs.
    /// Shall be used only for ops which have port_map attribute.
    /// \param node xml op representation
    std::vector<std::shared_ptr<ngraph::op::util::SubGraphOp::OutputDescription>> parseOutputDescription(
        const pugi::xml_node& node);

    // TODO consider to call only once per layer/TI-Loop node
    IoMap updated_io_map(const pugi::xml_node& node);

    /// \brief Traverses xml node representation in order to create nGraph function for it.
    /// \param node xml node representation
    /// \param weights weights attached to current node
    /// \return shared pointer to function representing input node
    std::shared_ptr<ngraph::Function> parse_function(const pugi::xml_node& root, const Blob::CPtr& weights);
    /// \brief Traverses xml node representation in order to get the purpose attribute of
    /// inputs/outputs in the body of Loop op. \param node xml node representation \return struct
    /// with value of purpuse attribute
    ngraph::op::v5::Loop::SpecialBodyPorts parsePurposeAttribute(const pugi::xml_node& node);

    GenericLayerParams parseGenericParams(const pugi::xml_node& node);

    std::shared_ptr<ngraph::Node> createNode(const ngraph::OutputVector& inputs,
                                             const pugi::xml_node& node,
                                             const Blob::CPtr& weights,
                                             const GenericLayerParams& params);

    // -- DATA --
    const pugi::xml_node m_node;
    const Blob::CPtr& m_weights;
    const std::unordered_map<std::string, ngraph::OpSet>& m_opsets;
    std::unordered_map<std::string, std::shared_ptr<ngraph::Variable>>& m_variables;

    ///
    /// store information about parameters/results order during function creation
    /// it will be used during Inputs/Outputs Description creation in SubGraph processing
    ///
    IoMap io_map;

    bool m_use_framework_node{false};
};

XmlDeserializer::IoMap XmlDeserializer::updated_io_map(const pugi::xml_node& node) {
    auto body_node = node.child("body");

    if (body_node.empty()) {
        IE_THROW() << "Missing body part.";
    }
    // Fill map: parameter/result id to parameter/result number in Function

    auto extend_io_map = io_map;

    FOREACH_CHILD (layer, body_node.child("layers"), "layer") {
        auto type = XMLParseUtils::GetStrAttr(layer, "type");

        if (type == "Parameter") {
            auto id = XMLParseUtils::GetUIntAttr(layer, "id");
            extend_io_map.inputs.insert({id, -1});  // try add as unconnected
        } else if (type == "Result") {
            auto id = XMLParseUtils::GetUIntAttr(layer, "id");
            extend_io_map.outputs.insert({id, -1});  // try add as unconnected
        }
    }
    return extend_io_map;
}

std::vector<std::shared_ptr<ngraph::op::util::SubGraphOp::InputDescription>> XmlDeserializer::parseInputDescription(
    const pugi::xml_node& node) {
    std::vector<std::shared_ptr<ngraph::op::util::SubGraphOp::InputDescription>> inputs;
    const auto up_io_map = updated_io_map(node);

    // Parse PortMap: external_port_id for inputs does not always appear in consecutive order
    std::map<uint64_t, pugi::xml_node> input_map;
    FOREACH_CHILD (input, node.child("port_map"), "input") {
        int64_t ext_port_id = XMLParseUtils::GetInt64Attr(input, "external_port_id");
        input_map.emplace(ext_port_id, input);
    }

    for (const auto& input : input_map) {
        auto& xml_input = input.second;
        auto axis_attr = xml_input.attribute("axis");
        int64_t ti_input_index = XMLParseUtils::GetInt64Attr(xml_input, "external_port_id");
        size_t body_parameter_index = XMLParseUtils::GetUIntAttr(xml_input, "internal_layer_id");

        // if axis is set, then slicing is enabled. Create ngraph::TensorIterator::SlicedInput.
        if (!axis_attr.empty()) {
            size_t axis = XMLParseUtils::GetUIntAttr(xml_input, "axis");
            int64_t start = XMLParseUtils::GetInt64Attr(xml_input, "start", 0);
            int64_t stride = XMLParseUtils::GetInt64Attr(xml_input, "stride", 1);
            int64_t end = XMLParseUtils::GetInt64Attr(xml_input, "end", -1);
            int64_t part_size = XMLParseUtils::GetInt64Attr(xml_input, "part_size", 1);

            const auto input_index = up_io_map.inputs.at(body_parameter_index);

            inputs.push_back(std::make_shared<ngraph::op::util::SubGraphOp::SliceInputDescription>(ti_input_index,
                                                                                                   input_index,
                                                                                                   start,
                                                                                                   stride,
                                                                                                   part_size,
                                                                                                   end,
                                                                                                   axis));
        } else {
            // otherwise find corresponding back edge and create ngraph::TensorIterator::MergedInput
            bool is_back_edge_exist = false;
            FOREACH_CHILD (xml_edge, node.child("back_edges"), "edge") {
                size_t to_layer = XMLParseUtils::GetUIntAttr(xml_edge, "to-layer");

                if (to_layer == body_parameter_index) {
                    size_t from_layer = XMLParseUtils::GetUIntAttr(xml_edge, "from-layer");

                    const auto input_index = up_io_map.inputs.at(body_parameter_index);
                    const auto output_index = up_io_map.outputs.at(from_layer);

                    inputs.push_back(
                        std::make_shared<ngraph::op::util::SubGraphOp::MergedInputDescription>(ti_input_index,
                                                                                               input_index,
                                                                                               output_index));

                    is_back_edge_exist = true;
                    break;
                }
            }

            // ti_input_index = -1 means that Parameter of the body is not connected to inputs of
            // TensorIterator and is used only for internal needs.
            if (!is_back_edge_exist && ti_input_index >= 0) {
                const auto input_index = up_io_map.inputs.at(body_parameter_index);

                inputs.push_back(
                    std::make_shared<ngraph::op::util::SubGraphOp::InvariantInputDescription>(ti_input_index,
                                                                                              input_index));
            }
        }
    }
    return inputs;
}

std::vector<std::shared_ptr<ngraph::op::util::SubGraphOp::OutputDescription>> XmlDeserializer::parseOutputDescription(
    const pugi::xml_node& node) {
    std::vector<std::shared_ptr<ngraph::op::util::SubGraphOp::OutputDescription>> outputs;
    const auto up_io_map = updated_io_map(node);

    // Parse PortMap: outputs
    std::map<int64_t, pugi::xml_node> output_map;
    FOREACH_CHILD (output, node.child("port_map"), "output") {
        int64_t ext_port_id = XMLParseUtils::GetInt64Attr(output, "external_port_id");
        output_map.emplace(ext_port_id, output);
    }

    uint64_t output_number = 0;
    for (const auto& output : output_map) {
        auto& xml_output = output.second;
        auto axis_attr = xml_output.attribute("axis");
        size_t body_result_index = XMLParseUtils::GetUIntAttr(xml_output, "internal_layer_id");

        // if external_port_id < 0 it means that this body result isn't connected to the Loop output
        // and is used only for internal needs. For TensorIterator external_port_id is always > 0.
        if (XMLParseUtils::GetInt64Attr(xml_output, "external_port_id") >= 0) {
            // if axis is set, then concatenation is enabled. Create
            // ngraph::TensorIterator::ConcatOutput.
            if (!axis_attr.empty()) {
                int64_t axis = XMLParseUtils::GetInt64Attr(xml_output, "axis");
                int64_t start = XMLParseUtils::GetInt64Attr(xml_output, "start", 0);
                int64_t stride = XMLParseUtils::GetInt64Attr(xml_output, "stride", 1);
                int64_t end = XMLParseUtils::GetInt64Attr(xml_output, "end", -1);
                int64_t part_size = XMLParseUtils::GetInt64Attr(xml_output, "part_size", 1);

                const auto output_index = up_io_map.outputs.at(body_result_index);

                outputs.push_back(std::make_shared<ngraph::op::util::SubGraphOp::ConcatOutputDescription>(output_index,
                                                                                                          output_number,
                                                                                                          start,
                                                                                                          stride,
                                                                                                          part_size,
                                                                                                          end,
                                                                                                          axis));
            } else {
                // otherwise create ngraph::TensorIterator::BodyOutput. -1 means last iteration.
                const auto output_index = up_io_map.outputs.at(body_result_index);

                outputs.push_back(std::make_shared<ngraph::op::util::SubGraphOp::BodyOutputDescription>(output_index,
                                                                                                        output_number,
                                                                                                        -1));
            }
            output_number++;
        }
    }
    return outputs;
}

ngraph::op::v5::Loop::SpecialBodyPorts XmlDeserializer::parsePurposeAttribute(const pugi::xml_node& node) {
    ngraph::op::v5::Loop::SpecialBodyPorts result = {-1, -1};
    const auto up_io_map = updated_io_map(node);

    NGRAPH_CHECK(!up_io_map.inputs.empty() || !up_io_map.outputs.empty(),
                 "No parameters or results found in body Function.");

    // Parse PortMap: external_port_id for inputs/outputs does not always appear in consecutive
    // order
    std::map<uint64_t, pugi::xml_node> input_map;
    FOREACH_CHILD (input, node.child("port_map"), "input") {
        int64_t ext_port_id = XMLParseUtils::GetInt64Attr(input, "external_port_id");
        input_map.emplace(ext_port_id, input);
    }
    std::map<int64_t, pugi::xml_node> output_map;
    FOREACH_CHILD (output, node.child("port_map"), "output") {
        int64_t ext_port_id = XMLParseUtils::GetInt64Attr(output, "external_port_id");
        output_map.emplace(ext_port_id, output);
    }

    for (const auto& input : input_map) {
        auto& xml_input = input.second;
        auto purpose = XMLParseUtils::GetStrAttr(xml_input, "purpose", "");
        size_t body_parameter_index = XMLParseUtils::GetUIntAttr(xml_input, "internal_layer_id");
        if (purpose == "current_iteration") {
            result.current_iteration_input_idx = up_io_map.inputs.at(body_parameter_index);
        }
    }

    for (const auto& output : output_map) {
        auto& xml_output = output.second;
        auto purpose = XMLParseUtils::GetStrAttr(xml_output, "purpose", "");
        size_t body_parameter_index = XMLParseUtils::GetUIntAttr(xml_output, "internal_layer_id");
        if (purpose == "execution_condition") {
            result.body_condition_output_idx = up_io_map.outputs.at(body_parameter_index);
        }
    }

    return result;
}

void XmlDeserializer::on_adapter(const std::string& name, ngraph::ValueAccessor<void>& adapter) {
    static const std::unordered_set<std::string> skip_names = {"input_descriptions",
                                                               "output_descriptions",
                                                               "special_body_ports"};
    std::string val;

    // for TensorIterator look for 'port_map' as 'data' does not exist
    if (m_node.child("port_map")) {
        if (auto a = ngraph::as_type<
                ngraph::AttributeAdapter<std::vector<std::shared_ptr<ngraph::op::util::SubGraphOp::InputDescription>>>>(
                &adapter)) {
            a->set(parseInputDescription(m_node));
        } else if (auto a = ngraph::as_type<ngraph::AttributeAdapter<
                       std::vector<std::shared_ptr<ngraph::op::util::SubGraphOp::OutputDescription>>>>(&adapter)) {
            a->set(parseOutputDescription(m_node));
        } else if (auto a =
                       ngraph::as_type<ngraph::AttributeAdapter<ngraph::op::v5::Loop::SpecialBodyPorts>>(&adapter)) {
            a->set(parsePurposeAttribute(m_node));
        }
    }

    if (skip_names.count(name) && !getStrAttribute(m_node.child("data"), name, val))
        return;
    if (auto a = ngraph::as_type<ngraph::AttributeAdapter<ngraph::element::Type>>(&adapter)) {
        static_cast<ngraph::element::Type&>(*a) = details::convertPrecision(val);
    } else if (auto a = ngraph::as_type<ngraph::AttributeAdapter<ngraph::PartialShape>>(&adapter)) {
        std::vector<int64_t> shape;
        std::vector<ngraph::Dimension> dims;
        if (!getParameters<int64_t>(m_node.child("data"), name, shape))
            return;
        for (const auto& dim : shape)
            dims.emplace_back(dim);
        static_cast<ngraph::PartialShape&>(*a) = ngraph::PartialShape(dims);
    } else if (auto a = ngraph::as_type<ngraph::AttributeAdapter<ngraph::Shape>>(&adapter)) {
        std::vector<size_t> shape;
        if (!getParameters<size_t>(m_node.child("data"), name, shape))
            return;
        static_cast<ngraph::Shape&>(*a) = ngraph::Shape(shape);
    } else if (auto a = ngraph::as_type<ngraph::AttributeAdapter<ngraph::Strides>>(&adapter)) {
        std::vector<size_t> shape;
        if (!getParameters<size_t>(m_node.child("data"), name, shape))
            return;
        static_cast<ngraph::Strides&>(*a) = ngraph::Strides(shape);
#ifdef __APPLE__
    } else if (auto a = ngraph::as_type<ngraph::AttributeAdapter<std::vector<size_t>>>(&adapter)) {
        std::vector<size_t> result;
        if (!getParameters<size_t>(m_node.child("data"), name, result))
            return;
        static_cast<std::vector<size_t>&>(*a) = result;
#else
    } else if (auto a = ngraph::as_type<ngraph::AttributeAdapter<std::vector<size_t>>>(&adapter)) {
        std::vector<size_t> result;
        if (!getParameters<size_t>(m_node.child("data"), name, result))
            return;
        a->set(result);
#endif
    } else if (auto a = ngraph::as_type<ngraph::AttributeAdapter<ngraph::AxisSet>>(&adapter)) {
        std::vector<size_t> axes;
        if (!getParameters<size_t>(m_node.child("data"), name, axes))
            return;
        static_cast<ngraph::AxisSet&>(*a) = ngraph::AxisSet(axes);
    } else if (auto a = ngraph::as_type<ngraph::AttributeAdapter<ngraph::op::TopKSortType>>(&adapter)) {
        if (!getStrAttribute(m_node.child("data"), name, val))
            return;
        static_cast<ngraph::op::TopKSortType&>(*a) = ngraph::as_enum<ngraph::op::TopKSortType>(val);
    } else if (auto a = ngraph::as_type<ngraph::AttributeAdapter<ngraph::op::TopKMode>>(&adapter)) {
        if (!getStrAttribute(m_node.child("data"), name, val))
            return;
        static_cast<ngraph::op::TopKMode&>(*a) = ngraph::as_enum<ngraph::op::TopKMode>(val);
    } else if (auto a = ngraph::as_type<ngraph::AttributeAdapter<ngraph::CoordinateDiff>>(&adapter)) {
        std::vector<size_t> shape;
        if (!getParameters<size_t>(m_node.child("data"), name, shape))
            return;
        std::vector<std::ptrdiff_t> coord_diff(shape.begin(), shape.end());
        static_cast<ngraph::CoordinateDiff&>(*a) = ngraph::CoordinateDiff(coord_diff);
    } else if (auto a = ngraph::as_type<ngraph::AttributeAdapter<std::shared_ptr<ngraph::Variable>>>(&adapter)) {
        std::string variable_id;
        if (!getStrAttribute(m_node.child("data"), name, variable_id))
            return;
        if (!m_variables.count(variable_id)) {
            m_variables[variable_id] = std::make_shared<ngraph::Variable>(
                ngraph::VariableInfo{ngraph::PartialShape::dynamic(), ngraph::element::dynamic, variable_id});
        }
        a->set(m_variables[variable_id]);
    } else if (auto a = ngraph::as_type<ngraph::AttributeAdapter<std::shared_ptr<ngraph::runtime::AlignedBuffer>>>(
                   &adapter)) {
        std::string value;
        pugi::xml_node dn = m_node.child("data");
        auto type = XMLParseUtils::GetStrAttr(m_node, "type");

        if (dn.empty())
            IE_THROW() << "No attrtibutes defined for " << type << " op!";

        if (getStrAttribute(dn, name, value)) {
            auto buffer = std::make_shared<ngraph::runtime::AlignedBuffer>(value.size());
            auto data = static_cast<char*>(buffer->get_ptr());
            value.copy(data, value.size());
            a->set(buffer);
        } else if (name == "value" && type == "Const") {
            std::vector<int64_t> shape;
            std::string el_type_str;

            size_t offset = XMLParseUtils::GetUInt64Attr(dn, "offset");
            size_t size = XMLParseUtils::GetUInt64Attr(dn, "size");
            if (!getStrAttribute(dn, "element_type", el_type_str))
                return;
            if (!getParameters<int64_t>(dn, "shape", shape))
                return;

            ngraph::element::Type el_type = details::convertPrecision(el_type_str);

            size_t length = m_weights->byteSize();
            if (!length)
                IE_THROW() << "Empty weights data in bin file or bin file cannot be found!";
            if (length < offset + size)
                IE_THROW() << "Incorrect weights in bin file!";
            if (size < std::ceil(ngraph::shape_size(shape) * el_type.bitwidth() / 8.f))
                IE_THROW() << "Attribute and shape size are inconsistent for " << type << " op!";

            char* data = m_weights->cbuffer().as<char*>() + offset;

            using SharedBuffer = ngraph::runtime::SharedBuffer<const Blob::CPtr>;
            auto buffer = std::make_shared<SharedBuffer>(data, size, m_weights);
            a->set(buffer);
        }
    } else if (auto a = ngraph::as_type<ngraph::AttributeAdapter<ngraph::op::FrameworkNodeAttrs>>(&adapter)) {
        const auto& type = XMLParseUtils::GetStrAttr(m_node, "type");
        const auto& version = XMLParseUtils::GetStrAttr(m_node, "version");

        ngraph::op::FrameworkNodeAttrs node_attrs;
        node_attrs.set_opset_name(version);
        node_attrs.set_type_name(type);

        pugi::xml_node dn = m_node.child("data");

        if (!dn.empty()) {
            for (const auto& data_attr : dn.attributes()) {
                node_attrs[data_attr.name()] = data_attr.as_string();
            }
        }

        a->set(node_attrs);
    } else if (const auto& a = ngraph::as_type<ngraph::AttributeAdapter<ngraph::element::TypeVector>>(&adapter)) {
        ngraph::element::TypeVector types;
        if (!getParameters<ngraph::element::Type>(m_node.child("data"), name, types))
            return;
        a->set(types);
    } else {
        IE_THROW() << "Error IR reading. Attribute adapter can not be found for " << name << " parameter";
    }
}

void XmlDeserializer::on_adapter(const std::string& name,
                                 ngraph::ValueAccessor<std::shared_ptr<ngraph::Function>>& adapter) {
    std::shared_ptr<ngraph::Function> ngraph_function;
    if (!name.compare("body")) {
        auto body_node = m_node.child(name.c_str());
        if (body_node.empty()) {
            IE_THROW() << "TensorIterator has no body.";
        }
        ngraph_function = parse_function(m_node.child(name.c_str()), m_weights);
    } else if (!name.compare("net")) {
        ngraph_function = parse_function(m_node, m_weights);
    } else {
        IE_THROW() << "Error: not recognized adapter name: " << name << ".";
    }
    adapter.set(ngraph_function);
}

std::shared_ptr<ngraph::Function> XmlDeserializer::parse_function(const pugi::xml_node& root,
                                                                  const Blob::CPtr& weights) {
    // OV_ITT_SCOPE_CHAIN(FIRST_INFERENCE, taskChain, itt::domains::V10Reader_RT, "V10Parser", "Parse");

    struct FunctionNodes {
        ngraph::ParameterVector parameters;
        ngraph::ResultVector results;
        ngraph::NodeVector all;
        ngraph::SinkVector sinks;
    };

    struct edge {
        size_t fromLayerId, fromPortId, toPortId;
    };
    struct node_params {
        pugi::xml_node xml;
        GenericLayerParams params;
    };

    std::map<size_t /*layer-id*/, node_params> params;

    std::vector<size_t /*layer-id*/> outputs;
    std::unordered_set<std::string> opName;

    // Read all layers and store their parameters in params map
    FOREACH_CHILD (node, root.child("layers"), "layer") {
        auto node_param = parseGenericParams(node);
        if (opName.find(node_param.name) != opName.end() && node_param.type != "Result")
            IE_THROW() << "Invalid IR! " << node_param.name << " name is not unique!";
        opName.insert(node_param.name);
        params[node_param.layerId] = {node, node_param};
        if (node_param.type == "Result" || node_param.type == "Assign") {
            outputs.push_back(node_param.layerId);
        }
    }

    std::map<size_t /*to-layer-id*/, std::vector<edge>> edges;
    std::map<size_t, std::shared_ptr<ngraph::Node>> id_to_node;

    // Read all edges and store them for further usage
    FOREACH_CHILD (_ec, root.child("edges"), "edge") {
        size_t fromLayer = XMLParseUtils::GetUIntAttr(_ec, "from-layer");
        size_t fromPort = XMLParseUtils::GetUIntAttr(_ec, "from-port");
        size_t toLayer = XMLParseUtils::GetUIntAttr(_ec, "to-layer");
        size_t toPort = XMLParseUtils::GetUIntAttr(_ec, "to-port");
        edges[toLayer].push_back({fromLayer, fromPort, toPort});
    }

    // Run DFS starting from outputs to get nodes topological order
    std::set<size_t> used;
    std::vector<size_t> order;
    std::function<void(size_t)> dfs = [&edges, &order, &used, &dfs](const size_t id) {
        if (used.count(id))
            return;
        used.insert(id);
        for (auto& edge : edges[id]) {
            dfs(edge.fromLayerId);
        }
        order.push_back(id);
    };
    std::for_each(outputs.begin(), outputs.end(), dfs);

    // OV_ITT_SCOPE_NEXT(FIRST_INFERENCE, taskChain, "ConstructNgraphNodes");

    FunctionNodes func_nodes;

    std::map<std::string, std::shared_ptr<ngraph::Node>> variable_id_to_read_value;

    //  Following topological order create nGraph operations
    for (auto& layer_id : order) {
        auto& p = params[layer_id];
        const auto& edgeIt = edges.find(layer_id);
        if (edgeIt == edges.end())
            continue;
        ngraph::OutputVector inputs(edgeIt->second.size());
        for (auto& e : edgeIt->second) {
            auto input_node = id_to_node[e.fromLayerId];
            if (!input_node) {
                IE_THROW() << "Attempt to access node " << e.fromLayerId << " that not in graph.";
            }
            auto& p_output = params[e.fromLayerId].params;
            size_t const realInputPortId = p.params.getRealInputPortId(e.toPortId);
            if (realInputPortId >= inputs.size())
                IE_THROW() << p.params.type << " layer " << p.params.name << " with id: " << p.params.layerId
                           << " is inconsistent!";
            inputs[realInputPortId] = input_node->output(p_output.getRealOutputPortId(e.fromPortId));
        }

        auto node = createNode(inputs, p.xml, weights, p.params);
        id_to_node[layer_id] = node;

        // Check that output shape after nGraph node validation the same as in IR
        // because IR always right!
        // Temporary disabled!
        //        for (size_t i = 0; i < p.params.outputPorts.size(); ++i) {
        //            if (p.params.outputPorts[i].dims != node->output(i).get_shape()) {
        //                IE_THROW() << "Shape after nGraph infer " <<
        //                details::dumpVec(node->output(i).get_shape())
        //                                   << " differ from IR shapes: " <<
        //                                   details::dumpVec(p.params.outputPorts[i].dims);
        //            }
        //        }

        if (const auto& parameter_node = std::dynamic_pointer_cast<ngraph::op::Parameter>(node)) {
            io_map.inputs.insert({layer_id, func_nodes.parameters.size()});
            func_nodes.parameters.emplace_back(parameter_node);
        }

        if (const auto& result_node = std::dynamic_pointer_cast<ngraph::op::Result>(node)) {
            io_map.outputs.insert({layer_id, func_nodes.results.size()});
            func_nodes.results.emplace_back(result_node);
        }

        if (const auto& sink = std::dynamic_pointer_cast<ngraph::op::Sink>(node)) {
            func_nodes.sinks.emplace_back(sink);
        }

        if (const auto& read_value = std::dynamic_pointer_cast<ngraph::op::ReadValueBase>(node)) {
            variable_id_to_read_value[read_value->get_variable_id()] = read_value;
        }

        func_nodes.all.emplace_back(node);
    }

    // OV_ITT_SCOPE_NEXT(FIRST_INFERENCE, taskChain, "ConstructNgraphFunction");

    auto function = std::make_shared<ngraph::Function>(func_nodes.results,
                                                       func_nodes.sinks,
                                                       func_nodes.parameters,
                                                       XMLParseUtils::GetStrAttr(root, "name", ""));
    for (const auto& sink : func_nodes.sinks) {
        if (const auto& assign = std::dynamic_pointer_cast<ngraph::op::AssignBase>(sink)) {
            assign->add_control_dependency(variable_id_to_read_value.at(assign->get_variable_id()));
        }
    }

    return function;
}

GenericLayerParams XmlDeserializer::parseGenericParams(const pugi::xml_node& node) {
    const auto parsePort = [this](const pugi::xml_node& parentNode,
                                  const GenericLayerParams& params,
                                  bool input) -> GenericLayerParams::LayerPortData {
        GenericLayerParams::LayerPortData port;

        port.portId = XMLParseUtils::GetIntAttr(parentNode, "id");

        FOREACH_CHILD (node, parentNode, "dim") {
            int64_t dim = 0;
            const pugi::char_t* dimVal = node.child_value();
            std::stringstream ss(dimVal);
            if (!(ss >> dim) || dim < -1) {
                IE_THROW() << "dimension (" << dimVal << ") in node " << node.name()
                           << " must be greater or equal to -1: at offset " << node.offset_debug();
            }
            port.dims.push_back(dim);
        }

        ngraph::element::Type type(ngraph::element::Type_t::undefined);
        // Input port hasn't precision
        if (!input) {
            const std::string& preStr = XMLParseUtils::GetStrAttr(parentNode, "precision");
            type = InferenceEngine::details::convertPrecision(preStr);
        }
        port.precision = type;
        std::vector<std::string> names;
        if (getParameters<std::string>(parentNode, "names", names)) {
            for (size_t i = 0; i < names.size(); i++) {
                std::string name = names[i];
                // Restore original name if it contains delimiter
                // getParameters(...) returns the vector of names which were split by delimiter ','
                // but some names can contain ',' as a part of name, in this case we use '\' to
                // escape delimiter the cycle below is needed in order to find names which contained
                // delimiter and restore the original name
                while (i < names.size() && names[i].at(names[i].length() - 1) == '\\') {
                    name.replace(names[i].length() - 1, 1, ",");
                    name += names[++i];
                }
                port.names.emplace(name);
            }
        }
        return port;
    };
    GenericLayerParams params;

    params.layerId = XMLParseUtils::GetIntAttr(node, "id");
    params.version = XMLParseUtils::GetStrAttr(node, "version");

    params.type = XMLParseUtils::GetStrAttr(node, "type");

    params.name = XMLParseUtils::GetStrAttr(node, "name");

    auto outNode = node.child("output");
    if (!outNode.empty()) {
        FOREACH_CHILD (_cn, outNode, "port") { params.outputPorts.emplace_back(parsePort(_cn, params, false)); }
    }
    auto inpNode = node.child("input");
    if (!inpNode.empty()) {
        FOREACH_CHILD (_cn, inpNode, "port") { params.inputPorts.emplace_back(parsePort(_cn, params, true)); }
    }
    return params;
}

std::shared_ptr<ngraph::Node> XmlDeserializer::createNode(const std::vector<ngraph::Output<ngraph::Node>>& inputs,
                                                          const pugi::xml_node& node,
                                                          const Blob::CPtr& weights,
                                                          const GenericLayerParams& params) {
    // Check that inputs are correctly defined
    for (size_t i = 0; i < inputs.size(); i++) {
        if (!inputs[i].get_node())
            IE_THROW() << params.type << " layer " << params.name << " with id: " << params.layerId
                       << " has incorrect input with index " << i << "!";
        if (ngraph::element::Type_t::undefined == inputs[i].get_element_type())
            IE_THROW() << params.type << " layer " << params.name << " with id: " << params.layerId
                       << " has undefined element type for input with index " << i << "!";
    }

    std::shared_ptr<ngraph::Node> ngraphNode;

    // Find registered opset
    auto opsetIt = m_opsets.find(params.version);

    // Try to create operation from loaded opsets
    static const std::unordered_set<std::string> experimental_ops_added_to_opset = {
        "ExperimentalDetectronDetectionOutput",
        "ExperimentalDetectronGenerateProposalsSingleImage",
        "ExperimentalDetectronPriorGridGenerator",
        "ExperimentalDetectronROIFeatureExtractor",
        "ExperimentalDetectronTopKROIs",
        "GRUCell",
        "RNNCell",
        "Proposal"};

    if (experimental_ops_added_to_opset.count(params.type) &&
        (params.version == "experimental" || params.version == "extension")) {
        opsetIt = m_opsets.find("opset6");
    }

    if (!ngraphNode && opsetIt != m_opsets.end()) {
        auto const& type = params.type == "Const" ? "Constant" : params.type;

        if (params.version == "opset1") {
            // MVN, ROIPooling and ReorgYolo were missing in opset1
            if (type == "MVN" || type == "ROIPooling" || type == "ReorgYolo") {
                opsetIt = m_opsets.find("opset2");
                if (opsetIt == m_opsets.end()) {
                    IE_THROW() << "Cannot create " << params.type << " layer " << params.name
                               << " id:" << params.layerId << " from unsupported opset: " << params.version;
                }
            }
        }

        auto const& opset = opsetIt->second;

        ngraphNode = std::shared_ptr<ngraph::Node>(opset.create_insensitive(type));
        if (!ngraphNode) {
            IE_THROW() << "Opset " << params.version << " doesn't contain the operation with type: " << type;
        }
        // Share Weights form constant blob
        if (auto constant = std::dynamic_pointer_cast<ngraph::op::Constant>(ngraphNode)) {
            constant->alloc_buffer_on_visit_attributes(false);
        }
        ngraphNode->set_arguments(inputs);
        XmlDeserializer visitor(node, weights, m_opsets, m_variables);

        if (ngraphNode->visit_attributes(visitor)) {
            ngraphNode->constructor_validate_and_infer_types();
        }

        // To be sure that all default values will be initialized:
        ngraphNode = ngraphNode->clone_with_new_inputs(ngraphNode->input_values());
    }

    if (!ngraphNode && m_use_framework_node) {
        ngraphNode = std::make_shared<ngraph::op::FrameworkNode>(inputs);
        XmlDeserializer visitor(node, weights, m_opsets, m_variables);
        ngraphNode->visit_attributes(visitor);

        size_t index{0};
        for (const auto& output_params : params.outputPorts) {
            ngraphNode->set_output_type(index, output_params.precision, ngraph::PartialShape(output_params.dims));
            ++index;
        }
    }

    if (!ngraphNode) {
        IE_THROW() << "Cannot create " << params.type << " layer " << params.name << " id:" << params.layerId
                   << " from unsupported opset: " << params.version;
    }

    // Save run time info
    auto& rtInfo = ngraphNode->get_rt_info();
    pugi::xml_node dn = node.child("data");
    if (dn) {
        const auto pr_data = dn.attribute("PrimitivesPriority");
        if (pr_data) {
            rtInfo["PrimitivesPriority"] = std::make_shared<::ngraph::VariantWrapper<std::string>>(pr_data.value());
        }
        const auto aw_data = dn.attribute("alt_width");
        if (aw_data) {
            rtInfo["alt_width"] = std::make_shared<::ngraph::VariantWrapper<std::string>>(aw_data.value());
        }
    }

    ngraphNode->set_friendly_name(params.name);
    for (size_t i = 0; i < params.outputPorts.size() && i < ngraphNode->get_output_size(); ++i) {
        if (!params.outputPorts[i].names.empty())
            ngraphNode->get_output_tensor(i).set_names(params.outputPorts[i].names);
    }

    return ngraphNode;
}

}  // namespace

namespace ngraph {
namespace frontend {
std::shared_ptr<Function> InputModelIR::convert() {
    std::unordered_map<std::string, ngraph::OpSet> opsets;
    std::unordered_map<std::string, std::shared_ptr<ngraph::Variable>> variables;

    // Load default opsets
    opsets["opset1"] = ngraph::get_opset1();
    opsets["opset2"] = ngraph::get_opset2();
    opsets["opset3"] = ngraph::get_opset3();
    opsets["opset4"] = ngraph::get_opset4();
    opsets["opset5"] = ngraph::get_opset5();
    opsets["opset6"] = ngraph::get_opset6();
    opsets["opset7"] = ngraph::get_opset7();
    opsets["opset8"] = ngraph::get_opset8();

    // Load custom opsets
    for (const auto& ext : m_exts) {
        for (const auto& it : ext->getOpSets()) {
            if (opsets.find(it.first) != opsets.end())
                IE_THROW() << "Cannot add opset with name: " << it.first
                           << ". Opset with the same name already exists.";
            opsets[it.first] = it.second;
        }
    }

    XmlDeserializer visitor(m_root, m_weights, opsets, variables);
    bool use_framework_node{false};
    for (const auto& ext : m_exts) {
        const InferenceEngine::Version* version = nullptr;
        ext->GetVersion(version);
        if (version && version->description && strcmp(version->description, "framework_node_ext") == 0) {
            use_framework_node = true;
            break;
        }
    }
    visitor.use_framework_node(use_framework_node);

    std::shared_ptr<ngraph::Function> function;
    visitor.on_attribute("net", function);
    return function;
}
}  // namespace frontend
}  // namespace ngraph
