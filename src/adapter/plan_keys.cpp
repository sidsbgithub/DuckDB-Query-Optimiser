// Keys made from DuckDB's own serialization of a plan, whether a bound plan may be recycled, and the key of a whole
// query result (ADR-0004).

#include "plan_keys.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"

#include <cstring>

namespace duckdb {

namespace recycling {

//===--------------------------------------------------------------------===//
// RecyclingKeySerializer
//===--------------------------------------------------------------------===//
RecyclingKeySerializer::RecyclingKeySerializer(const std::unordered_set<idx_t> &defined) : defined(defined) {
	options.serialize_default_values = true;
	options.serialization_compatibility = SerializationCompatibility::Latest();
	key.reserve(16384);
}

void RecyclingKeySerializer::OnPropertyBegin(const field_id_t field_id, const char *tag) {
	Raw('P');
	Raw(field_id);
	BeginProperty(tag);
}

void RecyclingKeySerializer::OnPropertyEnd() {
	EndProperty();
}

void RecyclingKeySerializer::OnOptionalPropertyBegin(const field_id_t field_id, const char *tag, bool present) {
	Raw('O');
	Raw(field_id);
	Raw(present);
	BeginProperty(tag);
}

void RecyclingKeySerializer::OnOptionalPropertyEnd(bool present) {
	EndProperty();
}

void RecyclingKeySerializer::OnObjectBegin() {
	Raw('{');
}

void RecyclingKeySerializer::OnObjectEnd() {
	Raw('}');
}

void RecyclingKeySerializer::OnListBegin(idx_t count) {
	Raw('[');
	Raw(count);
}

void RecyclingKeySerializer::OnListEnd() {
	Raw(']');
}

void RecyclingKeySerializer::OnNullableBegin(bool present) {
	Raw('N');
	Raw(present);
}

void RecyclingKeySerializer::OnNullableEnd() {
}

void RecyclingKeySerializer::WriteNull() {
	Raw('0');
}

void RecyclingKeySerializer::WriteValue(bool value) {
	Typed('b', value);
}

void RecyclingKeySerializer::WriteValue(uint8_t value) {
	Typed('c', value);
}

void RecyclingKeySerializer::WriteValue(int8_t value) {
	Typed('C', value);
}

void RecyclingKeySerializer::WriteValue(uint16_t value) {
	Typed('h', value);
}

void RecyclingKeySerializer::WriteValue(int16_t value) {
	Typed('H', value);
}

void RecyclingKeySerializer::WriteValue(uint32_t value) {
	Typed('i', value);
}

void RecyclingKeySerializer::WriteValue(int32_t value) {
	Typed('I', value);
}

void RecyclingKeySerializer::WriteValue(uint64_t value) {
	if (InBinding("table_index")) {
		binding_table = value;
	} else if (block_scans && InBinding("column_index")) {
		auto scan = block_scans->find(binding_table);
		if (scan != block_scans->end()) {
			value = scan->second.get().GetColumnIds()[value].GetPrimaryIndex();
		}
	}
	if (index_tag == QUERY_LOCATION) {
		// Where an expression appears in the statement's text does not change what it computes.
		value = 0;
	} else if (index_tag != NOT_AN_INDEX && value != DConstants::INVALID_INDEX) {
		// A join that is not a MARK join keeps an unused mark index.
		if (index_tag == TABLE_INDEX && !defined.count(value)) {
			closed = false;
		}
		value = canonical.emplace(value, canonical.size()).first->second;
	}
	Typed('l', value);
}

void RecyclingKeySerializer::WriteValue(int64_t value) {
	Typed('L', value);
}

void RecyclingKeySerializer::WriteValue(hugeint_t value) {
	Typed('g', value);
}

void RecyclingKeySerializer::WriteValue(uhugeint_t value) {
	Typed('G', value);
}

void RecyclingKeySerializer::WriteValue(float value) {
	Typed('f', value);
}

void RecyclingKeySerializer::WriteValue(double value) {
	Typed('d', value);
}

void RecyclingKeySerializer::WriteValue(const string_t value) {
	Text(value.GetData(), value.GetSize());
}

void RecyclingKeySerializer::WriteValue(const string &value) {
	Text(value.data(), value.size());
}

void RecyclingKeySerializer::WriteValue(const char *str) {
	Text(str, strlen(str));
}

void RecyclingKeySerializer::WriteDataPtr(const_data_ptr_t ptr, idx_t count) {
	Raw('D');
	Raw(count);
	if (!skipping) {
		key.append(const_char_ptr_cast(ptr), count);
	}
}

// Tags are string literals: each is classified once per thread.
RecyclingKeySerializer::IndexTagKind RecyclingKeySerializer::IndexTag(const char *tag) {
	static thread_local std::unordered_map<const char *, IndexTagKind> kinds;
	auto entry = kinds.find(tag);
	if (entry == kinds.end()) {
		entry = kinds.emplace(tag, ClassifyTag(tag)).first;
	}
	return entry->second;
}

static bool TagIs(const char *tag, const char *name) {
	return tag && strcmp(tag, name) == 0;
}

RecyclingKeySerializer::IndexTagKind RecyclingKeySerializer::ClassifyTag(const char *tag) {
	static const char *const TABLE_INDEX_TAGS[] = {"table_index",   "group_index",  "aggregate_index",
	                                               "groupings_index", "window_index", "cte_index",
	                                               "unnest_index",  "pivot_index",  "excluded_table_index"};
	if (!tag) {
		return NOT_AN_INDEX;
	}
	if (strcmp(tag, "mark_index") == 0) {
		return MARK_INDEX;
	}
	if (strcmp(tag, "query_location") == 0) {
		return QUERY_LOCATION;
	}
	for (auto candidate : TABLE_INDEX_TAGS) {
		if (strcmp(tag, candidate) == 0) {
			return TABLE_INDEX;
		}
	}
	return NOT_AN_INDEX;
}

bool RecyclingKeySerializer::InBinding(const char *field) const {
	auto depth = tags.size();
	return depth >= 2 && TagIs(tags[depth - 1], field) && TagIs(tags[depth - 2], "binding");
}

void RecyclingKeySerializer::BeginProperty(const char *tag) {
	bool skip = (block_scans && TagIs(tag, "column_indexes")) || (without_values && TagIs(tag, "value"));
	tags.push_back(tag);
	skipped.push_back(skip);
	if (skip) {
		skipping++;
	}
	index_tag = IndexTag(tag);
}

void RecyclingKeySerializer::EndProperty() {
	if (skipped.back()) {
		skipping--;
	}
	tags.pop_back();
	skipped.pop_back();
	index_tag = NOT_AN_INDEX;
}

void RecyclingKeySerializer::Text(const char *data, idx_t size) {
	Raw('s');
	Raw(size);
	if (!skipping) {
		key.append(data, size);
	}
}

//===--------------------------------------------------------------------===//
// Eligibility of bound plans, and the keys of whole query results
//===--------------------------------------------------------------------===//
// A DELIM_GET reads the rows of the DELIM_JOIN whose delim side holds it, which must be inside the plan:
// `in_delim_side`.
bool IsRecyclableQuery(LogicalOperator &op, bool in_delim_side) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_PROJECTION:
	case LogicalOperatorType::LOGICAL_FILTER:
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
	case LogicalOperatorType::LOGICAL_WINDOW:
	case LogicalOperatorType::LOGICAL_UNNEST:
	case LogicalOperatorType::LOGICAL_LIMIT:
	case LogicalOperatorType::LOGICAL_ORDER_BY:
	case LogicalOperatorType::LOGICAL_TOP_N:
	case LogicalOperatorType::LOGICAL_DISTINCT:
	case LogicalOperatorType::LOGICAL_CHUNK_GET:
	case LogicalOperatorType::LOGICAL_EXPRESSION_GET:
	case LogicalOperatorType::LOGICAL_DUMMY_SCAN:
	case LogicalOperatorType::LOGICAL_EMPTY_RESULT:
	case LogicalOperatorType::LOGICAL_CTE_REF:
	case LogicalOperatorType::LOGICAL_MATERIALIZED_CTE:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
	case LogicalOperatorType::LOGICAL_UNION:
	case LogicalOperatorType::LOGICAL_EXCEPT:
	case LogicalOperatorType::LOGICAL_INTERSECT:
		break;
	case LogicalOperatorType::LOGICAL_DELIM_GET:
		if (!in_delim_side) {
			return false;
		}
		break;
	case LogicalOperatorType::LOGICAL_GET: {
		auto &scan = op.Cast<LogicalGet>();
		auto table = scan.GetTable();
		if (!table || !table->IsDuckTable() || table->temporary) {
			return false;
		}
		break;
	}
	default:
		return false;
	}
	bool recyclable = true;
	LogicalOperatorVisitor::EnumerateExpressions(op, [&](unique_ptr<Expression> *expression) {
		auto &expr = **expression;
		if (!expr.IsConsistent() || expr.HasParameter() || expr.HasSubquery()) {
			recyclable = false;
		}
	});
	if (!recyclable) {
		return false;
	}
	for (idx_t i = 0; i < op.children.size(); i++) {
		bool delim_side = in_delim_side;
		if (op.type == LogicalOperatorType::LOGICAL_DELIM_JOIN) {
			delim_side = delim_side || i == (op.Cast<LogicalComparisonJoin>().delim_flipped ? 0 : 1);
		}
		if (!IsRecyclableQuery(*op.children[i], delim_side)) {
			return false;
		}
	}
	return true;
}

void CollectTableIndices(const LogicalOperator &op, std::unordered_set<idx_t> &defined) {
	for (auto index : op.GetTableIndex()) {
		defined.insert(index);
	}
	for (auto &child : op.children) {
		CollectTableIndices(*child, defined);
	}
}

string QueryResultKey(ClientContext &context, LogicalOperator &root) {
	std::unordered_set<idx_t> defined;
	CollectTableIndices(root, defined);
	RecyclingKeySerializer serializer(defined);
	if (!TrySerialize(serializer, root) || !serializer.closed) {
		return string();
	}
	// Whether rows keep their insertion order changes which order a result may come in.
	return (Settings::Get<PreserveInsertionOrderSetting>(context) ? "query_ordered:" : "query:") + serializer.key;
}

void CollectColumns(const Expression &expression, column_binding_set_t &columns) {
	if (expression.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		columns.insert(expression.Cast<BoundColumnRefExpression>().binding);
	}
	ExpressionIterator::EnumerateChildren(expression, [&](const Expression &child) { CollectColumns(child, columns); });
}

} // namespace recycling
} // namespace duckdb
