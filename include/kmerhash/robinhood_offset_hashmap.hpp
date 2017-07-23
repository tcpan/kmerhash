/*
 * Copyright 2017 Georgia Institute of Technology
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * robinhood_offset_hashmap.hpp
 *
 * current testing version.  has the following featuers
 * 1. batch mode interface
 * 2. prefetching for batch modes
 * 3. using offsets
 * 4. separate key and value arrays did not work - prob more mem usage...
 *
 *
 *  Created on: July 13, 2017
 *      Author: tpan
 *
 *  for robin hood hashing
 */

#ifndef KMERHASH_ROBINHOOD_OFFSET_HASHMAP_HPP_
#define KMERHASH_ROBINHOOD_OFFSET_HASHMAP_HPP_

#include <vector>   // for vector.
#include <array>
#include <type_traits> // for is_constructible
#include <iterator>  // for iterator traits
#include <algorithm> // for transform

#include "kmerhash/aux_filter_iterator.hpp"   // for join iteration of 2 iterators and filtering based on 1, while returning the other.
#include "kmerhash/math_utils.hpp"
#include "mmintrin.h"  // emm: _mm_stream_si64
#include "containers/fsc_container_utils.hpp"

#include <stdlib.h> // posix_memalign
#include <stdexcept>

#include "kmerhash/hyperloglog64.hpp"  // for size estimation.

// should be easier for prefetching
#define LOOK_AHEAD 16

namespace fsc {
/// when inserting, does NOT replace existing.
struct DiscardReducer {
	template <typename T>
	inline T operator()(T const & x, T const & y) {
		return x;
	}
};
/// when inserting, REPALCES the existing
struct ReplaceReducer {
	template <typename T>
	inline T operator()(T const & x, T const & y) {
		return y;
	}
};
/// other reducer types include plus, max, etc.


/**
 * @brief Open Addressing hashmap that uses Robin Hood hashing, with doubling for resizing, circular internal array.  modified from standard robin hood hashmap to use bucket offsets, in attempt to improve speed.
 * @details  at the moment, nothing special for k-mers yet.
 * 		This class has the following implementation characteristics
 * 			vector of structs
 * 			open addressing with robin hood hashing.
 * 			doubling for reallocation
 * 			circular internal array
 *
 *  like standard robin hood hashmap implementation in "hashmap_robinhood_doubling.hpp", we use an auxillary array.  however, the array is interpreted differently.
 *  whereas standard imple stores the distance of the current array position from the bucket that the key is "hashed to",
 *  the "offsets" implementation stores at each position the starting offset of entries for the current bucket, and the empty/occupied bit indicates
 *  whether the current bucket has content or not (to distinguish between an empty bucket from an occupied bucket with offset 0.).
 *
 *  this is possible as robin hood hashing places entries for a bucket consecutively
 *
 *  benefits:
 *  1. no need to scan in order to find start of a bucket.
 *  2. end of a bucket is determined by the next bucket's offset.
 *
 *  Auxillary array interpretation:
 *
 *  where as RH standard aux array element represents the distance from source for the corresponding element in container.
 *    idx   ----a---b------------
 *    aux   ===|=|=|=|=|4|3|=====   _4_ is recorded at position hash(X) + _4_ of aux array.
 *    data  -----------|X|Y|-----,  X is inserted into bucket hash(X) = a.  in container position hash(X) + _4_.
 *
 *    empty positions are set to 0x80, and same for all info entries.
 *
 *  this aux array instead has each element represent the offset for the first entry of this bucket.
 *    idx   ----a---b------------
 *    aux   ===|4|=|3|=|=|=|=====   _4_ is recorded at position hash(X) of aux array.
 *    data  -----------|X|Y|-----,  X is inserted into bucket hash(X), in container position hash(X) + _4_.
 *
 *    empty positions are set with high bit 1, but can has distance larger than 0.
 *
 *  in standard, we linear scan info_container from position hash(Y) and must go to hash(Y) + 4 linearly.
 *    each aux entry is essentailyl independent of others.
 *    have to check and compare each data entry.
 *
 *  in this class, we just need to look at position hash(Y) and hash(Y) + 1 to know where to start and end in the container
 *    for find/count/update, those 2 are the only ones we need.
 *    for insert, we need to find the end of the range to modify, even after the insertion point.
 *      first search for end of range from current bucket (linear scan)
 *      then move from insertion point to end of range to right by 1.
 *      then insert at insertion point
 *      finally update the aux array from hash(Y) +1, to end of bucket range (ends on empty or dist = 0), by adding 1...
 *    for deletion, we need to again find the end of the range to modify, from the point of the deletion
 *      search for end of range from current bucket
 *      then move from deletion point to end of range to left by 1
 *      finally update the aux array from hash(Y) +1 to end of bucket range(ends on dist == 0), by subtracting 1...
 *    for rehash, from the first non-empty, to the next empty
 *      call insert on the entire range.
 *
 *  need AT LEAST 1 extra element, since copy_downsize and copy_upsize rely on those.
 *
 *  TODO:
 *  [ ] batch mode operations to allow more opportunities for optimization including SIMD
 *  [ ] predicated version of operations
 *  [ ] macros for repeated code.
 *  [x] use bucket offsets instead of distance to bucket.
 *  [ ] faster insertion in batch
 *  [ ] faster find?
 *  [x] estimate distinct element counts in input.
 *
 *  [ ] verify that iterator returned has correct data offset and info offset (which are different)
 *
 *  [x] insert_with_hint (iterator version) needs to increase the size when lsize is greater than max load
 *  [x] insert_with_hint needs to increase size when any has 127 for offset.
 *  [ ] insert_integrated needs to increase size when any has 127 for offset. (worth doing?)
 *  [x] downsize needs to prevent offset of 127.
 *
 */
template <typename Key, typename T, typename Hash = ::std::hash<Key>,
		typename Equal = ::std::equal_to<Key>,
		typename Allocator = ::std::allocator<std::pair<const Key, T> >,
		typename Reducer = ::fsc::DiscardReducer
		>
class hashmap_robinhood_offsets_reduction {

public:

	using key_type              = Key;
	using mapped_type           = T;
	using value_type            = ::std::pair<Key, T>;
	using hasher                = Hash;
	using key_equal             = Equal;
	using reducer               = Reducer;

protected:

	//=========  start INFO_TYPE definitions.
	// MSB is to indicate if current BUCKET is empty.  rest 7 bits indicate offset for the first BUCKET entry if not empty, or where it would have been.
	// i.e. if empty, flipping the bit should indicate offset position of where the bucket entry goes.
	// relationship to prev and next:
	//    empty, pos == 0: prev: 0 & (empty or full);    1 & empty.       next: 0 & (empty or full)
	//    empty, pos >  0: prev: ( 0<= p <= pos) & full; (pos+1) & empty  next: (pos-1) & (empty or full)
	//	  full,  pos == 0: prev: 0 & (empty or full);    1 & empty.       next: (p >= 0) & (empty or full)
	//	  full,  pos >  0: prev: ( 0<= p <= pos) & full; (pos+1) & empty  next: (p >= pos) & (empty or full)
	// container has a valid entry for each of last 3.
	using info_type = uint8_t;

	static constexpr info_type info_empty = 0x80;
	static constexpr info_type info_mask = 0x7F;
	static constexpr info_type info_normal = 0x00;   // this is used to initialize the reprobe distances.

	inline bool is_empty(info_type const & x) const {
		return x >= info_empty;  // empty 0x80
	}
	inline bool is_normal(info_type const & x) const {
		return x < info_empty;  // normal. both top bits are set. 0xC0
	}
	inline void set_empty(info_type & x) {
		x |= info_empty;  // nothing here.
	}
	inline void set_normal(info_type & x) {
		x &= info_mask;  // nothing here.
	}
	inline info_type get_offset(info_type const & x) const {
		return x & info_mask;  // nothing here.
	}
	// make above explicit by preventing automatic type conversion.
	template <typename TT> inline bool is_empty(TT const & x) const  = delete;
	template <typename TT> inline bool is_normal(TT const & x) const = delete;
	template <typename TT> inline void set_empty(TT & x) = delete;
	template <typename TT> inline void set_normal(TT & x) = delete;
	template <typename TT> inline info_type get_offset(TT const & x) const = delete;

	//=========  end INFO_TYPE definitions.
	// filter
	struct valid_entry_filter {
		inline bool operator()(info_type const & x) {   // a container entry is empty only if the corresponding info is empty (0x80), not just have empty flag set.
			return x != info_empty;   // (curr bucket is empty and position is also empty.  otherwise curr bucket is here or prev bucket is occupying this)
		};
	};



	using container_type		= ::std::vector<value_type, Allocator>;
	using info_container_type	= ::std::vector<info_type, Allocator>;
	hyperloglog64<key_type, hasher, 12> hll;  // precision of 6bits  uses 4096 bytes, which should fit in L1 cache.  error rate : 1.03/(2^12)


public:

	using allocator_type        = typename container_type::allocator_type;
	using reference 			= typename container_type::reference;
	using const_reference	    = typename container_type::const_reference;
	using pointer				= typename container_type::pointer;
	using const_pointer		    = typename container_type::const_pointer;
	using iterator              = ::bliss::iterator::aux_filter_iterator<typename container_type::iterator, typename info_container_type::iterator, valid_entry_filter>;
	using const_iterator        = ::bliss::iterator::aux_filter_iterator<typename container_type::const_iterator, typename info_container_type::const_iterator, valid_entry_filter>;
	using size_type             = typename container_type::size_type;
	using difference_type       = typename container_type::difference_type;


protected:


	//=========  start BUCKET_ID_TYPE definitions.
	// find_pos returns 2 pieces of information:  assigned bucket, and the actual position.  actual position is set to all bits on if not found.
	// so 3 pieces of information needs to be incorporated.
	//   note that pos - bucket = offset, where offset just needs 1 bytes.
	//   we can try to limit bucket id or pos to only 56 bits, to avoid constructing a pair.
	// 56 bits gives 64*10^15, or 64 peta entries (local entries).  should be enough for now...
	// majority - looking for position, not bucket, but sometimes do need bucket pos.
	// also need to know if NOT FOUND - 1. bucket is empty, 2. bucket does not contain this entry.
	//    if bucket empty, then the offset has the empty flag on.  pos should bucket + offset.
	//    if bucket does not contain entry, then pos should be start pos of next bucket.  how to indicate nothing found? use the MSB of the 56 bits.
	// if found, then return pos, and offset of original bucket (for convenience).
	// NOTE that the pos part may not point to the first entry of the bucket.

	// failed insert (pos_flag set) means the same as successful find.

	using bucket_id_type = size_t;
	//    static constexpr bucket_id_type bid_pos_mask = ~(static_cast<bucket_id_type>(0)) >> 9;   // lower 55 bits set.
	//    static constexpr bucket_id_type bid_pos_exists = 1UL << 55;  // 56th bit set.
	static constexpr bucket_id_type bid_pos_mask = ~(static_cast<bucket_id_type>(0)) >> 1;   // lower 63 bits set.
	static constexpr bucket_id_type bid_pos_exists = 1ULL << 63;  // 64th bit set.
	// failed is speial, correspond to all bits set (max distnace failed).  not using 0x800000... because that indicates failed inserting due to occupied.
	static constexpr bucket_id_type insert_failed = bid_pos_mask;  // unrealistic value that also indicates it's missing.
	static constexpr bucket_id_type find_failed = bid_pos_mask;  // unrealistic value that also indicates it's missing.
	//    static constexpr bucket_id_type bid_info_mask = static_cast<bucket_id_type>(info_mask) << 56;   // lower 55 bits set.
	//    static constexpr bucket_id_type bid_info_empty = static_cast<bucket_id_type>(info_empty) << 56;  // 56th bit set.


	inline bucket_id_type make_missing_bucket_id(size_t const & pos) const { //, info_type const & info) const {
		//      assert(pos <= bid_pos_mask);
		//      return (static_cast<bucket_id_type>(info) << 56) | pos;
		assert(pos < bid_pos_exists);
		return static_cast<bucket_id_type>(pos);
	}
	inline bucket_id_type make_existing_bucket_id(size_t & pos) const { //, info_type const & info) const {
		//      return make_missing_bucket_id(pos, info) | bid_pos_exists;
		return static_cast<bucket_id_type>(pos) | bid_pos_exists;
		//      reinterpret_cast<uint32_t*>(&pos)[1] |= 0x80000000U;
		//      return static_cast<bucket_id_type>(pos);
	}

	// NOT USED
	//    inline bool is_empty(bucket_id_type const & x) const {
	//      return x >= bid_info_empty;  // empty 0x80....
	//    }
	// NOT USED
	//    inline bool is_normal(bucket_id_type const & x) const {
	//      return x < bid_info_empty;  // normal. both top bits are set. 0xC0
	//    }
	inline bool present(bucket_id_type const & x) const {
		//return (x & bid_pos_exists) > 0;
		return x > bid_pos_mask;
	}
	inline bool missing(bucket_id_type const & x) const {
		// return (x & bid_pos_exists) == 0;
		return x < bid_pos_exists;
	}
	// NOT USED
	//    inline info_type get_info(bucket_id_type const & x) const {
	//      return static_cast<info_type>(x >> 56);
	//    }

	inline size_t get_pos(bucket_id_type const & x) const {
		return x & bid_pos_mask;
	}
	// NOT USED
	//    inline size_t get_offset(bucket_id_type const & x) const {
	//      return (x & bid_info_mask) >> 56;
	//    }

	// make above explicit by preventing automatic type conversion.
	//    template <typename SS, typename TT>
	//    inline bucket_id_type make_missing_bucket_id(SS const & pos /*, TT const & info */) const  = delete;
	//    template <typename SS, typename TT>
	//    inline bucket_id_type make_existing_bucket_id(SS const & pos /*, TT const & info */) const  = delete;
	template <typename TT> inline bool present(TT const & x) const = delete;
	template <typename TT> inline bool missing(TT const & x) const = delete;
	// NOT USED.	template <typename TT> inline info_type get_info(TT const & x) const = delete;
	template <typename TT> inline size_t get_pos(TT const & x) const = delete;
	// NOT USED.	template <typename TT> inline size_t get_offset(TT const & x) const = delete;


	//=========  end BUCKET_ID_TYPE definitions.


	// =========  prefetch constants.
	static constexpr uint32_t info_per_cacheline = 64 / sizeof(info_type);
	static constexpr uint32_t value_per_cacheline = 64 / sizeof(value_type);
	static constexpr uint32_t info_prefetch_iters = (LOOK_AHEAD + info_per_cacheline - 1) / info_per_cacheline;
	static constexpr uint32_t value_prefetch_iters = (LOOK_AHEAD + value_per_cacheline - 1) / value_per_cacheline;
	// =========  END prefetech constants.


	size_t lsize;
	mutable size_t buckets;
	mutable size_t mask;
	mutable size_t min_load;
	mutable size_t max_load;
	mutable double min_load_factor;
	mutable double max_load_factor;

#if defined(REPROBE_STAT)
	// some stats.
	mutable size_type upsize_count;
	mutable size_type downsize_count;
	mutable size_type reprobes;   // for use as temp variable
	mutable info_type max_reprobes;
	mutable size_type moves;
	mutable size_type max_moves;
	mutable size_type shifts;
	mutable size_type max_shifts;
#endif


	valid_entry_filter filter;
	hasher hash;
	key_equal eq;
	reducer reduc;

	container_type container;
	info_container_type info_container;

public:

	/**
	 * _capacity is the number of usable entries, not the capacity of the underlying container.
	 */
	explicit hashmap_robinhood_offsets_reduction(size_t const & _capacity = 128,
			double const & _min_load_factor = 0.4,
			double const & _max_load_factor = 0.9) :
			lsize(0), buckets(next_power_of_2(_capacity)), mask(buckets - 1),
#if defined (REPROBE_STAT)
			upsize_count(0), downsize_count(0),
#endif
			container(buckets + info_empty), info_container(buckets + info_empty, info_empty)
	{
		// set the min load and max load thresholds.  there should be a good separation so that when resizing, we don't encounter a resize immediately.
		set_min_load_factor(_min_load_factor);
		set_max_load_factor(_max_load_factor);
	};

	/**
	 * initialize and insert, allocate about 1/4 of input, and resize at the end to bound the usage.
	 */
	template <typename Iter, typename = typename std::enable_if<
			::std::is_constructible<value_type, typename ::std::iterator_traits<Iter>::value_type>::value  ,int>::type >
	hashmap_robinhood_offsets_reduction(Iter begin, Iter end,
			double const & _min_load_factor = 0.4,
			double const & _max_load_factor = 0.9) :
			hashmap_robinhood_offsets_reduction(::std::distance(begin, end) / 4, _min_load_factor, _max_load_factor) {

		insert(begin, end);
	}

	~hashmap_robinhood_offsets_reduction() {
#if defined(REPROBE_STAT)
		::std::cout << "SUMMARY:" << std::endl;
		::std::cout << "  upsize\t= " << upsize_count << std::endl;
		::std::cout << "  downsize\t= " << downsize_count << std::endl;
#endif
	}


	hashmap_robinhood_offsets_reduction(hashmap_robinhood_offsets_reduction const & other) :
		hll(other.hll),
		lsize(other.lsize),
		buckets(other.buckets),
		mask(other.mask),
		min_load(other.min_load),
		max_load(other.max_load),
		min_load_factor(other.min_load_factor),
		max_load_factor(other.max_load_factor),
#if defined(REPROBE_STAT)
		// some stats.
		upsize_count(other.upsize_count),
		downsize_count(other.downsize_count),
		reprobes(other.reprobes),
		max_reprobes(other.max_reprobes),
		moves(other.moves),
		max_moves(other.max_moves),
		shifts(other.shifts),
		max_shifts(other.max_shifts),
#endif
		filter(other.filter),
		hash(other.hash),
		eq(other.eq),
		reduc(other.reduc),
		container(other.container),
		info_container(other.info_container) {};

	hashmap_robinhood_offsets_reduction & operator=(hashmap_robinhood_offsets_reduction const & other) {
		hll = other.hll;
		lsize = other.lsize;
		buckets = other.buckets;
		mask = other.mask;
		min_load = other.min_load;
		max_load = other.max_load;
		min_load_factor = other.min_load_factor;
		max_load_factor = other.max_load_factor;
#if defined(REPROBE_STAT)
		// some stats.
		upsize_count = other.upsize_count;
		downsize_count = other.downsize_count;
		reprobes = other.reprobes;
		max_reprobes = other.max_reprobes;
		moves = other.moves;
		max_moves = other.max_moves;
		shifts = other.shifts;
		max_shifts = other.max_shifts;
#endif
		filter = other.filter;
		hash = other.hash;
		eq = other.eq;
		reduc = other.reduc;
		container = other.container;
		info_container = other.info_container;
	}

	hashmap_robinhood_offsets_reduction(hashmap_robinhood_offsets_reduction && other) :
		hll(std::move(other.hll)),
		lsize(std::move(other.lsize)),
		buckets(std::move(other.buckets)),
		mask(std::move(other.mask)),
		min_load(std::move(other.min_load)),
		max_load(std::move(other.max_load)),
		min_load_factor(std::move(other.min_load_factor)),
		max_load_factor(std::move(other.max_load_factor)),
#if defined(REPROBE_STAT)
		// some stats.
		upsize_count(std::move(other.upsize_count)),
		downsize_count(std::move(other.downsize_count)),
		reprobes(std::move(other.reprobes)),
		max_reprobes(std::move(other.max_reprobes)),
		moves(std::move(other.moves)),
		max_moves(std::move(other.max_moves)),
		shifts(std::move(other.shifts)),
		max_shifts(std::move(other.max_shifts)),
#endif
		filter(std::move(other.filter)),
		hash(std::move(other.hash)),
		eq(std::move(other.eq)),
		reduc(std::move(other.reduc)),
		container(std::move(other.container)),
		info_container(std::move(other.info_container)) {}

	hashmap_robinhood_offsets_reduction & operator=(hashmap_robinhood_offsets_reduction && other) {
		hll = std::move(other.hll);
		lsize = std::move(other.lsize);
		buckets = std::move(other.buckets);
		mask = std::move(other.mask);
		min_load = std::move(other.min_load);
		max_load = std::move(other.max_load);
		min_load_factor = std::move(other.min_load_factor);
		max_load_factor = std::move(other.max_load_factor);
#if defined(REPROBE_STAT)
		// some stats.
		upsize_count = std::move(other.upsize_count);
		downsize_count = std::move(other.downsize_count);
		reprobes = std::move(other.reprobes);
		max_reprobes = std::move(other.max_reprobes);
		moves = std::move(other.moves);
		max_moves = std::move(other.max_moves);
		shifts = std::move(other.shifts);
		max_shifts = std::move(other.max_shifts);
#endif
		filter = std::move(other.filter);
		hash = std::move(other.hash);
		eq = std::move(other.eq);
		reduc = std::move(other.reduc);
		container = std::move(other.container);
		info_container = std::move(other.info_container);
	}

	void swap(hashmap_robinhood_offsets_reduction && other) {
		std::swap(hll, std::move(other.hll));
		std::swap(lsize, std::move(other.lsize));
		std::swap(buckets, std::move(other.buckets));
		std::swap(mask, std::move(other.mask));
		std::swap(min_load, std::move(other.min_load));
		std::swap(max_load, std::move(other.max_load));
		std::swap(min_load_factor, std::move(other.min_load_factor));
		std::swap(max_load_factor, std::move(other.max_load_factor));
#if defined(REPROBE_STAT)
		// some stats.
		std::swap(upsize_count, std::move(other.upsize_count));
		std::swap(downsize_count, std::move(other.downsize_count));
		std::swap(reprobes, std::move(other.reprobes));
		std::swap(max_reprobes, std::move(other.max_reprobes));
		std::swap(moves, std::move(other.moves));
		std::swap(max_moves, std::move(other.max_moves));
		std::swap(shifts, std::move(other.shifts));
		std::swap(max_shifts, std::move(other.max_shifts));
#endif
		std::swap(filter, std::move(other.filter));
		std::swap(hash, std::move(other.hash));
		std::swap(eq, std::move(other.eq));
		std::swap(reduc, std::move(other.reduc));
		std::swap(container, std::move(other.container));
		std::swap(info_container, std::move(other.info_container));
	}


	/**
	 * @brief set the load factors.
	 */
	inline void set_min_load_factor(double const & _min_load_factor) {
		min_load_factor = _min_load_factor;
		min_load = static_cast<size_t>(static_cast<double>(buckets) * min_load_factor);

	}

	inline void set_max_load_factor(double const & _max_load_factor) {
		max_load_factor = _max_load_factor;
		max_load = static_cast<size_t>(static_cast<double>(buckets) * max_load_factor);
	}

	/**
	 * @brief get the load factors.
	 */
	inline double get_load_factor() {
		return static_cast<double>(lsize) / static_cast<double>(buckets);
	}

	inline double get_min_load_factor() {
		return min_load_factor;
	}

	inline double get_max_load_factor() {
		return max_load_factor;
	}

	size_t capacity() {
		return buckets;
	}


	/**
	 * @brief iterators
	 */
	iterator begin() {
		return iterator(container.begin(), info_container.begin(), info_container.end(), filter);
	}

	iterator end() {
		return iterator(container.end(), info_container.end(), filter);
	}

	const_iterator cbegin() const {
		return const_iterator(container.cbegin(), info_container.cbegin(), info_container.cend(), filter);
	}

	const_iterator cend() const {
		return const_iterator(container.cend(), info_container.cend(), filter);
	}



	void print() const {
		std::cout << "lsize " << lsize << "\tbuckets " << buckets << "\tmax load factor " << max_load_factor << std::endl;
		size_type i = 0, j = 0;

		container_type tmp;
		size_t offset = 0;
		for (; i < buckets; ++i) {
			std::cout << "buc: " << std::setw(10) << i <<
					", inf: " << std::setw(3) << static_cast<size_t>(info_container[i]) <<
					", off: " << std::setw(3) << static_cast<size_t>(get_offset(info_container[i])) <<
					", pos: " << std::setw(10) << (i + get_offset(info_container[i])) <<
					", cnt: " << std::setw(3) << (is_empty(info_container[i]) ? 0UL : (get_offset(info_container[i+1]) - get_offset(info_container[i]) + 1)) <<
					std::endl;


			if (! is_empty(info_container[i])) {
				offset = i + get_offset(info_container[i]);
				tmp.clear();
				tmp.insert(tmp.end(), container.begin() + offset,
						container.begin() + i + 1 + get_offset(info_container[i + 1]));
				std::sort(tmp.begin(), tmp.end(), [](typename container_type::value_type const & x,
						typename container_type::value_type const & y){
					return x.first < y.first;
				});
				for (j = 0; j < tmp.size(); ++j) {
					std::cout << std::setw(72) << (offset + j) <<
							", hash: " << std::setw(16) << std::hex << (hash(container[i].first) & mask) << std::dec <<
							", key: " << std::setw(22) << tmp[j].first <<
							", val: " << std::setw(22) << tmp[j].second <<
							std::endl;
				}
			}
		}

		for (i = buckets; i < info_container.size(); ++i) {
			std::cout << "PAD: " << std::setw(10) << i <<
					", inf: " << std::setw(3) << static_cast<size_t>(info_container[i]) <<
					", off: " << std::setw(3) << static_cast<size_t>(get_offset(info_container[i])) <<
					", pos: " << std::setw(10) << (i + get_offset(info_container[i])) <<
					", cnt: " << std::setw(3) << (is_empty(info_container[i]) ? 0UL : (get_offset(info_container[i+1]) - get_offset(info_container[i]) + 1)) <<
					"\n" << std::setw(72) << i <<
					", hash: " << std::setw(16) << std::hex << (hash(container[i].first) & mask) << std::dec <<
					", key: " << container[i].first <<
					", val: " << container[i].second <<
					std::endl;
		}
	}

	void print_raw() const {
		std::cout << "lsize " << lsize << "\tbuckets " << buckets << "\tmax load factor " << max_load_factor << std::endl;
		size_type i = 0;

		for (i = 0; i < buckets; ++i) {
			std::cout << "buc: " << std::setw(10) << i <<
					", inf: " << std::setw(3) << static_cast<size_t>(info_container[i]) <<
					", off: " << std::setw(3) << static_cast<size_t>(get_offset(info_container[i])) <<
					", pos: " << std::setw(10) << (i + get_offset(info_container[i])) <<
					", cnt: " << std::setw(3) << (is_empty(info_container[i]) ? 0UL : (get_offset(info_container[i+1]) - get_offset(info_container[i]) + 1)) <<
					"\n" << std::setw(72) << i <<
					", hash: " << std::setw(16) << std::hex << (hash(container[i].first) & mask) << std::dec <<
					", key: " << container[i].first <<
					", val: " << container[i].second <<
					std::endl;
		}

		for (i = buckets; i < info_container.size(); ++i) {
			std::cout << "PAD: " << std::setw(10) << i <<
					", inf: " << std::setw(3) << static_cast<size_t>(info_container[i]) <<
					", off: " << std::setw(3) << static_cast<size_t>(get_offset(info_container[i])) <<
					", pos: " << std::setw(10) << (i + get_offset(info_container[i])) <<
					", cnt: " << std::setw(3) << (is_empty(info_container[i]) ? 0UL : (get_offset(info_container[i+1]) - get_offset(info_container[i]) + 1)) <<
					"\n" << std::setw(72) << i <<
					", hash: " << std::setw(16) << std::hex << (hash(container[i].first) & mask) << std::dec <<
					", key: " << container[i].first <<
					", val: " << container[i].second <<
					std::endl;
		}
	}

	void print_raw(size_t const & first, size_t const &last, std::string prefix) const {
		std::cout << prefix <<
				" lsize " << lsize <<
				"\tbuckets " << buckets <<
				"\tmax load factor " << max_load_factor <<
				"\t printing [" << first << " .. " << last << "]" << std::endl;
		size_type i = 0;

		for (i = first; i <= last; ++i) {
			std::cout << prefix <<
					" buc: " << std::setw(10) << i <<
					", inf: " << std::setw(3) << static_cast<size_t>(info_container[i]) <<
					", off: " << std::setw(3) << static_cast<size_t>(get_offset(info_container[i])) <<
					", pos: " << std::setw(10) << (i + get_offset(info_container[i])) <<
					", cnt: " << std::setw(3) << (is_empty(info_container[i]) ? 0UL : (get_offset(info_container[i+1]) - get_offset(info_container[i]) + 1)) <<
					"\n" << std::setw(72) << i <<
					", hash: " << std::setw(16) << std::hex << (hash(container[i].first) & mask) << std::dec <<
					", key: " << container[i].first <<
					", val: " << container[i].second <<
					std::endl;
		}
	}

	void print(size_t const & first, size_t const &last, std::string prefix) const {
		std::cout << prefix <<
				" lsize " << lsize <<
				"\tbuckets " << buckets <<
				"\tmax load factor " << max_load_factor <<
				"\t printing [" << first << " .. " << last << "]" << std::endl;
		size_type i = 0, j = 0;

		container_type tmp;
		size_t offset = 0;
		for (i = first; i <= last; ++i) {
			std::cout << prefix <<
					" buc: " << std::setw(10) << i <<
					", inf: " << std::setw(3) << static_cast<size_t>(info_container[i]) <<
					", off: " << std::setw(3) << static_cast<size_t>(get_offset(info_container[i])) <<
					", pos: " << std::setw(10) << (i + get_offset(info_container[i])) <<
					", cnt: " << std::setw(3) << (is_empty(info_container[i]) ? 0UL : (get_offset(info_container[i+1]) - get_offset(info_container[i]) + 1)) <<
					std::endl;


			if (! is_empty(info_container[i])) {
				offset = i + get_offset(info_container[i]);
				tmp.clear();
				tmp.insert(tmp.end(), container.begin() + offset,
						container.begin() + i + 1 + get_offset(info_container[i + 1]));
				std::sort(tmp.begin(), tmp.end(), [](typename container_type::value_type const & x,
						typename container_type::value_type const & y){
					return x.first < y.first;
				});
				for (j = 0; j < tmp.size(); ++j) {
					std::cout << prefix <<
							" " << std::setw(72) << (offset + j) <<
							", hash: " << std::setw(16) << std::hex << (hash(container[i].first) & mask) << std::dec <<
							", key: " << std::setw(22) << tmp[j].first <<
							", val: " << std::setw(22) << tmp[j].second <<
							std::endl;
				}
			}
		}
	}

	std::vector<std::pair<key_type, mapped_type> > to_vector() const {
		std::vector<std::pair<key_type, mapped_type> > output(lsize);

		auto it = std::copy(this->cbegin(), this->cend(), output.begin());
		output.erase(it, output.end());

		return output;
	}

	std::vector<key_type > keys() const {
		std::vector<key_type > output(lsize);

		auto it = std::transform(this->cbegin(), this->cend(), output.begin(),
				[](value_type const & x){ return x.first; });
		output.erase(it, output.end());

		return output;
	}


	size_t size() const {
		return this->lsize;
	}

	/**
	 * @brief  mark all entries as clear.
	 */
	void clear() {
		this->lsize = 0;
		std::fill(this->info_container.begin(), this->info_container.end(), info_empty);
	}

	/**
	 * @brief reserve space for specified entries.
	 */
	void reserve(size_type n) {
		//    if (n > this->max_load) {   // if requested more than current max load, then we need to resize up.
		rehash(static_cast<size_t>(static_cast<double>(n) / this->max_load_factor));
		// rehash to the new size.    current bucket count should be less than next_power_of_2(n).
		//    }  // do not resize down.  do so only when erase.
	}

	/**
	 * @brief reserve space for specified buckets.
	 * @details note that buckets > entries.
	 */
	void rehash(size_type const & b) {

		// check it's power of 2
		size_type n = next_power_of_2(b);

#if defined(REPROBE_STAT)
		std::cout << "REHASH current " << buckets << " b " << b << " n " << n << " lsize " << lsize << std::endl;
#endif

		//    print();

		if ((n != buckets) && (lsize < static_cast<size_type>(max_load_factor * static_cast<double>(n)))) {  // don't resize if lsize is larger than the new max load.

			if ((lsize > 0) && (n < buckets)) {  // down sizing. check if we overflow info
				while (this->copy_downsize_max_offset(n) > 127)  // if downsizing creates offset > 127, then increase n and check again.
					n <<= 1;
			}

			// if after checking we cannot downsize, then we stop and return.
			if ( n == buckets )  return;

			// this MAY cause infocontainer to be evicted from cache...
			container_type tmp(n + std::numeric_limits<info_type>::max() + 1);
			info_container_type tmp_info(n + std::numeric_limits<info_type>::max() + 1, info_empty);

			if (lsize > 0) {
				if (n > buckets) {
					this->copy_upsize(tmp, tmp_info, n);
#if defined(REPROBE_STAT)
					++upsize_count;
#endif
				} else {
					this->copy_downsize(tmp, tmp_info, n);
#if defined(REPROBE_STAT)
					++downsize_count;
#endif
				}
			}

			// new size and mask
			buckets = n;
			mask = n - 1;

			min_load = static_cast<size_type>(static_cast<double>(n) * min_load_factor);
			max_load = static_cast<size_type>(static_cast<double>(n) * max_load_factor);

			// swap in.
			container.swap(tmp);
			info_container.swap(tmp_info);
		}
	}


protected:
	// checks and makes sure that we don't have offsets greater than 127.
	// return max_offset have to just try and see, no prediction right now.
	size_t copy_downsize_max_offset(size_type const & target_buckets) {
		assert((target_buckets & (target_buckets - 1)) == 0);   // assert this is a power of 2.


		if (target_buckets > buckets) return 0;

		size_t id = 0, bid;

		//    std::cout << "RESIZE DOWN " << target_buckets << std::endl;

		size_t new_start = 0, new_end = 0;

		size_t blocks = buckets / target_buckets;

		size_t bl;

		// strategies:
		// 1. fill in target, then throw away if fails.  read 1x, write 1x normal, failure read 2x, write 2x
		// 2. fill in target_info first, then target.  read 2x (second time faster in L3?) write 1x normal.  write 1.5 x failure
		// 3. compute a max offset value.  read 2x (second time faster in L3?) write 1x normal or failure
		// choose 3.

		// calculate the max offset.
		size_t max_offset = 0;

		// iterate over all matching blocks.  fill one target bucket at a time and immediately fill the target info.
		for (bid = 0; bid < target_buckets; ++bid) {
			// starting offset is maximum of bid and prev offset.  (allows handling of empty but offset positions)
			new_start = std::max(bid, new_end);
			new_end = new_start;

			for (bl = 0; bl < blocks; ++bl) {
				id = bid + bl * target_buckets;  // id within each block.

				if (is_normal(info_container[id])) {
					// get the range
					new_end += (1 + get_offset(info_container[id + 1]) - get_offset(info_container[id]));
				}
			}

			// offset - current bucket id.
			max_offset = std::max(max_offset, new_start - bid);

			// early termination
			if (max_offset > 127) return max_offset;
		}

		//  std::cout << " info: " << (target_buckets - 1) << " info " << static_cast<size_t>(target_info[target_buckets - 1]) << " entry " << target[target_buckets - 1].first << std::endl;
		// adjust the target_info at the end, in the padding region.
		// new_end is end of all occupied entries.  target_bucket is the last bid.
		max_offset = std::max(max_offset, new_end - target_buckets);

		return max_offset;
	}


	void copy_downsize(container_type & target, info_container_type & target_info,
			size_type const & target_buckets) {
		assert((target_buckets & (target_buckets - 1)) == 0);   // assert this is a power of 2.

		size_t id = 0, bid;
		size_t pos;
		size_t endd;

		//    std::cout << "RESIZE DOWN " << target_buckets << std::endl;

		size_t new_start = 0, new_end = 0;

		size_t blocks = buckets / target_buckets;

		// iterate over all matching blocks.  fill one target bucket at a time and immediately fill the target info.
		size_t bl;
		for (bid = 0; bid < target_buckets; ++bid) {
			// starting offset is maximum of bid and prev offset.
			new_start = std::max(bid, new_end);
			new_end = new_start;

			for (bl = 0; bl < blocks; ++bl) {
				id = bid + bl * target_buckets;


				if (is_normal(info_container[id])) {
					// get the range
					pos = id + get_offset(info_container[id]);
					endd = id + 1 + get_offset(info_container[id + 1]);

					// copy the range.
					//        std::cout << id << " infos " << static_cast<size_t>(info_container[id]) << "," << static_cast<size_t>(info_container[id + 1]) << ", " <<
					//        		" copy from " << pos << " to " << new_end << " length " << (endd - pos) << std::endl;
					memmove(&(target[new_end]), &(container[pos]), sizeof(value_type) * (endd - pos));

					new_end += (endd - pos);

				}
			}

			// offset - current bucket id.
			target_info[bid] = ((new_end - new_start) == 0 ? info_empty : info_normal) + new_start - bid;
		}
		//  std::cout << " info: " << (target_buckets - 1) << " info " << static_cast<size_t>(target_info[target_buckets - 1]) << " entry " << target[target_buckets - 1].first << std::endl;
		// adjust the target_info at the end, in the padding region.
//		for (bid = target_buckets; bid < new_end; ++bid) {
//			new_start = std::max(bid, new_end);  // fixed new_end.  get new start.
//			// if last one is not empty, then first padding position is same distance with
//			target_info[bid] = info_empty + new_start - bid;
//			//    std::cout << " info: " << bid << " info " << static_cast<size_t>(target_info[bid]) << " entry " << target[bid].first << std::endl;
//		}
		for (bid = target_buckets; bid < new_end; ++bid) {
			// if last one is not empty, then first padding position is same distance with
			target_info[bid] = info_empty + new_end - bid;
		}

	}



	/**
	 * @brief inserts a range into the current hash table.
	 * @details
	 * essentially splitting the source into multiple non-overlapping ranges.
	 *    each partition is filled nearly sequentially, so figure out the scaling factor, and create an array as large as the scaling factor.
	 *
	 */
	void copy_upsize(container_type & target, info_container_type & target_info,
			size_type const & target_buckets) {
		size_type m = target_buckets - 1;
		assert((target_buckets & m) == 0);   // assert this is a power of 2.

		//    std::cout << "RESIZE UP " << target_buckets << std::endl;

		size_t id, bid, p;
		size_t pos;
		size_t endd;
		value_type v;

		size_t bl;
		size_t blocks = target_buckets / buckets;
		std::vector<size_t> offsets(blocks + 1, 0);
		std::vector<size_t> len(blocks, 0);

		// let's store the hash in order to avoid redoing hash.  This is needed only because we need to first count the number in a block,
		//  so that at block boundaries we have the right offsets.
		std::vector<size_t> hashes(lsize);
		size_t j = 0;
		// compute and store all hashes, and at the same time compute end of last bucket in each block.
		for (bid = 0; bid < buckets; ++bid) {
			if (is_normal(info_container[bid])) {

				pos = bid + get_offset(info_container[bid]);
				endd = bid + 1 + get_offset(info_container[bid + 1]);

				for (p = pos; p < endd; ++p, ++j) {
					// eval the target id.
					hashes[j] = hash(container[p].first);
					id = hashes[j] & m;

					// figure out which block it is in.
					bl = id / buckets;

					// count.  at least the bucket id + 1, or last insert target position + 1.
					offsets[bl + 1] = std::max(offsets[bl + 1], id) + 1;
				}
			}
		}

		//    for (bl = 0; bl <= blocks; ++bl) {
		//    	std::cout << "OFFSETS "  << offsets[bl] << std::endl;
		//    }

		// now that we have the right offsets,  start moving things.
		j = 0;
		size_t pp;
		for (bid = 0; bid < buckets; ++bid) {
			if (is_normal(info_container[bid])) {

				pos = bid + get_offset(info_container[bid]);
				endd = bid + 1 + get_offset(info_container[bid + 1]);

				std::fill(len.begin(), len.end(), 0);

				for (p = pos; p < endd; ++p, ++j) {
					// eval the target id.
					id = hashes[j] & m;

					// figure out which block it is in.
					bl = id / buckets;

					// now copy
					pp = std::max(offsets[bl], id);
					target[pp] = container[p];

					//    			std::cout << " moved from " << p << " to " << pp << " block " << bl << " with offset " << offsets[bl] << " len " << len[bl] << std::endl;

					// count.
					offsets[bl] = pp + 1;
					++len[bl];

				}

				// update all positive ones.
				for (bl = 0; bl < blocks; ++bl) {
					id = bid + bl * buckets;
					target_info[id] = (len[bl] == 0 ? info_empty : info_normal) + static_cast<info_type>(std::max(offsets[bl], id) - id - len[bl]);
					//    			std::cout << " updated info at " << id << " to " << static_cast<size_t>(target_info[id]) << ". block " << bl << " with offset " << offsets[bl] << " len " << len[bl] << std::endl;
				}
			} else {
				for (bl = 0; bl < blocks; ++bl) {
					id = bid + bl * buckets;
					target_info[id] = info_empty + static_cast<info_type>(std::max(offsets[bl], id) - id);
					//    			std::cout << " updated empty info at " << id << " to " << static_cast<size_t>(target_info[id]) << ". block " << bl << " with offset " << offsets[bl] << " len " << len[bl] << std::endl;
				}

			}
		}
		// clean up the last part.
		size_t new_start;
		for (bid = target_buckets; bid < offsets[blocks]; ++bid) {
			new_start = std::max(bid, offsets[blocks]);  // fixed new_end.  get new start.
			// if last one is not empty, then first padding position is same distance with
			target_info[bid] = info_empty + new_start - bid;
			//		std::cout << " info: " << bid << " info " << static_cast<size_t>(target_info[bid]) << " entry " << target[bid].first << std::endl;
		}

	}


	/**
	 * return the position in container where the current key is found.  if not found, max is returned.
	 */
	template <typename OutPredicate = ::bliss::filter::TruePredicate,
			typename InPredicate = ::bliss::filter::TruePredicate >
	bucket_id_type find_pos_with_hint(key_type const & k, size_t const & bid,
			OutPredicate const & out_pred = OutPredicate(),
			InPredicate const & in_pred = InPredicate() ) const {

		assert(bid < buckets);

		if (! std::is_same<InPredicate, ::bliss::filter::TruePredicate>::value)
			if (!in_pred(k)) return find_failed;

		info_type offset = info_container[bid];
		size_t start = bid + get_offset(offset);  // distance is at least 0, and definitely not empty

		// no need to check for empty?  if i is empty, then this one should be.
		// otherwise, we are checking distance so doesn't matter if empty.

		// first get the bucket id
		if (is_empty(offset) ) {
			// std::cout << "Empty entry at " << i << " val " << static_cast<size_t>(info_container[i]) << std::endl;
			//return make_missing_bucket_id(start, offset);
			return make_missing_bucket_id(start);
		}

		// get the next bucket id
		size_t end = bid + 1 + get_offset(info_container[bid + 1]);   // distance is at least 0, and can be empty.

#if defined(REPROBE_STAT)
		size_t reprobe = 0;
#endif
		// now we scan through the current.
		for (; start < end; ++start) {

			if (eq(k, container[start].first)) {
#if defined(REPROBE_STAT)
				this->reprobes += reprobe;
				this->max_reprobes = std::max(this->max_reprobes, static_cast<info_type>(reprobe));
#endif
				//				return make_existing_bucket_id(start, offset);
				if (!std::is_same<InPredicate, ::bliss::filter::TruePredicate>::value)
					if (!out_pred(container[start])) return find_failed;

				// else found one.
				return make_existing_bucket_id(start);
			}

#if defined(REPROBE_STAT)
			++reprobe;
#endif
		}

#if defined(REPROBE_STAT)
		this->reprobes += reprobe;
		this->max_reprobes = std::max(this->max_reprobes, static_cast<info_type>(reprobe));
#endif

		return make_missing_bucket_id(start);
		//		return make_missing_bucket_id(end, offset);
	}

	/**
	 * return the bucket id where the current key is found.  if not found, max is returned.
	 */
	template <typename OutPredicate = ::bliss::filter::TruePredicate,
			typename InPredicate = ::bliss::filter::TruePredicate >
	inline bucket_id_type find_pos(key_type const & k,
			OutPredicate const & out_pred = OutPredicate(),
			InPredicate const & in_pred = InPredicate() ) const {
		size_t i = hash(k) & mask;
		return find_pos_with_hint(k, i, out_pred, in_pred);
	}

	/**
	 * return the next position in "container" that is empty - i.e. info has 0x80.
	 *
	 * one property to use - we can jump the "distance" in the current info_container, there are guaranteed no
	 *   empty entries within that range.
	 *
	 *   remember that distances can be non-zero for empty BUCKETs.
	 *
	 *   container should not be completely full because of rehashing, so there should be no need to break search into 2 parts.
	 *
	 */
	inline size_t find_next_empty_pos(info_container_type const & target_info, size_t const & pos) const {
		size_t end = pos;
		for (; (end < target_info.size()) && (target_info[end] != info_empty); ) {
			// can skip ahead with target_info[end]
			end += std::max(get_offset(target_info[end]), static_cast<info_type>(1));  // move forward at least 1 (when info_normal)
		}

		return end;
	}

	/**
	 * return the next position in "container" that is pointing to self - i.e. offset == 0.
	 *
	 * one property to use - we can jump the "distance" in the current info_container, there are guaranteed no
	 *   empty entries within that range.
	 *
	 *   remember that distances can be non-zero for empty BUCKETs.
	 *
	 *   container should not be completely full because of rehashing, so there should be no need to break search into 2 parts.
	 *
	 */
	inline size_t find_next_zero_offset_pos(info_container_type const & target_info, size_t const & pos) const {
		info_type dist;
		size_t end = pos;
		for (; end < target_info.size(); ) {
			dist = get_offset(target_info[end]);
			if (dist == 0) return end;
			// can skip ahead with target_info[end]
			end += dist;
		}

		return end;
	}


	/**
	 * find next non-empty position.  including self.
	 */
	inline size_type find_next_non_empty_pos(info_container_type const & target_info, size_t const & pos) const {
		size_t end = pos;
		for (; (end < target_info.size()) && !is_normal(target_info[end]); ) {
			// can skip ahead with target_info[end]
			end += std::max(get_offset(target_info[end]), static_cast<info_type>(1));  // 1 is when info is info_empty
		}

		return end;
	}



	/**
	 * @brief insert a single key-value pair into container at the desired position
	 *
	 * note that swap only occurs at bucket boundaries.
	 *
	 * return insertion position, and update id to end of
			// insert if empty, or if reprobe distance is larger than current position's.  do this via swap.
			// continue to probe if we swapped out a normal entry.
			// this logic relies on choice of bits for empty entries.

			// we want the reprobe distance to be larger than empty, so we need to make
			// normal to have high bits 1 and empty 0.

			// save the insertion position (or equal position) for the first insert or match, but be aware that after the swap
			//  there will be other inserts or swaps. BUT NOT OTHER MATCH since this is a MAP.

		// 4 cases:
		// A. empty bucket, offset == 0        use this bucket. offset at original bucket converted to non-empty.  move vv in.  next bucket unchanged.  done
		// B. empty bucket, offset > 0         use this bucket. offset at original bucket converted to non-empty.  swap vv in.  go to next bucket
		// C. non empty bucket, offset == 0.   use next bucket. offset at original bucket not changed              swap vv in.  go to next bucket
		// D. non empty bucket, offset > 0.    use next bucket. offset at original bucket not changed.             swap vv in.  go to next bucket

//      // alternative for each insertion: vertical curr, horizontal next.
//      // operations.  Op X: move vv to curr pos
//      //				Op Y: swap vv with curr pos
//      //				Op Z: no swap
//      //				Op R: change curr info to non empty
//      //				Op S: increment next info by 1
//      //				Op T: increment next infoS by 1, up till a full entry, or info_empty.
//      //				Op Y2: move vv to next pos, change curr info to non empty, next infoS +1 until either full, or info_empty.  done
//      //				Op Z: swap vv to next pos, change curr info to non empty, next infos +1,  go to next bucket (using next next bucket).  repeat
//      //				Op NA: not possible and
//      // 		A		B		C		D
//      // A	X		NA		X 		NA
//      // B	Y		Y2		Z		Z
//      // C
//      // D
//      //
//      // need operation to shift empty bucket offsets.

		// alternative, scan forward in info_container for info_empty slots, as many as needed, (location where we can insert).  also PREFETCH here
		// then compact container back to front while updating the associated info_container
		//		(update is from start of next bucket to empty slot, shift all those by 1, each empty space encounter increases shift distance by 1.)
		//      increased shift distance then increases the number of swaps.  possibly memmove would be better.
		//      each swap is 2 reads and 2 writes.  whole cacheline access might be simpler when bucket is small, and swap may be better when shift amount is small.
		//		  use large buckets to break up the shift.  everything in between just use memmove (catch multiple small buckets with 1 memmove).
		//   treat as optimization.  initially, just memmove for all.
		// finally move in the new data.

		// return bucket_id_type with info_type of CURRENT info_type
	 */

	bucket_id_type insert_with_hint(container_type & target,
			info_container_type & target_info,
			size_t const & id,
			value_type const & v) {

		assert(id < buckets);

		// get the starting position
		info_type info = target_info[id];

		//    std::cout << "info " << static_cast<size_t>(info) << std::endl;
		set_normal(target_info[id]);   // if empty, change it.  if normal, same anyways.

		// if this is empty and no shift, then insert and be done.
		if (info == info_empty) {
			target[id] = v;
			return make_missing_bucket_id(id);
			//      return make_missing_bucket_id(id, target_info[id]);
		}

		// the bucket is either non-empty, or empty but offset by some amount

		// get the range for this bucket.
		size_t start = id + get_offset(info);
		size_t next = id + 1 + get_offset(target_info[id + 1]);

		// now search within bucket to see if already present.
		if (is_normal(info)) {  // only for full bucket, of course.

#if defined(REPROBE_STAT)
			size_t reprobe = 0;
#endif
			for (size_t i = start; i < next; ++i) {
				if (eq(v.first, target[i].first)) {
					// check if value and what's in container match.
					//          std::cout << "EXISTING.  " << v.first << ", " << target[i].first << std::endl;
#if defined(REPROBE_STAT)
					this->reprobes += reprobe;
					this->max_reprobes = std::max(this->max_reprobes, static_cast<info_type>(reprobe));
#endif

					// reduction if needed.  should optimize out if not needed.
					if (! std::is_same<reducer, ::fsc::DiscardReducer>::value) target[i].second = reduc(target[i].second, v.second);

					//return make_existing_bucket_id(i, info);
					return make_existing_bucket_id(i);
				}
#if defined(REPROBE_STAT)
				++reprobe;
#endif
			}

#if defined(REPROBE_STAT)
			this->reprobes += reprobe;
			this->max_reprobes = std::max(this->max_reprobes, static_cast<info_type>(reprobe));
#endif
		}

		// now for the non-empty, or empty with offset, shift and insert, starting from NEXT bucket.


		// first swap in at start of bucket, then deal with the swapped.
		// swap until empty info
		// then insert into last empty,
		// then update info until info_empty

		// scan for the next empty position AND update all info entries.
		size_t end = id + 1;
		//  while ((end < target_info.size()) && (target_info[end] != info_empty) ) { // loop until empty
		//    ++(target_info[end]);  // increment
		//    ++end;
		//  }
		//  if (end < target_info.size())
		//	  // increment last.
		//	  ++(target_info[end]);
		while (target_info[end] != info_empty) { // loop until empty
			++(target_info[end]);  // increment
			assert(get_offset(target_info[end]) > 0);

			++end;
		}
		// increment last.
		++(target_info[end]);
		assert(get_offset(target_info[end]) > 0);


		if (end < next) {
			std::cout << "val " << v.first << " id " << id <<
					" info " << static_cast<size_t>(info) <<
					" start info " << static_cast<size_t>(target_info[id]) <<
					" next info " << static_cast<size_t>(target_info[id+1]) <<
					" start " << static_cast<size_t>(start) <<
					" next " << static_cast<size_t>(next) <<
					" end " << static_cast<size_t>(end) <<
					" buckets " << buckets <<
					" actual " << target_info.size() << std::endl;

			std::cout << "INFOs from start " << (id - get_offset(info)) << " to id " << id << ": " << std::endl;
			print(0, id, "\t");

//			std::cout << "INFOs from prev " << (id - get_offset(info)) << " to id " << id << ": " << std::endl;
//			print((id - get_offset(info)), id, "\t");
//
			std::cout << "INFOs from id " << id << " to end " << end << ": " << std::endl;
			print(id, end, "\t");

			std::cout << "INFOs from end " << end << " to next " << next << ": " << std::endl;
			print(end, next, "\t");

			//print();
			throw std::logic_error("end should not be before next");
		}
		// now compact backwards.  first do the container via MEMMOVE
		// can potentially be optimized to use only swap, if distance is long enough.
		memmove(&(target[next + 1]), &(target[next]), sizeof(value_type) * (end - next));

		// that's it.
		target[next] = v;

#if defined(REPROBE_STAT)
		this->shifts += (end - id);
		this->max_shifts = std::max(this->max_shifts, (end - id));
		this->moves += (end - next);
		this->max_moves = std::max(this->max_moves, (end - next));
#endif

		//    return make_missing_bucket_id(next, target_info[id]);
		return make_missing_bucket_id(next);

	}

	// insert with hint, while checking to see if any offset is almost overflowing.
	bucket_id_type insert_with_hint_new(container_type & target,
			info_container_type & target_info,
			size_t const & id,
			value_type const & v) {

		assert(id < buckets);

		// get the starting position
		info_type info = target_info[id];
		//    std::cout << "info " << static_cast<size_t>(info) << std::endl;


		// if this is empty and no shift, then insert and be done.
		if (info == info_empty) {
			set_normal(target_info[id]);   // if empty, change it.  if normal, same anyways.
			target[id] = v;
			return make_missing_bucket_id(id);
			//      return make_missing_bucket_id(id, target_info[id]);
		}

		// the bucket is either non-empty, or empty but offset by some amount

		// get the range for this bucket.
		size_t start = id + get_offset(info);
		size_t next = id + 1 + get_offset(target_info[id + 1]);

		// now search within bucket to see if already present.
		if (is_normal(info)) {  // only for full bucket, of course.

#if defined(REPROBE_STAT)
			size_t reprobe = 0;
#endif
			for (size_t i = start; i < next; ++i) {
				if (eq(v.first, target[i].first)) {
					// check if value and what's in container match.
					//          std::cout << "EXISTING.  " << v.first << ", " << target[i].first << std::endl;
#if defined(REPROBE_STAT)
					this->reprobes += reprobe;
					this->max_reprobes = std::max(this->max_reprobes, static_cast<info_type>(reprobe));
#endif

					// reduction if needed.  should optimize out if not needed.
					if (! std::is_same<reducer, ::fsc::DiscardReducer>::value)
						target[i].second = reduc(target[i].second, v.second);

					//return make_existing_bucket_id(i, info);
					return make_existing_bucket_id(i);
				}
#if defined(REPROBE_STAT)
				++reprobe;
#endif
			}

#if defined(REPROBE_STAT)
			this->reprobes += reprobe;
			this->max_reprobes = std::max(this->max_reprobes, static_cast<info_type>(reprobe));
#endif
		}

		// now for the non-empty, or empty with offset, shift and insert, starting from NEXT bucket.


		// first swap in at start of bucket, then deal with the swapped.
		// swap until empty info
		// then insert into last empty,
		// then update info until info_empty

		// scan for the next empty position AND update all info entries.
		size_t end = id + 1;
	    for (; (end < target_info.size()) && (target_info[end] != info_empty); ++end ) {
	    	// loop until finding an empty spot
	    	if (get_offset(target_info[end]) == 127)
	    		return insert_failed;   // for upsizing.
	    }


		if (end < next) {
			std::cout << "val " << v.first << " id " << id <<
					" info " << static_cast<size_t>(info) <<
					" start info " << static_cast<size_t>(target_info[id]) <<
					" next info " << static_cast<size_t>(target_info[id+1]) <<
					" start " << static_cast<size_t>(start) <<
					" next " << static_cast<size_t>(next) <<
					" end " << static_cast<size_t>(end) <<
					" buckets " << buckets <<
					" actual " << target_info.size() << std::endl;

			std::cout << "INFOs from start " << (id - get_offset(info)) << " to id " << id << ": " << std::endl;
			print(0, id, "\t");

//			std::cout << "INFOs from prev " << (id - get_offset(info)) << " to id " << id << ": " << std::endl;
//			print((id - get_offset(info)), id, "\t");
//
			std::cout << "INFOs from id " << id << " to end " << end << ": " << std::endl;
			print(id, end, "\t");

			std::cout << "INFOs from end " << end << " to next " << next << ": " << std::endl;
			print(end, next, "\t");

			//print();
			throw std::logic_error("end should not be before next");
		}

		// now update, move, and insert.
		set_normal(target_info[id]);   // if empty, change it.  if normal, same anyways.
		for (size_t i = id + 1; i <= end; ++i) {
			++(target_info[i]);
			assert(get_offset(target_info[i]) > 0);
		}

		// now compact backwards.  first do the container via MEMMOVE
		// can potentially be optimized to use only swap, if distance is long enough.
		memmove(&(target[next + 1]), &(target[next]), sizeof(value_type) * (end - next));

		// that's it.
		target[next] = v;

#if defined(REPROBE_STAT)
		this->shifts += (end - id);
		this->max_shifts = std::max(this->max_shifts, (end - id));
		this->moves += (end - next);
		this->max_moves = std::max(this->max_moves, (end - next));
#endif

		//    return make_missing_bucket_id(next, target_info[id]);
		return make_missing_bucket_id(next);

	}


	/// this function searches for empty, do memmove, then increment info.  this is slightly slower than searching for empty and incrementing info at the same time.
	bucket_id_type insert_with_hint_old(container_type & target,
			info_container_type & target_info,
			size_t const & id,
			value_type const & v) {

		assert(id < buckets);

		// get the starting position
		info_type info = target_info[id];

		//    std::cout << "info " << static_cast<size_t>(info) << std::endl;
		set_normal(target_info[id]);   // if empty, change it.  if normal, same anyways.

		// if this is empty and no shift, then insert and be done.
		if (info == info_empty) {
			target[id] = v;
			return make_missing_bucket_id(id);
			//      return make_missing_bucket_id(id, target_info[id]);
		}

		// the bucket is either non-empty, or empty but offset by some amount

		// get the range for this bucket.
		size_t start = id + get_offset(info);
		size_t next = id + 1 + get_offset(target_info[id + 1]);

		// now search within bucket to see if already present.
		if (is_normal(info)) {  // only for full bucket, of course.

#if defined(REPROBE_STAT)
			size_t reprobe = 0;
#endif
			for (size_t i = start; i < next; ++i) {
				if (eq(v.first, target[i].first)) {
					// check if value and what's in container match.
					//          std::cout << "EXISTING.  " << v.first << ", " << target[i].first << std::endl;
#if defined(REPROBE_STAT)
					this->reprobes += reprobe;
					this->max_reprobes = std::max(this->max_reprobes, static_cast<info_type>(reprobe));
#endif

					// reduction if needed.  should optimize out if not needed.
					if (! std::is_same<reducer, ::fsc::DiscardReducer>::value) target[i].second = reduc(target[i].second, v.second);

					//return make_existing_bucket_id(i, info);
					return make_existing_bucket_id(i);
				}
#if defined(REPROBE_STAT)
				++reprobe;
#endif
			}

#if defined(REPROBE_STAT)
			this->reprobes += reprobe;
			this->max_reprobes = std::max(this->max_reprobes, static_cast<info_type>(reprobe));
#endif
		}

		// now for the non-empty, or empty with offset, shift and insert, starting from NEXT bucket.


		// first swap in at start of bucket, then deal with the swapped.
		// swap until empty info
		// then insert into last empty,
		// then update info until info_empty

		// scan for the next empty position
		size_t end = find_next_empty_pos(target_info, next);

		if (end < next) {
			std::cout << "val " << v.first << " id " << id <<
					" info " << static_cast<size_t>(info) <<
					" start info " << static_cast<size_t>(target_info[id]) <<
					" next info " << static_cast<size_t>(target_info[id+1]) <<
					" start " << static_cast<size_t>(start) <<
					" next " << static_cast<size_t>(next) <<
					" end " << static_cast<size_t>(end) <<
					" buckets " << buckets <<
					" actual " << target_info.size() << std::endl;

			std::cout << "INFOs from 0 " << 0 << " to id " << id << ": " << std::endl;
			print(0, id, "\t");

//			std::cout << "INFOs from prev " << (id - get_offset(info)) << " to id " << id << ": " << std::endl;
//			print((id - get_offset(info)), id, "\t");
//
			std::cout << "INFOs from id " << id << " to end " << end << ": " << std::endl;
			print(id, end, "\t");

			std::cout << "INFOs from end " << end << " to next " << next << ": " << std::endl;
			print(end, next, "\t");

			//print();
			throw std::logic_error("end should not be before next");
		}

		// now compact backwards.  first do the container via MEMMOVE
		// can potentially be optimized to use only swap, if distance is long enough.
		memmove(&(target[next + 1]), &(target[next]), sizeof(value_type) * (end - next));
		// and increment the infos.
		for (size_t i = id + 1; i <= end; ++i) {
			++(target_info[i]);
			assert(get_offset(target_info[i]) > 0);
		}

#if defined(REPROBE_STAT)
		this->shifts += (end - id);
		this->max_shifts = std::max(this->max_shifts, (end - id));
		this->moves += (end - next);
		this->max_moves = std::max(this->max_moves, (end - next));
#endif

		// that's it.
		target[next] = v;
		//    return make_missing_bucket_id(next, target_info[id]);
		return make_missing_bucket_id(next);

	}




	// batch insert, minimizing number of loop conditionals and rehash checks.
	// provide a set of precomputed hash values.  Also allows resize in case any estimation is not accurate.
	template <typename InputIter, typename HashIter>
	void insert_with_hint(InputIter input, HashIter hashes, size_t input_size) {


#if defined(REPROBE_STAT)
		if ((reinterpret_cast<size_t>(container.data()) % sizeof(value_type)) > 0) {
			std::cout << "WARNING: container alignment not on value boundary" << std::endl;
		} else {
			std::cout << "STATUS: container alignment on value boundary" << std::endl;
		}
		reset_reprobe_stats();
		size_type before = lsize;
#endif
		bucket_id_type id, bid1, bid;

		size_t ii;

		//prefetch only if target_buckets is larger than LOOK_AHEAD
		size_t max_prefetch = std::min(input_size, static_cast<size_t>(2 * LOOK_AHEAD));
		// prefetch 2*LOOK_AHEAD of the info_container.
		for (ii = 0; ii < max_prefetch; ++ii) {
			_mm_prefetch(reinterpret_cast<const char *>(&(*(hashes + ii))), _MM_HINT_T0);

			// prefetch input
			_mm_prefetch(reinterpret_cast<const char *>(&(*(input + ii))), _MM_HINT_T0);
		}

		for (ii = 0; ii < max_prefetch; ++ii) {

			id = *(hashes + ii) & mask;
			// prefetch the info_container entry for ii.
			_mm_prefetch((const char *)&(info_container[id]), _MM_HINT_T0);
			//          if (((id + 1) % 64) == info_align)
			//            _mm_prefetch((const char *)&(info_container[id + 1]), _MM_HINT_T0);

			// prefetch container as well - would be NEAR but may not be exact.
			_mm_prefetch((const char *)&(container[id]), _MM_HINT_T0);

		}


		// iterate based on size between rehashes
		size_t max2 = (input_size > (2*LOOK_AHEAD)) ? input_size - (2*LOOK_AHEAD) : 0;
		size_t max1 = (input_size > LOOK_AHEAD) ? input_size - LOOK_AHEAD : 0;
		size_t i = 0; //, i1 = LOOK_AHEAD, i2 = 2*LOOK_AHEAD;

		size_t lmax;
		size_t insert_bid;

		while (max2 > i) {

#if defined(REPROBE_STAT)
			std::cout << "checking if rehash needed.  i = " << i << std::endl;
#endif

			// first check if we need to resize.
			if (lsize >= max_load) {
				rehash(buckets << 1);

#if defined(REPROBE_STAT)
				std::cout << "rehashed.  size = " << buckets << std::endl;
				if ((reinterpret_cast<size_t>(container.data()) % sizeof(value_type))  > 0) {
					std::cout << "WARNING: container alignment not on value boundary" << std::endl;
				} else {
					std::cout << "STATUS: container alignment on value boundary" << std::endl;
				}
#endif

			}

			lmax = i + std::min(max_load - lsize, max2 - i);


			for (; i < lmax; ++i) {

				_mm_prefetch(reinterpret_cast<const char *>(&(*(hashes + i + 2 * LOOK_AHEAD))), _MM_HINT_T0);
				// prefetch input
				_mm_prefetch(reinterpret_cast<const char *>(&(*(input + i + 2 * LOOK_AHEAD))), _MM_HINT_T0);


				// prefetch container
				bid = *(hashes + i + LOOK_AHEAD) & mask;
				if (is_normal(info_container[bid])) {
					bid1 = bid + 1;
					bid += get_offset(info_container[bid]);
					bid1 += get_offset(info_container[bid1]);

					for (size_t j = bid; j < bid1; j += value_per_cacheline) {
						_mm_prefetch((const char *)&(container[j]), _MM_HINT_T0);
					}
				}

				// first get the bucket id
				insert_bid = insert_with_hint_new(container, info_container, *(hashes + i) & mask, *(input + i));
				while (insert_bid == insert_failed) {
					rehash(buckets << 1);  // resize.
					insert_bid = insert_with_hint_new(container, info_container, *(hashes + i) & mask, *(input + i));
				}
				if (missing(insert_bid))
					++lsize;

				//      std::cout << "insert vec lsize " << lsize << std::endl;
				// prefetch info_container.
				bid = *(hashes + i + 2 * LOOK_AHEAD) & mask;
				_mm_prefetch((const char *)&(info_container[bid]), _MM_HINT_T0);
				//            if (((bid + 1) % 64) == info_align)
				//              _mm_prefetch((const char *)&(info_container[bid + 1]), _MM_HINT_T0);
			}
		}


		//if ((lsize + 2 * LOOK_AHEAD) >= max_load) rehash(buckets << 1);  // TODO: SHOULD PREFETCH AGAIN


		// second to last LOOK_AHEAD
		for (; i < max1; ++i) {


			// === same code as in insert(1)..

			bid = *(hashes + i + LOOK_AHEAD) & mask;


			// prefetch container
			if (is_normal(info_container[bid])) {
				bid1 = bid + 1;
				bid += get_offset(info_container[bid]);
				bid1 += get_offset(info_container[bid1]);

				for (size_t j = bid; j < bid1; j += value_per_cacheline) {
					_mm_prefetch((const char *)&(container[j]), _MM_HINT_T0);
				}
			}

			insert_bid = insert_with_hint_new(container, info_container, *(hashes + i) & mask, *(input + i));
			while (insert_bid == insert_failed) {
				rehash(buckets << 1);  // resize.
				insert_bid = insert_with_hint_new(container, info_container, *(hashes + i) & mask, *(input + i));
			}
			if (missing(insert_bid))
				++lsize;

			//      std::cout << "insert vec lsize " << lsize << std::endl;


		}


		// last LOOK_AHEAD
		for (; i < input_size; ++i) {

			// === same code as in insert(1)..

			insert_bid = insert_with_hint_new(container, info_container, *(hashes + i) & mask, *(input + i));
			while (insert_bid == insert_failed) {
				rehash(buckets << 1);  // resize.
				insert_bid = insert_with_hint_new(container, info_container, *(hashes + i) & mask, *(input + i));
			}
			if (missing(insert_bid))
				++lsize;

			//      std::cout << "insert vec lsize " << lsize << std::endl;

		}


#if defined(REPROBE_STAT)
		print_reprobe_stats("INSERT VEC", input_size, (lsize - before));
#endif

	}

#if defined(REPROBE_STAT)
	void reset_reprobe_stats() const {
		this->reprobes = 0;
		this->max_reprobes = 0;
		this->moves = 0;
		this->max_moves = 0;
		this->shifts = 0;
		this->max_shifts = 0;

	}

	void print_reprobe_stats(std::string const & operation, size_t input_size, size_t success_count) const {
		std::cout << "hash table stat: lsize " << lsize << " buckets " << buckets << std::endl;

		std::cout << "hash table op stat: " << operation << ":" <<
				"\tsuccess=" << success_count << "\ttotal=" << input_size << std::endl;

		std::cout << "hash table reprobe stat: " << operation << ":" <<
				"\treprobe max=" << static_cast<unsigned int>(this->max_reprobes) << "\treprobe total=" << this->reprobes <<
				"\tmove max=" << static_cast<unsigned int>(this->max_moves) << "\tmove total=" << this->moves <<
				"\tshift scanned max=" << static_cast<unsigned int>(this->max_shifts) << "\tshift scan total=" << this->shifts << std::endl;
	}
#endif







public:

	/**
	 * @brief insert a single key-value pair into container.
	 *
	 * note that swap only occurs at bucket boundaries.
	 */
	std::pair<iterator, bool> insert(value_type const & vv) {

#if defined(REPROBE_STAT)
		reset_reprobe_stats();
#endif

		// first check if we need to resize.
		if (lsize >= max_load) rehash(buckets << 1);

		// first get the bucket id
		bucket_id_type id = hash(vv.first) & mask;  // target bucket id.

		id = insert_with_hint_new(container, info_container, id, vv);
		while (id == insert_failed) {
			rehash(buckets << 1);  // resize.
			id = insert_with_hint_new(container, info_container, id, vv);
		}
		bool success = missing(id);
		size_t bid = get_pos(id);

		if (success) ++lsize;

#if defined(REPROBE_STAT)
		print_reprobe_stats("INSERT 1", 1, (success ? 1 : 0));
#endif

		//		std::cout << "insert 1 lsize " << lsize << std::endl;
		return std::make_pair(iterator(container.begin() + bid, info_container.begin()+ bid, info_container.end(), filter), success);

	}

	std::pair<iterator, bool> insert(key_type const & key, mapped_type const & val) {
		return insert(std::make_pair(key, val));
	}


	// insert with iterator.  uses size estimate.
	template <typename Iter, typename std::enable_if<std::is_constructible<value_type,
	typename std::iterator_traits<Iter>::value_type >::value, int >::type = 1>
	void insert(Iter begin, Iter end) {
#if defined(REPROBE_STAT)
		std::cout << "INSERT ITERATOR" << std::endl;

		if ((reinterpret_cast<size_t>(container.data()) % sizeof(value_type)) > 0) {
			std::cout << "WARNING: container alignment not on value boundary" << std::endl;
		} else {
			std::cout << "STATUS: container alignment on value boundary" << std::endl;
		}

		reset_reprobe_stats();
		size_type before = lsize;
#endif

		size_t input_size = std::distance(begin, end);

		if (input_size == 0) return;

		// compute hash value array, and estimate the number of unique entries in current.  then merge with current and get the after count.
		//std::vector<size_t> hash_vals;
		//hash_vals.reserve(input.size());
		size_t* hash_vals = nullptr;
		int ret = posix_memalign(reinterpret_cast<void **>(&hash_vals), 16, sizeof(size_t) * input_size);
		if (ret) {
			free(hash_vals);
			throw std::length_error("failed to allocate aligned memory");
		}

		hyperloglog64<key_type, hasher, 12> hll_local;

		size_t hval;
		Iter it = begin;
		for (size_t i = 0; i < input_size; ++i, ++it) {
			hval = hash(it->first);
			hll_local.update_via_hashval(hval);
			// using mm_stream here does not make a differnece.
			//_mm_stream_si64(reinterpret_cast<long long int*>(hash_vals + i), *(reinterpret_cast<long long int*>(&hval)));
			hash_vals[i] = hval;
		}

		// estimate the number of unique entries in input.
#if defined(REPROBE_STAT)
		double distinct_input_est = hll_local.estimate();
#endif

		hll_local.merge(hll);
		double distinct_total_est = hll_local.estimate();

#if defined(REPROBE_STAT)
		std::cout << " estimate input cardinality as " << distinct_input_est << " total after insertion " << distinct_total_est << std::endl;
#endif
		// assume one element per bucket as ideal, resize now.  should not resize if don't need to.
		reserve(static_cast<size_t>(static_cast<double>(distinct_total_est) * 1.0));   // this updates the bucket counts also.  overestimate by 10 percent just to be sure.

		// now try to insert.  hashing done already.
		insert_with_hint(begin, hash_vals, input_size);


		//        insert_with_hint(sh_input.data(), sh_hash_val.data(), sh_input.size());
		// finally, update the hyperloglog estimator.  just swap.
		hll.swap(hll_local);
		free(hash_vals);

#if defined(REPROBE_STAT)
		print_reprobe_stats("INSERT ITER", input_size, (lsize - before));
#endif
	}


	/// batch insert, minimizing number of loop conditionals and rehash checks.
	// NOT REALLY SORT.  JUST PREFETCHED VERSION AND AVOIDING RESIZE CHECKS.
	template <typename LESS = ::std::less<key_type> >
	void insert_sort(::std::vector<value_type> const & input) {

#if defined(REPROBE_STAT)
		std::cout << "INSERT MIN REHASH CHECK (not really sort)" << std::endl;

		if ((reinterpret_cast<size_t>(container.data()) % sizeof(value_type)) > 0) {
			std::cout << "WARNING: container alignment not on value boundary" << std::endl;
		} else {
			std::cout << "STATUS: container alignment on value boundary" << std::endl;
		}
		reset_reprobe_stats();
		size_type before = lsize;
#endif
		bucket_id_type id, bid1, bid;

		size_t ii;
		size_t hash_val;

		std::array<size_t, 2 * LOOK_AHEAD>  hashes;

		//prefetch only if target_buckets is larger than LOOK_AHEAD
		size_t max_prefetch2 = std::min(info_container.size(), static_cast<size_t>(2 * LOOK_AHEAD));
		// prefetch 2*LOOK_AHEAD of the info_container.
		for (ii = 0; ii < max_prefetch2; ++ii) {
			hash_val = hash(input[ii].first);
			hashes[ii] = hash_val;
			id = hash_val & mask;
			// prefetch the info_container entry for ii.
			_mm_prefetch((const char *)&(info_container[id]), _MM_HINT_T0);
			//		      if (((id + 1) % 64) == info_align)
			//		        _mm_prefetch((const char *)&(info_container[id + 1]), _MM_HINT_T0);

			// prefetch container as well - would be NEAR but may not be exact.
			_mm_prefetch((const char *)&(container[id]), _MM_HINT_T0);

		}

		// iterate based on size between rehashes
		constexpr size_t hash_mask = 2 * LOOK_AHEAD - 1;
		size_t max2 = (input.size() > (2*LOOK_AHEAD)) ? input.size() - (2*LOOK_AHEAD) : 0;
		size_t max1 = (input.size() > LOOK_AHEAD) ? input.size() - LOOK_AHEAD : 0;
		size_t i = 0; //, i1 = LOOK_AHEAD, i2 = 2*LOOK_AHEAD;

		size_t lmax;

		while (max2 > i) {

#if defined(REPROBE_STAT)
			std::cout << "checking if rehash needed.  i = " << i << std::endl;
#endif

			// first check if we need to resize.
			if (lsize >= max_load) {
				rehash(buckets << 1);

#if defined(REPROBE_STAT)
				std::cout << "rehashed.  size = " << buckets << std::endl;
				if ((reinterpret_cast<size_t>(container.data()) % sizeof(value_type))  > 0) {
					std::cout << "WARNING: container alignment not on value boundary" << std::endl;
				} else {
					std::cout << "STATUS: container alignment on value boundary" << std::endl;
				}
#endif


			}

			lmax = i + std::min(max_load - lsize, max2 - i);


			for (; i < lmax; ++i) {
				_mm_prefetch((const char *)&(hashes[(i + 2 * LOOK_AHEAD) & hash_mask]), _MM_HINT_T0);
				// prefetch input
				_mm_prefetch((const char *)&(input[i + 2 * LOOK_AHEAD]), _MM_HINT_T0);


				// prefetch container
				bid = hashes[(i + LOOK_AHEAD) & hash_mask] & mask;
				if (is_normal(info_container[bid])) {
					bid1 = bid + 1;
					bid += get_offset(info_container[bid]);
					bid1 += get_offset(info_container[bid1]);

					for (size_t j = bid; j < bid1; j += value_per_cacheline) {
						_mm_prefetch((const char *)&(container[j]), _MM_HINT_T0);
					}
				}

				// first get the bucket id
				id = hashes[i & hash_mask] & mask;  // target bucket id.
				if (missing(insert_with_hint(container, info_container, id, input[i])))
					++lsize;

				//      std::cout << "insert vec lsize " << lsize << std::endl;
				// prefetch info_container.
				hash_val = hash(input[(i + 2 * LOOK_AHEAD)].first);
				bid = hash_val & mask;
				_mm_prefetch((const char *)&(info_container[bid]), _MM_HINT_T0);
				//            if (((bid + 1) % 64) == info_align)
				//              _mm_prefetch((const char *)&(info_container[bid + 1]), _MM_HINT_T0);

				hashes[(i + 2 * LOOK_AHEAD)  & hash_mask] = hash_val;
			}

		}
		//if ((lsize + 2 * LOOK_AHEAD) >= max_load) rehash(buckets << 1);  // TODO: SHOULD PREFETCH AGAIN


		// second to last LOOK_AHEAD
		for (; i < max1; ++i) {


			// === same code as in insert(1)..

			// first check if we need to resize.
			if (lsize >= max_load) rehash(buckets << 1);  // TODO: SHOULD PREFETCH AGAIN

			bid = hashes[(i + LOOK_AHEAD) & hash_mask] & mask;

			// first get the bucket id
			id = hashes[i & hash_mask] & mask;  // target bucket id.

			// prefetch container
			if (is_normal(info_container[bid])) {
				bid1 = bid + 1;
				bid += get_offset(info_container[bid]);
				bid1 += get_offset(info_container[bid1]);

				for (size_t j = bid; j < bid1; j += value_per_cacheline) {
					_mm_prefetch((const char *)&(container[j]), _MM_HINT_T0);
				}
			}

			if (missing(insert_with_hint(container, info_container, id, input[i])))
				++lsize;

			//      std::cout << "insert vec lsize " << lsize << std::endl;

		}


		// last LOOK_AHEAD
		for (; i < input.size(); ++i) {

			// === same code as in insert(1)..

			// first check if we need to resize.
			if (lsize >= max_load) rehash(buckets << 1);  // TODO: SHOULD PREFETCH AGAIN

			// first get the bucket id
			id = hashes[i & hash_mask] & mask;  // target bucket id.

			if (missing(insert_with_hint(container, info_container, id, input[i])))
				++lsize;

			//      std::cout << "insert vec lsize " << lsize << std::endl;

		}


#if defined(REPROBE_STAT)
		print_reprobe_stats("INSERT VEC", input.size(), (lsize - before));
#endif

	}




	/// batch insert, minimizing number of loop conditionals and rehash checks.
	// integrated means that the insert_with_hint calls per element is integrated here.
	// NO MEASURABLE ADVANTAGE over just insert.
	template <typename LESS = ::std::less<key_type> >
	void insert_integrated(::std::vector<value_type> const & input) {

#if defined(REPROBE_STAT)
		std::cout << "INSERT INTEGRATED" << std::endl;

		if ((reinterpret_cast<size_t>(container.data()) % sizeof(value_type)) > 0) {
			std::cout << "WARNING: container alignment not on value boundary" << std::endl;
		} else {
			std::cout << "STATUS: container alignment on value boundary" << std::endl;
		}
		reset_reprobe_stats();
		size_type before = lsize;
#endif
		bucket_id_type id, bid1, bid;

		size_t ii;
		size_t hash_val;

		std::array<size_t, 2 * LOOK_AHEAD>  hashes;

		//prefetch only if target_buckets is larger than LOOK_AHEAD
		size_t max_prefetch2 = std::min(info_container.size(), static_cast<size_t>(2 * LOOK_AHEAD));
		// prefetch 2*LOOK_AHEAD of the info_container.
		for (ii = 0; ii < max_prefetch2; ++ii) {
			hash_val = hash(input[ii].first);
			hashes[ii] = hash_val;
			id = hash_val & mask;
			// prefetch the info_container entry for ii.
			_mm_prefetch((const char *)&(info_container[id]), _MM_HINT_T0);
			//		      if (((id + 1) % 64) == info_align)
			//		        _mm_prefetch((const char *)&(info_container[id + 1]), _MM_HINT_T0);

			// prefetch container as well - would be NEAR but may not be exact.
			_mm_prefetch((const char *)&(container[id]), _MM_HINT_T0);

		}

		// iterate based on size between rehashes
		constexpr size_t hash_mask = 2 * LOOK_AHEAD - 1;
		size_t max2 = (input.size() > (2*LOOK_AHEAD)) ? input.size() - (2*LOOK_AHEAD) : 0;
		size_t max1 = (input.size() > LOOK_AHEAD) ? input.size() - LOOK_AHEAD : 0;
		size_t i = 0; //, i1 = LOOK_AHEAD, i2 = 2*LOOK_AHEAD;

		size_t lmax;

		info_type info;
		size_t eid;
		bool matched;
		key_type k;

#if defined(REPROBE_STAT)
		size_t reprobe;
#endif

		while (max2 > i) {

#if defined(REPROBE_STAT)
			std::cout << "checking if rehash needed.  i = " << i << std::endl;
#endif

			// first check if we need to resize.
			if (lsize >= max_load) {
				rehash(buckets << 1);

#if defined(REPROBE_STAT)
				std::cout << "rehashed.  size = " << buckets << std::endl;
				if ((reinterpret_cast<size_t>(container.data()) % sizeof(value_type))  > 0) {
					std::cout << "WARNING: container alignment not on value boundary" << std::endl;
				} else {
					std::cout << "STATUS: container alignment on value boundary" << std::endl;
				}
#endif
			}

			lmax = i + std::min(max_load - lsize, max2 - i);


			for (; i < lmax; ++i) {
				_mm_prefetch((const char *)&(hashes[(i + 2 * LOOK_AHEAD) & hash_mask]), _MM_HINT_T0);
				// prefetch input
				_mm_prefetch((const char *)&(input[i + 2 * LOOK_AHEAD]), _MM_HINT_T0);


				// prefetch container
				id = hashes[(i + LOOK_AHEAD) & hash_mask] & mask;
				info = info_container[id];
				if (is_normal(info)) {

					bid = id + get_offset(info);
					bid1 = id + 1 + get_offset(info_container[id + 1]);

					for (size_t j = bid; j < bid1; j += value_per_cacheline) {
						_mm_prefetch((const char *)&(container[j]), _MM_HINT_T0);
					}
				}

				// first get the bucket id
				id = hashes[i & hash_mask] & mask;  // target bucket id.

				//============= try to insert. ==============
				// get the starting position
				info = info_container[id];

				//		std::cout << "info " << static_cast<size_t>(info) << std::endl;
				set_normal(info_container[id]);   // if empty, change it.  if normal, same anyways.

				if (info == info_empty) {  // empty bucket and no shift. insert here.
					container[id] = input[i];
					++lsize;
				} else {
					// not empty, or empty but shifted so current id is occupied.

					// get range
					bid = id + get_offset(info);
					bid1 = id + 1 + get_offset(info_container[id + 1]);
					matched = false;

					if (is_normal(info)) {  // non empty bucket.
						// now search within bucket to see if already present.
						k = input[i].first;
						matched = eq(k, container[bid].first);
#if defined(REPROBE_STAT)
						reprobe = 0;
#endif
						for (ii = bid+1; !matched && (ii < bid1); ++ii) {
							matched = eq(k, container[ii].first);
#if defined(REPROBE_STAT)
							++reprobe;
#endif
						}

#if defined(REPROBE_STAT)
						this->reprobes += reprobe;
						this->max_reprobes = std::max(this->max_reprobes, static_cast<info_type>(reprobe));
#endif
					}

					if (matched) {
						// reduction if needed.  should optimize out if not needed.
						if (! std::is_same<reducer, ::fsc::DiscardReducer>::value)
							container[i].second = reduc(container[i].second, input[i].second);

					} else {
						// no match. shift and insert. staring from next bucket.

						// now for the non-empty, or empty with offset, shift and insert, starting from NEXT bucket.

						// scan for the next empty position AND update all info entries.
						eid = id + 1;
						while (info_container[eid] != info_empty) { // loop until empty
							++(info_container[eid]);  // increment
							assert(get_offset(info_container[eid]) > 0);

							++eid;
						}
						// increment last.
						++(info_container[eid]);
						assert(get_offset(info_container[eid]) > 0);


						//eid = find_next_empty_pos(info_container, bid1);

						if (eid < bid1) {
							// first empty position available for insertion is before the starting point.
							std::cout << "val " << input[i].first << " id " << id <<
									" info " << static_cast<size_t>(info) <<
									" start info " << static_cast<size_t>(info_container[id]) <<
									" bid1 info " << static_cast<size_t>(info_container[id+1]) <<
									" start " << static_cast<size_t>(bid) <<
									" bid1 " << static_cast<size_t>(bid1) <<
									" end " << static_cast<size_t>(eid) <<
									" buckets " << buckets <<
									" actual " << info_container.size() << std::endl;

							std::cout << "INFOs from end " << eid << " to next " << bid1 << ": " << std::endl;
							print(eid, bid1, "\t");
						}

						// now move backwards.  first do the container via MEMMOVE
						// using only swap creates more memory accesses for small buckets.  not worth it.
						memmove(&(container[bid1 + 1]), &(container[bid1]), sizeof(value_type) * (eid - bid1));

						// then insert
						container[bid1] = input[i];

						//    				// finally update all info entries.
						//    	    		for (ii = id + 1; ii <= eid; ++ii) {
						//    	    			++(info_container[ii]);
						//    	    		}
						// and increment the infos.
#if defined(REPROBE_STAT)
						this->shifts += (eid - id);
						this->max_shifts = std::max(this->max_shifts, (eid - id));
						this->moves += (eid - bid1);
						this->max_moves = std::max(this->max_moves, (eid - bid1));
#endif

						// increment size.
						++lsize;
					}
				}
				//============= end insert.========

				//      std::cout << "insert vec lsize " << lsize << std::endl;

				// prefetch info_container.
				hash_val = hash(input[(i + 2 * LOOK_AHEAD)].first);
				id = hash_val & mask;
				_mm_prefetch((const char *)&(info_container[id]), _MM_HINT_T0);
				//            if (((bid + 1) % 64) == info_align)
				//              _mm_prefetch((const char *)&(info_container[bid + 1]), _MM_HINT_T0);

				hashes[(i + 2 * LOOK_AHEAD)  & hash_mask] = hash_val;
			}

		}
		//if ((lsize + 2 * LOOK_AHEAD) >= max_load) rehash(buckets << 1);  // TODO: SHOULD PREFETCH AGAIN


		// second to last LOOK_AHEAD
		for (; i < max1; ++i) {


			// === same code as in insert(1)..

			// first check if we need to resize.
			if (lsize >= max_load) rehash(buckets << 1);  // TODO: SHOULD PREFETCH AGAIN

			id = hashes[(i + LOOK_AHEAD) & hash_mask] & mask;
			info = info_container[id];
			// prefetch container
			if (is_normal(info)) {
				bid = id + get_offset(info);
				bid1 = id + 1 + get_offset(info_container[id + 1]);

				for (size_t j = bid; j < bid1; j += value_per_cacheline) {
					_mm_prefetch((const char *)&(container[j]), _MM_HINT_T0);
				}
			}

			// first get the bucket id
			id = hashes[i & hash_mask] & mask;  // target bucket id.


			//============= try to insert. ==============
			// get the starting position
			info = info_container[id];

			//		std::cout << "info " << static_cast<size_t>(info) << std::endl;
			set_normal(info_container[id]);   // if empty, change it.  if normal, same anyways.

			if (info == info_empty) {  // empty bucket and no shift. insert here.
				container[id] = input[i];
				++lsize;
			} else {
				// not empty, or empty but shifted so current id is occupied.

				// get range
				bid = id + get_offset(info);
				bid1 = id + 1 + get_offset(info_container[id + 1]);
				matched = false;

				if (is_normal(info)) {  // non empty bucket.
					// now search within bucket to see if already present.
					k = input[i].first;
					matched = eq(k, container[bid].first);
#if defined(REPROBE_STAT)
					reprobe = 0;
#endif
					for (ii = bid+1; !matched && (ii < bid1); ++ii) {
						matched = eq(k, container[ii].first);
#if defined(REPROBE_STAT)
						++reprobe;
#endif
					}

#if defined(REPROBE_STAT)
					this->reprobes += reprobe;
					this->max_reprobes = std::max(this->max_reprobes, static_cast<info_type>(reprobe));
#endif
				}

				if (matched) {
					// reduction if needed.  should optimize out if not needed.
					if (! std::is_same<reducer, ::fsc::DiscardReducer>::value)
						container[i].second = reduc(container[i].second, input[i].second);

				} else {
					// no match. shift and insert. staring from next bucket.

					// now for the non-empty, or empty with offset, shift and insert, starting from NEXT bucket.

					// scan for the next empty position AND update all info entries.
					eid = id + 1;
					while (info_container[eid] != info_empty) { // loop until empty
						++(info_container[eid]);  // increment
						assert(get_offset(info_container[eid]) > 0);
						++eid;
					}
					// increment last.
					++(info_container[eid]);
					assert(get_offset(info_container[eid]) > 0);

					//eid = find_next_empty_pos(info_container, bid1);

					if (eid < bid1) {
						std::cout << "val " << input[i].first << " id " << id <<
								" info " << static_cast<size_t>(info) <<
								" start info " << static_cast<size_t>(info_container[id]) <<
								" bid1 info " << static_cast<size_t>(info_container[id+1]) <<
								" start " << static_cast<size_t>(bid) <<
								" bid1 " << static_cast<size_t>(bid1) <<
								" end " << static_cast<size_t>(eid) <<
								" buckets " << buckets <<
								" actual " << info_container.size() << std::endl;

						std::cout << "INFOs from end " << eid << " to next " << bid1 << ": " << std::endl;
						print(eid, bid1, "\t");
					}

					// now move backwards.  first do the container via MEMMOVE
					// using only swap creates more memory accesses for small buckets.  not worth it.
					memmove(&(container[bid1 + 1]), &(container[bid1]), sizeof(value_type) * (eid - bid1));

					// then insert
					container[bid1] = input[i];

					//    				// finally update all info entries.
					//    	    		for (ii = id + 1; ii <= eid; ++ii) {
					//    	    			++(info_container[ii]);
					//    	    		}
					// and increment the infos.
#if defined(REPROBE_STAT)
					this->shifts += (eid - id);
					this->max_shifts = std::max(this->max_shifts, (eid - id));
					this->moves += (eid - bid1);
					this->max_moves = std::max(this->max_moves, (eid - bid1));
#endif

					// increment size.
					++lsize;
				}
			}
			//============= end insert.========


			//      std::cout << "insert vec lsize " << lsize << std::endl;

		}


		// last LOOK_AHEAD
		for (; i < input.size(); ++i) {

			// === same code as in insert(1)..

			// first check if we need to resize.
			if (lsize >= max_load) rehash(buckets << 1);  // TODO: SHOULD PREFETCH AGAIN

			// first get the bucket id
			id = hashes[i & hash_mask] & mask;  // target bucket id.

			//============= try to insert. ==============
			// get the starting position
			info = info_container[id];

			//		std::cout << "info " << static_cast<size_t>(info) << std::endl;
			set_normal(info_container[id]);   // if empty, change it.  if normal, same anyways.

			if (info == info_empty) {  // empty bucket and no shift. insert here.
				container[id] = input[i];
				++lsize;
			} else {
				// not empty, or empty but shifted so current id is occupied.

				// get range
				bid = id + get_offset(info);
				bid1 = id + 1 + get_offset(info_container[id + 1]);
				matched = false;

				if (is_normal(info)) {  // non empty bucket.
					// now search within bucket to see if already present.
					k = input[i].first;
					matched = eq(k, container[bid].first);
#if defined(REPROBE_STAT)
					reprobe = 0;
#endif
					for (ii = bid+1; !matched && (ii < bid1); ++ii) {
						matched = eq(k, container[ii].first);
#if defined(REPROBE_STAT)
						++reprobe;
#endif
					}

#if defined(REPROBE_STAT)
					this->reprobes += reprobe;
					this->max_reprobes = std::max(this->max_reprobes, static_cast<info_type>(reprobe));
#endif
				}

				if (matched) {
					// reduction if needed.  should optimize out if not needed.
					if (! std::is_same<reducer, ::fsc::DiscardReducer>::value)
						container[i].second = reduc(container[i].second, input[i].second);

				} else {
					// no match. shift and insert. staring from next bucket.

					// now for the non-empty, or empty with offset, shift and insert, starting from NEXT bucket.

					// scan for the next empty position AND update all info entries.
					eid = id + 1;
					while (info_container[eid] != info_empty) { // loop until empty
						++(info_container[eid]);  // increment
						assert(get_offset(info_container[eid]) > 0);

						++eid;
					}
					// increment last.
					++(info_container[eid]);
					assert(get_offset(info_container[eid]) > 0);


					//eid = find_next_empty_pos(info_container, bid1);

					if (eid < bid1) {
						std::cout << "val " << input[i].first <<
								" id " << id <<
								" info " << static_cast<size_t>(info) <<
								" start info " << static_cast<size_t>(info_container[id]) <<
								" bid1 info " << static_cast<size_t>(info_container[id+1]) <<
								" start " << static_cast<size_t>(bid) <<
								" bid1 " << static_cast<size_t>(bid1) <<
								" end " << static_cast<size_t>(eid) <<
								" buckets " << buckets <<
								" actual " << info_container.size() << std::endl;

						std::cout << "INFOs from end " << eid << " to next " << bid1 << ": " << std::endl;
						print(eid, bid1, "\t");

					}


					// now move backwards.  first do the container via MEMMOVE
					// using only swap creates more memory accesses for small buckets.  not worth it.
					memmove(&(container[bid1 + 1]), &(container[bid1]), sizeof(value_type) * (eid - bid1));

					// then insert
					container[bid1] = input[i];

					//    				// finally update all info entries.
					//    	    		for (ii = id + 1; ii <= eid; ++ii) {
					//    	    			++(info_container[ii]);
					//    	    		}
					// and increment the infos.
#if defined(REPROBE_STAT)
					this->shifts += (eid - id);
					this->max_shifts = std::max(this->max_shifts, (eid - id));
					this->moves += (eid - bid1);
					this->max_moves = std::max(this->max_moves, (eid - bid1));
#endif

					// increment size.
					++lsize;
				}
			}
			//============= end insert.========


		}


#if defined(REPROBE_STAT)
		print_reprobe_stats("INSERT VEC", input.size(), (lsize - before));
#endif

	}


	/// insert with estimated size to avoid resizing.  uses more memory because of the hash?
	// similar to insert with iterator in structure.  prefetch stuff is delegated to insert_with_hint_no_resize.
	void insert(::std::vector<value_type> const & input) {

#if defined(REPROBE_STAT)
		std::cout << "INSERT VECTOR" << std::endl;
		if ((reinterpret_cast<size_t>(container.data()) % sizeof(value_type)) > 0) {
			std::cout << "WARNING: container alignment not on value boundary" << std::endl;
		} else {
			std::cout << "STATUS: container alignment on value boundary" << std::endl;
		}

		reset_reprobe_stats();
		size_type before = lsize;
#endif

		// compute hash value array, and estimate the number of unique entries in current.  then merge with current and get the after count.
		//std::vector<size_t> hash_vals;
		//hash_vals.reserve(input.size());
		size_t* hash_vals = nullptr;
		int ret = posix_memalign(reinterpret_cast<void **>(&hash_vals), 16, sizeof(size_t) * input.size());
		if (ret) {
			free(hash_vals);
			throw std::length_error("failed to allocate aligned memory");
		}

		hyperloglog64<key_type, hasher, 12> hll_local;

		size_t hval;
		for (size_t i = 0; i < input.size(); ++i) {
			hval = hash(input[i].first);
			hll_local.update_via_hashval(hval);
			// using mm_stream here does not make a differnece.
			//_mm_stream_si64(reinterpret_cast<long long int*>(hash_vals + i), *(reinterpret_cast<long long int*>(&hval)));
			hash_vals[i] = hval;
		}

		// estimate the number of unique entries in input.
#if defined(REPROBE_STAT)
		double distinct_input_est = hll_local.estimate();
#endif

		hll_local.merge(hll);
		double distinct_total_est = hll_local.estimate();

#if defined(REPROBE_STAT)
		std::cout << " estimate input cardinality as " << distinct_input_est << " total after insertion " << distinct_total_est << std::endl;
#endif
		// assume one element per bucket as ideal, resize now.  should not resize if don't need to.
		reserve(static_cast<size_t>(static_cast<double>(distinct_total_est) * 1.0));   // this updates the bucket counts also.  overestimate by 10%


		// now try to insert.  hashing done already.
		insert_with_hint(input.begin(), hash_vals, input.size());

		//        insert_with_hint(sh_input.data(), sh_hash_val.data(), sh_input.size());
		// finally, update the hyperloglog estimator.  just swap.
		hll.swap(hll_local);
		free(hash_vals);

#if defined(REPROBE_STAT)
		print_reprobe_stats("INSERT VEC", input.size(), (lsize - before));
#endif
	}


	/// insert with prefetch, checks maxload each iteration, also with old insert_with_hint
	// similar to insert_sorted
	void insert_shuffled(::std::vector<value_type> const & input) {

#if defined(REPROBE_STAT)
		std::cout << "INSERT SHUFFLED" << std::endl;

		if ((reinterpret_cast<size_t>(container.data()) % sizeof(value_type)) > 0) {
			std::cout << "WARNING: container alignment not on value boundary" << std::endl;
		} else {
			std::cout << "STATUS: container alignment on value boundary" << std::endl;
		}
		reset_reprobe_stats();
		size_type before = lsize;
#endif
		bucket_id_type id, bid1, bid;

		size_t ii;
		size_t hash_val;

		std::array<size_t, 2 * LOOK_AHEAD>  hashes;

		//prefetch only if target_buckets is larger than LOOK_AHEAD
		size_t max_prefetch2 = std::min(info_container.size(), static_cast<size_t>(2 * LOOK_AHEAD));
		// prefetch 2*LOOK_AHEAD of the info_container.
		for (ii = 0; ii < max_prefetch2; ++ii) {
			hash_val = hash(input[ii].first);
			hashes[ii] = hash_val;
			id = hash_val & mask;
			// prefetch the info_container entry for ii.
			_mm_prefetch((const char *)&(info_container[id]), _MM_HINT_T0);
			//          if (((id + 1) % 64) == info_align)
			//            _mm_prefetch((const char *)&(info_container[id + 1]), _MM_HINT_T0);

			// prefetch container as well - would be NEAR but may not be exact.
			_mm_prefetch((const char *)&(container[id]), _MM_HINT_T0);

		}

		// iterate based on size between rehashes
		constexpr size_t hash_mask = 2 * LOOK_AHEAD - 1;
		size_t max2 = (input.size() > (2*LOOK_AHEAD)) ? input.size() - (2*LOOK_AHEAD) : 0;
		size_t max1 = (input.size() > LOOK_AHEAD) ? input.size() - LOOK_AHEAD : 0;
		size_t i = 0; //, i1 = LOOK_AHEAD, i2 = 2*LOOK_AHEAD;

		size_t lmax;

		while (max2 > i) {

#if defined(REPROBE_STAT)
			std::cout << "checking if rehash needed.  i = " << i << std::endl;
#endif

			// first check if we need to resize.
			if (lsize >= max_load) {
				rehash(buckets << 1);

#if defined(REPROBE_STAT)
				std::cout << "rehashed.  size = " << buckets << std::endl;
				if ((reinterpret_cast<size_t>(container.data()) % sizeof(value_type))  > 0) {
					std::cout << "WARNING: container alignment not on value boundary" << std::endl;
				} else {
					std::cout << "STATUS: container alignment on value boundary" << std::endl;
				}
#endif


			}

			lmax = i + std::min(max_load - lsize, max2 - i);


			for (; i < lmax; ++i) {
				_mm_prefetch((const char *)&(hashes[(i + 2 * LOOK_AHEAD) & hash_mask]), _MM_HINT_T0);
				// prefetch input
				_mm_prefetch((const char *)&(input[i + 2 * LOOK_AHEAD]), _MM_HINT_T0);


				// prefetch container
				bid = hashes[(i + LOOK_AHEAD) & hash_mask] & mask;
				if (is_normal(info_container[bid])) {
					bid1 = bid + 1;
					bid += get_offset(info_container[bid]);
					bid1 += get_offset(info_container[bid1]);

					for (size_t j = bid; j < bid1; j += value_per_cacheline) {
						_mm_prefetch((const char *)&(container[j]), _MM_HINT_T0);
					}
				}

				// first get the bucket id
				id = hashes[i & hash_mask] & mask;  // target bucket id.
				if (missing(insert_with_hint_old(container, info_container, id, input[i])))
					++lsize;

				//      std::cout << "insert vec lsize " << lsize << std::endl;
				// prefetch info_container.
				hash_val = hash(input[(i + 2 * LOOK_AHEAD)].first);
				bid = hash_val & mask;
				_mm_prefetch((const char *)&(info_container[bid]), _MM_HINT_T0);
				//            if (((bid + 1) % 64) == info_align)
				//              _mm_prefetch((const char *)&(info_container[bid + 1]), _MM_HINT_T0);

				hashes[(i + 2 * LOOK_AHEAD)  & hash_mask] = hash_val;
			}

		}
		//if ((lsize + 2 * LOOK_AHEAD) >= max_load) rehash(buckets << 1);  // TODO: SHOULD PREFETCH AGAIN


		// second to last LOOK_AHEAD
		for (; i < max1; ++i) {


			// === same code as in insert(1)..

			// first check if we need to resize.
			if (lsize >= max_load) rehash(buckets << 1);  // TODO: SHOULD PREFETCH AGAIN

			bid = hashes[(i + LOOK_AHEAD) & hash_mask] & mask;

			// first get the bucket id
			id = hashes[i & hash_mask] & mask;  // target bucket id.

			// prefetch container
			if (is_normal(info_container[bid])) {
				bid1 = bid + 1;
				bid += get_offset(info_container[bid]);
				bid1 += get_offset(info_container[bid1]);

				for (size_t j = bid; j < bid1; j += value_per_cacheline) {
					_mm_prefetch((const char *)&(container[j]), _MM_HINT_T0);
				}
			}

			if (missing(insert_with_hint_old(container, info_container, id, input[i])))
				++lsize;

			//      std::cout << "insert vec lsize " << lsize << std::endl;

		}


		// last LOOK_AHEAD
		for (; i < input.size(); ++i) {

			// === same code as in insert(1)..

			// first check if we need to resize.
			if (lsize >= max_load) rehash(buckets << 1);  // TODO: SHOULD PREFETCH AGAIN

			// first get the bucket id
			id = hashes[i & hash_mask] & mask;  // target bucket id.

			if (missing(insert_with_hint_old(container, info_container, id, input[i])))
				++lsize;

			//      std::cout << "insert vec lsize " << lsize << std::endl;

		}


#if defined(REPROBE_STAT)
		print_reprobe_stats("INSERT VEC", input.size(), (lsize - before));
#endif

	}

protected:
	template <typename Iter, typename std::enable_if<
		std::is_constructible<
			value_type,
			typename std::iterator_traits<Iter>::value_type >::value,
		int >::type = 1>
	inline key_type const & get_key(Iter it) const {
		return it->first;
	}
	template <typename Iter, typename std::enable_if<
		std::is_constructible<
			key_type,
			typename std::iterator_traits<Iter>::value_type >::value,
		int >::type = 1>
	inline key_type const & get_key(Iter it) const {
		return *it;
	}
	template <typename Iter, typename std::enable_if<
		std::is_constructible<
			value_type,
			typename std::iterator_traits<Iter>::value_type >::value,
		int >::type = 1>
	inline void copy_value(Iter it, value_type const & val) const {
		*it = val;
	}
	template <typename Iter, typename std::enable_if<
		std::is_constructible<
			mapped_type,
			typename std::iterator_traits<Iter>::value_type >::value,
		int >::type = 1>
	inline void copy_value(Iter it, value_type const & val) const {
		*it = val.second;
	}



	struct eval_exists {
		hashmap_robinhood_offsets_reduction const & self;
		eval_exists(hashmap_robinhood_offsets_reduction const & _self,
				container_type const & _cont) : self(_self) {}

		template <typename OutIter>
		inline uint8_t operator()(OutIter & it, bucket_id_type const & bid) const {
			uint8_t rs = self.present(bid);
			*it = rs;
			++it;
			return rs;
		}
	};
	struct eval_find {
		hashmap_robinhood_offsets_reduction const & self;
		container_type const & cont;

		eval_find(hashmap_robinhood_offsets_reduction const & _self,
				container_type const & _cont) : self(_self), cont(_cont) {}

		template <typename OutIter>
		inline uint8_t operator()(OutIter & it, bucket_id_type const & bid) const {
			if (self.present(bid)) {
				self.copy_value(it, cont[self.get_pos(bid)]);
				++it;
				return 1;
			}
			return 0;
		}
	};

	template <typename Reduc>
	struct eval_update {
		hashmap_robinhood_offsets_reduction const & self;
		container_type const & cont;

		eval_update(hashmap_robinhood_offsets_reduction const & _self,
				container_type const & _cont) : self(_self), cont(_cont) {}

		template <typename Iter, typename R = Reduc,
				typename ::std::enable_if<
					!std::is_same<R, ::fsc::DiscardReducer>::value, int>::type = 1>
		inline uint8_t operator()(Iter & it, bucket_id_type const & bid) {
			if (self.present(bid)) {
				cont[self.get_pos(bid)].second =
						self.reduc(cont[self.get_pos(bid)].second, it->second);
				return 1;
			}
			return 0;
		}
		template <typename Iter, typename R = Reduc,
				typename ::std::enable_if<
					std::is_same<R, ::fsc::DiscardReducer>::value, int>::type = 1>
		inline uint8_t operator()(Iter & it, bucket_id_type const & bid) {
			return self.present(bid);
		}
	};


	template <typename Iter, typename OutIter, typename Eval,
			typename OutPredicate = ::bliss::filter::TruePredicate,
			typename InPredicate = ::bliss::filter::TruePredicate>
	size_t internal_find(Iter begin, Iter end, OutIter out,
			Eval const & eval,
			OutPredicate const & out_pred = OutPredicate(),
			InPredicate const & in_pred = InPredicate() ) const  {

#if defined(REPROBE_STAT)
		reset_reprobe_stats();
#endif

		size_t cnt = 0;

		std::vector<size_t>  hashes(2 * LOOK_AHEAD, 0);

		//prefetch only if target_buckets is larger than LOOK_AHEAD
		size_t ii = 0;
		size_t h;

		// prefetch 2*LOOK_AHEAD of the info_container.
		for (Iter it = begin; (ii < (2* LOOK_AHEAD)) && (it != end); ++it, ++ii) {
			h =  hash(get_key(it));
			hashes[ii] = h;
			// prefetch the info_container entry for ii.
			_mm_prefetch((const char *)&(info_container[h & mask]), _MM_HINT_T0);

			// prefetch container as well - would be NEAR but may not be exact.
			_mm_prefetch((const char *)&(container[h & mask]), _MM_HINT_T0);
		}

		size_t total = std::distance(begin, end);

		size_t id, bid, bid1;
		Iter it, new_end = begin;
		std::advance(new_end, (total > (2 * LOOK_AHEAD)) ? (total - (2 * LOOK_AHEAD)) : 0);
		size_t i = 0;
		constexpr size_t hash_mask = 2 * LOOK_AHEAD - 1;
		bucket_id_type found;

		for (it = begin; it != new_end; ++it) {

			// first get the bucket id
			id = hashes[i] & mask;  // target bucket id.

			// prefetch info_container.
			h = hash(get_key(it + 2 * LOOK_AHEAD));
			hashes[i] = h;
			_mm_prefetch((const char *)&(info_container[h & mask]), _MM_HINT_T0);

			// prefetch container
			bid = hashes[(i + LOOK_AHEAD) & hash_mask] & mask;
			if (is_normal(info_container[bid])) {
				bid1 = bid + 1 + get_offset(info_container[bid + 1]);
				bid += get_offset(info_container[bid]);

				for (size_t j = bid; j < bid1; j += value_per_cacheline) {
					_mm_prefetch((const char *)&(container[j]), _MM_HINT_T0);
				}
			}

			found = find_pos_with_hint(get_key(it), id);
			cnt += eval(out, found);  // out is incremented here

			++i;
			i &= hash_mask;
		}

		new_end = begin;
		std::advance(new_end, (total > LOOK_AHEAD) ? (total - LOOK_AHEAD) : 0);
		for (; it != new_end; ++it) {

			// first get the bucket id
			id = hashes[i] & mask;  // target bucket id.

			// prefetch container
			bid = hashes[(i + LOOK_AHEAD) & hash_mask] & mask;
			if (is_normal(info_container[bid])) {
				bid1 = bid + 1 + get_offset(info_container[bid + 1]);
				bid += get_offset(info_container[bid]);

				for (size_t j = bid; j < bid1; j += value_per_cacheline) {
					_mm_prefetch((const char *)&(container[j]), _MM_HINT_T0);
				}
			}

			found = find_pos_with_hint(get_key(it), id);
			cnt += eval(out, found);  // out is incremented here

			++i;
			i &= hash_mask;
		}


		for (; it != end; ++it) {

			// first get the bucket id
			id = hashes[i] & mask;  // target bucket id.

			found = find_pos_with_hint(get_key(it), id);
			cnt += eval(out, found);  // out is incremented here

			++i;
			i &= hash_mask;
		}


#if defined(REPROBE_STAT)
		print_reprobe_stats("INTERNAL_FIND ITER PAIR", std::distance(begin, end), total);
#endif

		return cnt;
	}




public:


	/**
	 * @brief count the presence of a key
	 */
	template <typename OutPredicate = ::bliss::filter::TruePredicate,
			typename InPredicate = ::bliss::filter::TruePredicate >
	inline bool exists( key_type const & k,
			OutPredicate const & out_pred = OutPredicate(),
			InPredicate const & in_pred = InPredicate()  ) const {

		return present(find_pos(k, out_pred, in_pred));
	}


	template <typename Iter,
		typename OutPredicate = ::bliss::filter::TruePredicate,
		typename InPredicate = ::bliss::filter::TruePredicate
	>
	std::vector<uint8_t> exists(Iter begin, Iter end,
			OutPredicate const & out_pred = OutPredicate(),
			InPredicate const & in_pred = InPredicate() ) {

		eval_exists ev(*this, container);

		std::vector<uint8_t> results;
		results.reserve(std::distance(begin, end));

		::fsc::back_emplace_iterator<::std::vector<uint8_t> > count_emplace_iter(results);

		internal_find(begin, end, count_emplace_iter, ev, out_pred, in_pred);

		return results;
	}


	template <typename Iter, typename OutIter,
		typename OutPredicate = ::bliss::filter::TruePredicate,
		typename InPredicate = ::bliss::filter::TruePredicate,
		typename std::enable_if<std::is_constructible<uint8_t,
			typename std::iterator_traits<Iter>::value_type >::value,
			int >::type = 1
	>
	size_t exists(Iter begin, Iter end, OutIter out,
			OutPredicate const & out_pred = OutPredicate(),
			InPredicate const & in_pred = InPredicate() ) {

		eval_exists ev(*this, container);

		return internal_find(begin, end, out, ev, out_pred, in_pred);
	}


	/**
	 * @brief count the presence of a key
	 */
	template <typename OutPredicate = ::bliss::filter::TruePredicate,
			typename InPredicate = ::bliss::filter::TruePredicate >
	inline uint8_t count( key_type const & k,
			OutPredicate const & out_pred = OutPredicate(),
			InPredicate const & in_pred = InPredicate()  ) const {

		return present(find_pos(k, out_pred, in_pred)) ? 1 : 0;
	}


	template <typename Iter,
		typename OutPredicate = ::bliss::filter::TruePredicate,
		typename InPredicate = ::bliss::filter::TruePredicate
	>
	std::vector<uint8_t> count(Iter begin, Iter end,
			OutPredicate const & out_pred = OutPredicate(),
			InPredicate const & in_pred = InPredicate() ) {

		return exists(begin, end, out_pred, in_pred);
	}


	template <typename Iter, typename OutIter,
		typename OutPredicate = ::bliss::filter::TruePredicate,
		typename InPredicate = ::bliss::filter::TruePredicate,
		typename std::enable_if<std::is_constructible<uint8_t,
			typename std::iterator_traits<OutIter>::value_type >::value,
			int >::type = 1
	>
	size_t count(Iter begin, Iter end, OutIter out,
			OutPredicate const & out_pred = OutPredicate(),
			InPredicate const & in_pred = InPredicate() ) {

		return exists(begin, end, out, out_pred, in_pred);
	}


// returning value_type for count probably does not make sense.  no need to report kmer again.
//	template <typename Iter,
//		typename OutPredicate = ::bliss::filter::TruePredicate,
//		typename InPredicate = ::bliss::filter::TruePredicate
//	>
//	std::vector<value_type> count(Iter begin, Iter end,
//			OutPredicate const & out_pred = OutPredicate(),
//			InPredicate const & in_pred = InPredicate() ) {
//		std::vector<value_type> results;
//		results.reserve(std::distance(begin, end));
//
//		::fsc::back_emplace_iterator<::std::vector<size_type> > count_emplace_iter(results);
//
//		count(begin, end, count_emplace_iter, out_pred, in_pred);
//
//		return results;
//
//	}


// FOR RECORD.
//	template <typename Iter, typename std::enable_if<std::is_constructible<key_type,
//	typename std::iterator_traits<Iter>::value_type >::value, int >::type = 1>
//	std::vector<size_type> count(Iter begin, Iter end) {
//#if defined(REPROBE_STAT)
//		reset_reprobe_stats();
//#endif
//		std::vector<size_t>  hashes(2 * LOOK_AHEAD, 0);
//
//		//prefetch only if target_buckets is larger than LOOK_AHEAD
//		size_t ii = 0;
//		// prefetch 2*LOOK_AHEAD of the info_container.
//		for (Iter it = begin; (ii < (2* LOOK_AHEAD)) && (it != end); ++it, ++ii) {
//			hashes[ii] = hash(*it);
//			// prefetch the info_container entry for ii.
//			_mm_prefetch((const char *)&(info_container[hashes[ii] & mask]), _MM_HINT_T0);
//
//			// prefetch container as well - would be NEAR but may not be exact.
//			_mm_prefetch((const char *)&(container[hashes[ii] & mask]), _MM_HINT_T0);
//		}
//
//
//		size_t total = std::distance(begin, end);
//		::std::vector<size_type> counts;
//		counts.reserve(total);
//
//		size_t id, bid, bid1;
//		bucket_id_type found;
//		Iter it2 = begin;
//		std::advance(it2, 2 * LOOK_AHEAD);
//		size_t i = 0, i1 = LOOK_AHEAD, i2=2 * LOOK_AHEAD;
//		constexpr size_t hash_mask = 2 * LOOK_AHEAD - 1;
//
//		for (auto it = begin; it != end; ++it, ++it2, ++i, ++i1, ++i2) {
//
//			// first get the bucket id
//			id = hashes[i & hash_mask] & mask;  // target bucket id.
//
//			// prefetch info_container.
//			if (i2 < total) {
//				ii = i2 & hash_mask;
//				hashes[ii] = hash(*it2);
//				_mm_prefetch((const char *)&(info_container[hashes[ii] & mask]), _MM_HINT_T0);
//			}
//			// prefetch container
//			if (i1 < total) {
//				bid = hashes[i1 & hash_mask] & mask;
//				if (is_normal(info_container[bid])) {
//					bid1 = bid + 1 + get_offset(info_container[bid + 1]);
//					bid += get_offset(info_container[bid]);
//
//					for (size_t j = bid; j < bid1; j += value_per_cacheline) {
//						_mm_prefetch((const char *)&(container[j]), _MM_HINT_T0);
//					}
//				}
//			}
//
//			found = find_pos_with_hint(*it, id);
//
//			counts.emplace_back(present(found));
//		}
//
//
//#if defined(REPROBE_STAT)
//		print_reprobe_stats("COUNT ITER KEY", std::distance(begin, end), counts.size());
//#endif
//		return counts;
//	}


	/**
	 * @brief find the iterator for a key
	 */
	iterator find(key_type const & k) {
#if defined(REPROBE_STAT)
		reset_reprobe_stats();
#endif

		bucket_id_type idx = find_pos(k);

#if defined(REPROBE_STAT)
		print_reprobe_stats("FIND 1 KEY", 1, ( present(idx) ? 1: 0));
#endif

		if (present(idx))
			return iterator(container.begin() + get_pos(idx), info_container.begin()+ get_pos(idx),
					info_container.end(), filter);
		else
			return iterator(container.end(), info_container.end(), filter);

	}

	/**
	 * @brief find the iterator for a key
	 */
	const_iterator find(key_type const & k) const {
#if defined(REPROBE_STAT)
		reset_reprobe_stats();
#endif

		bucket_id_type idx = find_pos(k);

#if defined(REPROBE_STAT)
		print_reprobe_stats("FIND 1 KEY", 1, ( present(idx) ? 1: 0));
#endif

		if (present(idx))
			return const_iterator(container.cbegin() + get_pos(idx), info_container.cbegin()+ get_pos(idx),
					info_container.cend(), filter);
		else
			return const_iterator(container.cend(), info_container.cend(), filter);

	}


	template <typename Iter,
		typename OutPredicate = ::bliss::filter::TruePredicate,
		typename InPredicate = ::bliss::filter::TruePredicate
	>
	std::vector<value_type> find(Iter begin, Iter end,
			OutPredicate const & out_pred = OutPredicate(),
			InPredicate const & in_pred = InPredicate() ) {

		eval_find ev(*this, container);

		std::vector<value_type> results;
		results.reserve(std::distance(begin, end));

		::fsc::back_emplace_iterator<::std::vector<value_type> >
			find_emplace_iter(results);

		internal_find(begin, end, find_emplace_iter, ev, out_pred, in_pred);

		return results;
	}

	template <typename Iter, typename OutIter,
		typename OutPredicate = ::bliss::filter::TruePredicate,
		typename InPredicate = ::bliss::filter::TruePredicate,
		typename std::enable_if<std::is_constructible<value_type,
			typename std::iterator_traits<OutIter>::value_type >::value,
		int >::type = 1
	>
	size_t find(Iter begin, Iter end, OutIter out,
			OutPredicate const & out_pred = OutPredicate(),
			InPredicate const & in_pred = InPredicate() ) {

		eval_find ev(*this, container);

		return internal_find(begin, end, out, ev, out_pred, in_pred);
	}



// OLD version.
//	template <typename Iter, typename std::enable_if<std::is_constructible<value_type,
//	typename std::iterator_traits<Iter>::value_type >::value, int >::type = 1>
//	std::vector<value_type> find(Iter begin, Iter end) {
//#if defined(REPROBE_STAT)
//		reset_reprobe_stats();
//#endif
//
//		std::vector<size_t>  hashes(2 * LOOK_AHEAD, 0);
//
//		//prefetch only if target_buckets is larger than LOOK_AHEAD
//		size_t ii = 0;
//		// prefetch 2*LOOK_AHEAD of the info_container.
//		for (Iter it = begin; (ii < (2* LOOK_AHEAD)) && (it != end); ++it, ++ii) {
//			hashes[ii] = hash((*it).first);
//			// prefetch the info_container entry for ii.
//			_mm_prefetch((const char *)&(info_container[hashes[ii] & mask]), _MM_HINT_T0);
//
//			// prefetch container as well - would be NEAR but may not be exact.
//			_mm_prefetch((const char *)&(container[hashes[ii] & mask]), _MM_HINT_T0);
//		}
//
//
//		size_t total = std::distance(begin, end);
//		::std::vector<value_type> results;
//		results.reserve(total);
//
//		size_t id, bid, bid1;
//		bucket_id_type found;
//		Iter it2 = begin;
//		std::advance(it2, 2 * LOOK_AHEAD);
//		size_t i = 0, i1 = LOOK_AHEAD, i2=2 * LOOK_AHEAD;
//		constexpr size_t hash_mask = 2 * LOOK_AHEAD - 1;
//
//		for (auto it = begin; it != end; ++it, ++it2, ++i, ++i1, ++i2) {
//
//			// first get the bucket id
//			id = hashes[i & hash_mask] & mask;  // target bucket id.
//
//			// prefetch info_container.
//			if (i2 < total) {
//				ii = i2 & hash_mask;
//				hashes[ii] = hash((*it2).first);
//				_mm_prefetch((const char *)&(info_container[hashes[ii] & mask]), _MM_HINT_T0);
//			}
//			// prefetch container
//			if (i1 < total) {
//				bid = hashes[i1 & hash_mask] & mask;
//				if (is_normal(info_container[bid])) {
//					bid1 = bid + 1 + get_offset(info_container[bid + 1]);
//					bid += get_offset(info_container[bid]);
//
//					for (size_t j = bid; j < bid1; j += value_per_cacheline) {
//						_mm_prefetch((const char *)&(container[j]), _MM_HINT_T0);
//					}
//				}
//			}
//
//			found = find_pos_with_hint((*it).first, id);
//
//			if (present(found)) results.emplace_back(container[get_pos(found)]);
//		}
//
//#if defined(REPROBE_STAT)
//		print_reprobe_stats("FIND ITER PAIR", std::distance(begin, end), results.size());
//#endif
//		return results;
//	}
//
//
//	template <typename Iter, typename std::enable_if<std::is_constructible<key_type,
//	typename std::iterator_traits<Iter>::value_type >::value, int >::type = 1>
//	std::vector<value_type> find(Iter begin, Iter end) {
//#if defined(REPROBE_STAT)
//		reset_reprobe_stats();
//#endif
//		std::vector<size_t>  hashes(2 * LOOK_AHEAD, 0);
//
//		//prefetch only if target_buckets is larger than LOOK_AHEAD
//		size_t ii = 0;
//		// prefetch 2*LOOK_AHEAD of the info_container.
//		for (Iter it = begin; (ii < (2* LOOK_AHEAD)) && (it != end); ++it, ++ii) {
//			hashes[ii] = hash(*it);
//			// prefetch the info_container entry for ii.
//			_mm_prefetch((const char *)&(info_container[hashes[ii] & mask]), _MM_HINT_T0);
//
//			// prefetch container as well - would be NEAR but may not be exact.
//			_mm_prefetch((const char *)&(container[hashes[ii] & mask]), _MM_HINT_T0);
//		}
//
//
//		size_t total = std::distance(begin, end);
//		::std::vector<value_type> results;
//		results.reserve(total);
//
//		size_t id, bid, bid1;
//		bucket_id_type found;
//		Iter it2 = begin;
//		std::advance(it2, 2 * LOOK_AHEAD);
//		size_t i = 0, i1 = LOOK_AHEAD, i2=2 * LOOK_AHEAD;
//		constexpr size_t hash_mask = 2 * LOOK_AHEAD - 1;
//
//		for (auto it = begin; it != end; ++it, ++it2, ++i, ++i1, ++i2) {
//
//			// first get the bucket id
//			id = hashes[i & hash_mask] & mask;  // target bucket id.
//
//			// prefetch info_container.
//			if (i2 < total) {
//				ii = i2 & hash_mask;
//				hashes[ii] = hash(*it2);
//				_mm_prefetch((const char *)&(info_container[hashes[ii] & mask]), _MM_HINT_T0);
//			}
//			// prefetch container
//			if (i1 < total) {
//				bid = hashes[i1 & hash_mask] & mask;
//				if (is_normal(info_container[bid])) {
//					bid1 = bid + 1 + get_offset(info_container[bid + 1]);
//					bid += get_offset(info_container[bid]);
//
//					for (size_t j = bid; j < bid1; j += value_per_cacheline) {
//						_mm_prefetch((const char *)&(container[j]), _MM_HINT_T0);
//					}
//				}
//			}
//
//			found = find_pos_with_hint(*it, id);
//
//			if (present(found)) results.emplace_back(container[get_pos(found)]);
//		}
//
//
//#if defined(REPROBE_STAT)
//		print_reprobe_stats("FIND ITER PAIR", std::distance(begin, end), results.size());
//#endif
//		return results;
//	}


	/* ========================================================
	 *  update.  should only update existing entries
	 *   need separate insert with reducer.
	 */


	/**
	 * @brief.  updates current value.  does NOT insert new entries.
	 */
	void update(key_type const & k, mapped_type const & val) {
		// find and update.  if not present, insert.
		bucket_id_type bid = find_pos(k);

		if (present(bid)) {  // not inserted and no exception, so an equal entry has been found.

			if (! std::is_same<Reducer, ::fsc::DiscardReducer>::value)  container[get_pos(bid)].second = r(container[get_pos(bid)].second, val);   // so update.

		}
	}

	void update(value_type const & vv) {
		update(vv.first, vv.second);
	}


	template <typename Iter, typename OutIter,
		typename OutPredicate = ::bliss::filter::TruePredicate,
		typename InPredicate = ::bliss::filter::TruePredicate,
		typename std::enable_if<std::is_constructible<value_type,
			typename std::iterator_traits<Iter>::value_type >::value,
		int >::type = 1
	>
	size_t update(Iter begin, Iter end,
			OutPredicate const & out_pred = OutPredicate(),
			InPredicate const & in_pred = InPredicate() ) {

		eval_update<Reducer> ev(*this, container);

		return internal_find(begin, end, begin, ev, out_pred, in_pred);
	}


//	template <typename Iter>
//	void update_reduce(Iter begin, Iter end) {
//#if defined(REPROBE_STAT)
//		reset_reprobe_stats();
//#endif
//		std::vector<size_t>  hashes(2 * LOOK_AHEAD, 0);
//
//		//prefetch only if target_buckets is larger than LOOK_AHEAD
//		size_t ii = 0;
//		// prefetch 2*LOOK_AHEAD of the info_container.
//		for (Iter it = begin; (ii < (2* LOOK_AHEAD)) && (it != end); ++it, ++ii) {
//			hashes[ii] = hash(*it);
//			// prefetch the info_container entry for ii.
//			_mm_prefetch((const char *)&(info_container[hashes[ii] & mask]), _MM_HINT_T0);
//
//			// prefetch container as well - would be NEAR but may not be exact.
//			_mm_prefetch((const char *)&(container[hashes[ii] & mask]), _MM_HINT_T0);
//		}
//
//
//		size_t total = std::distance(begin, end);
//
//		size_t id, bid, bid1;
//		bucket_id_type found;
//		Iter it2 = begin;
//		std::advance(it2, 2 * LOOK_AHEAD);
//		size_t i = 0, i1 = LOOK_AHEAD, i2=2 * LOOK_AHEAD;
//		constexpr size_t hash_mask = 2 * LOOK_AHEAD - 1;
//
//#if defined(REPROBE_STAT)
//		size_t cc = 0;
//#endif
//
//		for (auto it = begin; it != end; ++it, ++it2, ++i, ++i1, ++i2) {
//
//			// first get the bucket id
//			id = hashes[i & hash_mask] & mask;  // target bucket id.
//
//			// prefetch info_container.
//			if (i2 < total) {
//				ii = i2 & hash_mask;
//				hashes[ii] = hash(*it2);
//				_mm_prefetch((const char *)&(info_container[hashes[ii] & mask]), _MM_HINT_T0);
//			}
//			// prefetch container
//			if (i1 < total) {
//				bid = hashes[i1 & hash_mask] & mask;
//				if (is_normal(info_container[bid])) {
//					bid1 = bid + 1 + get_offset(info_container[bid + 1]);
//					bid += get_offset(info_container[bid]);
//
//					for (size_t j = bid; j < bid1; j += value_per_cacheline) {
//						_mm_prefetch((const char *)&(container[j]), _MM_HINT_T0);
//					}
//				}
//			}
//
//			found = find_pos_with_hint((*it).first, id);
//
//			if (present(found)) {
//				if (! std::is_same<reducer, ::fsc::DiscardReducer>::value) container[get_pos(found)].second = r(container[get_pos(found)].second, it->second);
//#if defined(REPROBE_STAT)
//				++cc;
//#endif
//			}
//		}
//
//
//#if defined(REPROBE_STAT)
//		print_reprobe_stats("COND UPDATE ITER", std::distance(begin, end), cc);
//#endif
//
//	}



protected:
	/**
	 * @brief erases a key.  performs backward shift.  swap at bucket boundaries only.
	 */
	size_type erase_and_compact(key_type const & k, bucket_id_type const & bid) {
		bucket_id_type found = find_pos_with_hint(k, bid);  // get the matching position

		if (missing(found)) {
			// did not find. done
			return 0;
		}

		--lsize;   // reduce the size by 1.

		size_t pos = get_pos(found);   // get bucket id
		size_t pos1 = pos + 1;
		// get the end of the non-empty range, starting from the next position.
		size_type bid1 = bid + 1;  // get the next bucket, since bucket contains offset for current bucket.

		size_type end = find_next_zero_offset_pos(info_container, bid1);

		// debug		std::cout << "erasing " << k << " hash " << bid << " at " << found << " end is " << end << std::endl;

		// move to backward shift.  move [found+1 ... end-1] to [found ... end - 2].  end is excluded because it has 0 dist.
		memmove(&(container[pos]), &(container[pos1]), (end - pos1) * sizeof(value_type));



		// debug		print();

		// now change the offsets.
		// if that was the last entry for the bucket, then need to change this to say empty.
		if (get_offset(info_container[bid]) == get_offset(info_container[bid1])) {  // both have same distance, so bid has only 1 entry
			set_empty(info_container[bid]);
		}


		// start from bid+1, end at end - 1.
		for (size_t i = bid1; i < end; ++i ) {
			--(info_container[i]);
		}

#if defined(REPROBE_STAT)
		this->shifts += (end - bid1);
		this->max_shifts = std::max(this->max_shifts, (end - bid1));
		this->moves += (end - pos1);
		this->max_moves = std::max(this->max_moves, (end - pos1));
#endif

		return 1;

	}

	//============ ERASE
	//  ERASE should do it in batch.  within each bucket, erase and compact, track end points.
	//  then one pass front to back compact across buckets.


public:

	/// single element erase with key.
	size_type erase_no_resize(key_type const & k) {
#if defined(REPROBE_STAT)
		reset_reprobe_stats();
#endif
		size_t bid = hash(k) & mask;

		size_t erased = erase_and_compact(k, bid);

#if defined(REPROBE_STAT)
		print_reprobe_stats("ERASE 1", 1, erased);
#endif
		return erased;
	}

	/// batch erase with iterator of value pairs.
	template <typename Iter, typename std::enable_if<std::is_constructible<value_type,
	typename std::iterator_traits<Iter>::value_type >::value, int >::type = 1>
	size_type erase_no_resize(Iter begin, Iter end) {

#if defined(REPROBE_STAT)
		reset_reprobe_stats();
#endif

		size_type before = lsize;

		std::vector<size_t>  hashes(2 * LOOK_AHEAD, 0);

		//prefetch only if target_buckets is larger than LOOK_AHEAD
		size_t ii = 0;
		// prefetch 2*LOOK_AHEAD of the info_container.
		for (Iter it = begin; (ii < (2* LOOK_AHEAD)) && (it != end); ++it, ++ii) {
			hashes[ii] = hash((*it).first);
			// prefetch the info_container entry for ii.
			_mm_prefetch((const char *)&(info_container[hashes[ii] & mask]), _MM_HINT_T0);

			// prefetch container as well - would be NEAR but may not be exact.
			_mm_prefetch((const char *)&(container[hashes[ii] & mask]), _MM_HINT_T0);
		}

		size_t total = std::distance(begin, end);

		size_t id, bid, bid1;
		Iter it2 = begin;
		std::advance(it2, 2 * LOOK_AHEAD);
		size_t i = 0, i1 = LOOK_AHEAD, i2=2 * LOOK_AHEAD;
		constexpr size_t hash_mask = 2 * LOOK_AHEAD - 1;

		for (auto it = begin; it != end; ++it, ++it2, ++i, ++i1, ++i2) {

			// first get the bucket id
			id = hashes[i & hash_mask] & mask;  // target bucket id.

			// prefetch info_container.
			if (i2 < total) {
				ii = i2 & hash_mask;
				hashes[ii] = hash((*it2).first);
				_mm_prefetch((const char *)&(info_container[hashes[ii] & mask]), _MM_HINT_T0);
			}
			// prefetch container
			if (i1 < total) {
				bid = hashes[i1 & hash_mask] & mask;
				if (is_normal(info_container[bid])) {
					bid1 = bid + 1 + get_offset(info_container[bid + 1]);
					bid += get_offset(info_container[bid]);

					for (size_t j = bid; j < bid1; j += value_per_cacheline) {
						_mm_prefetch((const char *)&(container[j]), _MM_HINT_T0);
					}
				}
			}

			erase_and_compact((*it).first, id);
		}


#if defined(REPROBE_STAT)
		print_reprobe_stats("ERASE ITER PAIR", std::distance(begin, end), before - lsize);
#endif
		return before - lsize;
	}

	/// batch erase with iterator of keys.
	template <typename Iter, typename std::enable_if<std::is_constructible<key_type,
	typename std::iterator_traits<Iter>::value_type >::value, int >::type = 1>
	size_type erase_no_resize(Iter begin, Iter end) {
#if defined(REPROBE_STAT)
		reset_reprobe_stats();
#endif

		size_type before = lsize;

		std::vector<size_t>  hashes(2 * LOOK_AHEAD, 0);

		//prefetch only if target_buckets is larger than LOOK_AHEAD
		size_t ii = 0;
		// prefetch 2*LOOK_AHEAD of the info_container.
		for (Iter it = begin; (ii < (2* LOOK_AHEAD)) && (it != end); ++it, ++ii) {
			hashes[ii] = hash(*it);
			// prefetch the info_container entry for ii.
			_mm_prefetch((const char *)&(info_container[hashes[ii] & mask]), _MM_HINT_T0);

			// prefetch container as well - would be NEAR but may not be exact.
			_mm_prefetch((const char *)&(container[hashes[ii] & mask]), _MM_HINT_T0);
		}

		size_t total = std::distance(begin, end);

		size_t id, bid, bid1;
		Iter it2 = begin;
		std::advance(it2, 2 * LOOK_AHEAD);
		size_t i = 0, i1 = LOOK_AHEAD, i2=2 * LOOK_AHEAD;
		constexpr size_t hash_mask = 2 * LOOK_AHEAD - 1;

		for (auto it = begin; it != end; ++it, ++it2, ++i, ++i1, ++i2) {

			// first get the bucket id
			id = hashes[i & hash_mask] & mask;  // target bucket id.

			// prefetch info_container.
			if (i2 < total) {
				ii = i2 & hash_mask;
				hashes[ii] = hash(*it2);
				_mm_prefetch((const char *)&(info_container[hashes[ii] & mask]), _MM_HINT_T0);
			}
			// prefetch container
			if (i1 < total) {
				bid = hashes[i1 & hash_mask] & mask;
				if (is_normal(info_container[bid])) {
					bid1 = bid + 1 + get_offset(info_container[bid + 1]);
					bid += get_offset(info_container[bid]);

					for (size_t j = bid; j < bid1; j += value_per_cacheline) {
						_mm_prefetch((const char *)&(container[j]), _MM_HINT_T0);
					}
				}
			}

			erase_and_compact(*it, id);
		}

#if defined(REPROBE_STAT)
		print_reprobe_stats("ERASE ITER KEY", std::distance(begin, end), before - lsize);
#endif
		return before - lsize;
	}

	/**
	 * @brief erases a key.
	 */
	size_type erase(key_type const & k) {

		size_type res = erase_no_resize(k);

		if (lsize < min_load) rehash(buckets >> 1);

		return res;
	}

	template <typename Iter>
	size_type erase(Iter begin, Iter end) {

		size_type erased = erase_no_resize(begin, end);

		if (lsize < min_load) reserve(lsize);

		return erased;
	}

};

template <typename Key, typename T, typename Hash, typename Equal, typename Allocator, typename Reducer >
constexpr typename hashmap_robinhood_offsets_reduction<Key, T, Hash, Equal, Allocator, Reducer>::info_type hashmap_robinhood_offsets_reduction<Key, T, Hash, Equal, Allocator, Reducer>::info_empty;
template <typename Key, typename T, typename Hash, typename Equal, typename Allocator, typename Reducer >
constexpr typename hashmap_robinhood_offsets_reduction<Key, T, Hash, Equal, Allocator, Reducer>::info_type hashmap_robinhood_offsets_reduction<Key, T, Hash, Equal, Allocator, Reducer>::info_mask;
template <typename Key, typename T, typename Hash, typename Equal, typename Allocator, typename Reducer >
constexpr typename hashmap_robinhood_offsets_reduction<Key, T, Hash, Equal, Allocator, Reducer>::info_type hashmap_robinhood_offsets_reduction<Key, T, Hash, Equal, Allocator, Reducer>::info_normal;

template <typename Key, typename T, typename Hash, typename Equal, typename Allocator, typename Reducer >
constexpr typename hashmap_robinhood_offsets_reduction<Key, T, Hash, Equal, Allocator, Reducer>::bucket_id_type hashmap_robinhood_offsets_reduction<Key, T, Hash, Equal, Allocator, Reducer>::bid_pos_mask;
template <typename Key, typename T, typename Hash, typename Equal, typename Allocator, typename Reducer >
constexpr typename hashmap_robinhood_offsets_reduction<Key, T, Hash, Equal, Allocator, Reducer>::bucket_id_type hashmap_robinhood_offsets_reduction<Key, T, Hash, Equal, Allocator, Reducer>::bid_pos_exists;
template <typename Key, typename T, typename Hash, typename Equal, typename Allocator, typename Reducer >
constexpr typename hashmap_robinhood_offsets_reduction<Key, T, Hash, Equal, Allocator, Reducer>::bucket_id_type hashmap_robinhood_offsets_reduction<Key, T, Hash, Equal, Allocator, Reducer>::insert_failed;
template <typename Key, typename T, typename Hash, typename Equal, typename Allocator, typename Reducer >
constexpr typename hashmap_robinhood_offsets_reduction<Key, T, Hash, Equal, Allocator, Reducer>::bucket_id_type hashmap_robinhood_offsets_reduction<Key, T, Hash, Equal, Allocator, Reducer>::find_failed;
//template <typename Key, typename T, typename Hash, typename Equal, typename Allocator, typename Reducer >
//constexpr typename hashmap_robinhood_offsets_reduction<Key, T, Hash, Equal, Allocator, Reducer>::bucket_id_type hashmap_robinhood_offsets_reduction<Key, T, Hash, Equal, Allocator, Reducer>::bid_info_mask;
//template <typename Key, typename T, typename Hash, typename Equal, typename Allocator, typename Reducer >
//constexpr typename hashmap_robinhood_offsets_reduction<Key, T, Hash, Equal, Allocator, Reducer>::bucket_id_type hashmap_robinhood_offsets_reduction<Key, T, Hash, Equal, Allocator, Reducer>::bid_info_empty;
template <typename Key, typename T, typename Hash, typename Equal, typename Allocator, typename Reducer >
constexpr uint32_t hashmap_robinhood_offsets_reduction<Key, T, Hash, Equal, Allocator, Reducer>::info_per_cacheline;
template <typename Key, typename T, typename Hash, typename Equal, typename Allocator, typename Reducer >
constexpr uint32_t hashmap_robinhood_offsets_reduction<Key, T, Hash, Equal, Allocator, Reducer>::value_per_cacheline;
template <typename Key, typename T, typename Hash, typename Equal, typename Allocator, typename Reducer >
constexpr uint32_t hashmap_robinhood_offsets_reduction<Key, T, Hash, Equal, Allocator, Reducer>::info_prefetch_iters;
template <typename Key, typename T, typename Hash, typename Equal, typename Allocator, typename Reducer >
constexpr uint32_t hashmap_robinhood_offsets_reduction<Key, T, Hash, Equal, Allocator, Reducer>::value_prefetch_iters;


//========== ALIASED TYPES

template <typename Key, typename T, typename Hash = ::std::hash<Key>,
		typename Equal = ::std::equal_to<Key>,
		typename Allocator = ::std::allocator<std::pair<const Key, T> > >
using hashmap_robinhood_offsets = hashmap_robinhood_offsets_reduction<Key, T, Hash, Equal, Allocator, ::fsc::DiscardReducer>;

template <typename Key, typename T, typename Hash = ::std::hash<Key>,
		typename Equal = ::std::equal_to<Key>,
		typename Allocator = ::std::allocator<std::pair<const Key, T> > >
using hashmap_robinhood_offsets_count = hashmap_robinhood_offsets_reduction<Key, T, Hash, Equal, Allocator, ::std::plus<T> >;

}  // namespace fsc
#endif /* KMERHASH_ROBINHOOD_OFFSET_HASHMAP_HPP_ */
