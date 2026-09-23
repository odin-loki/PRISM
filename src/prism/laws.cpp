// String API over the pure verdict module (src/prism/verdict/verdict.cpp).
#include "prism/laws.hpp"

#include "prism/models.hpp"
#include "prism/verdict.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace prism::laws {

namespace {

template <class Pred>
bool test(std::string_view status, Pred p) {
    auto v = verdict::parse(status);
    return v && p(*v);
}

}  // namespace

bool is_proof(std::string_view status) { return test(status, verdict::is_proof); }
bool is_formal(std::string_view status) { return test(status, verdict::is_formal); }
bool is_answered(std::string_view status) { return test(status, verdict::is_answered); }
bool is_no_answer(std::string_view status) { return test(status, verdict::no_answer); }
bool is_status(std::string_view status) { return verdict::parse(status).has_value(); }

void refuse_merge(std::string_view a, std::string_view b) {
    auto va = verdict::parse(a), vb = verdict::parse(b);
    if (!va || !vb) return;
    std::string as(a), bs(b);
    switch (verdict::merge_refusal(*va, *vb)) {
        case verdict::Refusal::None: return;
        case verdict::Refusal::Formal:
            throw std::runtime_error("refusing to merge formal statuses " + as + " and " + bs);
        case verdict::Refusal::PromoteFuzz:
            throw std::runtime_error("refusing to promote fuzz " + as + "/" + bs + " to a proof");
        case verdict::Refusal::PromoteModel:
            throw std::runtime_error("refusing to promote model output " + as + "/" + bs + " to a proof");
        case verdict::Refusal::NotRunClean:
            throw std::runtime_error("refusing to merge NOTRUN with " + (a == NOTRUN ? bs : as) +
                                     ": a stage that did not run is never clean");
    }
}

bool may_rewrite(std::string_view from, std::string_view to) {
    auto a = verdict::parse(from), b = verdict::parse(to);
    return a && b && verdict::may_rewrite(*a, *b);
}

int audit_report(RunReport& report) {
    int violations = 0;
    for (auto& s : report.stages) {
        std::vector<Finding> errors;
        for (auto& f : s.findings) {
            auto v = verdict::parse(f.status);
            if (!v) continue;
            auto c = f.extra.find(std::string(CERTIFICATE_KEY));
            bool cert = c != f.extra.end() && c->second == CERTIFICATE_CHECKED;
            auto r = verdict::audit(s.name, *v, cert);
            if (!r.violation) continue;
            ++violations;
            std::string msg = "verdict audit: " + s.name + " may not emit " + f.status;
            if (*v == verdict::Verdict::ProvedCertified && verdict::may_prove(verdict::stage_origin(s.name)))
                msg += " without a checked certificate";
            Finding e;
            e.stage = s.name;
            e.status = std::string(ERROR);
            e.file = f.file;
            e.function = f.function;
            e.line = f.line;
            e.message = msg;
            e.strength = std::string(STRENGTH_FINDS);
            e.extra["audit"] = "verdict";
            e.extra["original_status"] = f.status;
            errors.push_back(std::move(e));
            f.extra["audit_original"] = f.status;
            f.status = std::string(verdict::name(r.status));
            f.message = "[" + msg + "] " + f.message;
        }
        for (auto& e : errors) s.findings.push_back(std::move(e));
        if (!errors.empty()) s.records = static_cast<int>(s.findings.size());
    }
    return violations;
}

}  // namespace prism::laws
