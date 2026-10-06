// bookbrainz — BookBrainz WS terminal client.
//
// Same architecture as musicbrainz.cpp: every entity is a tag type
// carrying endpoint / from / to_json; dispatch is derived from the
// type. Adding an entity = one line in AllEntities.
//
// API surface mirrors bookbrainz-site/src/api (routes + swagger):
//   lookup:  GET /1/<entity>/<bbid>
//   browse:  GET /1/<entity>?<linked>=<bbid> (exactly one linked entity)
//   search:  GET /1/search?q=..&type=..&size=..&from=..
//
// Zero external C deps: transport is popen("curl"), JSON is the vendored
// nlohmann/json. Human-readable terminal output by default, --json for
// machine-readable output.

#include "json.hpp"

#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

// CLI errors: message to stderr, nonzero exit. Usage errors exit 2,
// request/runtime failures exit 1.
struct CliError : std::runtime_error {
    int code;
    CliError(int c, const std::string& m) : std::runtime_error(m), code(c) {}
};

namespace bookbrainz {

// ---------------------------------------------------------------------------
// URL helpers
// ---------------------------------------------------------------------------
static std::string uesc(const std::string& s) {
    std::ostringstream o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            o << c;
        else
            o << '%' << std::hex << std::uppercase << std::setw(2)
              << std::setfill('0') << static_cast<int>(c);
    }
    return o.str();
}

using PMap = std::map<std::string, std::string>;

static std::string qs(const PMap& p) {
    std::string r;
    for (auto i = p.begin(); i != p.end(); ++i) {
        if (i != p.begin()) r += "&";
        r += i->first + "=" + uesc(i->second);
    }
    return r;
}

static std::string bpath(const std::string& path, const PMap& p = {}) {
    std::string u = "/1/" + path;
    if (!p.empty()) u += "?" + qs(p);
    return u;
}

// ---------------------------------------------------------------------------
// HTTP + self rate limiting (well inside the 100req/5min server policy)
// ---------------------------------------------------------------------------
namespace {
std::mutex rl_mtx;
auto rl_last = std::chrono::steady_clock::now() - std::chrono::seconds(2);
} // namespace

struct CurlPipe {
    FILE* f = nullptr;
    int rc = -1;
    explicit CurlPipe(const std::string& cmd) : f(popen(cmd.c_str(), "r")) {
        if (!f) throw std::runtime_error("popen failed");
    }
    CurlPipe(const CurlPipe&) = delete;
    CurlPipe& operator=(const CurlPipe&) = delete;
    ~CurlPipe() {
        if (f) pclose(f);
    }
    std::string read_all() {
        std::array<char, 4096> b;
        std::string d;
        while (fgets(b.data(), b.size(), f)) d += b.data();
        return d;
    }
    int finish() {
        if (f) {
            rc = pclose(f);
            f = nullptr;
        }
        return rc;
    }
};

static std::string bb_get(const std::string& path) {
    {
        std::lock_guard<std::mutex> l(rl_mtx);
        auto n = std::chrono::steady_clock::now();
        auto e = std::chrono::duration_cast<std::chrono::milliseconds>(n - rl_last).count();
        if (e < 1000)
            std::this_thread::sleep_for(std::chrono::milliseconds(1000 - e));
        rl_last = std::chrono::steady_clock::now();
    }
    const std::string url = std::string("https://api.bookbrainz.org") + path;
    const std::string cmd =
        "curl -s -f -H 'User-Agent: bookbrainz-cli/0.1.0 (+https://github.com/ningxilai/emacs-stdio-jsonrpc)' '" +
        url + "'";
    CurlPipe p(cmd);
    std::string body = p.read_all();
    if (p.finish() != 0)
        throw CliError(1, "request failed: " + url);
    return body;
}

// ---------------------------------------------------------------------------
// Null-tolerant JSON accessors (API returns explicit nulls)
// ---------------------------------------------------------------------------
static std::string sstr(const json& j, const char* k) {
    auto it = j.find(k);
    if (it == j.end() || it->is_null()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    if (it->is_number_unsigned())
        return std::to_string(it->get<unsigned long long>());
    if (it->is_boolean()) return it->get<bool>() ? "true" : "false";
    return {};
}

// Reads a field that the API returns either as a bare string or as
// an object carrying the display value under "name" (BB is inconsistent
// across entities: authorType/gender/areas are strings, *Type fields vary).
static std::string sstr_or_name(const json& j, const char* k) {
    auto it = j.find(k);
    if (it == j.end() || it->is_null()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_object()) return sstr(*it, "name");
    return {};
}

static int as_int(const json& j, const char* k, int d = 0) {
    auto it = j.find(k);
    if (it == j.end() || it->is_null()) return d;
    if (it->is_number()) return static_cast<int>(it->get<long long>());
    if (it->is_string()) {
        try {
            return std::stoi(it->get<std::string>());
        } catch (...) {
            return d;
        }
    }
    return d;
}

static bool sbool(const json& j, const char* k, bool d = false) {
    auto it = j.find(k);
    if (it == j.end() || it->is_null()) return d;
    if (it->is_boolean()) return it->get<bool>();
    return d;
}

// ---------------------------------------------------------------------------
// Entity tag types. defaultAlias {name, sortName} is the display handle
// across all BookBrainz entities.
// ---------------------------------------------------------------------------
static std::string alias_name(const json& j) {
    if (j.contains("defaultAlias") && j["defaultAlias"].is_object())
        return sstr(j["defaultAlias"], "name");
    return {};
}

static std::string alias_sort(const json& j) {
    if (j.contains("defaultAlias") && j["defaultAlias"].is_object())
        return sstr(j["defaultAlias"], "sortName");
    return {};
}

struct Author {
    static constexpr std::string_view endpoint = "author";
    static constexpr bool searchable = true;
    static constexpr bool lookable = true;
    static constexpr bool browsable = true;
    // Mirrors validateBrowseRequestQueryParameters in routes/author.js.
    static constexpr std::array<std::string_view, 6> browse_links = {
        "edition", "author", "series", "edition-group", "work", "publisher"};

    std::string bbid, name, sort_name, type, disambiguation, gender;
    std::string begin_date, end_date, begin_area, end_area;
    bool ended = false;
    static Author from(const json& j) {
        Author a;
        a.bbid = sstr(j, "bbid");
        a.name = alias_name(j);
        a.sort_name = alias_sort(j);
        a.type = sstr(j, "authorType");
        if (a.type.empty()) a.type = sstr(j, "type");
        a.disambiguation = sstr(j, "disambiguation");
        a.gender = sstr_or_name(j, "gender");
        a.begin_date = sstr(j, "beginDate");
        a.end_date = sstr(j, "endDate");
        a.ended = sbool(j, "ended");
        a.begin_area = sstr_or_name(j, "beginArea");
        a.end_area = sstr_or_name(j, "endArea");
        return a;
    }
    json to_json() const {
        return {{"bbid", bbid},
                {"name", name},
                {"sort-name", sort_name},
                {"type", type},
                {"disambiguation", disambiguation},
                {"gender", gender},
                {"begin-date", begin_date},
                {"end-date", end_date},
                {"ended", ended},
                {"begin-area", begin_area},
                {"end-area", end_area}};
    }
};

struct EditionCredit {
    std::string author_bbid, author_name;
    static EditionCredit from(const json& j) {
        EditionCredit c;
        if (j.contains("author") && j["author"].is_object()) {
            c.author_bbid = sstr(j["author"], "bbid");
            c.author_name = alias_name(j["author"]);
        }
        if (c.author_name.empty()) c.author_name = sstr(j, "name");
        return c;
    }
    json to_json() const {
        return {{"author-bbid", author_bbid}, {"author-name", author_name}};
    }
};

struct Edition {
    static constexpr std::string_view endpoint = "edition";
    static constexpr bool searchable = true;
    static constexpr bool lookable = true;
    static constexpr bool browsable = true;
    // Mirrors validateBrowseRequestQueryParameters in routes/edition.js.
    static constexpr std::array<std::string_view, 6> browse_links = {
        "author", "edition", "edition-group", "series", "work", "publisher"};

    std::string bbid, name, sort_name, disambiguation, format, status;
    std::string release_date;
    int pages = 0, depth = 0, height = 0, width = 0, weight = 0;
    std::vector<std::string> languages;
    std::vector<EditionCredit> authors;
    std::vector<std::string> search_authors; // search hits carry plain names
    std::vector<std::string> publisher_ids, publisher_names;
    static Edition from(const json& j) {
        Edition e;
        e.bbid = sstr(j, "bbid");
        e.name = alias_name(j);
        e.sort_name = alias_sort(j);
        e.disambiguation = sstr(j, "disambiguation");
        e.format = sstr(j, "editionFormat");
        e.status = sstr(j, "status");
        e.release_date = sstr(j, "releaseEventDate");
        e.pages = as_int(j, "pages");
        e.depth = as_int(j, "depth");
        e.height = as_int(j, "height");
        e.width = as_int(j, "width");
        e.weight = as_int(j, "weight");
        if (j.contains("languages") && j["languages"].is_array())
            for (const auto& l : j["languages"]) {
                if (l.is_string())
                    e.languages.push_back(l.get<std::string>());
                else if (l.is_object() && l.contains("name"))
                    e.languages.push_back(sstr(l, "name"));
            }
        if (j.contains("authorCredits") && j["authorCredits"].is_array())
            for (const auto& c : j["authorCredits"])
                e.authors.push_back(EditionCredit::from(c));
        if (j.contains("authors") && j["authors"].is_array())
            for (const auto& a : j["authors"])
                if (a.is_string()) e.search_authors.push_back(a.get<std::string>());
        if (j.contains("publishers") && j["publishers"].is_array())
            for (const auto& p : j["publishers"]) {
                e.publisher_ids.push_back(sstr(p, "bbid"));
                std::string nm = alias_name(p);
                if (nm.empty()) nm = sstr(p, "name");
                e.publisher_names.push_back(nm);
            }
        return e;
    }
    json to_json() const {
        json o = {{"bbid", bbid},
                  {"name", name},
                  {"sort-name", sort_name},
                  {"disambiguation", disambiguation},
                  {"format", format},
                  {"status", status},
                  {"release-date", release_date},
                  {"pages", pages},
                  {"languages", languages}};
        json ca = json::array();
        for (const auto& c : authors) ca.push_back(c.to_json());
        o["author-credits"] = std::move(ca);
        o["authors"] = search_authors;
        json pa = json::array();
        for (size_t i = 0; i < publisher_ids.size(); ++i)
            pa.push_back({{"bbid", publisher_ids[i]},
                          {"name", i < publisher_names.size() ? publisher_names[i] : ""}});
        o["publishers"] = std::move(pa);
        return o;
    }
};

struct EditionGroup {
    static constexpr std::string_view endpoint = "edition-group";
    static constexpr bool searchable = true;
    static constexpr bool lookable = true;
    static constexpr bool browsable = true;
    // Mirrors routes/edition-group.js (edition + series only).
    static constexpr std::array<std::string_view, 2> browse_links = {"edition",
                                                                     "series"};

    std::string bbid, name, sort_name, type, disambiguation;
    std::vector<std::string> search_authors; // search hits carry plain names
    static EditionGroup from(const json& j) {
        EditionGroup g;
        g.bbid = sstr(j, "bbid");
        g.name = alias_name(j);
        g.sort_name = alias_sort(j);
        g.type = sstr(j, "type");
        if (g.type.empty()) g.type = sstr_or_name(j, "editionGroupType");
        g.disambiguation = sstr(j, "disambiguation");
        if (j.contains("authors") && j["authors"].is_array())
            for (const auto& a : j["authors"])
                if (a.is_string()) g.search_authors.push_back(a.get<std::string>());
        return g;
    }
    json to_json() const {
        return {{"bbid", bbid},
                {"name", name},
                {"sort-name", sort_name},
                {"type", type},
                {"disambiguation", disambiguation},
                {"authors", search_authors}};
    }
};

struct Publisher {
    static constexpr std::string_view endpoint = "publisher";
    static constexpr bool searchable = true;
    static constexpr bool lookable = true;
    static constexpr bool browsable = true;
    // Mirrors routes/publisher.js.
    static constexpr std::array<std::string_view, 5> browse_links = {
        "author", "edition", "series", "work", "publisher"};

    std::string bbid, name, sort_name, type, disambiguation, begin_date, end_date;
    std::string area;
    bool ended = false;
    static Publisher from(const json& j) {
        Publisher p;
        p.bbid = sstr(j, "bbid");
        p.name = alias_name(j);
        p.sort_name = alias_sort(j);
        p.type = sstr(j, "type");
        if (p.type.empty()) p.type = sstr_or_name(j, "publisherType");
        p.disambiguation = sstr(j, "disambiguation");
        p.begin_date = sstr(j, "beginDate");
        p.end_date = sstr(j, "endDate");
        p.ended = sbool(j, "ended");
        p.area = sstr_or_name(j, "area");
        return p;
    }
    json to_json() const {
        return {{"bbid", bbid},
                {"name", name},
                {"sort-name", sort_name},
                {"type", type},
                {"disambiguation", disambiguation},
                {"begin-date", begin_date},
                {"end-date", end_date},
                {"ended", ended},
                {"area", area}};
    }
};

struct Series {
    static constexpr std::string_view endpoint = "series";
    static constexpr bool searchable = true;
    static constexpr bool lookable = true;
    static constexpr bool browsable = true;
    // Mirrors routes/series.js.
    static constexpr std::array<std::string_view, 5> browse_links = {
        "edition", "author", "edition-group", "work", "publisher"};

    std::string bbid, name, sort_name, type, disambiguation, ordering;
    static Series from(const json& j) {
        Series s;
        s.bbid = sstr(j, "bbid");
        s.name = alias_name(j);
        s.sort_name = alias_sort(j);
        s.type = sstr(j, "type");
        if (s.type.empty()) s.type = sstr_or_name(j, "seriesType");
        s.disambiguation = sstr(j, "disambiguation");
        s.ordering = sstr(j, "seriesOrderingType");
        return s;
    }
    json to_json() const {
        return {{"bbid", bbid},
                {"name", name},
                {"sort-name", sort_name},
                {"type", type},
                {"disambiguation", disambiguation},
                {"series-ordering-type", ordering}};
    }
};

struct Work {
    static constexpr std::string_view endpoint = "work";
    static constexpr bool searchable = true;
    static constexpr bool lookable = true;
    static constexpr bool browsable = true;
    // Mirrors routes/work.js.
    static constexpr std::array<std::string_view, 5> browse_links = {
        "author", "edition", "series", "work", "publisher"};

    std::string bbid, name, sort_name, type, disambiguation, language;
    std::vector<std::string> languages;
    static Work from(const json& j) {
        Work w;
        w.bbid = sstr(j, "bbid");
        w.name = alias_name(j);
        w.sort_name = alias_sort(j);
        w.type = sstr(j, "type");
        if (w.type.empty()) w.type = sstr_or_name(j, "workType");
        w.disambiguation = sstr(j, "disambiguation");
        w.language = sstr(j, "language");
        if (w.language.empty() && j.contains("languages") && j["languages"].is_array()) {
            for (const auto& l : j["languages"]) {
                std::string s = l.is_string() ? l.get<std::string>() : sstr(l, "name");
                if (!s.empty()) w.languages.push_back(s);
            }
            if (!w.languages.empty()) w.language = w.languages[0];
        }
        return w;
    }
    json to_json() const {
        return {{"bbid", bbid},
                {"name", name},
                {"sort-name", sort_name},
                {"type", type},
                {"disambiguation", disambiguation},
                {"language", language},
                {"languages", languages}};
    }
};

// The registry: adding an entity = one line here.
using AllEntities =
    std::tuple<Author, Edition, EditionGroup, Publisher, Series, Work>;

// ---------------------------------------------------------------------------
// Generic operations derived from the entity type. No per-entity strings.
// ---------------------------------------------------------------------------
inline int get_int(const json& p, const char* k, int d) {
    auto it = p.find(k);
    if (it == p.end() || it->is_null()) return d;
    if (it->is_number()) return static_cast<int>(it->get<long long>());
    return d;
}

inline std::string get_str(const json& p, const char* k, const std::string& d = {}) {
    auto it = p.find(k);
    if (it == p.end() || it->is_null()) return d;
    if (!it->is_string()) throw CliError(2, "expected string param");
    return it->get<std::string>();
}

// Search: GET /1/search?q=..&type=..&size=..&from=..
// Response: {resultCount, searchResult: [{bbid, defaultAlias, entityType}], totalCount}.
template <typename E>
json do_search(const json& p) {
    static_assert(E::searchable, "entity is not searchable");
    PMap pm;
    pm["q"] = get_str(p, "query");
    pm["type"] = std::string(E::endpoint);
    pm["size"] = std::to_string(get_int(p, "limit", 10));
    pm["from"] = std::to_string(get_int(p, "offset", 0));
    json raw = json::parse(bb_get(bpath("search", pm)));
    json result = {{"count", as_int(raw, "totalCount")},
                   {"offset", get_int(p, "offset", 0)}};
    json arr = json::array();
    if (raw.contains("searchResult") && raw["searchResult"].is_array())
        for (const auto& e : raw["searchResult"]) arr.push_back(E::from(e).to_json());
    result["results"] = std::move(arr);
    return result;
}

// Lookup: GET /1/<entity>/<bbid>.
template <typename E>
json do_lookup(const json& p) {
    static_assert(E::lookable, "entity is not lookable");
    json raw = json::parse(
        bb_get(bpath(std::string(E::endpoint) + "/" + get_str(p, "id"))));
    return E::from(raw).to_json();
}

// Browse: GET /1/<entity>?<linked>=<bbid>, exactly one linked entity.
template <typename E>
json do_browse(const json& p) {
    static_assert(E::browsable, "entity is not browsable");
    PMap pm;
    int found = 0;
    for (auto a : E::browse_links) {
        const std::string k(a);
        auto it = p.find(k);
        if (it != p.end() && !it->is_null()) {
            if (!it->is_string()) throw CliError(2, "browse link must be a string");
            pm[k] = it->get<std::string>();
            ++found;
        }
    }
    if (found != 1) throw CliError(2, "browse needs exactly one linked entity");
    pm["size"] = std::to_string(get_int(p, "limit", 10));
    pm["from"] = std::to_string(get_int(p, "offset", 0));
    json raw = json::parse(bb_get(bpath(std::string(E::endpoint), pm)));
    // Browse answers {bbid, <items...>}: take the first array member.
    json arr = json::array();
    if (raw.is_object())
        for (auto it = raw.begin(); it != raw.end(); ++it)
            if (it->is_array()) {
                for (const auto& e : *it) {
                    if (e.is_object() && e.contains("bbid"))
                        arr.push_back(E::from(e).to_json());
                }
                break;
            }
    long long total = (long long)arr.size();
    return {{"count", total}, {"offset", get_int(p, "offset", 0)}, {"results", std::move(arr)}};
}

// Runtime dispatch table: entity string -> compile-time entity type.
using SearchFn = json (*)(const json&);
using LookupFn = json (*)(const json&);
using BrowseFn = json (*)(const json&);

struct EntityOps {
    bool searchable = false, lookable = false, browsable = false;
    SearchFn search = nullptr;
    LookupFn lookup = nullptr;
    BrowseFn browse = nullptr;
};

template <typename E>
void add_ops(std::map<std::string, EntityOps>& m) {
    EntityOps o;
    if constexpr (E::searchable) {
        o.searchable = true;
        o.search = &do_search<E>;
    }
    if constexpr (E::lookable) {
        o.lookable = true;
        o.lookup = &do_lookup<E>;
    }
    if constexpr (E::browsable) {
        o.browsable = true;
        o.browse = &do_browse<E>;
    }
    m.emplace(std::string(E::endpoint), std::move(o));
}

template <typename... Es>
std::map<std::string, EntityOps> build_ops(std::tuple<Es...>*) {
    std::map<std::string, EntityOps> m;
    (add_ops<Es>(m), ...);
    return m;
}

// ---------------------------------------------------------------------------
// Terminal rendering (human-readable; --json keeps machine output).
// ---------------------------------------------------------------------------
static std::string jstr2(const json& j, const char* k) {
    auto it = j.find(k);
    if (it == j.end() || it->is_null() || !it->is_string()) return {};
    return it->get<std::string>();
}

static void meta_row(std::ostringstream& o, const char* label, const std::string& v) {
    if (!v.empty()) o << label << ": " << v << "\n";
}

static std::string summary(const std::string& entity, const json& it) {
    std::string name = jstr2(it, "name");
    if (name.empty()) name = jstr2(it, "title");
    std::string extra;
    if (entity == "author") {
        std::string t = jstr2(it, "type");
        if (!t.empty()) extra = " [" + t + "]";
    } else if (entity == "edition" || entity == "edition-group") {
        std::string f = jstr2(it, "format");
        if (!f.empty()) extra = " [" + f + "]";
        auto pit = it.find("pages");
        if (pit != it.end() && pit->is_number() && pit->get<long long>() > 0)
            extra += " (" + std::to_string(pit->get<long long>()) + "p)";
        std::string au;
        if (auto ait = it.find("authors");
            ait != it.end() && ait->is_array() && !ait->empty()) {
            const auto& a0 = (*ait)[0];
            au = a0.is_string() ? a0.get<std::string>() : jstr2(a0, "author-name");
        }
        if (au.empty()) {
            if (auto ait = it.find("author-credits");
                ait != it.end() && ait->is_array() && !ait->empty())
                au = jstr2((*ait)[0], "author-name");
        }
        if (!au.empty()) extra += " — " + au;
    } else if (entity == "work") {
        std::string l = jstr2(it, "language");
        if (!l.empty()) extra = " (" + l + ")";
    } else if (entity == "publisher" || entity == "series") {
        std::string t = jstr2(it, "type");
        if (!t.empty()) extra = " [" + t + "]";
    }
    return name + extra;
}

static void render_detail(std::ostringstream& o, const std::string& entity, const json& e) {
    meta_row(o, "ID", jstr2(e, "bbid"));
    if (entity == "author") {
        meta_row(o, "Name", jstr2(e, "name"));
        meta_row(o, "Sort Name", jstr2(e, "sort-name"));
        meta_row(o, "Type", jstr2(e, "type"));
        meta_row(o, "Gender", jstr2(e, "gender"));
        meta_row(o, "Disambiguation", jstr2(e, "disambiguation"));
        meta_row(o, "Begin", jstr2(e, "begin-date"));
        meta_row(o, "End", jstr2(e, "end-date"));
        meta_row(o, "Begin area", jstr2(e, "begin-area"));
        meta_row(o, "End area", jstr2(e, "end-area"));
    } else if (entity == "edition") {
        meta_row(o, "Title", jstr2(e, "name"));
        meta_row(o, "Disambiguation", jstr2(e, "disambiguation"));
        meta_row(o, "Format", jstr2(e, "format"));
        meta_row(o, "Status", jstr2(e, "status"));
        meta_row(o, "Release date", jstr2(e, "release-date"));
        if (auto pg = e.find("pages");
            pg != e.end() && pg->is_number() && pg->get<long long>() > 0)
            meta_row(o, "Pages", std::to_string(pg->get<long long>()));
        if (auto it = e.find("languages");
            it != e.end() && it->is_array() && !it->empty()) {
            o << "Languages: ";
            bool first = true;
            for (const auto& l : *it) {
                if (!l.is_string()) continue;
                if (!first) o << ", ";
                o << l.get<std::string>();
                first = false;
            }
            o << "\n";
        }
        if (auto it = e.find("authors");
            it != e.end() && it->is_array() && !it->empty()) {
            o << "\nAuthors (" << it->size() << ")\n";
            for (const auto& a : *it) {
                if (a.is_string()) {
                    o << "- " << a.get<std::string>() << "\n";
                    continue;
                }
                o << "- " << jstr2(a, "author-name") << "\n  " << jstr2(a, "author-bbid")
                  << "\n";
            }
        }
        if (auto it = e.find("publishers");
            it != e.end() && it->is_array() && !it->empty()) {
            o << "\nPublishers (" << it->size() << ")\n";
            for (const auto& p : *it)
                o << "- " << jstr2(p, "name") << "\n  " << jstr2(p, "bbid") << "\n";
        }
    } else if (entity == "work") {
        meta_row(o, "Title", jstr2(e, "name"));
        meta_row(o, "Type", jstr2(e, "type"));
        meta_row(o, "Disambiguation", jstr2(e, "disambiguation"));
        meta_row(o, "Language", jstr2(e, "language"));
    } else {
        // Generic fallback: scalar fields.
        for (auto it = e.begin(); it != e.end(); ++it) {
            if (it->is_string() && !it->get<std::string>().empty())
                o << it.key() << ": " << it->get<std::string>() << "\n";
            else if (it->is_number())
                o << it.key() << ": " << it->dump() << "\n";
            else if (it->is_boolean())
                o << it.key() << ": " << (it->get<bool>() ? "true" : "false") << "\n";
        }
    }
}

// ---------------------------------------------------------------------------
// CLI argument parsing.
//   bookbrainz search <entity> --query Q [--limit N] [--offset N] [--json]
//   bookbrainz lookup <entity> <bbid> [--json]
//   bookbrainz browse <entity> --<link> <bbid> [--limit N] [--offset N] [--json]
//   bookbrainz query <path> [--param k=v ...]   (always JSON)
// ---------------------------------------------------------------------------
struct Args {
    std::string op, entity, id, query;
    int limit = 10, offset = 0;
    bool json = false, help = false;
    PMap params;
    PMap extra; // browse link key -> value
};

static const char* kUsage =
    "usage:\n"
    "  bookbrainz search <entity> --query Q [--limit N] [--offset N] [--json]\n"
    "  bookbrainz lookup <entity> <bbid> [--json]\n"
    "  bookbrainz browse <entity> --<link> <bbid> [--limit N] [--offset N] [--json]\n"
    "  bookbrainz query <path> [--param k=v ...]\n"
    "entities:\n"
    "  author edition edition-group publisher series work\n";

static int parse_int_flag(const std::string& name, const std::string& v) {
    try {
        size_t n = 0;
        int r = std::stoi(v, &n);
        if (n != v.size() || r < 0) throw std::invalid_argument("");
        return r;
    } catch (...) {
        throw CliError(2, "invalid integer for " + name + ": '" + v + "'");
    }
}

static Args parse_args(int argc, char** argv) {
    Args a;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        std::string t = argv[i];
        if (t == "-h" || t == "--help") {
            a.help = true;
            return a;
        }
        if (t == "--json") {
            a.json = true;
            continue;
        }
        if (t.rfind("--", 0) == 0) {
            std::string k = t.substr(2), v;
            auto eq = k.find('=');
            if (eq != std::string::npos) {
                v = k.substr(eq + 1);
                k = k.substr(0, eq);
            } else {
                if (i + 1 >= argc) throw CliError(2, "flag --" + k + " needs a value");
                v = argv[++i];
            }
            if (k == "query") a.query = v;
            else if (k == "id") a.id = v;
            else if (k == "entity") a.entity = v;
            else if (k == "limit") a.limit = parse_int_flag("--limit", v);
            else if (k == "offset") a.offset = parse_int_flag("--offset", v);
            else if (k == "param") {
                auto e2 = v.find('=');
                if (e2 == std::string::npos)
                    throw CliError(2, "--param needs k=v, got '" + v + "'");
                a.params[v.substr(0, e2)] = v.substr(e2 + 1);
            } else
                a.extra[k] = v; // browse link keys pass through generically
            continue;
        }
        pos.push_back(t);
    }
    if (pos.empty()) {
        a.help = true; // bare `bookbrainz` behaves like --help
        return a;
    }
    a.op = pos[0];
    if (a.op == "search" || a.op == "lookup" || a.op == "browse") {
        if (pos.size() < 2) throw CliError(2, a.op + " needs an entity\n" + kUsage);
        a.entity = pos[1];
        if (a.op == "lookup") {
            if (pos.size() < 3 && a.id.empty())
                throw CliError(2, "lookup needs a bbid");
            if (!a.id.empty() && pos.size() >= 3 && pos[2] != a.id)
                throw CliError(2, "conflicting bbids");
            if (a.id.empty()) a.id = pos[2];
        } else if (a.op == "search") {
            if (pos.size() > 2) throw CliError(2, "unexpected argument '" + pos[2] + "'");
        } else if (pos.size() > 2)
            throw CliError(2, "unexpected argument '" + pos[2] + "'");
    } else if (a.op != "query")
        throw CliError(2, "unknown subcommand '" + a.op + "'\n" + kUsage);
    if (a.op == "query" && pos.size() >= 2) a.entity = pos[1]; // query reuses entity slot for path
    return a;
}

} // namespace bookbrainz

using namespace bookbrainz;

int main(int argc, char** argv) {
    try {
        Args a = parse_args(argc, argv);
        if (a.help) {
            std::cout << kUsage;
            return 0;
        }
        static const auto ops = [] {
            static AllEntities* tag = nullptr;
            return build_ops(tag);
        }();
        if (a.op == "query") {
            if (a.entity.empty()) throw CliError(2, "query needs a path");
            std::string rel = a.entity;
            while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
            if (rel.rfind("1/", 0) == 0) rel = rel.substr(2);
            PMap pm = a.params;
            std::cout << json::parse(bb_get(bpath(rel, pm))).dump(2) << "\n";
            return 0;
        }
        auto it = ops.find(a.entity);
        if (it == ops.end())
            throw CliError(2, "unknown entity '" + a.entity + "'");
        const EntityOps& eo = it->second;
        json params = json::object(), res;
        std::string list_key;
        if (a.op == "search") {
            if (!eo.searchable) throw CliError(2, "entity '" + a.entity + "' is not searchable");
            if (a.query.empty()) throw CliError(2, "search needs --query");
            params = {{"query", a.query},
                      {"limit", a.limit},
                      {"offset", a.offset}};
            res = eo.search(params);
            list_key = "results";
        } else if (a.op == "lookup") {
            if (!eo.lookable) throw CliError(2, "entity '" + a.entity + "' is not lookable");
            if (a.extra.count("inc"))
                throw CliError(2, "lookup takes no inc (BB sub-resources are separate endpoints)");
            params = {{"id", a.id}};
            res = eo.lookup(params);
            if (a.json) {
                std::cout << res.dump(2) << "\n";
                return 0;
            }
            std::ostringstream o;
            render_detail(o, a.entity, res);
            std::cout << o.str();
            return 0;
        } else if (a.op == "browse") {
            if (!eo.browsable) throw CliError(2, "entity '" + a.entity + "' is not browsable");
            params = json::object();
            for (auto& [k, v] : a.extra) params[k] = v;
            params["limit"] = a.limit;
            params["offset"] = a.offset;
            res = eo.browse(params);
            list_key = "results";
        } else
            throw CliError(2, "unknown subcommand '" + a.op + "'");
        if (a.json) {
            std::cout << res.dump(2) << "\n";
            return 0;
        }
        std::ostringstream o;
        auto li = res.find(list_key);
        size_t n = (li != res.end() && li->is_array()) ? li->size() : 0;
        auto ci = res.find("count");
        long long total = (ci != res.end() && ci->is_number()) ? ci->get<long long>() : (long long)n;
        std::string desc = a.op == "browse" ? ("linked " + a.extra.begin()->first)
                                            : ("\"" + a.query + "\"");
        o << a.entity << " " << desc << " — " << n << " of " << total << "\n";
        int i = 1;
        if (li != res.end() && li->is_array())
            for (const auto& e : *li) {
                auto id = e.find("bbid");
                std::string sid = (id != e.end() && id->is_string()) ? id->get<std::string>() : "";
                o << "  " << i++ << ". " << summary(a.entity, e) << "\n";
                if (!sid.empty()) o << "      " << sid << "\n";
            }
    std::cout << o.str();
    return 0;
    } catch (const CliError& e) {
        std::cerr << "bookbrainz: error: " << e.what() << "\n";
        return e.code;
    } catch (const std::exception& e) {
        std::cerr << "bookbrainz: error: " << e.what() << "\n";
        return 1;
    }
}
