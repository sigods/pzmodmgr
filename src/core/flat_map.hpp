#pragma once
#include <cstddef>
#include <utility>
#include <vector>

namespace pz {

// The manager's maps and sets hold a few dozen entries. A sorted tree buys
// nothing at that size and costs a node allocation per entry, so these flat
// containers keep the data in one vector and use linear lookup.
template <typename K, typename V>
class flat_map {
public:
    using value_type = std::pair<K, V>;
    using iterator = typename std::vector<value_type>::iterator;
    using const_iterator = typename std::vector<value_type>::const_iterator;

    iterator begin() { return entries_.begin(); }
    iterator end() { return entries_.end(); }
    const_iterator begin() const { return entries_.begin(); }
    const_iterator end() const { return entries_.end(); }

    bool empty() const { return entries_.empty(); }
    std::size_t size() const { return entries_.size(); }
    void clear() { entries_.clear(); }

    iterator find(const K& key) {
        for (auto it = entries_.begin(); it != entries_.end(); ++it)
            if (it->first == key) return it;
        return entries_.end();
    }
    const_iterator find(const K& key) const {
        for (auto it = entries_.begin(); it != entries_.end(); ++it)
            if (it->first == key) return it;
        return entries_.end();
    }

    V& operator[](const K& key) {
        auto it = find(key);
        if (it != entries_.end()) return it->second;
        entries_.emplace_back(key, V{});
        return entries_.back().second;
    }

    std::pair<iterator, bool> insert(value_type value) {
        return emplace(std::move(value.first), std::move(value.second));
    }

    std::pair<iterator, bool> emplace(K key, V value) {
        auto it = find(key);
        if (it != entries_.end()) return {it, false};
        entries_.emplace_back(std::move(key), std::move(value));
        return {entries_.end() - 1, true};
    }

    std::size_t erase(const K& key) {
        auto it = find(key);
        if (it == entries_.end()) return 0;
        entries_.erase(it);
        return 1;
    }

    iterator erase(iterator it) { return entries_.erase(it); }

    std::size_t count(const K& key) const { return find(key) == end() ? 0 : 1; }

private:
    std::vector<value_type> entries_;
};

template <typename T>
class flat_set {
public:
    using iterator = typename std::vector<T>::iterator;
    using const_iterator = typename std::vector<T>::const_iterator;

    iterator begin() { return entries_.begin(); }
    iterator end() { return entries_.end(); }
    const_iterator begin() const { return entries_.begin(); }
    const_iterator end() const { return entries_.end(); }

    bool empty() const { return entries_.empty(); }
    std::size_t size() const { return entries_.size(); }
    void clear() { entries_.clear(); }

    iterator find(const T& value) {
        for (auto it = entries_.begin(); it != entries_.end(); ++it)
            if (*it == value) return it;
        return entries_.end();
    }
    const_iterator find(const T& value) const {
        for (auto it = entries_.begin(); it != entries_.end(); ++it)
            if (*it == value) return it;
        return entries_.end();
    }

    std::pair<iterator, bool> insert(T value) {
        auto it = find(value);
        if (it != entries_.end()) return {it, false};
        entries_.push_back(std::move(value));
        return {entries_.end() - 1, true};
    }

    std::size_t erase(const T& value) {
        auto it = find(value);
        if (it == entries_.end()) return 0;
        entries_.erase(it);
        return 1;
    }

    iterator erase(iterator it) { return entries_.erase(it); }

    std::size_t count(const T& value) const {
        return find(value) == end() ? 0 : 1;
    }

private:
    std::vector<T> entries_;
};

}  // namespace pz
