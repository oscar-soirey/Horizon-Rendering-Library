// Storage of the objects created through the public API, indexed by HRL_id.
//
// A flat map: (id, value) pairs kept sorted by id in one contiguous array.
// HRL ids only grow, so inserting a new object appends at the end; lookups are
// a binary search. Compared with the hash maps used before:
//   * iteration is contiguous and follows the creation order (deterministic:
//     light order, viewport order, draw order no longer depend on a hash),
//   * no allocation per object.
// The interface is the subset of std::map used by HRL. As with any vector,
// inserting or erasing invalidates iterators and references to the entries
// (the pointed objects themselves do not move).
#ifndef HRL_ID_MAP_H
#define HRL_ID_MAP_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

template <typename V>
class HRL_IdMap
{
public:
	using key_type = uint32_t;
	using mapped_type = V;
	using value_type = std::pair<uint32_t, V>;
	using iterator = typename std::vector<value_type>::iterator;
	using const_iterator = typename std::vector<value_type>::const_iterator;

	iterator begin() { return items_.begin(); }
	iterator end() { return items_.end(); }
	const_iterator begin() const { return items_.begin(); }
	const_iterator end() const { return items_.end(); }
	size_t size() const { return items_.size(); }
	bool empty() const { return items_.empty(); }
	void clear() { items_.clear(); }
	void reserve(size_t n) { items_.reserve(n); }

	iterator find(uint32_t id)
	{
		iterator it = LowerBound(id);
		return (it != items_.end() && it->first == id) ? it : items_.end();
	}
	const_iterator find(uint32_t id) const
	{
		const_iterator it = LowerBound(id);
		return (it != items_.end() && it->first == id) ? it : items_.end();
	}
	size_t count(uint32_t id) const { return find(id) != items_.end() ? 1u : 0u; }
	bool contains(uint32_t id) const { return find(id) != items_.end(); }

	// Inserts when the id is not present. Returns (entry, inserted).
	template <typename Arg>
	std::pair<iterator, bool> emplace(uint32_t id, Arg&& value)
	{
		if (items_.empty() || items_.back().first < id) // usual case: a new, larger id
		{
			items_.emplace_back(id, std::forward<Arg>(value));
			return {items_.end() - 1, true};
		}
		iterator it = LowerBound(id);
		if (it != items_.end() && it->first == id) return {it, false};
		return {items_.emplace(it, id, std::forward<Arg>(value)), true};
	}
	std::pair<iterator, bool> insert(const value_type& entry) { return emplace(entry.first, entry.second); }

	V& operator[](uint32_t id) { return emplace(id, V{}).first->second; }
	V& at(uint32_t id) { return find(id)->second; }
	const V& at(uint32_t id) const { return find(id)->second; }

	size_t erase(uint32_t id)
	{
		iterator it = find(id);
		if (it == items_.end()) return 0;
		items_.erase(it);
		return 1;
	}
	iterator erase(const_iterator it) { return items_.erase(it); }

private:
	iterator LowerBound(uint32_t id)
	{
		return std::lower_bound(items_.begin(), items_.end(), id,
			[](const value_type& entry, uint32_t key) { return entry.first < key; });
	}
	const_iterator LowerBound(uint32_t id) const
	{
		return std::lower_bound(items_.begin(), items_.end(), id,
			[](const value_type& entry, uint32_t key) { return entry.first < key; });
	}

	std::vector<value_type> items_;
};

#endif
