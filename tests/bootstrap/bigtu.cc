#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

template <int N>
struct Chain {
    static long value(long x) { return Chain<N - 1>::value(x) + N; }
};
template <>
struct Chain<0> {
    static long value(long x) { return x; }
};

template <typename T>
struct Bag {
    std::vector<T> items;
    std::map<std::string, std::vector<T> > buckets;
    std::set<T> unique;

    void add(const T &v, const std::string &key) {
        items.push_back(v);
        buckets[key].push_back(v);
        unique.insert(v);
    }

    std::string describe() const {
        std::ostringstream out;
        out << items.size() << ':' << buckets.size() << ':' << unique.size();
        return out.str();
    }

    T fold() const {
        T acc = T();
        for (typename std::vector<T>::const_iterator it = items.begin();
             it != items.end(); ++it) {
            acc = acc + *it;
        }
        return acc;
    }

    void sorted(std::vector<T> &out) const {
        out = items;
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }
};

template <typename A, typename B>
struct Pairs {
    std::vector<std::pair<A, B> > rows;
    std::map<A, B> index;

    void put(const A &a, const B &b) {
        rows.push_back(std::make_pair(a, b));
        index[a] = b;
    }

    std::size_t merged(const Pairs &other) {
        for (typename std::vector<std::pair<A, B> >::const_iterator it =
                 other.rows.begin();
             it != other.rows.end(); ++it) {
            put(it->first, it->second);
        }
        return rows.size();
    }
};

template <typename T>
long exercise(const T &seed, const std::string &key) {
    Bag<T> bag;
    for (int i = 0; i < 8; i++) {
        bag.add(seed, key);
    }
    std::vector<T> out;
    bag.sorted(out);
    Pairs<std::string, T> pairs;
    pairs.put(key, seed);
    Pairs<std::string, T> more;
    more.put(key + "x", seed);
    return static_cast<long>(bag.describe().size() + out.size() +
                             pairs.merged(more));
}

struct Point {
    long x, y;
    Point() : x(0), y(0) {}
    explicit Point(long v) : x(v), y(v * 2) {}
    bool operator<(const Point &o) const { return x < o.x || (x == o.x && y < o.y); }
    bool operator==(const Point &o) const { return x == o.x && y == o.y; }
    Point operator+(const Point &o) const {
        Point p;
        p.x = x + o.x;
        p.y = y + o.y;
        return p;
    }
};

std::ostream &operator<<(std::ostream &os, const Point &p) {
    return os << p.x << ',' << p.y;
}

template <int K>
long spread(long acc) {
    Bag<long> bag;
    bag.add(acc + K, "k");
    std::vector<long> v;
    bag.sorted(v);
    std::function<long(long)> f = std::bind(std::plus<long>(), K, std::placeholders::_1);
    return spread<K - 1>(acc + f(bag.fold()) + static_cast<long>(v.size()));
}
template <>
long spread<0>(long acc) {
    return acc;
}

}

long bigtu_total();

long bigtu_total() {
    long acc = Chain<60>::value(1);
    acc += exercise<long>(3, "l");
    acc += exercise<std::string>(std::string("s"), "s");
    acc += exercise<Point>(Point(2), "p");
    acc += spread<8>(acc);

    std::vector<std::unique_ptr<Bag<long> > > owned;
    for (int i = 0; i < 4; i++) {
        owned.push_back(std::unique_ptr<Bag<long> >(new Bag<long>()));
        owned.back()->add(i, "own");
    }
    for (std::size_t i = 0; i < owned.size(); i++) {
        acc += owned[i]->fold();
    }

    std::map<std::string, std::tuple<long, double, std::string> > tuples;
    tuples["a"] = std::make_tuple(1L, 2.0, std::string("three"));
    acc += static_cast<long>(tuples.size());

    std::vector<long> nums(64);
    for (std::size_t i = 0; i < nums.size(); i++) {
        nums[i] = static_cast<long>(i);
    }
    acc += std::accumulate(nums.begin(), nums.end(), 0L);
    return acc;
}
