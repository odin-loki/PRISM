// Roadmap 2.8: libclang front end of the Clang-AST lint layer.
//
// libclang's C API is loaded at run time (dlopen / LoadLibrary): the build
// needs only <clang-c/Index.h> (CMake option PRISM_LIBCLANG), and a machine
// without libclang.so falls back to clang as a process (astlint.cpp) instead
// of failing to start. The unit is parsed in process (no -ast-dump=json text,
// no second parse of the dump), and only the declarations of the checked
// files (the main file and scanned project headers) are visited through the
// cursor API; they are converted to the -ast-dump=json schema the checks read
// (astlint_internal.hpp). Facts the dump would only give for declarations in
// the checked files (nodiscard / noreturn / virtual / system header, record
// polymorphism, enumerators of enums nested in classes or namespaces) are
// read from the referenced cursor wherever it is declared.
//
// The C API hides some AST detail the dump shows; it is recovered as follows
// (and a check that needs it sees the same schema from both front ends):
//   * implicit casts are UnexposedExpr: castKind is inferred from the types;
//   * if/switch/while condition variables, for-statement slots, GNU case
//     ranges, new[]/delete[], sizeof/alignof: from the file's bytes at the
//     cursor extents (a construct inside a macro expansion is left unknown);
//   * macro expansions: the preprocessing record's expansion ranges.

#include "astlint_internal.hpp"

#include "prism/laws.hpp"

#ifdef PRISM_HAS_LIBCLANG
#include <clang-c/Index.h>
#endif

#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>

#ifdef PRISM_HAS_LIBCLANG
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

namespace prism::astlint::detail {
namespace fs = std::filesystem;

#ifndef PRISM_HAS_LIBCLANG

bool libclang_load(const std::optional<fs::path>&, std::string* why, std::string*) {
    if (why) *why = "PRISM was built without libclang support (CMake PRISM_LIBCLANG=OFF or no clang-c/Index.h)";
    return false;
}

std::optional<Unit> libclang_unit(const std::vector<std::string>&, const std::string&, const FileSet&, bool,
                                  std::string& err) {
    err = "libclang support not built";
    return std::nullopt;
}

#else

namespace {

// ---------------------------------------------------------------- loading

#define PRISM_LIBCLANG_FNS(X)                                                                   \
    X(clang_createIndex) X(clang_disposeIndex) X(clang_parseTranslationUnit2)                   \
    X(clang_disposeTranslationUnit) X(clang_getNumDiagnostics) X(clang_getDiagnostic)           \
    X(clang_disposeDiagnostic) X(clang_getDiagnosticSeverity) X(clang_formatDiagnostic)         \
    X(clang_getCString) X(clang_disposeString) X(clang_getTranslationUnitCursor)                \
    X(clang_visitChildren) X(clang_getCursorKind) X(clang_getCursorLocation)                    \
    X(clang_getCursorExtent) X(clang_getRangeStart) X(clang_getRangeEnd)                        \
    X(clang_getExpansionLocation) X(clang_getFileName) X(clang_getFileContents)                 \
    X(clang_getCursorSpelling) X(clang_getCursorType) X(clang_getTypeSpelling)                  \
    X(clang_getCanonicalType) X(clang_getCursorReferenced) X(clang_getCanonicalCursor)          \
    X(clang_hashCursor) X(clang_Cursor_isNull) X(clang_getCursorBinaryOperatorKind)             \
    X(clang_getBinaryOperatorKindSpelling) X(clang_getCursorUnaryOperatorKind)                  \
    X(clang_getUnaryOperatorKindSpelling) X(clang_Cursor_Evaluate) X(clang_EvalResult_getKind)  \
    X(clang_EvalResult_getAsLongLong) X(clang_EvalResult_isUnsignedInt)                         \
    X(clang_EvalResult_getAsUnsigned) X(clang_EvalResult_getAsDouble)                           \
    X(clang_EvalResult_getAsStr) X(clang_EvalResult_dispose) X(clang_Cursor_getStorageClass)    \
    X(clang_CXXMethod_isVirtual) X(clang_CXXMethod_isPureVirtual)                               \
    X(clang_Location_isInSystemHeader) X(clang_getCursorSemanticParent)                         \
    X(clang_getEnumConstantDeclValue) X(clang_getTypeDeclaration) X(clang_getPointeeType)       \
    X(clang_getCursorResultType) X(clang_getResultType) X(clang_isExpression)                   \
    X(clang_isDeclaration) X(clang_isAttribute) X(clang_getTemplateCursorKind)                  \
    X(clang_getCursorKindSpelling) X(clang_isCursorDefinition) X(clang_getCursorDefinition)      \
    X(clang_getArrayElementType)

struct Api {
#define PRISM_DECL_FN(n) decltype(&::n) n = nullptr;
    PRISM_LIBCLANG_FNS(PRISM_DECL_FN)
#undef PRISM_DECL_FN
};

struct Loaded {
    bool ok = false;
    std::string why, where;
    Api api;
};

void* open_lib(const std::string& p) {
#ifdef _WIN32
    return reinterpret_cast<void*>(LoadLibraryA(p.c_str()));
#else
    return dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* sym(void* h, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(h), name));
#else
    return dlsym(h, name);
#endif
}

std::vector<std::string> candidates(const std::optional<fs::path>& clang) {
    std::vector<std::string> out;
    auto add_dir = [&](const fs::path& d) {
        std::error_code ec;
        if (!fs::is_directory(d, ec)) return;
#ifdef _WIN32
        for (auto* n : {"libclang.dll"}) out.push_back((d / n).string());
#elif defined(__APPLE__)
        for (auto* n : {"libclang.dylib"}) out.push_back((d / n).string());
#else
        for (auto* n : {"libclang.so", "libclang.so.1"}) {
            if (fs::exists(d / n, ec)) out.push_back((d / n).string());
        }
        // Versioned names (libclang-18.so.18, libclang.so.18.1 ...).
        std::vector<std::string> ver;
        for (auto& e : fs::directory_iterator(d, ec)) {
            auto n = e.path().filename().string();
            if (n.starts_with("libclang") && n.find(".so") != std::string::npos &&
                !n.starts_with("libclang-cpp") && !n.starts_with("libclangBasic"))
                ver.push_back(e.path().string());
        }
        std::sort(ver.rbegin(), ver.rend());
        out.insert(out.end(), ver.begin(), ver.end());
#endif
    };
    if (clang) {
        std::error_code ec;
        auto real = fs::canonical(*clang, ec);
        if (!ec) add_dir(real.parent_path().parent_path() / "lib");
    }
#ifdef PRISM_LIBCLANG_HINT
    out.push_back(PRISM_LIBCLANG_HINT);
#endif
#if !defined(_WIN32) && !defined(__APPLE__)
    {
        std::error_code ec;
        std::vector<fs::path> llvm;
        for (auto& e : fs::directory_iterator("/usr/lib", ec))
            if (e.path().filename().string().starts_with("llvm-")) llvm.push_back(e.path());
        std::sort(llvm.rbegin(), llvm.rend());
        for (auto& d : llvm) add_dir(d / "lib");
    }
    out.push_back("libclang.so.1");
    out.push_back("libclang.so");
#elif defined(__APPLE__)
    add_dir("/opt/homebrew/opt/llvm/lib");
    add_dir("/usr/local/opt/llvm/lib");
    add_dir("/Library/Developer/CommandLineTools/usr/lib");
#else
    out.push_back("libclang.dll");
#endif
    return out;
}

Loaded& loaded(const std::optional<fs::path>& clang) {
    static Loaded L;
    static std::once_flag once;
    std::call_once(once, [&] {
        std::string missing;
        for (auto& c : candidates(clang)) {
            void* h = open_lib(c);
            if (!h) continue;
            Api a;
            missing.clear();
#define PRISM_LOAD_FN(n)                                                   \
    a.n = reinterpret_cast<decltype(&::n)>(sym(h, #n));                    \
    if (!a.n && missing.empty()) missing = #n;
            PRISM_LIBCLANG_FNS(PRISM_LOAD_FN)
#undef PRISM_LOAD_FN
            if (!missing.empty()) continue;  // too old (the operator-kind API is libclang 17+)
            L.api = a;
            L.ok = true;
            L.where = c;
            return;
        }
        L.why = missing.empty() ? "libclang not found (install libclang1-18 / libclang-18-dev)"
                                : "libclang found but lacks " + missing + " (libclang 17 or newer is needed)";
    });
    return L;
}

// ---------------------------------------------------------------- conversion

struct FileText {
    std::string key;         // canonical path
    std::string_view text;   // libclang's buffer (valid while the TU lives)
    bool wanted = false;
    std::vector<std::pair<unsigned, unsigned>> macros;  // expansion ranges, sorted
};

bool is_ident(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

struct Conv {
    const Api& A;
    CXTranslationUnit tu;
    const FileSet& files;
    bool cxx;
    Unit& unit;
    std::unordered_map<CXFile, FileText> ftext;
    std::set<std::string> enums_done;
    std::set<std::string> facts_done;
    std::set<std::string> records_done;

    std::string str(CXString s) {
        const char* c = A.clang_getCString(s);
        std::string r = c ? c : "";
        A.clang_disposeString(s);
        return r;
    }

    FileText* file_of(CXFile f) {
        if (!f) return nullptr;
        auto it = ftext.find(f);
        if (it != ftext.end()) return &it->second;
        FileText ft;
        ft.key = canon_path(str(A.clang_getFileName(f)));
        ft.wanted = files.wanted(ft.key);
        if (ft.wanted) {
            std::size_t n = 0;
            const char* buf = A.clang_getFileContents(tu, f, &n);
            if (buf) ft.text = std::string_view(buf, n);
        }
        return &ftext.emplace(f, std::move(ft)).first->second;
    }

    struct Where {
        FileText* file = nullptr;
        unsigned offset = 0;
    };

    Where where(CXSourceLocation l) {
        CXFile f = nullptr;
        unsigned line = 0, col = 0, off = 0;
        A.clang_getExpansionLocation(l, &f, &line, &col, &off);
        return {file_of(f), off};
    }

    bool in_macro(const Where& w) const {
        if (!w.file) return false;
        auto& m = w.file->macros;
        auto it = std::upper_bound(m.begin(), m.end(), std::make_pair(w.offset, ~0u));
        if (it == m.begin()) return false;
        --it;
        return w.offset >= it->first && w.offset < it->second;
    }

    json loc_json(const Where& w) {
        if (!w.file) return json::object();
        json l = {{"_f", w.file->key}, {"offset", w.offset}};
        if (in_macro(w)) return json{{"expansionLoc", l}};
        return l;
    }

    std::string_view text_at(const Where& b, const Where& e) const {
        if (!b.file || b.file != e.file || b.file->text.empty() || e.offset < b.offset ||
            e.offset > b.file->text.size())
            return {};
        return b.file->text.substr(b.offset, e.offset - b.offset);
    }

    std::pair<Where, Where> extent(CXCursor c) {
        auto r = A.clang_getCursorExtent(c);
        return {where(A.clang_getRangeStart(r)), where(A.clang_getRangeEnd(r))};
    }

    std::string_view text_of(CXCursor c) {
        auto [b, e] = extent(c);
        return text_at(b, e);
    }

    std::string id_of(CXCursor c) {
        if (A.clang_Cursor_isNull(c)) return {};
        auto can = A.clang_getCanonicalCursor(c);
        auto w = where(A.clang_getCursorLocation(can));
        return std::to_string(A.clang_hashCursor(can)) + "@" + std::to_string(w.offset) + ":" +
               std::to_string(static_cast<int>(A.clang_getCursorKind(can)));
    }

    json type_json(CXType t) {
        json j = json::object();
        auto q = str(A.clang_getTypeSpelling(t));
        j["qualType"] = q;
        auto d = str(A.clang_getTypeSpelling(A.clang_getCanonicalType(t)));
        if (d != q) j["desugaredQualType"] = d;
        return j;
    }

    // A declaration's type in the dump's spelling. libclang gives a parameter's
    // type as written (`char[16]`); the dump gives the adjusted type
    // (`char *`), which the checks expect (sizeof of an array parameter).
    json decl_type_json(CXCursor c) {
        CXType t = A.clang_getCursorType(c);
        if (A.clang_getCursorKind(c) == CXCursor_ParmDecl &&
            (t.kind == CXType_ConstantArray || t.kind == CXType_IncompleteArray || t.kind == CXType_VariableArray ||
             t.kind == CXType_DependentSizedArray)) {
            auto el = str(A.clang_getTypeSpelling(A.clang_getArrayElementType(t)));
            json j = json::object();
            j["qualType"] = el + (el.ends_with("*") ? "*" : " *");
            return j;
        }
        return type_json(t);
    }

    static std::string decl_kind(CXCursorKind k, bool cxx) {
        switch (k) {
            case CXCursor_FunctionDecl: return "FunctionDecl";
            case CXCursor_CXXMethod: return "CXXMethodDecl";
            case CXCursor_Constructor: return "CXXConstructorDecl";
            case CXCursor_Destructor: return "CXXDestructorDecl";
            case CXCursor_ConversionFunction: return "CXXConversionDecl";
            case CXCursor_VarDecl: return "VarDecl";
            case CXCursor_ParmDecl: return "ParmVarDecl";
            case CXCursor_FieldDecl: return "FieldDecl";
            case CXCursor_EnumDecl: return "EnumDecl";
            case CXCursor_EnumConstantDecl: return "EnumConstantDecl";
            case CXCursor_StructDecl:
            case CXCursor_ClassDecl:
            case CXCursor_UnionDecl: return cxx ? "CXXRecordDecl" : "RecordDecl";
            case CXCursor_Namespace: return "NamespaceDecl";
            case CXCursor_LinkageSpec: return "LinkageSpecDecl";
            case CXCursor_FunctionTemplate: return "FunctionTemplateDecl";
            case CXCursor_ClassTemplate: return "ClassTemplateDecl";
            case CXCursor_ClassTemplatePartialSpecialization: return "ClassTemplatePartialSpecializationDecl";
            case CXCursor_TemplateTypeParameter: return "TemplateTypeParmDecl";
            case CXCursor_NonTypeTemplateParameter: return "NonTypeTemplateParmDecl";
            case CXCursor_TemplateTemplateParameter: return "TemplateTemplateParmDecl";
            case CXCursor_TypedefDecl: return "TypedefDecl";
            case CXCursor_TypeAliasDecl: return "TypeAliasDecl";
            case CXCursor_TypeAliasTemplateDecl: return "TypeAliasTemplateDecl";
            case CXCursor_UsingDirective: return "UsingDirectiveDecl";
            case CXCursor_UsingDeclaration: return "UsingDecl";
            case CXCursor_StaticAssert: return "StaticAssertDecl";
            case CXCursor_FriendDecl: return "FriendDecl";
            case CXCursor_CXXAccessSpecifier: return "AccessSpecDecl";
            case CXCursor_NamespaceAlias: return "NamespaceAliasDecl";
            case CXCursor_LabelStmt: return "LabelStmt";
            default: return {};
        }
    }

    static bool is_ref(CXCursorKind k) {
        return k == CXCursor_TypeRef || k == CXCursor_TemplateRef || k == CXCursor_NamespaceRef ||
               k == CXCursor_MemberRef || k == CXCursor_LabelRef || k == CXCursor_OverloadedDeclRef ||
               k == CXCursor_VariableRef || k == CXCursor_CXXBaseSpecifier ||
               k == CXCursor_CXXAccessSpecifier;
    }

    std::vector<CXCursor> kids(CXCursor c) {
        std::vector<CXCursor> v;
        A.clang_visitChildren(
            c,
            [](CXCursor k, CXCursor, CXClientData d) {
                static_cast<std::vector<CXCursor>*>(d)->push_back(k);
                return CXChildVisit_Continue;
            },
            &v);
        return v;
    }

    // ---- facts about referenced declarations

    bool attr_text_has(CXCursor attr, std::string_view what) {
        auto t = text_of(attr);
        return t.find(what) != std::string_view::npos;
    }

    std::string attr_kind(CXCursor a) {
        auto k = A.clang_getCursorKind(a);
        if (k == CXCursor_WarnUnusedResultAttr) return "WarnUnusedResultAttr";
        if (k == CXCursor_CXXFinalAttr) return "FinalAttr";
        if (k == CXCursor_CXXOverrideAttr) return "OverrideAttr";
        if (k == CXCursor_PureAttr) return "PureAttr";
        if (k == CXCursor_ConstAttr) return "ConstAttr";
        if (k == CXCursor_UnexposedAttr) {
            // The attribute's own spelling (note_facts loads the text of the
            // declaring file, system headers included).
            auto [b, e] = extent(a);
            std::string s(text_at(b, e));
            if (s.find("noreturn") != std::string::npos) return "NoReturnAttr";
            if (s.find("unused") != std::string::npos) return "UnusedAttr";
            if (s.find("cleanup") != std::string::npos) return "CleanupAttr";
            if (s.find("fallthrough") != std::string::npos) return "FallThroughAttr";
            if (s.find("warn_unused_result") != std::string::npos || s.find("nodiscard") != std::string::npos)
                return "WarnUnusedResultAttr";
        }
        return "UnexposedAttr";
    }

    // Load the text of any file (system headers too) for attribute spelling.
    void load_text(FileText* f, CXSourceLocation l) {
        if (!f || !f->text.empty()) return;
        CXFile cf = nullptr;
        unsigned a, b, c;
        A.clang_getExpansionLocation(l, &cf, &a, &b, &c);
        std::size_t n = 0;
        const char* buf = cf ? A.clang_getFileContents(tu, cf, &n) : nullptr;
        if (buf) f->text = std::string_view(buf, n);
    }

    void note_facts(CXCursor ref) {
        auto id = id_of(ref);
        if (id.empty() || !facts_done.insert(id).second) return;
        DeclFacts f;
        auto k = A.clang_getCursorKind(ref);
        const bool fn = k == CXCursor_FunctionDecl || k == CXCursor_CXXMethod || k == CXCursor_Constructor ||
                        k == CXCursor_Destructor || k == CXCursor_ConversionFunction ||
                        k == CXCursor_FunctionTemplate;
        if (!fn) return;
        if (k == CXCursor_CXXMethod || k == CXCursor_Destructor) f.is_virtual = A.clang_CXXMethod_isVirtual(ref);
        auto rt = A.clang_getCursorResultType(ref);
        f.result = rt.kind == CXType_LValueReference ? "lvalue"
                   : rt.kind == CXType_RValueReference ? "xvalue"
                                                       : "prvalue";
        // Every redeclaration: attributes may sit on any of them.
        auto can = A.clang_getCanonicalCursor(ref);
        auto loc = A.clang_getCursorLocation(can);
        auto w = where(loc);
        const bool system = A.clang_Location_isInSystemHeader(loc);
        auto name_at = w.offset;
        // An implicit declaration (C: call of an undeclared function) sits at
        // the call; its extent is the bare name.
        auto [eb, ee] = extent(can);
        load_text(eb.file, A.clang_getRangeStart(A.clang_getCursorExtent(can)));
        auto dtext = text_at(eb, ee);
        const bool implicit = dtext.find('(') == std::string_view::npos;
        // The project's own function: defined (with a body) in a checked file.
        // A prototype of a libc name without a body is still the libc function.
        {
            auto def = A.clang_getCursorDefinition(ref);
            if (!A.clang_Cursor_isNull(def) && !system && !implicit) {
                auto dw = where(A.clang_getCursorLocation(def));
                f.user = dw.file && dw.file->wanted && k == CXCursor_FunctionDecl;
            }
        }
        for (CXCursor d : {ref, can, A.clang_getCursorDefinition(ref)}) {
            if (A.clang_Cursor_isNull(d)) continue;
            for (auto& a : kids(d)) {
                if (!A.clang_isAttribute(A.clang_getCursorKind(a))) continue;
                auto [ab, ae] = extent(a);
                load_text(ab.file, A.clang_getRangeStart(A.clang_getCursorExtent(a)));
                auto ak = attr_kind(a);
                if (ak == "WarnUnusedResultAttr") f.nodiscard = true;
                if (ak == "NoReturnAttr") f.noreturn = true;
            }
            // C11 _Noreturn is a keyword, not an attribute cursor.
            auto [db, de] = extent(d);
            auto t = text_at(db, de);
            auto nm = where(A.clang_getCursorLocation(d));
            if (!t.empty() && nm.file == db.file && nm.offset >= db.offset) {
                auto head = t.substr(0, nm.offset - db.offset);
                if (head.find("_Noreturn") != std::string_view::npos ||
                    head.find("noreturn") != std::string_view::npos)
                    f.noreturn = true;
            }
        }
        (void)name_at;
        // [[nodiscard]] on the returned class.
        if (!f.nodiscard) {
            auto td = A.clang_getTypeDeclaration(A.clang_getCanonicalType(rt));
            if (!A.clang_Cursor_isNull(td)) {
                for (auto& a : kids(td))
                    if (A.clang_getCursorKind(a) == CXCursor_WarnUnusedResultAttr) f.nodiscard = true;
            }
        }
        unit.facts[id] = f;
    }

    void note_record(CXType t, int depth = 0) {
        auto can = A.clang_getCanonicalType(t);
        auto td = A.clang_getTypeDeclaration(can);
        if (A.clang_Cursor_isNull(td)) return;
        auto k = A.clang_getCursorKind(td);
        if (k != CXCursor_StructDecl && k != CXCursor_ClassDecl) return;
        auto name = record_name(str(A.clang_getTypeSpelling(can)));
        if (name.empty() || !records_done.insert(name).second || depth > 16) return;
        auto def = A.clang_getCursorDefinition(td);
        if (A.clang_Cursor_isNull(def)) return;
        RecordFacts r;
        for (auto& c : kids(def)) {
            auto ck = A.clang_getCursorKind(c);
            if (ck == CXCursor_CXXFinalAttr) r.is_final = true;
            if ((ck == CXCursor_CXXMethod || ck == CXCursor_Destructor) && A.clang_CXXMethod_isVirtual(c)) {
                r.polymorphic = true;
                if (ck == CXCursor_Destructor) r.virtual_dtor = true;
            }
            if (ck == CXCursor_CXXBaseSpecifier) {
                auto bt = A.clang_getCursorType(c);
                note_record(bt, depth + 1);
                auto bn = record_name(str(A.clang_getTypeSpelling(A.clang_getCanonicalType(bt))));
                if (!bn.empty()) r.bases.push_back(bn);
            }
        }
        unit.records[name] = r;
    }

    void note_enum(CXCursor constant) {
        auto en = A.clang_getCursorSemanticParent(constant);
        if (A.clang_getCursorKind(en) != CXCursor_EnumDecl) return;
        auto id = id_of(en);
        if (!enums_done.insert(id).second) return;
        json e = {{"kind", "EnumDecl"}, {"id", id}, {"name", str(A.clang_getCursorSpelling(en))},
                  {"_extra", true}, {"inner", json::array()}};
        for (auto& c : kids(en)) {
            if (A.clang_getCursorKind(c) != CXCursor_EnumConstantDecl) continue;
            e["inner"].push_back({{"kind", "EnumConstantDecl"},
                                  {"id", id_of(c)},
                                  {"name", str(A.clang_getCursorSpelling(c))},
                                  {"value", A.clang_getEnumConstantDeclValue(c)}});
        }
        unit.decls.push_back(std::move(e));
    }

    // ---- casts

    static int type_class(CXTypeKind k) {
        // 1 integer, 2 floating, 3 pointer, 4 bool, 5 array, 6 function, 7 nullptr, 0 other
        if (k == CXType_Bool) return 4;
        if ((k >= CXType_Char_U && k <= CXType_UInt128) || (k >= CXType_Char_S && k <= CXType_Int128) ||
            k == CXType_Enum)
            return 1;
        if (k == CXType_Float || k == CXType_Double || k == CXType_LongDouble || k == CXType_Float128 ||
            k == CXType_Half || k == CXType_Float16)
            return 2;
        if (k == CXType_Pointer || k == CXType_BlockPointer || k == CXType_ObjCObjectPointer ||
            k == CXType_MemberPointer)
            return 3;
        if (k == CXType_ConstantArray || k == CXType_IncompleteArray || k == CXType_VariableArray ||
            k == CXType_DependentSizedArray)
            return 5;
        if (k == CXType_FunctionProto || k == CXType_FunctionNoProto) return 6;
        if (k == CXType_NullPtr) return 7;
        return 0;
    }

    std::string cast_kind(CXCursor parent, CXCursor child) {
        auto pt = A.clang_getCanonicalType(A.clang_getCursorType(parent));
        auto ct = A.clang_getCanonicalType(A.clang_getCursorType(child));
        auto pk = type_class(pt.kind), ck = type_class(ct.kind);
        auto ps = str(A.clang_getTypeSpelling(pt)), cs = str(A.clang_getTypeSpelling(ct));
        auto unq = [](std::string s) {
            for (auto* q : {"const ", "volatile "})
                if (s.starts_with(q)) s.erase(0, std::strlen(q));
            return s;
        };
        auto cck = A.clang_getCursorKind(child);
        if (unq(ps) == unq(cs)) {
            if (cck == CXCursor_DeclRefExpr || cck == CXCursor_MemberRefExpr || cck == CXCursor_ArraySubscriptExpr ||
                cck == CXCursor_UnaryOperator || cck == CXCursor_ParenExpr)
                return "LValueToRValue";
            return "NoOp";
        }
        if (ck == 1 && pk == 1) return "IntegralCast";
        if (ck == 1 && pk == 2) return "IntegralToFloating";
        if (ck == 2 && pk == 1) return "FloatingToIntegral";
        if (ck == 2 && pk == 2) return "FloatingCast";
        if (ck == 1 && pk == 4) return "IntegralToBoolean";
        if (ck == 2 && pk == 4) return "FloatingToBoolean";
        if (ck == 3 && pk == 4) return "PointerToBoolean";
        if (ck == 5 && pk == 3) return "ArrayToPointerDecay";
        if (ck == 6 && pk == 3) return "FunctionToPointerDecay";
        if ((ck == 7 || ck == 1) && pk == 3) return "NullToPointer";
        if (ck == 3 && pk == 3) return "BitCast";
        if (ck == 4 && pk == 1) return "IntegralCast";
        if (cck == CXCursor_CallExpr && A.clang_getCursorKind(A.clang_getCursorReferenced(child)) ==
                                            CXCursor_ConversionFunction)
            return "UserDefinedConversion";
        return "Unknown";
    }

    // ---- nodes

    static bool is_expr_kind(CXCursorKind k) { return k >= CXCursor_FirstExpr && k <= CXCursor_LastExpr; }

    // First offset after `name` and its array declarator groups.
    std::optional<unsigned> after_declarator(CXCursor var) {
        auto nm = where(A.clang_getCursorLocation(var));
        if (!nm.file || nm.file->text.empty() || in_macro(nm)) return std::nullopt;
        auto t = nm.file->text;
        std::size_t i = nm.offset;
        while (i < t.size() && is_ident(t[i])) ++i;
        for (;;) {
            while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i]))) ++i;
            if (i >= t.size() || t[i] != '[') break;
            int depth = 0;
            for (; i < t.size(); ++i) {
                if (t[i] == '[') ++depth;
                else if (t[i] == ']' && --depth == 0) {
                    ++i;
                    break;
                }
            }
        }
        return static_cast<unsigned>(i);
    }

    // Top-level ';' offsets inside the parentheses of a for statement.
    std::optional<std::pair<unsigned, unsigned>> for_semis(CXCursor f) {
        auto [b, e] = extent(f);
        auto t = text_at(b, e);
        if (t.size() < 4 || !t.starts_with("for") || in_macro(b)) return std::nullopt;
        std::size_t i = 3;
        while (i < t.size() && t[i] != '(') ++i;
        int depth = 0;
        std::vector<unsigned> semis;
        for (; i < t.size(); ++i) {
            char c = t[i];
            if (c == '"' || c == '\'') {
                for (++i; i < t.size() && t[i] != c; ++i)
                    if (t[i] == '\\') ++i;
                continue;
            }
            if (c == '(' || c == '[' || c == '{') ++depth;
            else if (c == ')' || c == ']' || c == '}') {
                if (--depth == 0) break;
            } else if (c == ';' && depth == 1) {
                semis.push_back(b.offset + static_cast<unsigned>(i));
            }
        }
        if (semis.size() != 2) return std::nullopt;
        return std::make_pair(semis[0], semis[1]);
    }

    char next_char_after(CXCursor c) {
        auto [b, e] = extent(c);
        if (!e.file || e.file->text.empty()) return 0;
        auto t = e.file->text;
        std::size_t i = e.offset;
        while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i]))) ++i;
        return i < t.size() ? t[i] : 0;
    }

    json wrap_declstmt(json var) {
        json d = {{"kind", "DeclStmt"}, {"inner", json::array({std::move(var)})}};
        if (auto it = d["inner"][0].find("range"); it != d["inner"][0].end()) d["range"] = *it;
        return d;
    }

    json node(CXCursor c) {
        auto k = A.clang_getCursorKind(c);
        json n = json::object();
        auto [b, e] = extent(c);
        n["range"] = {{"begin", loc_json(b)}, {"end", loc_json(e)}};
        n["loc"] = loc_json(where(A.clang_getCursorLocation(c)));

        std::vector<CXCursor> ch;
        std::vector<CXCursor> attrs;
        for (auto& x : kids(c)) {
            auto xk = A.clang_getCursorKind(x);
            if (A.clang_isAttribute(xk)) attrs.push_back(x);
            else if (!is_ref(xk)) ch.push_back(x);
        }
        auto conv_all = [&](const std::vector<CXCursor>& v) {
            json arr = json::array();
            for (auto& x : v) arr.push_back(node(x));
            return arr;
        };
        auto add_attrs = [&](json& inner) {
            for (auto& a : attrs) inner.push_back({{"kind", attr_kind(a)}, {"range", {{"begin", loc_json(extent(a).first)}}}});
        };

        std::string dk = decl_kind(k, cxx);
        if (!dk.empty()) {
            n["kind"] = dk;
            n["id"] = id_of(c);
            n["name"] = str(A.clang_getCursorSpelling(c));
            if (k != CXCursor_Namespace && k != CXCursor_LinkageSpec) n["type"] = decl_type_json(c);
            if (k == CXCursor_VarDecl) {
                auto sc = A.clang_Cursor_getStorageClass(c);
                if (sc == CX_SC_Static) n["storageClass"] = "static";
                else if (sc == CX_SC_Extern) n["storageClass"] = "extern";
                else if (sc == CX_SC_Register) n["storageClass"] = "register";
                // Array bounds are children too: only what follows the
                // declarator is the initialiser.
                auto from = after_declarator(c);
                std::vector<CXCursor> init;
                for (auto& x : ch) {
                    if (!is_expr_kind(A.clang_getCursorKind(x))) continue;
                    if (from && extent(x).first.offset < *from && extent(x).first.file == b.file) continue;
                    init.push_back(x);
                }
                json inner = conv_all(init);
                if (!init.empty()) n["init"] = "c";
                add_attrs(inner);
                if (!inner.empty()) n["inner"] = std::move(inner);
                return n;
            }
            if (k == CXCursor_EnumConstantDecl) {
                n["value"] = A.clang_getEnumConstantDeclValue(c);
                return n;
            }
            if (k == CXCursor_CXXMethod || k == CXCursor_Destructor) {
                if (A.clang_CXXMethod_isVirtual(c)) n["virtual"] = true;
                if (A.clang_CXXMethod_isPureVirtual(c)) n["pure"] = true;
            }
            if (k == CXCursor_StructDecl || k == CXCursor_ClassDecl) note_record(A.clang_getCursorType(c));
            if (k == CXCursor_FunctionTemplate) {
                // Dump shape: FunctionTemplateDecl { params..., FunctionDecl { parms, body } }.
                json fn = n;
                fn["kind"] = decl_kind(A.clang_getTemplateCursorKind(c), cxx);
                json params = json::array(), body = json::array();
                for (auto& x : ch) {
                    auto xk = A.clang_getCursorKind(x);
                    if (xk == CXCursor_TemplateTypeParameter || xk == CXCursor_NonTypeTemplateParameter ||
                        xk == CXCursor_TemplateTemplateParameter)
                        params.push_back(node(x));
                    else
                        body.push_back(node(x));
                }
                add_attrs(body);
                fn["inner"] = std::move(body);
                params.push_back(std::move(fn));
                n["inner"] = std::move(params);
                return n;
            }
            if (k == CXCursor_ClassTemplate || k == CXCursor_ClassTemplatePartialSpecialization) {
                json rec = n;
                rec["kind"] = "CXXRecordDecl";
                json params = json::array(), members = json::array();
                for (auto& x : ch) {
                    auto xk = A.clang_getCursorKind(x);
                    if (xk == CXCursor_TemplateTypeParameter || xk == CXCursor_NonTypeTemplateParameter ||
                        xk == CXCursor_TemplateTemplateParameter)
                        params.push_back(node(x));
                    else
                        members.push_back(node(x));
                }
                rec["inner"] = std::move(members);
                params.push_back(std::move(rec));
                n["inner"] = std::move(params);
                return n;
            }
            if (k == CXCursor_LabelStmt) n["kind"] = "LabelStmt";
            json inner = conv_all(ch);
            add_attrs(inner);
            if (!inner.empty()) n["inner"] = std::move(inner);
            return n;
        }

        if (is_expr_kind(k)) n["type"] = type_json(A.clang_getCursorType(c));

        switch (k) {
            case CXCursor_UnexposedExpr: {
                if (ch.size() == 1 && is_expr_kind(A.clang_getCursorKind(ch[0]))) {
                    n["kind"] = "ImplicitCastExpr";
                    n["castKind"] = cast_kind(c, ch[0]);
                } else {
                    n["kind"] = "UnexposedExpr";
                }
                break;
            }
            case CXCursor_DeclRefExpr: {
                n["kind"] = "DeclRefExpr";
                auto r = A.clang_getCursorReferenced(c);
                if (!A.clang_Cursor_isNull(r)) {
                    auto rk = A.clang_getCursorKind(r);
                    std::string rkn = decl_kind(rk, cxx);
                    if (rkn.empty()) rkn = str(A.clang_getCursorKindSpelling(rk));
                    n["referencedDecl"] = {{"id", id_of(r)},
                                           {"kind", rkn},
                                           {"name", str(A.clang_getCursorSpelling(r))},
                                           {"type", decl_type_json(r)}};
                    if (rk == CXCursor_EnumConstantDecl) note_enum(r);
                    else note_facts(r);
                    n["valueCategory"] = rk == CXCursor_EnumConstantDecl ? "prvalue" : "lvalue";
                } else {
                    n["dependent"] = true;
                }
                break;
            }
            case CXCursor_MemberRefExpr: {
                n["kind"] = "MemberExpr";
                n["name"] = str(A.clang_getCursorSpelling(c));
                auto r = A.clang_getCursorReferenced(c);
                if (!A.clang_Cursor_isNull(r)) {
                    n["referencedMemberDecl"] = id_of(r);
                    note_facts(r);
                } else {
                    n["dependent"] = true;
                }
                if (ch.empty()) {
                    n["isArrow"] = true;
                    n["inner"] = json::array({{{"kind", "CXXThisExpr"}, {"implicit", true}, {"range", n["range"]}}});
                    return n;
                }
                auto bt = A.clang_getCanonicalType(A.clang_getCursorType(ch[0]));
                n["isArrow"] = bt.kind == CXType_Pointer;
                break;
            }
            case CXCursor_CallExpr: {
                auto r = A.clang_getCursorReferenced(c);
                auto rk = A.clang_Cursor_isNull(r) ? CXCursor_NoDeclFound : A.clang_getCursorKind(r);
                if (rk == CXCursor_Constructor) {
                    n["kind"] = "CXXConstructExpr";
                    n["valueCategory"] = "prvalue";
                    break;
                }
                auto rname = A.clang_Cursor_isNull(r) ? std::string() : str(A.clang_getCursorSpelling(r));
                if (!A.clang_Cursor_isNull(r)) {
                    note_facts(r);
                    auto rt = A.clang_getCursorResultType(r);
                    n["valueCategory"] = rt.kind == CXType_LValueReference   ? "lvalue"
                                         : rt.kind == CXType_RValueReference ? "xvalue"
                                                                             : "prvalue";
                }
                if (!ch.empty() && A.clang_getCursorKind(ch[0]) == CXCursor_MemberRefExpr) {
                    n["kind"] = "CXXMemberCallExpr";
                    break;
                }
                if (rname.starts_with("operator") && ch.size() >= 2) {
                    // Source order (a, operator=, b) -> dump order (callee, a, b).
                    for (std::size_t i = 0; i < ch.size(); ++i) {
                        auto ck = A.clang_getCursorKind(ch[i]);
                        auto cr = A.clang_getCursorReferenced(ch[i]);
                        if ((ck == CXCursor_UnexposedExpr || ck == CXCursor_DeclRefExpr) &&
                            !A.clang_Cursor_isNull(cr) && id_of(cr) == id_of(r) &&
                            str(A.clang_getCursorSpelling(ch[i])) == rname) {
                            auto callee = ch[i];
                            ch.erase(ch.begin() + static_cast<long>(i));
                            ch.insert(ch.begin(), callee);
                            n["kind"] = "CXXOperatorCallExpr";
                            n["operator"] = rname.substr(8);
                            break;
                        }
                    }
                    if (n.contains("kind")) break;
                }
                n["kind"] = "CallExpr";
                if (A.clang_Cursor_isNull(r)) n["dependent"] = true;
                break;
            }
            case CXCursor_IntegerLiteral:
            case CXCursor_CharacterLiteral: {
                n["kind"] = k == CXCursor_IntegerLiteral ? "IntegerLiteral" : "CharacterLiteral";
                n["valueCategory"] = "prvalue";
                if (auto ev = A.clang_Cursor_Evaluate(c)) {
                    if (A.clang_EvalResult_getKind(ev) == CXEval_Int) {
                        if (A.clang_EvalResult_isUnsignedInt(ev))
                            n["value"] = std::to_string(A.clang_EvalResult_getAsUnsigned(ev));
                        else
                            n["value"] = std::to_string(A.clang_EvalResult_getAsLongLong(ev));
                    }
                    A.clang_EvalResult_dispose(ev);
                }
                if (k == CXCursor_CharacterLiteral && n.contains("value"))
                    n["value"] = std::stoll(n["value"].get<std::string>());
                break;
            }
            case CXCursor_FloatingLiteral: {
                n["kind"] = "FloatingLiteral";
                n["valueCategory"] = "prvalue";
                if (auto ev = A.clang_Cursor_Evaluate(c)) {
                    if (A.clang_EvalResult_getKind(ev) == CXEval_Float)
                        n["value"] = std::to_string(A.clang_EvalResult_getAsDouble(ev));
                    A.clang_EvalResult_dispose(ev);
                }
                break;
            }
            case CXCursor_StringLiteral: {
                n["kind"] = "StringLiteral";
                n["valueCategory"] = "lvalue";
                n["value"] = str(A.clang_getCursorSpelling(c));
                if (auto ev = A.clang_Cursor_Evaluate(c)) {
                    if (A.clang_EvalResult_getKind(ev) == CXEval_StrLiteral) {
                        const char* s = A.clang_EvalResult_getAsStr(ev);
                        if (s) n["str"] = std::string(s);
                    }
                    A.clang_EvalResult_dispose(ev);
                }
                break;
            }
            case CXCursor_BinaryOperator:
            case CXCursor_CompoundAssignOperator: {
                n["kind"] = k == CXCursor_BinaryOperator ? "BinaryOperator" : "CompoundAssignOperator";
                n["opcode"] = str(A.clang_getBinaryOperatorKindSpelling(A.clang_getCursorBinaryOperatorKind(c)));
                break;
            }
            case CXCursor_UnaryOperator: {
                n["kind"] = "UnaryOperator";
                auto uk = A.clang_getCursorUnaryOperatorKind(c);
                n["opcode"] = str(A.clang_getUnaryOperatorKindSpelling(uk));
                if (uk == CXUnaryOperator_PostInc || uk == CXUnaryOperator_PostDec) n["isPostfix"] = true;
                n["valueCategory"] = (uk == CXUnaryOperator_Deref) ? "lvalue" : "prvalue";
                break;
            }
            case CXCursor_UnaryExpr: {
                n["kind"] = "UnaryExprOrTypeTraitExpr";
                auto t = text_at(b, e);
                std::string nm = "sizeof";
                for (auto* w : {"sizeof", "_Alignof", "alignof", "__alignof__", "__alignof"})
                    if (t.starts_with(w)) nm = w == std::string_view("sizeof") ? "sizeof" : "alignof";
                if (t.empty() || in_macro(b)) nm = "unknown";
                n["name"] = nm;
                bool has_expr = false;
                for (auto& x : ch) has_expr = has_expr || is_expr_kind(A.clang_getCursorKind(x));
                if (!has_expr) n["argType"] = json::object();
                break;
            }
            case CXCursor_CXXNewExpr: {
                n["kind"] = "CXXNewExpr";
                n["valueCategory"] = "prvalue";
                auto t = text_at(b, e);
                if (!t.empty() && !in_macro(b)) {
                    // new T[n] / new T[n]{...}: a '[' before any '(' or '{' after the type.
                    auto p = t.find("new");
                    bool arr = false;
                    if (p != std::string_view::npos) {
                        std::size_t i = p + 3;
                        while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i]))) ++i;
                        if (i < t.size() && t[i] == '(') {  // placement arguments
                            int d = 0;
                            for (; i < t.size(); ++i) {
                                if (t[i] == '(') ++d;
                                else if (t[i] == ')' && --d == 0) {
                                    ++i;
                                    break;
                                }
                            }
                        }
                        for (; i < t.size(); ++i) {
                            if (t[i] == '[') {
                                arr = true;
                                break;
                            }
                            if (t[i] == '(' || t[i] == '{') break;
                        }
                    }
                    n["isArray"] = arr;
                } else {
                    n["arrayUnknown"] = true;
                }
                break;
            }
            case CXCursor_CXXDeleteExpr: {
                n["kind"] = "CXXDeleteExpr";
                auto t = text_at(b, e);
                if (!t.empty() && !in_macro(b)) {
                    std::size_t i = t.find("delete");
                    i = i == std::string_view::npos ? t.size() : i + 6;
                    while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i]))) ++i;
                    n["isArrayAsWritten"] = i < t.size() && t[i] == '[';
                } else {
                    n["arrayUnknown"] = true;
                }
                if (!ch.empty()) note_record(A.clang_getPointeeType(A.clang_getCursorType(ch[0])));
                break;
            }
            case CXCursor_CXXBoolLiteralExpr: {
                n["kind"] = "CXXBoolLiteralExpr";
                n["valueCategory"] = "prvalue";
                n["value"] = text_at(b, e) == "true";
                break;
            }
            case CXCursor_IfStmt:
            case CXCursor_SwitchStmt: {
                n["kind"] = k == CXCursor_IfStmt ? "IfStmt" : "SwitchStmt";
                json inner = json::array();
                std::size_t i = 0;
                if (ch.size() >= 3) {
                    auto k0 = A.clang_getCursorKind(ch[0]);
                    if (k0 == CXCursor_DeclStmt || (is_expr_kind(k0) && next_char_after(ch[0]) == ';')) {
                        n["hasInit"] = true;
                        inner.push_back(node(ch[0]));
                        i = 1;
                    }
                }
                if (i < ch.size() && A.clang_getCursorKind(ch[i]) == CXCursor_VarDecl) {
                    n["hasVar"] = true;
                    inner.push_back(wrap_declstmt(node(ch[i])));
                    ++i;
                }
                for (; i < ch.size(); ++i) inner.push_back(node(ch[i]));
                if (k == CXCursor_IfStmt && inner.size() >= 3 + (n.value("hasInit", false) ? 1 : 0) +
                                                               (n.value("hasVar", false) ? 1 : 0))
                    n["hasElse"] = true;
                n["inner"] = std::move(inner);
                return n;
            }
            case CXCursor_WhileStmt: {
                n["kind"] = "WhileStmt";
                json inner = json::array();
                std::size_t i = 0;
                if (!ch.empty() && A.clang_getCursorKind(ch[0]) == CXCursor_VarDecl) {
                    n["hasVar"] = true;
                    inner.push_back(wrap_declstmt(node(ch[0])));
                    i = 1;
                }
                for (; i < ch.size(); ++i) inner.push_back(node(ch[i]));
                n["inner"] = std::move(inner);
                return n;
            }
            case CXCursor_ForStmt: {
                n["kind"] = "ForStmt";
                auto semis = for_semis(c);
                if (!semis || ch.empty()) {
                    n["slotsUnknown"] = true;
                    break;
                }
                json slots = json::array({json::object(), json::object(), json::object(), json::object()});
                for (std::size_t i = 0; i + 1 < ch.size(); ++i) {
                    auto off = extent(ch[i]).first.offset;
                    int slot = off < semis->first ? 0 : off < semis->second ? 2 : 3;
                    if (slot == 2 && A.clang_getCursorKind(ch[i]) == CXCursor_VarDecl) {
                        slots[1] = wrap_declstmt(node(ch[i]));
                        continue;
                    }
                    slots[static_cast<std::size_t>(slot)] = node(ch[i]);
                }
                slots.push_back(node(ch.back()));
                n["inner"] = std::move(slots);
                return n;
            }
            case CXCursor_CaseStmt: {
                n["kind"] = "CaseStmt";
                json inner = json::array();
                if (ch.size() >= 3) {
                    auto between = text_at(extent(ch[0]).second, extent(ch[1]).first);
                    if (between.find("...") != std::string_view::npos) n["isGNURange"] = true;
                }
                std::size_t labels = n.value("isGNURange", false) ? 2 : 1;
                for (std::size_t i = 0; i < ch.size(); ++i) {
                    json x = node(ch[i]);
                    if (i < labels) {
                        json ce = {{"kind", "ConstantExpr"}, {"range", x["range"]}, {"inner", json::array({x})}};
                        if (x.contains("type")) ce["type"] = x["type"];
                        if (auto ev = A.clang_Cursor_Evaluate(ch[i])) {
                            if (A.clang_EvalResult_getKind(ev) == CXEval_Int)
                                ce["value"] = std::to_string(A.clang_EvalResult_getAsLongLong(ev));
                            A.clang_EvalResult_dispose(ev);
                        }
                        inner.push_back(std::move(ce));
                    } else {
                        inner.push_back(std::move(x));
                    }
                }
                n["inner"] = std::move(inner);
                return n;
            }
            case CXCursor_UnexposedStmt: {
                // [[fallthrough]]; and other attributed statements.
                auto t = text_at(b, e);
                if ((t.starts_with("[[") || t.starts_with("__attribute__")) && !in_macro(b)) {
                    n["kind"] = "AttributedStmt";
                    json inner = json::array();
                    auto close = t.find(t.starts_with("[[") ? "]]" : "))");
                    auto head = t.substr(0, close == std::string_view::npos ? t.size() : close);
                    if (head.find("fallthrough") != std::string_view::npos) inner.push_back({{"kind", "FallThroughAttr"}});
                    else inner.push_back({{"kind", "UnexposedAttr"}});
                    for (auto& x : ch) inner.push_back(node(x));
                    n["inner"] = std::move(inner);
                    return n;
                }
                n["kind"] = "UnexposedStmt";
                break;
            }
            case CXCursor_AsmStmt: n["kind"] = "GCCAsmStmt"; break;
            case CXCursor_MSAsmStmt: n["kind"] = "MSAsmStmt"; break;
            case CXCursor_CXXThisExpr: n["kind"] = "CXXThisExpr"; break;
            case CXCursor_CXXNullPtrLiteralExpr: n["kind"] = "CXXNullPtrLiteralExpr"; n["valueCategory"] = "prvalue"; break;
            case CXCursor_GNUNullExpr: n["kind"] = "GNUNullExpr"; break;
            case CXCursor_ConditionalOperator: n["kind"] = "ConditionalOperator"; break;
            case CXCursor_CStyleCastExpr: n["kind"] = "CStyleCastExpr"; break;
            case CXCursor_ParenExpr: n["kind"] = "ParenExpr"; break;
            case CXCursor_ArraySubscriptExpr: n["kind"] = "ArraySubscriptExpr"; n["valueCategory"] = "lvalue"; break;
            case CXCursor_LambdaExpr: n["kind"] = "LambdaExpr"; break;
            case CXCursor_InitListExpr: n["kind"] = "InitListExpr"; break;
            case CXCursor_StmtExpr: n["kind"] = "StmtExpr"; break;
            case CXCursor_CXXThrowExpr: n["kind"] = "CXXThrowExpr"; break;
            case CXCursor_CXXTryStmt: n["kind"] = "CXXTryStmt"; break;
            case CXCursor_CXXCatchStmt: n["kind"] = "CXXCatchStmt"; break;
            case CXCursor_CXXForRangeStmt: n["kind"] = "CXXForRangeStmt"; break;
            case CXCursor_CompoundStmt: n["kind"] = "CompoundStmt"; break;
            case CXCursor_DeclStmt: n["kind"] = "DeclStmt"; break;
            case CXCursor_ReturnStmt: n["kind"] = "ReturnStmt"; break;
            case CXCursor_BreakStmt: n["kind"] = "BreakStmt"; break;
            case CXCursor_ContinueStmt: n["kind"] = "ContinueStmt"; break;
            case CXCursor_GotoStmt: n["kind"] = "GotoStmt"; break;
            case CXCursor_IndirectGotoStmt: n["kind"] = "IndirectGotoStmt"; break;
            case CXCursor_NullStmt: n["kind"] = "NullStmt"; break;
            case CXCursor_DefaultStmt: n["kind"] = "DefaultStmt"; break;
            case CXCursor_DoStmt: n["kind"] = "DoStmt"; break;
            case CXCursor_CXXStaticCastExpr: n["kind"] = "CXXStaticCastExpr"; break;
            case CXCursor_CXXDynamicCastExpr: n["kind"] = "CXXDynamicCastExpr"; break;
            case CXCursor_CXXReinterpretCastExpr: n["kind"] = "CXXReinterpretCastExpr"; break;
            case CXCursor_CXXConstCastExpr: n["kind"] = "CXXConstCastExpr"; break;
            case CXCursor_CXXFunctionalCastExpr: n["kind"] = "CXXFunctionalCastExpr"; break;
            case CXCursor_CXXTypeidExpr: n["kind"] = "CXXTypeidExpr"; break;
            case CXCursor_PackExpansionExpr: n["kind"] = "PackExpansionExpr"; n["dependent"] = true; break;
            case CXCursor_SizeOfPackExpr: n["kind"] = "SizeOfPackExpr"; n["dependent"] = true; break;
            default: {
                n["kind"] = str(A.clang_getCursorKindSpelling(k));
                break;
            }
        }
        json inner = conv_all(ch);
        add_attrs(inner);
        if (!inner.empty()) n["inner"] = std::move(inner);
        return n;
    }
};

std::string first_error(const Api& A, CXTranslationUnit tu) {
    for (unsigned i = 0, n = A.clang_getNumDiagnostics(tu); i < n; ++i) {
        auto d = A.clang_getDiagnostic(tu, i);
        std::string msg;
        if (A.clang_getDiagnosticSeverity(d) >= CXDiagnostic_Error) {
            auto s = A.clang_formatDiagnostic(d, CXDiagnostic_DisplaySourceLocation | CXDiagnostic_DisplayColumn);
            const char* c = A.clang_getCString(s);
            msg = c ? c : "error";
            A.clang_disposeString(s);
        }
        A.clang_disposeDiagnostic(d);
        if (!msg.empty()) return msg;
    }
    return {};
}

}  // namespace

bool libclang_load(const std::optional<fs::path>& clang, std::string* why, std::string* where) {
    auto& L = loaded(clang);
    if (why) *why = L.why;
    if (where) *where = L.where;
    return L.ok;
}

std::optional<Unit> libclang_unit(const std::vector<std::string>& args, const std::string& src,
                                  const FileSet& files, bool cxx, std::string& err) {
    auto& L = loaded(std::nullopt);
    if (!L.ok) {
        err = L.why;
        return std::nullopt;
    }
    const Api& A = L.api;
    std::vector<const char*> argv;
    for (auto& a : args) argv.push_back(a.c_str());
    CXIndex idx = A.clang_createIndex(0, 0);
    CXTranslationUnit tu = nullptr;
    unsigned opts = CXTranslationUnit_DetailedPreprocessingRecord | CXTranslationUnit_IgnoreNonErrorsFromIncludedFiles;
    auto rc = A.clang_parseTranslationUnit2(idx, src.c_str(), argv.data(), static_cast<int>(argv.size()), nullptr,
                                            0, opts, &tu);
    if (rc != CXError_Success || !tu) {
        err = rc == CXError_Crashed ? "libclang crashed while parsing" : "libclang could not parse the unit";
        if (tu) A.clang_disposeTranslationUnit(tu);
        A.clang_disposeIndex(idx);
        return std::nullopt;
    }
    if (auto e = first_error(A, tu); !e.empty()) {
        err = "does not parse: " + e;
        A.clang_disposeTranslationUnit(tu);
        A.clang_disposeIndex(idx);
        return std::nullopt;
    }
    // Released on every exit, a conversion that throws included.
    struct Release {
        const Api& A;
        CXIndex idx;
        CXTranslationUnit tu;
        ~Release() {
            A.clang_disposeTranslationUnit(tu);
            A.clang_disposeIndex(idx);
        }
    } release{A, idx, tu};
    Unit u;
    u.cxx = cxx;
    u.backend = "libclang";
    Conv cv{A, tu, files, cxx, u, {}, {}, {}, {}};
    auto top = cv.kids(A.clang_getTranslationUnitCursor(tu));
    // Pass 1: macro expansion ranges in the checked files.
    for (auto& c : top) {
        if (A.clang_getCursorKind(c) != CXCursor_MacroExpansion) continue;
        auto [b, e] = cv.extent(c);
        if (!b.file || !b.file->wanted || b.file != e.file) continue;
        b.file->macros.emplace_back(b.offset, std::max(e.offset, b.offset + 1));
    }
    for (auto& [f, ft] : cv.ftext) std::sort(ft.macros.begin(), ft.macros.end());
    // Pass 2: declarations of the checked files.
    for (auto& c : top) {
        auto k = A.clang_getCursorKind(c);
        if (!A.clang_isDeclaration(k) && k != CXCursor_LinkageSpec) continue;
        auto w = cv.where(A.clang_getCursorLocation(c));
        if (!w.file || !w.file->wanted) continue;
        u.decls.push_back(cv.node(c));
    }
    return u;
}

#endif  // PRISM_HAS_LIBCLANG

}  // namespace prism::astlint::detail
