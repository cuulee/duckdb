
#include "execution/aggregate_hashtable.hpp"
#include "common/exception.hpp"
#include "common/types/vector_operations.hpp"

using namespace duckdb;
using namespace std;

SuperLargeHashTable::SuperLargeHashTable(size_t initial_capacity,
                                         size_t group_width,
                                         size_t payload_width,
                                         vector<ExpressionType> aggregate_types,
                                         bool parallel)
    : entries(0), capacity(0), data(nullptr), group_width(group_width),
      payload_width(payload_width), aggregate_types(aggregate_types),
      max_chain(0), parallel(parallel) {
	// HT tuple layout is as follows:
	// [FLAG][GROUPS][PAYLOAD][COUNT]
	// [FLAG] is the state of the tuple in memory
	// [GROUPS] is the groups
	// [PAYLOAD] is the payload (i.e. the aggregates)
	// [COUNT] is an 8-byte count for each element
	tuple_size = FLAG_SIZE + (group_width + payload_width + sizeof(int64_t));
	Resize(initial_capacity);
}

SuperLargeHashTable::~SuperLargeHashTable() {}

void SuperLargeHashTable::Resize(size_t size) {
	if (size <= capacity) {
		throw NotImplementedException("Cannot downsize!");
	}

	if (entries > 0) {
		throw NotImplementedException(
		    "Resizing a filled HT not implemented yet!");
	} else {
		data = new uint8_t[size * tuple_size];
		owned_data = unique_ptr<uint8_t[]>(data);
		for (size_t i = 0; i < size; i++) {
			data[i * tuple_size] = EMPTY_CELL;
		}

		capacity = size;
	}
}

void SuperLargeHashTable::AddChunk(DataChunk &groups, DataChunk &payload) {
	if (groups.count == 0) {
		return;
	}
	// first create a hash of all the values
	Vector hashes(TypeId::INTEGER, groups.count);
	VectorOperations::Hash(*groups.data[0], hashes);
	for (size_t i = 1; i < groups.column_count; i++) {
		VectorOperations::CombineHash(hashes, *groups.data[i], hashes);
	}

	// list of addresses for the tuples
	Vector addresses(TypeId::POINTER, groups.count);
	// first cast from the hash type to the address type
	VectorOperations::Cast(hashes, addresses);
	// now compute the entry in the table based on the hash using a modulo
	VectorOperations::Modulo(addresses, capacity, addresses);
	// get the physical address of the tuple
	// multiply the position by the tuple size and add the base address
	VectorOperations::Multiply(addresses, tuple_size, addresses);
	VectorOperations::Add(addresses, (uint64_t)data, addresses);

	if (parallel) {
		throw NotImplementedException("Parallel HT not implemented");
	}

	// now we actually access the base table
	uint8_t group_data[group_width];
	sel_t new_entries[addresses.count];
	sel_t updated_entries[addresses.count];

	size_t new_count = 0, updated_count = 0;

	void **ptr = (void **)addresses.data;
	for (size_t i = 0; i < addresses.count; i++) {
		// first copy the group data for this tuple into a local space
		size_t group_position = 0;
		for (size_t grp = 0; grp < groups.column_count; grp++) {
			size_t data_size = GetTypeIdSize(groups.data[grp]->type);
			memcpy(group_data + group_position,
			       groups.data[grp]->data + data_size * i, data_size);
			group_position += data_size;
		}

		// place this tuple in the hash table
		uint8_t *entry = (uint8_t *)ptr[i];

		size_t chain = 0;
		do {
			// check if the group is the actual group for this tuple or if it is
			// empty otherwise we have to do linear probing
			if (*entry == EMPTY_CELL) {
				// cell is empty; zero initialize the aggregates and add an
				// entry
				*entry = FULL_CELL;
				memcpy(entry + FLAG_SIZE, group_data, group_width);
				memset(entry + group_width + FLAG_SIZE, 0,
				        payload_width + sizeof(uint64_t));
				new_entries[new_count++] = i;
				entries++;
				break;
			}
			if (memcmp(group_data, entry + FLAG_SIZE, group_width) == 0) {
				// cell has the current group, just point to this cell
				updated_entries[updated_count++] = i;
				break;
			}

			// collision: move to then next location
			chain++;
			entry += tuple_size;
			if (entry > data + tuple_size * capacity) {
				entry = data;
			}
		} while (true);

		// update the address pointer with the final position
		ptr[i] = entry + FLAG_SIZE + group_width;
		max_chain = max(chain, max_chain);
	}

	// now every cell has an entry
	// update the aggregates
	Vector one(Value::NumericValue(TypeId::BIGINT, 1));
	size_t j = 0;
	for (size_t i = 0; i < aggregate_types.size(); i++) {
		if (aggregate_types[i] == ExpressionType::AGGREGATE_COUNT_STAR) {
			continue;
		}
		if (new_count > 0) {
			payload.data[j]->sel_vector = addresses.sel_vector = new_entries;
			payload.data[j]->count = addresses.count = new_count;
			// for any entries for which a new entry was created, set the
			// initial value
			switch (aggregate_types[i]) {
			case ExpressionType::AGGREGATE_COUNT:
				VectorOperations::Scatter::SetCount(*payload.data[j],
				                                    addresses);
				break;
			case ExpressionType::AGGREGATE_SUM:
			case ExpressionType::AGGREGATE_AVG:
			case ExpressionType::AGGREGATE_MIN:
			case ExpressionType::AGGREGATE_MAX:
				VectorOperations::Scatter::Set(*payload.data[j], addresses);
				break;
			default:
				throw NotImplementedException("Unimplemented aggregate type!");
			}
		}
		if (updated_count > 0) {
			// for any entries for which a group was found, update the aggregate
			payload.data[j]->sel_vector = addresses.sel_vector =
			    updated_entries;
			payload.data[j]->count = addresses.count = updated_count;

			switch (aggregate_types[i]) {
			case ExpressionType::AGGREGATE_COUNT:
				VectorOperations::Scatter::AddOne(*payload.data[j], addresses);
				break;
			case ExpressionType::AGGREGATE_SUM:
			case ExpressionType::AGGREGATE_AVG:
				// addition
				VectorOperations::Scatter::Add(*payload.data[j], addresses);
				break;
			case ExpressionType::AGGREGATE_MIN:
				// min
				VectorOperations::Scatter::Min(*payload.data[j], addresses);
				break;
			case ExpressionType::AGGREGATE_MAX:
				// max
				VectorOperations::Scatter::Max(*payload.data[j], addresses);
				break;
			default:
				throw NotImplementedException("Unimplemented aggregate type!");
			}
		}
		// move to the next aggregate chunk
		addresses.sel_vector = nullptr;
		addresses.count = groups.count;
		VectorOperations::Add(addresses, GetTypeIdSize(payload.data[j]->type),
		                      addresses);
		j++;
	}
	// update the counts in each bucket
	VectorOperations::Scatter::Add(one, addresses);
}

static void _average_gather_loop(void **source, size_t offset, Vector &dest);

void SuperLargeHashTable::Scan(size_t &scan_position, DataChunk &groups,
                               DataChunk &result) {
	result.Reset();

	uint8_t *ptr;
	uint8_t *start = data + scan_position * tuple_size;
	uint8_t *end = data + capacity * tuple_size;
	if (start >= end)
		return;

	Vector addresses(TypeId::POINTER, result.maximum_size);
	void **data_pointers = (void **)addresses.data;

	// scan the table for full cells starting from the scan position
	size_t entry = 0;
	for (ptr = start; ptr < end && entry < result.maximum_size;
	     ptr += tuple_size) {
		if (*ptr == FULL_CELL) {
			// found entry
			data_pointers[entry++] = ptr + FLAG_SIZE;
		}
	}
	if (entry == 0) {
		return;
	}
	addresses.count = entry;
	// fetch the group columns
	for (size_t i = 0; i < groups.column_count; i++) {
		auto column = groups.data[i].get();
		column->count = entry;
		VectorOperations::Gather::Set(addresses, *column);
		VectorOperations::Add(addresses, GetTypeIdSize(column->type),
		                      addresses);
	}

	size_t current_bytes = 0;
	for (size_t i = 0; i < aggregate_types.size(); i++) {
		auto target = result.data[i].get();
		target->count = entry;

		if (aggregate_types[i] == ExpressionType::AGGREGATE_COUNT_STAR) {
			// we fetch the total counts later because they are stored at the
			// end
			continue;
		}
		if (aggregate_types[i] == ExpressionType::AGGREGATE_AVG) {
			// for the average we only have computed the sum
			// so we need to divide by the count
			// we do this in one instruction: loop over the data and
			// first get the distance in the HT from this point to the count
			size_t offset_to_count = payload_width - current_bytes;
			// now fetch both the count and the sum and average them in one loop
			_average_gather_loop(data_pointers, offset_to_count, *target);
		} else {
			VectorOperations::Gather::Set(addresses, *target);
		}
		VectorOperations::Add(addresses, GetTypeIdSize(target->type),
		                      addresses);
		current_bytes += GetTypeIdSize(target->type);
	}
	for (size_t i = 0; i < aggregate_types.size(); i++) {
		// now we can fetch the counts
		if (aggregate_types[i] == ExpressionType::AGGREGATE_COUNT_STAR) {
			auto target = result.data[i].get();
			target->count = entry;
			VectorOperations::Gather::Set(addresses, *target);
		}
	}
	groups.count = entry;
	result.count = entry;
	scan_position = ptr - start;
}

//===--------------------------------------------------------------------===//
// Compute the average
//===--------------------------------------------------------------------===//
template <class T>
static void _gather_average_templated_loop(T **source, size_t offset,
                                           Vector &result) {
	T *ldata = (T *)result.data;
	for (size_t i = 0; i < result.count; i++) {
		uint64_t count = ((uint64_t *)((uint64_t)source[i] + offset))[0];
		ldata[i] = source[i][0] / count;
	}
}

static void _average_gather_loop(void **source, size_t offset, Vector &dest) {
	switch (dest.type) {
	case TypeId::TINYINT:
		_gather_average_templated_loop<int8_t>((int8_t **)source, offset, dest);
		break;
	case TypeId::SMALLINT:
		_gather_average_templated_loop<int16_t>((int16_t **)source, offset,
		                                        dest);
		break;
	case TypeId::INTEGER:
		_gather_average_templated_loop<int32_t>((int32_t **)source, offset,
		                                        dest);
		break;
	case TypeId::BIGINT:
		_gather_average_templated_loop<int64_t>((int64_t **)source, offset,
		                                        dest);
		break;
	case TypeId::DECIMAL:
		_gather_average_templated_loop<double>((double **)source, offset, dest);
		break;
	case TypeId::POINTER:
		_gather_average_templated_loop<uint64_t>((uint64_t **)source, offset,
		                                         dest);
		break;
	case TypeId::DATE:
		_gather_average_templated_loop<date_t>((date_t **)source, offset, dest);
		break;
	default:
		throw NotImplementedException("Unimplemented type for gather");
	}
}
