// Trace of std::stop_source / std::stop_token. Includes <stop_token>.
// request_stop is true once and false the second time. stop_requested stays
// true. A default token is not stop_requested.
#include <cstdio>
#include <stop_token>

int main() {
    std::stop_token def;
    std::printf("def requested=%d possible=%d\n", def.stop_requested() ? 1 : 0, def.stop_possible() ? 1 : 0);

    std::stop_source src;
    std::printf("src possible=%d requested=%d\n", src.stop_possible() ? 1 : 0, src.stop_requested() ? 1 : 0);

    std::stop_token tok = src.get_token();
    const int first = src.request_stop() ? 1 : 0;
    const int second = src.request_stop() ? 1 : 0;
    std::printf("first=%d second=%d src=%d tok=%d\n", first, second, src.stop_requested() ? 1 : 0,
                tok.stop_requested() ? 1 : 0);

    std::stop_token copied = tok;
    std::printf("copy=%d\n", copied.stop_requested() ? 1 : 0);
    return 0;
}
