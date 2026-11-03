#include <memory>
struct Conn : std::enable_shared_from_this<Conn> {
    std::shared_ptr<Conn> self;
    Conn() {
        self = shared_from_this();
    }
    std::shared_ptr<Conn> handle() { return shared_from_this(); }
};
class Session : std::enable_shared_from_this<Session> {
public:
    int id = 0;
};
std::shared_ptr<Conn> enable_shared_local_bad(void) {
    Conn c;
    return c.shared_from_this();
}
std::shared_ptr<Conn> enable_shared_owned_ok(void) {
    auto p = std::make_shared<Conn>();
    return p->handle();
}
