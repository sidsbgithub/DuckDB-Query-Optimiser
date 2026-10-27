#pragma once

// Facts about bound plans that several passes share: whether a plan may be recycled, the table indices it defines,
// the columns an expression reads, and keys made from DuckDB's own serialization of a plan (RecyclingKeySerializer).

#include "duckdb/common/common.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

#include <unordered_map>
#include <unordered_set>

namespace duckdb {

class ClientContext;
class Expression;
class LogicalOperator;

namespace recycling {

// Writes DuckDB's own serialization of a plan, which describes it completely: DuckDB can rebuild the plan from it.
// Table indices are renumbered in the order they first appear, so a subtree bound inside a larger query matches the
// same statement bound on its own. A subtree that refers to a table index defined outside it is not closed: its
// result depends on its context.
class RecyclingKeySerializer : public Serializer {
public:
	explicit RecyclingKeySerializer(const std::unordered_set<idx_t> &defined);

	string key;
	bool closed = true;
	// Table index -> canonical index, in the order the table indices first appear.
	std::unordered_map<idx_t, idx_t> canonical;
	// For a block key: the scans whose columns the key describes by their table column, not by which of them the
	// statement happens to read. A column reference to one names the table column; the scan's own list is left out.
	optional_ptr<const std::unordered_map<idx_t, reference<LogicalGet>>> block_scans;
	// For a family key: leaves out constant values, so blocks that differ only in their constants share it.
	bool without_values = false;

	void OnPropertyBegin(const field_id_t field_id, const char *tag) override;
	void OnPropertyEnd() override;
	void OnOptionalPropertyBegin(const field_id_t field_id, const char *tag, bool present) override;
	void OnOptionalPropertyEnd(bool present) override;
	void OnObjectBegin() override;
	void OnObjectEnd() override;
	void OnListBegin(idx_t count) override;
	void OnListEnd() override;
	void OnNullableBegin(bool present) override;
	void OnNullableEnd() override;
	void WriteNull() override;
	void WriteValue(bool value) override;
	void WriteValue(uint8_t value) override;
	void WriteValue(int8_t value) override;
	void WriteValue(uint16_t value) override;
	void WriteValue(int16_t value) override;
	void WriteValue(uint32_t value) override;
	void WriteValue(int32_t value) override;
	// Table indices, and table columns of block scans, are written canonical.
	void WriteValue(uint64_t value) override;
	void WriteValue(int64_t value) override;
	void WriteValue(hugeint_t value) override;
	void WriteValue(uhugeint_t value) override;
	void WriteValue(float value) override;
	void WriteValue(double value) override;
	void WriteValue(const string_t value) override;
	void WriteValue(const string &value) override;
	void WriteValue(const char *str) override;
	void WriteDataPtr(const_data_ptr_t ptr, idx_t count) override;

private:
	enum IndexTagKind { NOT_AN_INDEX, TABLE_INDEX, MARK_INDEX, QUERY_LOCATION };
	static IndexTagKind IndexTag(const char *tag);
	static IndexTagKind ClassifyTag(const char *tag);
	// Whether the value being written is this field of a column binding.
	bool InBinding(const char *field) const;
	void BeginProperty(const char *tag);
	void EndProperty();
	template <class T>
	void Raw(const T &value) {
		if (!skipping) {
			key.append(reinterpret_cast<const char *>(&value), sizeof(T));
		}
	}
	template <class T>
	void Typed(char type, const T &value) {
		Raw(type);
		Raw(value);
	}
	void Text(const char *data, idx_t size);

	const std::unordered_set<idx_t> &defined;
	IndexTagKind index_tag = NOT_AN_INDEX;
	vector<const char *> tags;
	vector<uint8_t> skipped;
	idx_t skipping = 0;
	idx_t binding_table = DConstants::INVALID_INDEX;
};

// Writes a plan or an expression into the serializer as one object. False if DuckDB cannot serialize it.
template <class T>
bool TrySerialize(RecyclingKeySerializer &serializer, const T &object) {
	try {
		serializer.OnObjectBegin();
		object.Serialize(serializer);
		serializer.OnObjectEnd();
	} catch (std::exception &) {
		return false;
	}
	return true;
}

// Whether the plan's result depends only on committed DuckDB tables and the plan itself.
bool IsRecyclableQuery(LogicalOperator &op, bool in_delim_side = false);
// Adds every table index that `op` and its children define.
void CollectTableIndices(const LogicalOperator &op, std::unordered_set<idx_t> &defined);
// The key of a query result: the serialized bound plan, closed over its table indices. Empty if the plan is not
// closed or cannot be serialized.
string QueryResultKey(ClientContext &context, LogicalOperator &root);
// Adds every column binding that the expression reads.
void CollectColumns(const Expression &expression, column_binding_set_t &columns);

} // namespace recycling
} // namespace duckdb
