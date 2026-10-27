#include "recycling_key.hpp"

#include <algorithm>

namespace waddle {

// Appends one length-prefixed part, so that no two sequences of parts encode alike.
static void Part(std::string &key, const std::string &value) {
	key += std::to_string(value.size()) + ":" + value;
}

static void Parts(std::string &key, const std::vector<uint64_t> &values) {
	for (auto value : values) {
		Part(key, std::to_string(value));
	}
}

static bool EncodeExpression(const KeyExpression &expression, const std::vector<KeyBinding> &input_bindings,
                             std::string &key) {
	Part(key, std::to_string(expression.kind));
	Part(key, expression.type);
	if (expression.is_column_reference) {
		auto found = std::find(input_bindings.begin(), input_bindings.end(), expression.binding);
		if (found == input_bindings.end()) {
			return false;
		}
		Part(key, "input:" + std::to_string(found - input_bindings.begin()));
	} else {
		Part(key, expression.value);
	}
	for (auto &child : expression.children) {
		if (!EncodeExpression(child, input_bindings, key)) {
			return false;
		}
	}
	Part(key, "end_expression");
	return true;
}

bool EncodeRecyclingKey(const KeyNode &node, std::string &key) {
	Part(key, "children:" + std::to_string(node.children.size()));
	for (auto &child : node.children) {
		std::string child_key;
		if (!EncodeRecyclingKey(*child, child_key)) {
			return false;
		}
		Part(key, child_key);
	}
	Part(key, "operator:" + std::to_string(node.kind));
	if (!node.scan_source.empty()) {
		Part(key, "scan_source");
		Part(key, node.scan_source);
		Parts(key, node.scan_columns);
		Part(key, "scan_projection");
		Parts(key, node.scan_projection);
		Part(key, "scan_filters");
		for (auto &filter : node.scan_filters) {
			Part(key, std::to_string(filter.first));
			Part(key, filter.second);
		}
	}
	Part(key, "filter_projection");
	Parts(key, node.filter_projection);
	Part(key, "join_kind:" + std::to_string(node.join_kind));
	Part(key, "left_projection");
	Parts(key, node.left_projection);
	Part(key, "right_projection");
	Parts(key, node.right_projection);
	for (auto &condition : node.join_conditions) {
		Part(key, "join_comparison:" + std::to_string(condition.comparison));
		if (!EncodeExpression(condition.left, node.input_bindings, key) ||
		    !EncodeExpression(condition.right, node.input_bindings, key)) {
			return false;
		}
	}
	Part(key, "distinct_validity:" + std::to_string(node.distinct_validity));
	Part(key, "grouping_set");
	Parts(key, node.grouping_set);
	for (auto &group : node.groups) {
		Part(key, "group");
		if (!EncodeExpression(group, node.input_bindings, key)) {
			return false;
		}
	}
	for (auto &expression : node.expressions) {
		Part(key, "expression");
		if (!EncodeExpression(expression, node.input_bindings, key)) {
			return false;
		}
	}
	Part(key, "output_types");
	for (auto &type : node.output_types) {
		Part(key, type);
	}
	return true;
}

} // namespace waddle
