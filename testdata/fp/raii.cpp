// Correct C++: ownership and locking through RAII.
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fp {

std::mutex mu;
int shared_total = 0;

int add_locked(int v) {
    std::lock_guard<std::mutex> guard(mu);
    shared_total += v;
    return shared_total;
}

std::vector<int> make_squares(int n) {
    std::vector<int> out;
    out.reserve(static_cast<std::size_t>(n > 0 ? n : 0));
    for (int i = 0; i < n; ++i) out.push_back(i * i);
    return out;
}

std::string greet(const std::string& who) {
    auto p = std::make_unique<std::string>("hello, " + who);
    return *p;
}

}  // namespace fp
