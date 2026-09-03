/* tests/bootstrap/bigtu.cc - M98's peak-RSS fixture
 *
 * A translation unit whose job is to be expensive to compile.
 *
 * ---- why C++ and not a large C file ------------------------------------
 *
 * The number M98's fourth box asks for is "peak RSS of the largest
 * translation unit", and the reason anybody wants it is to know whether
 * a GCC bootstrap fits on this machine. GCC 14 is written in C++ and its
 * own worst translation units are C++ ones: a few thousand lines of
 * source, a few hundred thousand lines of headers, and a template
 * instantiation graph that is where the memory actually goes. A 5,000
 * line C file is a bigger *file* and a much smaller *compile* - it would
 * measure the wrong thing and flatter the answer.
 *
 * So this is deliberately header-heavy and template-heavy rather than
 * long: every instantiation below is one cc1plus has to keep in memory
 * at once. It is compiled with -c: the question is what the compiler
 * needs, not what the linker does.
 *
 * ---- what it must NOT be ----------------------------------------------
 *
 * Not unbounded. A fixture that grows the compiler's memory until
 * something dies measures this machine's ceiling rather than a build's
 * demand, and M102 already has a test for the ceiling. This is sized to
 * be a plausible worst case for a real project's file, and the harness
 * reports what it cost rather than asserting a number - a budget row
 * that nobody has measured yet would be a guess with a decimal point.
 *
 * **And it was cut down once, deliberately, with both numbers kept.**
 * As first written - Chain<200>, spread<64>, four instantiations of
 * `exercise` - it peaked at 261 MiB and took fifteen minutes of this
 * machine's time, which is a measurement worth having exactly once and
 * a test nobody will run twice. The sizes below are the same shape at
 * about a third of the work; milestones.md M98 records what the larger
 * one cost, because the larger number is the one that answers "would a
 * GCC bootstrap fit in this machine's memory" and the smaller one is
 * the one that can be run before every commit.
 */
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

/* A recursive template, instantiated to a depth the compiler has to hold
 * all of at once - the cheapest honest way to make a compile expensive
 * without making the file long. */
template <int N>
struct Chain {
    static long value(long x) { return Chain<N - 1>::value(x) + N; }
};
template <>
struct Chain<0> {
    static long value(long x) { return x; }
};

/* One container graph per element type, each dragging in its own copy of
 * the standard library's algorithms. */
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

/* Instantiate the whole graph over a spread of types. Each line below is
 * a full set of container, algorithm and stream instantiations. */
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

} /* namespace */

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
