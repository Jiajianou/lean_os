#include <algorithm>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>

struct Base { virtual ~Base() {} virtual int who() const { return 1; } };
struct Derived : Base { int who() const override { return 2; } };

int main() {
    std::vector<int> v{5, 3, 9, 1, 7};
    std::sort(v.begin(), v.end());
    if (v[0] != 1 || v[4] != 9 || v.size() != 5) {
        return 2;
    }
    if (std::find(v.begin(), v.end(), 7) == v.end()) {
        return 2;
    }

    std::string s = "lean";
    s += "_os";
    std::string big(200, 'x');
    big += "end";
    if (s != "lean_os" || s.size() != 7) {
        return 3;
    }
    if (big.size() != 203 || big.substr(200) != "end") {
        return 3;
    }
    if (std::stoi("1234") != 1234) {
        return 3;
    }

    std::map<std::string, int> m;
    m["one"] = 1;
    m["two"] = 2;
    m["three"] = 3;
    if (m.size() != 3 || m["two"] != 2) {
        return 8;
    }
    std::string keys;
    for (const auto &kv : m) {
        keys += kv.first;
        keys += " ";
    }
    if (keys != "one three two ") {
        return 8;
    }

    std::ostringstream os;
    os << "n=" << 42 << " s=" << s;
    if (os.str() != "n=42 s=lean_os") {
        return 4;
    }
    std::istringstream is("hello 7");
    std::string word;
    int n = 0;
    is >> word >> n;
    if (word != "hello" || n != 7) {
        return 4;
    }

    Derived d;
    Base *b = &d;
    if (b->who() != 2) {
        return 5;
    }
    if (dynamic_cast<Derived *>(b) == nullptr) {
        return 5;
    }
    Base plain;
    Base *pb = &plain;
    __asm__ volatile("" : "+r"(pb));
    if (dynamic_cast<Derived *>(pb) != nullptr) {
        return 5;
    }
    if (typeid(*b) != typeid(Derived) || typeid(*pb) == typeid(Derived)) {
        return 5;
    }

    int caught = 0;
    try {
        (void)v.at(99);
    } catch (const std::out_of_range &e) {
        caught |= 1;
    } catch (...) {
        return 6;
    }
    try {
        (void)std::stoi("not a number");
    } catch (const std::invalid_argument &) {
        caught |= 2;
    } catch (...) {
        return 6;
    }
    try {
        throw std::out_of_range("mine");
    } catch (const std::exception &e) {
        if (std::string(e.what()) != "mine") {
            return 6;
        }
        caught |= 4;
    }
    if (caught != 7) {
        return 6;
    }

    int from_thread = 0;
    std::thread t([&from_thread]() { from_thread = 99; });
    t.join();
    if (from_thread != 99) {
        return 7;
    }

    std::cout << "cxxlib: vector sorted, map ordered, string across the SSO "
                 "boundary, iostreams round-tripped, dynamic_cast and typeid "
                 "agreed, three exceptions from inside libstdc++ caught by "
                 "type, and a std::thread joined" << std::endl;
    return 0;
}
