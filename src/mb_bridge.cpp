// mb_bridge — MusicBrainz WS2 (fmt=json) <-> Emacs JSON-RPC bridge.
//
// Thorough type-as-value design: every entity is a tag type carrying
// endpoint / list-key / default-inc / from / to_json. Method names,
// registration, search and lookup are all derived from the type via
// templates + fold expressions. Adding an entity = one line in the
// type list at the bottom. No stringly dispatch in main().
//
// Zero external C deps: transport is popen("curl"), JSON is the vendored
// nlohmann/json pulled in by jsonrpc.hpp.

#include "jsonrpc.hpp"

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <csignal>
#include <iomanip>
#include <map>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// URL helpers (logic merged from libmusicbrainz5 Query.cc)
// ---------------------------------------------------------------------------
namespace mb {

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

static std::string bpath(const std::string& e, const std::string& id = "",
                         const std::string& rs = "", const PMap& p = {}) {
    std::ostringstream o;
    o << "/ws/2/" << e;
    if (!id.empty()) {
        o << "/" << uesc(id);
        if (!rs.empty()) o << "/" << uesc(rs);
    }
    PMap pp = p;
    pp["fmt"] = "json";
    o << "?" << qs(pp);
    return o.str();
}

// ---------------------------------------------------------------------------
// HTTP + MusicBrainz rate limiting (replaces libneon)
// ---------------------------------------------------------------------------
namespace {
std::mutex rl_mtx;
auto rl_last = std::chrono::steady_clock::now() - std::chrono::seconds(2);
} // namespace

// RAII wrappers: destructors own resource release, so early
// returns and exceptions can never leak fds or FILE handles.
struct PipeFds {
    int r = -1, w = -1;
    PipeFds() {
        int p[2] = {-1, -1};
        if (pipe(p) == -1) throw std::runtime_error("pipe failed");
        r = p[0];
        w = p[1];
    }
    PipeFds(const PipeFds&) = delete;
    PipeFds& operator=(const PipeFds&) = delete;
    ~PipeFds() {
        if (r != -1) close(r);
        if (w != -1) close(w);
    }
    void notify(const char* m, size_t n) const {
        if (w != -1) write(w, m, n);
    }
};

struct CurlPipe {
    FILE* f = nullptr;
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
};

static std::string mb_get(const std::string& path) {
    {
        std::lock_guard<std::mutex> l(rl_mtx);
        auto n = std::chrono::steady_clock::now();
        auto e = std::chrono::duration_cast<std::chrono::milliseconds>(n - rl_last).count();
        if (e < 1000)
            std::this_thread::sleep_for(std::chrono::milliseconds(1000 - e));
        rl_last = std::chrono::steady_clock::now();
    }
    const std::string url = std::string("https://musicbrainz.org") + path;
    const std::string cmd =
        "curl -s -H 'User-Agent: mb-emacs-bridge/1.0' '" + url + "'";
    CurlPipe p(cmd);
    return p.read_all();
}

// ---------------------------------------------------------------------------
// Robust JSON field accessors (MusicBrainz is loose with int-vs-string)
// ---------------------------------------------------------------------------
static std::string as_str(const json& j, const char* k) {
    auto it = j.find(k);
    if (it == j.end() || it->is_null()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    if (it->is_number_unsigned()) return std::to_string(it->get<unsigned long long>());
    if (it->is_boolean()) return it->get<bool>() ? "true" : "false";
    return {};
}

static long long as_ll(const json& j, const char* k, long long d = 0) {
    auto it = j.find(k);
    if (it == j.end() || it->is_null()) return d;
    if (it->is_number()) return it->get<long long>();
    if (it->is_string()) {
        try {
            return std::stoll(it->get<std::string>());
        } catch (...) {
            return d;
        }
    }
    return d;
}

static int as_int(const json& j, const char* k, int d = 0) {
    return static_cast<int>(as_ll(j, k, d));
}

// Null-safe extractors: MusicBrainz returns explicit null for absent
// fields, and json::value(k, default) throws type_error.302 on null.
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

static bool sbool(const json& j, const char* k, bool d = false) {
    auto it = j.find(k);
    if (it == j.end() || it->is_null()) return d;
    if (it->is_boolean()) return it->get<bool>();
    return d;
}

// ---------------------------------------------------------------------------
// Entity tag types. Each type IS the value: endpoint, list key, default inc,
// parser and serializer all live on the type itself.
// ---------------------------------------------------------------------------
struct NameCredit {
    std::string name, joinphrase, artist_id;
    static NameCredit from(const json& j) {
        NameCredit n;
        n.name = sstr(j, "name");
        n.joinphrase = sstr(j, "joinphrase");
        if (j.contains("artist") && j["artist"].is_object()) {
            n.artist_id = sstr(j["artist"], "id");
            if (n.name.empty()) n.name = sstr(j["artist"], "name");
        }
        return n;
    }
    // Mirrors IArtistCredit {name, joinphrase, artist: IArtist}.
    json to_json() const {
        return {{"name", name},
                {"joinphrase", joinphrase},
                {"artist", {{"id", artist_id}, {"name", name}}}};
    }
};

struct Artist {
    static constexpr std::string_view endpoint = "artist";
    static constexpr std::string_view list_key = "artists";
    static constexpr std::string_view default_inc = "";
    static constexpr bool searchable = true;
    // Mirrors ArtistIncludes = MiscIncludes | RelationsIncludes
    //   | recordings | releases | release-groups | works.
    static constexpr std::array<std::string_view, 23> allowed_inc = {
        "aliases",         "annotation", "tags",          "genres",
        "ratings",         "media",      "area-rels",     "artist-rels",
        "event-rels",      "genre-rels", "instrument-rels", "label-rels",
        "place-rels",      "recording-rels", "release-rels", "release-group-rels",
        "series-rels",     "url-rels",   "work-rels",     "recordings",
        "releases",        "release-groups", "works"};

    std::string id, type, name, sort_name, gender, country, disambiguation;
    std::string begin, end;
    bool ended = false;

    static Artist from(const json& j) {
        Artist a;
        a.id = sstr(j, "id");
        a.type = sstr(j, "type");
        a.name = sstr(j, "name");
        a.sort_name = sstr(j, "sort-name");
        a.gender = sstr(j, "gender");
        a.country = sstr(j, "country");
        a.disambiguation = sstr(j, "disambiguation");
        if (j.contains("life-span") && j["life-span"].is_object()) {
            a.begin = sstr(j["life-span"], "begin");
            a.end = sstr(j["life-span"], "end");
            a.ended = sbool(j["life-span"], "ended");
        }
        return a;
    }
    // Keys mirror IArtist: sort-name, life-span{begin,end,ended}.
    json to_json() const {
        json o = {{"id", id},
                  {"name", name},
                  {"sort-name", sort_name},
                  {"type", type},
                  {"gender", gender},
                  {"country", country},
                  {"disambiguation", disambiguation}};
        if (!begin.empty() || !end.empty())
            o["life-span"] = {{"begin", begin}, {"end", end}, {"ended", ended}};
        return o;
    }
};

struct Track {
    int position = 0;
    std::string number, title, recording_id;
    long long length = 0;
    static Track from(const json& j) {
        Track t;
        t.position = as_int(j, "position");
        t.number = as_str(j, "number");
        t.title = sstr(j, "title");
        t.length = as_ll(j, "length");
        t.recording_id = sstr(j, "id");
        if (j.contains("recording") && j["recording"].is_object()) {
            const auto& r = j["recording"];
            if (t.title.empty()) t.title = sstr(r, "title");
            if (t.length == 0) t.length = as_ll(r, "length");
            if (t.recording_id.empty()) t.recording_id = sstr(r, "id");
        }
        return t;
    }
    // Keys mirror ITrack: nested recording object.
    json to_json() const {
        return {{"position", position},
                {"number", number},
                {"title", title},
                {"length", length},
                {"recording", {{"id", recording_id}, {"title", title}, {"length", length}}}};
    }
};

struct Medium {
    int position = 0, track_count = 0;
    std::string format, title;
    std::vector<Track> tracks;
    static Medium from(const json& j) {
        Medium m;
        m.position = as_int(j, "position");
        m.format = sstr(j, "format");
        m.title = sstr(j, "title");
        m.track_count = as_int(j, "track-count");
        auto pit = j.find("tracks");
        if (pit == j.end()) pit = j.find("track-list");
        if (pit != j.end() && pit->is_array())
            for (const auto& t : *pit) m.tracks.push_back(Track::from(t));
        return m;
    }
    // Keys mirror IMedium: title, track-count.
    json to_json() const {
        json o = {{"position", position},
                  {"format", format},
                  {"title", title},
                  {"track-count", track_count}};
        json ta = json::array();
        for (const auto& t : tracks) ta.push_back(t.to_json());
        o["tracks"] = std::move(ta);
        return o;
    }
};

struct LabelInfo {
    std::string catalog_number, label_id, label_name;
    // Mirrors ILabelInfo {label: ILabel|null, catalog-number}.
    static LabelInfo from(const json& j) {
        LabelInfo l;
        l.catalog_number = sstr(j, "catalog-number");
        if (j.contains("label") && j["label"].is_object()) {
            l.label_id = sstr(j["label"], "id");
            l.label_name = sstr(j["label"], "name");
        }
        return l;
    }
    json to_json() const {
        return {{"catalog-number", catalog_number},
                {"label", {{"id", label_id}, {"name", label_name}}}};
    }
};

struct Release {
    static constexpr std::string_view endpoint = "release";
    static constexpr std::string_view list_key = "releases";
    static constexpr std::string_view default_inc =
        "artists+labels+recordings+release-groups+artist-credits+discids";
    static constexpr bool searchable = true;
    // Mirrors ReleaseIncludes = MiscIncludes | SubQueryIncludes
    //   | RelationsIncludes | artists | collections | labels | recordings
    //   | release-groups | recording-level-rels.
    static constexpr std::array<std::string_view, 29> allowed_inc = {
        "aliases",         "annotation",   "tags",
        "genres",          "ratings",      "media",
        "discids",         "isrcs",        "artist-credits",
        "various-artists", "area-rels",    "artist-rels",
        "event-rels",      "genre-rels",   "instrument-rels",
        "label-rels",      "place-rels",   "recording-rels",
        "release-rels",    "release-group-rels", "series-rels",
        "url-rels",        "work-rels",    "artists",
        "collections",     "labels",       "recordings",
        "release-groups",  "recording-level-rels"};

    std::string id, title, status, date, country, barcode, disambiguation;
    std::string rg_id, rg_title, rg_primary;
    std::vector<NameCredit> credit;
    std::vector<Medium> media;
    std::vector<LabelInfo> label_info;
    static Release from(const json& j) {
        Release r;
        r.id = sstr(j, "id");
        r.title = sstr(j, "title");
        r.status = sstr(j, "status");
        r.date = sstr(j, "date");
        r.country = sstr(j, "country");
        r.barcode = sstr(j, "barcode");
        r.disambiguation = sstr(j, "disambiguation");
        if (j.contains("artist-credit") && j["artist-credit"].is_array())
            for (const auto& n : j["artist-credit"])
                r.credit.push_back(NameCredit::from(n));
        if (j.contains("release-group") && j["release-group"].is_object()) {
            const auto& g = j["release-group"];
            r.rg_id = sstr(g, "id");
            r.rg_title = sstr(g, "title");
            r.rg_primary = sstr(g, "primary-type");
        }
        if (j.contains("media") && j["media"].is_array())
            for (const auto& m : j["media"]) r.media.push_back(Medium::from(m));
        if (j.contains("label-info") && j["label-info"].is_array())
            for (const auto& l : j["label-info"])
                r.label_info.push_back(LabelInfo::from(l));
        return r;
    }
    // Keys mirror IRelease: artist-credit, release-group{primary-type},
    // label-info[{catalog-number, label}].
    json to_json() const {
        json o = {{"id", id},
                  {"title", title},
                  {"status", status},
                  {"date", date},
                  {"country", country},
                  {"barcode", barcode},
                  {"disambiguation", disambiguation}};
        json ca = json::array();
        for (const auto& n : credit) ca.push_back(n.to_json());
        o["artist-credit"] = std::move(ca);
        if (!rg_id.empty())
            o["release-group"] = {
                {"id", rg_id}, {"title", rg_title}, {"primary-type", rg_primary}};
        json ma = json::array();
        for (const auto& m : media) ma.push_back(m.to_json());
        o["media"] = std::move(ma);
        json la = json::array();
        for (const auto& l : label_info) la.push_back(l.to_json());
        o["label-info"] = std::move(la);
        return o;
    }
};

struct Recording {
    static constexpr std::string_view endpoint = "recording";
    static constexpr std::string_view list_key = "recordings";
    static constexpr std::string_view default_inc = "artists+releases+isrcs+artist-credits";
    static constexpr bool searchable = true;
    // Mirrors RecordingIncludes = MiscIncludes | RelationsIncludes
    //   | SubQueryIncludes | artists | releases | isrcs.
    static constexpr std::array<std::string_view, 25> allowed_inc = {
        "aliases",         "annotation", "tags",
        "genres",          "ratings",    "media",
        "area-rels",       "artist-rels", "event-rels",
        "genre-rels",      "instrument-rels", "label-rels",
        "place-rels",      "recording-rels", "release-rels",
        "release-group-rels", "series-rels", "url-rels",
        "work-rels",       "discids",    "isrcs",
        "artist-credits",  "various-artists", "artists",
        "releases"};

    std::string id, title, disambiguation, first_release_date;
    long long length = 0;
    bool video = false;
    std::vector<NameCredit> credit;
    std::vector<std::string> release_ids, release_titles, isrcs;
    static Recording from(const json& j) {
        Recording r;
        r.id = sstr(j, "id");
        r.title = sstr(j, "title");
        r.length = as_ll(j, "length");
        r.disambiguation = sstr(j, "disambiguation");
        r.video = sbool(j, "video");
        r.first_release_date = sstr(j, "first-release-date");
        if (j.contains("artist-credit") && j["artist-credit"].is_array())
            for (const auto& n : j["artist-credit"])
                r.credit.push_back(NameCredit::from(n));
        if (j.contains("releases") && j["releases"].is_array())
            for (const auto& rel : j["releases"]) {
                r.release_ids.push_back(sstr(rel, "id"));
                r.release_titles.push_back(sstr(rel, "title"));
            }
        if (j.contains("isrcs") && j["isrcs"].is_array())
            for (const auto& s : j["isrcs"])
                if (s.is_string()) r.isrcs.push_back(s.get<std::string>());
        return r;
    }
    // Keys mirror IRecording: artist-credit, first-release-date.
    json to_json() const {
        json o = {{"id", id},
                  {"title", title},
                  {"length", length},
                  {"disambiguation", disambiguation},
                  {"video", video},
                  {"first-release-date", first_release_date}};
        json ca = json::array();
        for (const auto& n : credit) ca.push_back(n.to_json());
        o["artist-credit"] = std::move(ca);
        json ra = json::array();
        for (size_t i = 0; i < release_ids.size(); ++i)
            ra.push_back({{"id", release_ids[i]},
                          {"title", i < release_titles.size() ? release_titles[i] : ""}});
        o["releases"] = std::move(ra);
        o["isrcs"] = isrcs;
        return o;
    }
};

struct Disc {
    static constexpr std::string_view endpoint = "discid";
    static constexpr std::string_view list_key = "releases"; // unused
    static constexpr std::string_view default_inc = "";
    static constexpr bool searchable = false; // lookup-only
    // discid takes no inc parameter.
    static constexpr std::array<std::string_view, 0> allowed_inc = {};

    std::string id;
    int sectors = 0;
    std::vector<std::string> release_ids, release_titles;
    // discid fmt=json may nest under "disc" or be top-level: accept both.
    static Disc from(const json& j) {
        const json* d = &j;
        json holder;
        if (j.contains("disc") && j["disc"].is_object()) {
            holder = j["disc"];
            d = &holder;
        }
        Disc x;
        x.id = sstr(*d, "id");
        x.sectors = as_int(*d, "sectors");
        if (d->contains("releases") && (*d)["releases"].is_array())
            for (const auto& rel : (*d)["releases"]) {
                x.release_ids.push_back(sstr(rel, "id"));
                x.release_titles.push_back(sstr(rel, "title"));
            }
        return x;
    }
    json to_json() const {
        json o = {{"id", id}, {"sectors", sectors}};
        json ra = json::array();
        for (size_t i = 0; i < release_ids.size(); ++i)
            ra.push_back({{"id", release_ids[i]},
                          {"title", i < release_titles.size() ? release_titles[i] : ""}});
        o["releases"] = std::move(ra);
        return o;
    }
};

// The registry: adding an entity = one line here.
using AllEntities =
    std::tuple<Artist, Release, Recording, Disc>;

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
    if (!it->is_string())
        throw jsonrpc::JsonRpcException(jsonrpc::spec::kInvalidParams,
                                        "expected string param");
    return it->get<std::string>();
}

template <typename E>
json do_search(const json& p) {
    static_assert(E::searchable, "entity is lookup-only");
    PMap pm;
    pm["query"] = get_str(p, "query");
    pm["limit"] = std::to_string(get_int(p, "limit", 10));
    pm["offset"] = std::to_string(get_int(p, "offset", 0));
    json raw = json::parse(
        mb_get(bpath(std::string(E::endpoint), "", "", pm)));
    json result = {{"count", as_int(raw, "count")},
                   {"offset", as_int(raw, "offset")}};
    json arr = json::array();
    const std::string key(E::list_key);
    if (raw.contains(key) && raw[key].is_array())
        for (const auto& e : raw[key]) arr.push_back(E::from(e).to_json());
    result[E::list_key] = std::move(arr);
    return result;
}

// Validates an inc string against the entity's allowed set
// (C++ equivalent of musicbrainz-api's ArtistIncludes[] etc.).
// Tokens are separated by '+' or space, mirroring MB query syntax.
template <typename E>
void check_inc(const std::string& inc) {
    size_t i = 0;
    while (i < inc.size()) {
        while (i < inc.size() && (inc[i] == '+' || inc[i] == ' ')) ++i;
        if (i >= inc.size()) break;
        size_t j = i;
        while (j < inc.size() && inc[j] != '+' && inc[j] != ' ') ++j;
        std::string_view tok(inc.data() + i, j - i);
        bool ok = false;
        for (auto a : E::allowed_inc)
            if (a == tok) {
                ok = true;
                break;
            }
        if (!ok)
            throw jsonrpc::JsonRpcException(
                jsonrpc::spec::kInvalidParams,
                ("unknown inc '" + std::string(tok) + "' for " +
                 std::string(E::endpoint))
                    .c_str());
        i = j;
    }
}

template <typename E>
json do_lookup(const json& p) {
    PMap pm;
    std::string inc = get_str(p, "inc");
    if (inc.empty()) inc = std::string(E::default_inc);
    if (!inc.empty()) {
        check_inc<E>(inc);
        pm["inc"] = inc;
    }
    json raw = json::parse(
        mb_get(bpath(std::string(E::endpoint), get_str(p, "id"), "", pm)));
    return E::from(raw).to_json();
}

template <typename E>
void register_entity(jsonrpc::Conn& s) {
    const std::string ep(E::endpoint);
    if constexpr (E::searchable)
        s.register_method("search-" + ep, do_search<E>);
    s.register_method("lookup-" + ep, do_lookup<E>);
}

template <typename... Es>
void register_all(jsonrpc::Conn& s, std::tuple<Es...>*) {
    (register_entity<Es>(s), ...);
}

void register_all_entities(jsonrpc::Conn& s) {
    static AllEntities* tag = nullptr;
    register_all(s, tag);
}

} // namespace mb

namespace {
std::atomic<bool> gq{false};
mb::PipeFds* g_pipe = nullptr; // set in main; signal handler only writes
} // namespace

static void sh(int) {
    gq = true;
    if (g_pipe) g_pipe->notify("quit", 4);
}

int main() {
    mb::PipeFds pipe;
    g_pipe = &pipe;
    std::signal(SIGINT, sh);
    std::signal(SIGTERM, sh);
    auto wk = [&] { pipe.notify("wake", 4); };
    jsonrpc::Conn s(wk, std::cin, std::cout, std::cerr,
                     jsonrpc::Conn::kDefaultMaxContentLength, STDIN_FILENO);

    s.register_notification("exit", [&](const jsonrpc::json&) {
        gq = true;
        pipe.notify("quit", 4);
    });

    // Raw passthrough: {"entity","id","resource","params":{...}} -> MB JSON.
    s.register_method("query", [](const jsonrpc::json& p) -> jsonrpc::json {
        const std::string en = mb::get_str(p, "entity");
        const std::string id = mb::get_str(p, "id");
        std::string rs;
        if (auto it = p.find("resource"); it != p.end() && !it->is_null())
            rs = it->get<std::string>();
        mb::PMap pm;
        if (auto it = p.find("params");
            it != p.end() && it->is_object())
            for (auto& [k, v] : it->items()) {
                if (!v.is_string())
                    throw jsonrpc::JsonRpcException(
                        jsonrpc::spec::kInvalidParams, "params must be strings");
                pm[k] = v.get<std::string>();
            }
        return jsonrpc::json::parse(mb::mb_get(mb::bpath(en, id, rs, pm)));
    });

    mb::register_all_entities(s);

    s.start();
    struct pollfd f[2] = {{pipe.r, POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
    while (!gq && s.is_running()) {
        int r = poll(f, 2, 10);
        if (r > 0) {
            if (f[0].revents & POLLIN) {
                char b[64];
                read(pipe.r, b, sizeof(b));
            }
            if (f[1].revents & POLLIN) s.process_queue();
        }
        if (gq) break;
        s.process_queue();
    }
    s.stop();
    g_pipe = nullptr;
    return 0;
}
