// musicbrainz — MusicBrainz WS2 (fmt=json) terminal client.
//
// Thorough type-as-value design: every entity is a tag type carrying
// endpoint / list-key / default-inc / from / to_json. Dispatch is
// derived from the type. Adding an entity = one line in the type list.
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

// ---------------------------------------------------------------------------
// URL helpers (logic merged from libmusicbrainz5 Query.cc)
// ---------------------------------------------------------------------------
namespace musicbrainz {

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

// RAII wrapper: destructor owns pclose, so early returns and
// exceptions can never leak the FILE handle.
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
    // -f makes curl exit nonzero on HTTP errors; capture it once.
    int finish() {
        if (f) {
            rc = pclose(f);
            f = nullptr;
        }
        return rc;
    }
};

static std::string fetch(const std::string& path) {
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
        "curl -s -f -H 'User-Agent: musicbrainz-cli/0.1.0 (+https://github.com/ningxilai/emacs-stdio-jsonrpc)' '" +
        url + "'";
    CurlPipe p(cmd);
    std::string body = p.read_all();
    if (p.finish() != 0)
        throw CliError(1, "request failed: " + url);
    return body;
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

struct Tag {
    std::string name;
    int count = 0;
    // Mirrors WS2 "tags": [{count, name}].
    static Tag from(const json& j) {
        return {sstr(j, "name"), as_int(j, "count")};
    }
    json to_json() const {
        return {{"name", name}, {"count", count}};
    }
};

struct Genre {
    std::string id, name, disambiguation;
    int count = 0;
    // Mirrors WS2 "genres": [{id, name, disambiguation, count}].
    static Genre from(const json& j) {
        return {sstr(j, "id"), sstr(j, "name"), sstr(j, "disambiguation"),
                as_int(j, "count")};
    }
    json to_json() const {
        return {{"id", id}, {"name", name}, {"disambiguation", disambiguation}, {"count", count}};
    }
};

struct SameAs {
    std::string type, url;
    // Derived from WS2 relations targeting URLs (mirrors JSON-LD sameAs
    // used by BrainzWrap, but keeps the relation type alongside).
    static bool try_from(const json& r, SameAs& out) {
        if (!r.contains("url") || !r["url"].is_object()) return false;
        std::string u = sstr(r["url"], "resource");
        if (u.empty()) return false;
        out.type = sstr(r, "type");
        out.url = u;
        return true;
    }
    json to_json() const {
        return {{"type", type}, {"url", url}};
    }
};

static std::vector<Tag> parse_tags(const json& j) {
    std::vector<Tag> v;
    if (j.contains("tags") && j["tags"].is_array())
        for (const auto& t : j["tags"]) v.push_back(Tag::from(t));
    return v;
}

static std::vector<Genre> parse_genres(const json& j) {
    std::vector<Genre> v;
    if (j.contains("genres") && j["genres"].is_array())
        for (const auto& g : j["genres"]) v.push_back(Genre::from(g));
    return v;
}

static std::vector<SameAs> parse_sameas(const json& j) {
    std::vector<SameAs> v;
    if (j.contains("relations") && j["relations"].is_array())
        for (const auto& r : j["relations"]) {
            SameAs s;
            if (SameAs::try_from(r, s)) v.push_back(s);
        }
    return v;
}

static json tags_json(const std::vector<Tag>& v) {
    json a = json::array();
    for (const auto& t : v) a.push_back(t.to_json());
    return a;
}

static json genres_json(const std::vector<Genre>& v) {
    json a = json::array();
    for (const auto& g : v) a.push_back(g.to_json());
    return a;
}

static json sameas_json(const std::vector<SameAs>& v) {
    json a = json::array();
    for (const auto& s : v) a.push_back(s.to_json());
    return a;
}

struct Alias {
    std::string locale, name, sort_name, type;
    // Mirrors WS2 "aliases" entries.
    static Alias from(const json& j) {
        return {sstr(j, "locale"), sstr(j, "name"), sstr(j, "sort-name"),
                sstr(j, "type")};
    }
    json to_json() const {
        return {{"locale", locale},
                {"name", name},
                {"sort-name", sort_name},
                {"type", type}};
    }
};

static std::vector<Alias> parse_aliases(const json& j) {
    std::vector<Alias> v;
    if (j.contains("aliases") && j["aliases"].is_array())
        for (const auto& a : j["aliases"]) v.push_back(Alias::from(a));
    return v;
}

static json aliases_json(const std::vector<Alias>& v) {
    json a = json::array();
    for (const auto& x : v) a.push_back(x.to_json());
    return a;
}

struct Rating {
    bool present = false;
    double value = 0;
    int votes = 0;
    // Mirrors IRating {value, votes-count}; value may be null.
    static Rating from(const json& j) {
        Rating r;
        if (j.contains("rating") && j["rating"].is_object()) {
            const auto& x = j["rating"];
            auto it = x.find("value");
            if (it != x.end() && !it->is_null() && it->is_number()) {
                r.present = true;
                r.value = it->get<double>();
            }
            r.votes = as_int(x, "votes-count");
            if (r.votes > 0) r.present = true;
        }
        return r;
    }
};

struct Artist {
    static constexpr std::string_view endpoint = "artist";
    static constexpr std::string_view list_key = "artists";
    static constexpr std::string_view default_inc = "aliases+tags+genres+ratings+url-rels";
    static constexpr bool searchable = true;
    static constexpr bool lookable = true;
    static constexpr bool browsable = true;
    static constexpr std::string_view browse_key = "artists";
    // Mirrors BrowseArtistsEntityParams.
    static constexpr std::array<std::string_view, 6> browse_links = {
        "area", "collection", "recording", "release", "release-group", "work"};
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
    std::string begin, end, area_id, area_name, begin_area_id, begin_area_name;
    std::string end_area_id, end_area_name;
    bool ended = false;
    Rating rating;
    std::vector<Alias> aliases;
    std::vector<Tag> tags;
    std::vector<Genre> genres;
    std::vector<SameAs> same_as;

    static void area_ref(const json& j, const char* key, std::string& id,
                         std::string& name) {
        if (j.contains(key) && j[key].is_object()) {
            id = sstr(j[key], "id");
            name = sstr(j[key], "name");
        }
    }
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
        area_ref(j, "area", a.area_id, a.area_name);
        area_ref(j, "begin-area", a.begin_area_id, a.begin_area_name);
        area_ref(j, "end-area", a.end_area_id, a.end_area_name);
        a.rating = Rating::from(j);
        a.aliases = parse_aliases(j);
        a.tags = parse_tags(j);
        a.genres = parse_genres(j);
        a.same_as = parse_sameas(j);
        return a;
    }
    // Keys mirror IArtist (+area/begin-area/end-area light refs).
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
        if (!area_id.empty()) o["area"] = {{"id", area_id}, {"name", area_name}};
        if (!begin_area_id.empty())
            o["begin-area"] = {{"id", begin_area_id}, {"name", begin_area_name}};
        if (!end_area_id.empty())
            o["end-area"] = {{"id", end_area_id}, {"name", end_area_name}};
        if (rating.present)
            o["rating"] = {{"value", rating.value}, {"votes-count", rating.votes}};
        o["aliases"] = aliases_json(aliases);
        o["tags"] = tags_json(tags);
        o["genres"] = genres_json(genres);
        o["sameAs"] = sameas_json(same_as);
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
    std::vector<std::string> disc_ids;
    std::vector<int> disc_sectors;
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
        if (j.contains("discs") && j["discs"].is_array())
            for (const auto& d : j["discs"]) {
                m.disc_ids.push_back(sstr(d, "id"));
                m.disc_sectors.push_back(as_int(d, "sectors"));
            }
        return m;
    }
    // Keys mirror IMedium (+discs light refs for discid matching).
    json to_json() const {
        json o = {{"position", position},
                  {"format", format},
                  {"title", title},
                  {"track-count", track_count}};
        json ta = json::array();
        for (const auto& t : tracks) ta.push_back(t.to_json());
        o["tracks"] = std::move(ta);
        json da = json::array();
        for (size_t i = 0; i < disc_ids.size(); ++i)
            da.push_back({{"id", disc_ids[i]},
                          {"sectors", i < disc_sectors.size() ? disc_sectors[i] : 0}});
        o["discs"] = std::move(da);
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
        "artists+labels+recordings+release-groups+artist-credits+discids+tags+genres+url-rels";
    static constexpr bool searchable = true;
    static constexpr bool lookable = true;
    static constexpr bool browsable = true;
    static constexpr std::string_view browse_key = "releases";
    // Mirrors BrowseReleasesEntityParams.
    static constexpr std::array<std::string_view, 11> browse_links = {
        "area", "artist", "editor", "event", "label", "place",
        "recording", "release", "release-group", "track_artist", "work"};
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

    std::string id, title, status, quality, packaging, date, country, barcode, asin;
    std::string disambiguation, text_lang, text_script;
    std::string rg_id, rg_title, rg_primary;
    bool cover_front = false, cover_back = false, cover_artwork = false, cover_darkened = false;
    int cover_count = 0;
    bool has_cover = false;
    struct RelEvent {
        std::string date, area_id, area_name;
    };
    std::vector<NameCredit> credit;
    std::vector<Medium> media;
    std::vector<RelEvent> events;
    std::vector<Alias> aliases;
    std::vector<Tag> tags;
    std::vector<Genre> genres;
    std::vector<SameAs> same_as;
    std::vector<LabelInfo> label_info;
    static Release from(const json& j) {
        Release r;
        r.id = sstr(j, "id");
        r.title = sstr(j, "title");
        r.status = sstr(j, "status");
        r.quality = sstr(j, "quality");
        r.packaging = sstr(j, "packaging");
        r.date = sstr(j, "date");
        r.country = sstr(j, "country");
        r.barcode = sstr(j, "barcode");
        r.asin = sstr(j, "asin");
        r.disambiguation = sstr(j, "disambiguation");
        if (j.contains("text-representation") && j["text-representation"].is_object()) {
            r.text_lang = sstr(j["text-representation"], "language");
            r.text_script = sstr(j["text-representation"], "script");
        }
        if (j.contains("cover-art-archive") && j["cover-art-archive"].is_object()) {
            const auto& c = j["cover-art-archive"];
            r.cover_count = as_int(c, "count");
            r.cover_front = sbool(c, "front");
            r.cover_back = sbool(c, "back");
            r.cover_artwork = sbool(c, "artwork");
            r.cover_darkened = sbool(c, "darkened");
            r.has_cover = true;
        }
        if (j.contains("release-events") && j["release-events"].is_array())
            for (const auto& e : j["release-events"]) {
                RelEvent ev;
                ev.date = sstr(e, "date");
                if (e.contains("area") && e["area"].is_object()) {
                    ev.area_id = sstr(e["area"], "id");
                    ev.area_name = sstr(e["area"], "name");
                }
                r.events.push_back(ev);
            }
        r.aliases = parse_aliases(j);
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
        r.tags = parse_tags(j);
        r.genres = parse_genres(j);
        r.same_as = parse_sameas(j);
        return r;
    }
    // Keys mirror IRelease (+text-representation/packaging/quality/asin/
    // cover-art-archive/release-events per WS2 shape).
    json to_json() const {
        json o = {{"id", id},
                  {"title", title},
                  {"status", status},
                  {"quality", quality},
                  {"packaging", packaging},
                  {"date", date},
                  {"country", country},
                  {"barcode", barcode},
                  {"asin", asin},
                  {"disambiguation", disambiguation},
                  {"text-representation",
                   {{"language", text_lang}, {"script", text_script}}}};
        if (has_cover)
            o["cover-art-archive"] = {{"count", cover_count},
                                      {"front", cover_front},
                                      {"back", cover_back},
                                      {"artwork", cover_artwork},
                                      {"darkened", cover_darkened}};
        json ea = json::array();
        for (const auto& e : events)
            ea.push_back({{"date", e.date},
                          {"area", {{"id", e.area_id}, {"name", e.area_name}}}});
        o["release-events"] = std::move(ea);
        o["aliases"] = aliases_json(aliases);
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
        o["tags"] = tags_json(tags);
        o["genres"] = genres_json(genres);
        o["sameAs"] = sameas_json(same_as);
        return o;
    }
};

struct Recording {
    static constexpr std::string_view endpoint = "recording";
    static constexpr std::string_view list_key = "recordings";
    static constexpr std::string_view default_inc = "artists+releases+isrcs+artist-credits+tags+genres+url-rels";
    static constexpr bool searchable = true;
    static constexpr bool lookable = true;
    static constexpr bool browsable = true;
    static constexpr std::string_view browse_key = "recording";
    // Mirrors BrowseRecordingsEntityParams.
    static constexpr std::array<std::string_view, 4> browse_links = {
        "artist", "collection", "release", "work"};
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
    Rating rating;
    std::vector<NameCredit> credit;
    std::vector<std::string> release_ids, release_titles, isrcs;
    std::vector<Alias> aliases;
    std::vector<Tag> tags;
    std::vector<Genre> genres;
    std::vector<SameAs> same_as;
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
        r.rating = Rating::from(j);
        r.aliases = parse_aliases(j);
        r.tags = parse_tags(j);
        r.genres = parse_genres(j);
        r.same_as = parse_sameas(j);
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
        if (rating.present)
            o["rating"] = {{"value", rating.value}, {"votes-count", rating.votes}};
        o["aliases"] = aliases_json(aliases);
        o["tags"] = tags_json(tags);
        o["genres"] = genres_json(genres);
        o["sameAs"] = sameas_json(same_as);
        return o;
    }
};

struct Disc {
    static constexpr std::string_view endpoint = "discid";
    static constexpr std::string_view list_key = "releases"; // unused
    static constexpr std::string_view default_inc = "";
    static constexpr bool searchable = false; // lookup-only
    static constexpr bool lookable = true;
    static constexpr bool browsable = false;
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

// ---------------------------------------------------------------------------
// Remaining read-side entities, mirroring musicbrainz.types.ts shapes.
// Each carries endpoint / list_key / browse_key / inc sets, following
// the same tag-type pattern as Artist/Release/Recording.
// ---------------------------------------------------------------------------
struct Label {
    static constexpr std::string_view endpoint = "label";
    static constexpr std::string_view list_key = "labels";
    static constexpr std::string_view default_inc = "aliases+tags+genres+ratings+url-rels+releases";
    static constexpr bool searchable = true;
    static constexpr bool lookable = true;
    static constexpr bool browsable = true;
    static constexpr std::string_view browse_key = "label";
    // Mirrors BrowseLabelsEntityParams.
    static constexpr std::array<std::string_view, 3> browse_links = {
        "area", "collection", "release"};
    // Mirrors LabelIncludes = MiscIncludes | RelationsIncludes | releases.
    static constexpr std::array<std::string_view, 20> allowed_inc = {
        "aliases", "annotation", "tags", "genres", "ratings", "media",
        "area-rels", "artist-rels", "event-rels", "genre-rels",
        "instrument-rels", "label-rels", "place-rels", "recording-rels",
        "release-rels", "release-group-rels", "series-rels", "url-rels",
        "work-rels", "releases"};

    std::string id, type, name, sort_name, label_code, country, disambiguation;
    std::string begin, end, area_id, area_name;
    bool ended = false;
    std::vector<std::string> ipis, isnis;
    Rating rating;
    std::vector<Alias> aliases;
    std::vector<Tag> tags;
    std::vector<Genre> genres;
    std::vector<SameAs> same_as;
    static Label from(const json& j) {
        Label l;
        l.id = sstr(j, "id");
        l.type = sstr(j, "type");
        l.name = sstr(j, "name");
        l.sort_name = sstr(j, "sort-name");
        l.label_code = sstr(j, "label-code");
        l.country = sstr(j, "country");
        l.disambiguation = sstr(j, "disambiguation");
        if (j.contains("life-span") && j["life-span"].is_object()) {
            l.begin = sstr(j["life-span"], "begin");
            l.end = sstr(j["life-span"], "end");
            l.ended = sbool(j["life-span"], "ended");
        }
        if (j.contains("area") && j["area"].is_object()) {
            l.area_id = sstr(j["area"], "id");
            l.area_name = sstr(j["area"], "name");
        }
        if (j.contains("ipis") && j["ipis"].is_array())
            for (const auto& s : j["ipis"])
                if (s.is_string()) l.ipis.push_back(s.get<std::string>());
        if (j.contains("isnis") && j["isnis"].is_array())
            for (const auto& s : j["isnis"])
                if (s.is_string()) l.isnis.push_back(s.get<std::string>());
        l.rating = Rating::from(j);
        l.aliases = parse_aliases(j);
        l.tags = parse_tags(j);
        l.genres = parse_genres(j);
        l.same_as = parse_sameas(j);
        return l;
    }
    // Keys mirror ILabel (area/ipis/isnis included).
    json to_json() const {
        json o = {{"id", id},
                  {"name", name},
                  {"sort-name", sort_name},
                  {"type", type},
                  {"label-code", label_code},
                  {"country", country},
                  {"disambiguation", disambiguation},
                  {"ipis", ipis},
                  {"isnis", isnis}};
        if (!begin.empty() || !end.empty())
            o["life-span"] = {{"begin", begin}, {"end", end}, {"ended", ended}};
        if (!area_id.empty()) o["area"] = {{"id", area_id}, {"name", area_name}};
        if (rating.present)
            o["rating"] = {{"value", rating.value}, {"votes-count", rating.votes}};
        o["aliases"] = aliases_json(aliases);
        o["tags"] = tags_json(tags);
        o["genres"] = genres_json(genres);
        o["sameAs"] = sameas_json(same_as);
        return o;
    }
};

struct ReleaseGroup {
    static constexpr std::string_view endpoint = "release-group";
    static constexpr std::string_view list_key = "release-groups";
    static constexpr std::string_view default_inc = "artists+releases+tags+genres+url-rels";
    static constexpr bool searchable = true;
    static constexpr bool lookable = true;
    static constexpr bool browsable = true;
    static constexpr std::string_view browse_key = "release-groups";
    // Mirrors BrowseReleaseGroupsEntityParams.
    static constexpr std::array<std::string_view, 3> browse_links = {
        "artist", "collection", "release"};
    // Mirrors ReleaseGroupIncludes (25).
    static constexpr std::array<std::string_view, 25> allowed_inc = {
        "aliases", "annotation", "tags", "genres", "ratings", "media",
        "discids", "isrcs", "artist-credits", "various-artists",
        "area-rels", "artist-rels", "event-rels", "genre-rels",
        "instrument-rels", "label-rels", "place-rels", "recording-rels",
        "release-rels", "release-group-rels", "series-rels", "url-rels",
        "work-rels", "artists", "releases"};

    std::string id, type, title, disambiguation, first_release_date, primary_type;
    std::vector<std::string> secondary_types;
    std::vector<NameCredit> credit;
    std::vector<std::string> release_ids, release_titles;
    Rating rating;
    std::vector<Alias> aliases;
    std::vector<Tag> tags;
    std::vector<Genre> genres;
    std::vector<SameAs> same_as;
    static ReleaseGroup from(const json& j) {
        ReleaseGroup g;
        g.id = sstr(j, "id");
        g.type = sstr(j, "type");
        g.title = sstr(j, "title");
        g.disambiguation = sstr(j, "disambiguation");
        g.first_release_date = sstr(j, "first-release-date");
        g.primary_type = sstr(j, "primary-type");
        if (j.contains("secondary-types") && j["secondary-types"].is_array())
            for (const auto& s : j["secondary-types"])
                if (s.is_string()) g.secondary_types.push_back(s.get<std::string>());
        if (j.contains("artist-credit") && j["artist-credit"].is_array())
            for (const auto& n : j["artist-credit"])
                g.credit.push_back(NameCredit::from(n));
        if (j.contains("releases") && j["releases"].is_array())
            for (const auto& rel : j["releases"]) {
                g.release_ids.push_back(sstr(rel, "id"));
                g.release_titles.push_back(sstr(rel, "title"));
            }
        g.rating = Rating::from(j);
        g.aliases = parse_aliases(j);
        g.tags = parse_tags(j);
        g.genres = parse_genres(j);
        g.same_as = parse_sameas(j);
        return g;
    }
    // Keys mirror IReleaseGroup.
    json to_json() const {
        json o = {{"id", id},
                  {"type", type},
                  {"title", title},
                  {"disambiguation", disambiguation},
                  {"first-release-date", first_release_date},
                  {"primary-type", primary_type},
                  {"secondary-types", secondary_types}};
        json ca = json::array();
        for (const auto& n : credit) ca.push_back(n.to_json());
        o["artist-credit"] = std::move(ca);
        json ra = json::array();
        for (size_t i = 0; i < release_ids.size(); ++i)
            ra.push_back({{"id", release_ids[i]},
                          {"title", i < release_titles.size() ? release_titles[i] : ""}});
        o["releases"] = std::move(ra);
        if (rating.present)
            o["rating"] = {{"value", rating.value}, {"votes-count", rating.votes}};
        o["aliases"] = aliases_json(aliases);
        o["tags"] = tags_json(tags);
        o["genres"] = genres_json(genres);
        o["sameAs"] = sameas_json(same_as);
        return o;
    }
};

struct Work {
    static constexpr std::string_view endpoint = "work";
    static constexpr std::string_view list_key = "works";
    static constexpr std::string_view default_inc = "aliases+tags+genres+ratings+url-rels";
    static constexpr bool searchable = true;
    static constexpr bool lookable = true;
    static constexpr bool browsable = true;
    static constexpr std::string_view browse_key = "works";
    // Mirrors BrowseWorksEntityParams.
    static constexpr std::array<std::string_view, 2> browse_links = {
        "artist", "collection"};
    // Mirrors WorkIncludes = MiscIncludes | RelationsIncludes (19).
    static constexpr std::array<std::string_view, 19> allowed_inc = {
        "aliases", "annotation", "tags", "genres", "ratings", "media",
        "area-rels", "artist-rels", "event-rels", "genre-rels",
        "instrument-rels", "label-rels", "place-rels", "recording-rels",
        "release-rels", "release-group-rels", "series-rels", "url-rels",
        "work-rels"};

    struct Attribute {
        std::string type, value;
    };
    std::string id, type, title, disambiguation, language;
    std::vector<std::string> languages, iswcs;
    std::vector<Attribute> attributes;
    Rating rating;
    std::vector<Alias> aliases;
    std::vector<Tag> tags;
    std::vector<Genre> genres;
    std::vector<SameAs> same_as;
    static Work from(const json& j) {
        Work w;
        w.id = sstr(j, "id");
        w.type = sstr(j, "type");
        w.title = sstr(j, "title");
        w.disambiguation = sstr(j, "disambiguation");
        w.language = sstr(j, "language");
        if (j.contains("languages") && j["languages"].is_array())
            for (const auto& l : j["languages"])
                if (l.is_string()) w.languages.push_back(l.get<std::string>());
        if (j.contains("iswcs") && j["iswcs"].is_array())
            for (const auto& s : j["iswcs"])
                if (s.is_string()) w.iswcs.push_back(s.get<std::string>());
        if (j.contains("attributes") && j["attributes"].is_array())
            for (const auto& a : j["attributes"])
                w.attributes.push_back({sstr(a, "type"), sstr(a, "value")});
        w.rating = Rating::from(j);
        w.aliases = parse_aliases(j);
        w.tags = parse_tags(j);
        w.genres = parse_genres(j);
        w.same_as = parse_sameas(j);
        return w;
    }
    // Keys mirror IWork (attributes: [{type, value}] per WS2 shape).
    json to_json() const {
        json o = {{"id", id},
                  {"type", type},
                  {"title", title},
                  {"disambiguation", disambiguation},
                  {"language", language},
                  {"languages", languages},
                  {"iswcs", iswcs}};
        json aa = json::array();
        for (const auto& a : attributes)
            aa.push_back({{"type", a.type}, {"value", a.value}});
        o["attributes"] = std::move(aa);
        if (rating.present)
            o["rating"] = {{"value", rating.value}, {"votes-count", rating.votes}};
        o["aliases"] = aliases_json(aliases);
        o["tags"] = tags_json(tags);
        o["genres"] = genres_json(genres);
        o["sameAs"] = sameas_json(same_as);
        return o;
    }
};

struct Area {
    static constexpr std::string_view endpoint = "area";
    static constexpr std::string_view list_key = "areas";
    static constexpr std::string_view default_inc = "aliases+tags+genres+ratings+url-rels";
    static constexpr bool searchable = true;
    static constexpr bool lookable = true;
    static constexpr bool browsable = true;
    // IBrowseAreasResult uses singular "area" key.
    static constexpr std::string_view browse_key = "area";
    static constexpr std::array<std::string_view, 1> browse_links = {"collection"};
    // Mirrors AreaIncludes = MiscIncludes | RelationsIncludes (19).
    static constexpr std::array<std::string_view, 19> allowed_inc = {
        "aliases", "annotation", "tags", "genres", "ratings", "media",
        "area-rels", "artist-rels", "event-rels", "genre-rels",
        "instrument-rels", "label-rels", "place-rels", "recording-rels",
        "release-rels", "release-group-rels", "series-rels", "url-rels",
        "work-rels"};

    std::string id, type, name, sort_name, disambiguation, begin, end;
    bool ended = false;
    std::vector<std::string> iso_codes;
    std::vector<Alias> aliases;
    std::vector<Tag> tags;
    std::vector<Genre> genres;
    std::vector<SameAs> same_as;
    static Area from(const json& j) {
        Area a;
        a.id = sstr(j, "id");
        a.type = sstr(j, "type");
        a.name = sstr(j, "name");
        a.sort_name = sstr(j, "sort-name");
        a.disambiguation = sstr(j, "disambiguation");
        if (j.contains("iso-3166-1-codes") && j["iso-3166-1-codes"].is_array())
            for (const auto& c : j["iso-3166-1-codes"])
                if (c.is_string()) a.iso_codes.push_back(c.get<std::string>());
        if (j.contains("life-span") && j["life-span"].is_object()) {
            a.begin = sstr(j["life-span"], "begin");
            a.end = sstr(j["life-span"], "end");
            a.ended = sbool(j["life-span"], "ended");
        }
        a.aliases = parse_aliases(j);
        a.tags = parse_tags(j);
        a.genres = parse_genres(j);
        a.same_as = parse_sameas(j);
        return a;
    }
    // Keys mirror IArea.
    json to_json() const {
        json o = {{"id", id},
                  {"name", name},
                  {"sort-name", sort_name},
                  {"type", type},
                  {"disambiguation", disambiguation},
                  {"iso-3166-1-codes", iso_codes}};
        if (!begin.empty() || !end.empty())
            o["life-span"] = {{"begin", begin}, {"end", end}, {"ended", ended}};
        o["aliases"] = aliases_json(aliases);
        o["tags"] = tags_json(tags);
        o["genres"] = genres_json(genres);
        o["sameAs"] = sameas_json(same_as);
        return o;
    }
};

struct Place {
    static constexpr std::string_view endpoint = "place";
    static constexpr std::string_view list_key = "places";
    static constexpr std::string_view default_inc = "aliases+tags+genres+ratings+url-rels";
    static constexpr bool searchable = true;
    static constexpr bool lookable = true;
    static constexpr bool browsable = true;
    // IBrowsePlacesResult uses singular "place" key.
    static constexpr std::string_view browse_key = "place";
    // Mirrors BrowsePlacesEntityParams.
    static constexpr std::array<std::string_view, 2> browse_links = {"area", "collection"};
    // Mirrors PlaceIncludes (19).
    static constexpr std::array<std::string_view, 19> allowed_inc = {
        "aliases", "annotation", "tags", "genres", "ratings", "media",
        "area-rels", "artist-rels", "event-rels", "genre-rels",
        "instrument-rels", "label-rels", "place-rels", "recording-rels",
        "release-rels", "release-group-rels", "series-rels", "url-rels",
        "work-rels"};

    std::string id, type, name, disambiguation, address, begin, end;
    bool ended = false;
    double latitude = 0, longitude = 0;
    bool has_coords = false;
    std::string area_id, area_name;
    Rating rating;
    std::vector<Alias> aliases;
    std::vector<Tag> tags;
    std::vector<Genre> genres;
    std::vector<SameAs> same_as;
    static Place from(const json& j) {
        Place p;
        p.id = sstr(j, "id");
        p.type = sstr(j, "type");
        p.name = sstr(j, "name");
        p.disambiguation = sstr(j, "disambiguation");
        p.address = sstr(j, "address");
        if (j.contains("coordinates") && j["coordinates"].is_object()) {
            auto it1 = j["coordinates"].find("latitude");
            auto it2 = j["coordinates"].find("longitude");
            if (it1 != j["coordinates"].end() && it1->is_number() &&
                it2 != j["coordinates"].end() && it2->is_number()) {
                p.latitude = it1->get<double>();
                p.longitude = it2->get<double>();
                p.has_coords = true;
            }
        }
        if (j.contains("life-span") && j["life-span"].is_object()) {
            p.begin = sstr(j["life-span"], "begin");
            p.end = sstr(j["life-span"], "end");
            p.ended = sbool(j["life-span"], "ended");
        }
        if (j.contains("area") && j["area"].is_object()) {
            p.area_id = sstr(j["area"], "id");
            p.area_name = sstr(j["area"], "name");
        }
        p.rating = Rating::from(j);
        p.aliases = parse_aliases(j);
        p.tags = parse_tags(j);
        p.genres = parse_genres(j);
        p.same_as = parse_sameas(j);
        return p;
    }
    // Keys mirror IPlace.
    json to_json() const {
        json o = {{"id", id},
                  {"name", name},
                  {"type", type},
                  {"disambiguation", disambiguation},
                  {"address", address}};
        if (has_coords)
            o["coordinates"] = {{"latitude", latitude}, {"longitude", longitude}};
        if (!begin.empty() || !end.empty())
            o["life-span"] = {{"begin", begin}, {"end", end}, {"ended", ended}};
        if (!area_id.empty()) o["area"] = {{"id", area_id}, {"name", area_name}};
        if (rating.present)
            o["rating"] = {{"value", rating.value}, {"votes-count", rating.votes}};
        o["aliases"] = aliases_json(aliases);
        o["tags"] = tags_json(tags);
        o["genres"] = genres_json(genres);
        o["sameAs"] = sameas_json(same_as);
        return o;
    }
};

struct Event {
    static constexpr std::string_view endpoint = "event";
    static constexpr std::string_view list_key = "events";
    static constexpr std::string_view default_inc = "aliases+tags+genres+ratings+url-rels";
    static constexpr bool searchable = true;
    static constexpr bool lookable = true;
    static constexpr bool browsable = true;
    static constexpr std::string_view browse_key = "events";
    // Mirrors BrowseEventsEntityParams.
    static constexpr std::array<std::string_view, 4> browse_links = {
        "area", "artist", "collection", "place"};
    // Mirrors EventIncludes (19).
    static constexpr std::array<std::string_view, 19> allowed_inc = {
        "aliases", "annotation", "tags", "genres", "ratings", "media",
        "area-rels", "artist-rels", "event-rels", "genre-rels",
        "instrument-rels", "label-rels", "place-rels", "recording-rels",
        "release-rels", "release-group-rels", "series-rels", "url-rels",
        "work-rels"};

    std::string id, type, name, disambiguation, time, setlist, begin, end;
    bool cancelled = false, ended = false;
    Rating rating;
    std::vector<Alias> aliases;
    std::vector<Tag> tags;
    std::vector<Genre> genres;
    std::vector<SameAs> same_as;
    static Event from(const json& j) {
        Event e;
        e.id = sstr(j, "id");
        e.type = sstr(j, "type");
        e.name = sstr(j, "name");
        e.disambiguation = sstr(j, "disambiguation");
        e.time = sstr(j, "time");
        e.setlist = sstr(j, "setlist");
        e.cancelled = sbool(j, "cancelled");
        if (j.contains("life-span") && j["life-span"].is_object()) {
            e.begin = sstr(j["life-span"], "begin");
            e.end = sstr(j["life-span"], "end");
            e.ended = sbool(j["life-span"], "ended");
        }
        e.rating = Rating::from(j);
        e.aliases = parse_aliases(j);
        e.tags = parse_tags(j);
        e.genres = parse_genres(j);
        e.same_as = parse_sameas(j);
        return e;
    }
    // Keys mirror IEvent.
    json to_json() const {
        json o = {{"id", id},
                  {"name", name},
                  {"type", type},
                  {"disambiguation", disambiguation},
                  {"time", time},
                  {"setlist", setlist},
                  {"cancelled", cancelled}};
        if (!begin.empty() || !end.empty())
            o["life-span"] = {{"begin", begin}, {"end", end}, {"ended", ended}};
        if (rating.present)
            o["rating"] = {{"value", rating.value}, {"votes-count", rating.votes}};
        o["aliases"] = aliases_json(aliases);
        o["tags"] = tags_json(tags);
        o["genres"] = genres_json(genres);
        o["sameAs"] = sameas_json(same_as);
        return o;
    }
};

struct Series {
    static constexpr std::string_view endpoint = "series";
    static constexpr std::string_view list_key = "series";
    static constexpr std::string_view default_inc = "aliases+tags+genres+ratings+url-rels";
    static constexpr bool searchable = true;
    static constexpr bool lookable = true;
    static constexpr bool browsable = true;
    static constexpr std::string_view browse_key = "series";
    static constexpr std::array<std::string_view, 1> browse_links = {"collection"};
    // Mirrors SeriesIncludes (19).
    static constexpr std::array<std::string_view, 19> allowed_inc = {
        "aliases", "annotation", "tags", "genres", "ratings", "media",
        "area-rels", "artist-rels", "event-rels", "genre-rels",
        "instrument-rels", "label-rels", "place-rels", "recording-rels",
        "release-rels", "release-group-rels", "series-rels", "url-rels",
        "work-rels"};

    std::string id, type, name, disambiguation;
    std::vector<Alias> aliases;
    std::vector<Tag> tags;
    std::vector<Genre> genres;
    std::vector<SameAs> same_as;
    static Series from(const json& j) {
        Series s;
        s.id = sstr(j, "id");
        s.type = sstr(j, "type");
        s.name = sstr(j, "name");
        s.disambiguation = sstr(j, "disambiguation");
        s.aliases = parse_aliases(j);
        s.tags = parse_tags(j);
        s.genres = parse_genres(j);
        s.same_as = parse_sameas(j);
        return s;
    }
    // Keys mirror ISeries.
    json to_json() const {
        json o = {{"id", id},
                  {"name", name},
                  {"type", type},
                  {"disambiguation", disambiguation}};
        o["aliases"] = aliases_json(aliases);
        o["tags"] = tags_json(tags);
        o["genres"] = genres_json(genres);
        o["sameAs"] = sameas_json(same_as);
        return o;
    }
};

struct Instrument {
    static constexpr std::string_view endpoint = "instrument";
    static constexpr std::string_view list_key = "instruments";
    static constexpr std::string_view default_inc = "aliases+tags+genres+ratings+url-rels";
    static constexpr bool searchable = true;
    static constexpr bool lookable = true;
    static constexpr bool browsable = true;
    static constexpr std::string_view browse_key = "instruments";
    static constexpr std::array<std::string_view, 1> browse_links = {"collection"};
    // Mirrors InstrumentIncludes (19).
    static constexpr std::array<std::string_view, 19> allowed_inc = {
        "aliases", "annotation", "tags", "genres", "ratings", "media",
        "area-rels", "artist-rels", "event-rels", "genre-rels",
        "instrument-rels", "label-rels", "place-rels", "recording-rels",
        "release-rels", "release-group-rels", "series-rels", "url-rels",
        "work-rels"};

    std::string id, type, name, disambiguation, description;
    std::vector<Alias> aliases;
    std::vector<Tag> tags;
    std::vector<Genre> genres;
    std::vector<SameAs> same_as;
    static Instrument from(const json& j) {
        Instrument v;
        v.id = sstr(j, "id");
        v.type = sstr(j, "type");
        v.name = sstr(j, "name");
        v.disambiguation = sstr(j, "disambiguation");
        v.description = sstr(j, "description");
        v.aliases = parse_aliases(j);
        v.tags = parse_tags(j);
        v.genres = parse_genres(j);
        v.same_as = parse_sameas(j);
        return v;
    }
    // Keys mirror IInstrument.
    json to_json() const {
        json o = {{"id", id},
                  {"name", name},
                  {"type", type},
                  {"disambiguation", disambiguation},
                  {"description", description}};
        o["aliases"] = aliases_json(aliases);
        o["tags"] = tags_json(tags);
        o["genres"] = genres_json(genres);
        o["sameAs"] = sameas_json(same_as);
        return o;
    }
};

struct Collection {
    static constexpr std::string_view endpoint = "collection";
    static constexpr std::string_view list_key = "collections";
    static constexpr std::string_view default_inc = "";
    static constexpr bool searchable = false; // no search endpoint in TS
    static constexpr bool lookable = true;
    static constexpr bool browsable = true;
    static constexpr std::string_view browse_key = "collections";
    // Mirrors BrowseCollectionsEntityParams.
    static constexpr std::array<std::string_view, 10> browse_links = {
        "area", "artist", "editor", "event", "label",
        "place", "recording", "release", "release-group", "work"};
    // Mirrors CollectionIncludes = MiscIncludes | RelationsIncludes
    //   | user-collections (20).
    static constexpr std::array<std::string_view, 20> allowed_inc = {
        "aliases", "annotation", "tags", "genres", "ratings", "media",
        "area-rels", "artist-rels", "event-rels", "genre-rels",
        "instrument-rels", "label-rels", "place-rels", "recording-rels",
        "release-rels", "release-group-rels", "series-rels", "url-rels",
        "work-rels", "user-collections"};

    std::string id, type, name, editor, entity_type;
    int recording_count = 0;
    std::vector<Alias> aliases;
    std::vector<Tag> tags;
    std::vector<Genre> genres;
    std::vector<SameAs> same_as;
    static Collection from(const json& j) {
        Collection c;
        c.id = sstr(j, "id");
        c.type = sstr(j, "type");
        c.name = sstr(j, "name");
        c.editor = sstr(j, "editor");
        c.entity_type = sstr(j, "entity-type");
        c.recording_count = as_int(j, "recording-count");
        c.aliases = parse_aliases(j);
        c.tags = parse_tags(j);
        c.genres = parse_genres(j);
        c.same_as = parse_sameas(j);
        return c;
    }
    // Keys mirror ICollection.
    json to_json() const {
        json o = {{"id", id},
                  {"name", name},
                  {"type", type},
                  {"editor", editor},
                  {"entity-type", entity_type},
                  {"recording-count", recording_count}};
        o["aliases"] = aliases_json(aliases);
        o["tags"] = tags_json(tags);
        o["genres"] = genres_json(genres);
        o["sameAs"] = sameas_json(same_as);
        return o;
    }
};

struct Url {
    static constexpr std::string_view endpoint = "url";
    static constexpr std::string_view list_key = "urls";
    static constexpr std::string_view default_inc = "url-rels";
    static constexpr bool searchable = true;
    static constexpr bool lookable = true;
    static constexpr bool browsable = true;
    // Browse urls by resource URI (not an MBID).
    static constexpr std::string_view browse_key = "urls";
    static constexpr std::array<std::string_view, 1> browse_links = {"resource"};
    // Mirrors UrlIncludes = RelationsIncludes (13).
    static constexpr std::array<std::string_view, 13> allowed_inc = {
        "area-rels", "artist-rels", "event-rels", "genre-rels",
        "instrument-rels", "label-rels", "place-rels", "recording-rels",
        "release-rels", "release-group-rels", "series-rels", "url-rels",
        "work-rels"};

    std::string id, resource;
    std::vector<SameAs> same_as;
    static Url from(const json& j) {
        Url u;
        u.id = sstr(j, "id");
        u.resource = sstr(j, "resource");
        u.same_as = parse_sameas(j);
        return u;
    }
    // Keys mirror IUrl.
    json to_json() const {
        json o = {{"id", id}, {"resource", resource}};
        o["sameAs"] = sameas_json(same_as);
        return o;
    }
};

struct Annotation {
    static constexpr std::string_view endpoint = "annotation";
    static constexpr std::string_view list_key = "annotations";
    static constexpr std::string_view default_inc = "";
    static constexpr bool searchable = true;
    static constexpr bool lookable = false; // no lookup endpoint in TS
    static constexpr bool browsable = false;
    // Search accepts the same inc set (TS ISearchQuery<I>).
    static constexpr std::array<std::string_view, 19> allowed_inc = {
        "aliases", "annotation", "tags", "genres", "ratings", "media",
        "area-rels", "artist-rels", "event-rels", "genre-rels",
        "instrument-rels", "label-rels", "place-rels", "recording-rels",
        "release-rels", "release-group-rels", "series-rels", "url-rels",
        "work-rels"};

    std::string entity, name, text, type;
    static Annotation from(const json& j) {
        return {sstr(j, "entity"), sstr(j, "name"), sstr(j, "text"),
                sstr(j, "type")};
    }
    // Keys mirror IAnnotation.
    json to_json() const {
        return {{"entity", entity},
                {"name", name},
                {"text", text},
                {"type", type}};
    }
};

struct TagEntity {
    static constexpr std::string_view endpoint = "tag";
    static constexpr std::string_view list_key = "tags";
    static constexpr std::string_view default_inc = "";
    static constexpr bool searchable = true;
    static constexpr bool lookable = false; // no lookup endpoint in TS
    static constexpr bool browsable = false;
    // Search accepts the same inc set (TS ISearchQuery<I>).
    static constexpr std::array<std::string_view, 19> allowed_inc = {
        "aliases", "annotation", "tags", "genres", "ratings", "media",
        "area-rels", "artist-rels", "event-rels", "genre-rels",
        "instrument-rels", "label-rels", "place-rels", "recording-rels",
        "release-rels", "release-group-rels", "series-rels", "url-rels",
        "work-rels"};

    std::string name;
    static TagEntity from(const json& j) {
        return {sstr(j, "name")};
    }
    // Keys mirror ITag.
    json to_json() const {
        return {{"name", name}};
    }
};

struct CdStub {
    static constexpr std::string_view endpoint = "cdstub";
    static constexpr std::string_view list_key = "cdstubs";
    static constexpr std::string_view default_inc = "";
    static constexpr bool searchable = true;
    static constexpr bool lookable = false; // no lookup endpoint in TS
    static constexpr bool browsable = false;
    // Search accepts the same inc set (TS ISearchQuery<I>).
    static constexpr std::array<std::string_view, 19> allowed_inc = {
        "aliases", "annotation", "tags", "genres", "ratings", "media",
        "area-rels", "artist-rels", "event-rels", "genre-rels",
        "instrument-rels", "label-rels", "place-rels", "recording-rels",
        "release-rels", "release-group-rels", "series-rels", "url-rels",
        "work-rels"};

    std::string id, title, artist, barcode, comment;
    static CdStub from(const json& j) {
        return {sstr(j, "id"), sstr(j, "title"), sstr(j, "artist"),
                sstr(j, "barcode"), sstr(j, "comment")};
    }
    // Keys mirror ICdStub.
    json to_json() const {
        return {{"id", id},
                {"title", title},
                {"artist", artist},
                {"barcode", barcode},
                {"comment", comment}};
    }
};

// The registry: adding an entity = one line here.
using AllEntities =
    std::tuple<Artist, Release, Recording, Disc, Label, ReleaseGroup, Work,
               Area, Place, Event, Series, Instrument, Collection, Url,
               Annotation, TagEntity, CdStub>;

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
        throw CliError(2, "expected string param");
    return it->get<std::string>();
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
            throw CliError(2, "unknown inc '" + std::string(tok) + "' for " +
                                  std::string(E::endpoint));
        i = j;
    }
}


template <typename E>
json do_search(const json& p) {
    static_assert(E::searchable, "entity is lookup-only");
    PMap pm;
    pm["query"] = get_str(p, "query");
    pm["limit"] = std::to_string(get_int(p, "limit", 10));
    pm["offset"] = std::to_string(get_int(p, "offset", 0));
    std::string inc = get_str(p, "inc");
    if (!inc.empty()) {
        check_inc<E>(inc);
        pm["inc"] = inc;
    }
    json raw = json::parse(
        fetch(bpath(std::string(E::endpoint), "", "", pm)));
    json result = {{"count", as_int(raw, "count")},
                   {"offset", as_int(raw, "offset")}};
    json arr = json::array();
    const std::string key(E::list_key);
    if (raw.contains(key) && raw[key].is_array())
        for (const auto& e : raw[key]) {
            // IMatch: search hits carry a score.
            json item = E::from(e).to_json();
            if (e.contains("score") && e["score"].is_number())
                item["score"] = e["score"];
            arr.push_back(std::move(item));
        }
    result[E::list_key] = std::move(arr);
    return result;
}

template <typename E>
json do_lookup(const json& p) {
    static_assert(E::lookable, "entity is search-only");
    PMap pm;
    std::string inc = get_str(p, "inc");
    if (inc.empty()) inc = std::string(E::default_inc);
    if (!inc.empty()) {
        check_inc<E>(inc);
        pm["inc"] = inc;
    }
    json raw = json::parse(
        fetch(bpath(std::string(E::endpoint), get_str(p, "id"), "", pm)));
    return E::from(raw).to_json();
}

// Mirrors musicbrainz-api browse(): /<entity>?<linked>=<mbid>,
// exactly one linked key from the entity's BrowseXEntityParams set.
// Response keys mirror IBrowseXResult: <browse_key> (+ <endpoint>-count
// / <endpoint>-offset); area's list is a single object, normalized here.
template <typename E>
json do_browse(const json& p) {
    static_assert(E::browsable, "entity is not browsable");
    PMap pm;
    std::string linked;
    int found = 0;
    for (auto a : E::browse_links) {
        const std::string k(a);
        auto it = p.find(k);
        if (it != p.end() && !it->is_null()) {
            if (!it->is_string())
                throw CliError(2, "browse link must be a string");
            linked = k;
            pm[k] = it->get<std::string>();
            ++found;
        }
    }
    if (found != 1)
        throw CliError(2, "browse needs exactly one linked entity");
    pm["limit"] = std::to_string(get_int(p, "limit", 10));
    pm["offset"] = std::to_string(get_int(p, "offset", 0));
    std::string inc = get_str(p, "inc");
    if (!inc.empty()) {
        check_inc<E>(inc);
        pm["inc"] = inc;
    }
    (void)linked;
    json raw = json::parse(fetch(bpath(std::string(E::endpoint), "", "", pm)));
    const std::string ep(E::endpoint), bk(E::browse_key);
    json result = {{ep + "-count", as_int(raw, (ep + "-count").c_str())},
                   {ep + "-offset", as_int(raw, (ep + "-offset").c_str())}};
    json arr = json::array();
    if (raw.contains(bk)) {
        if (raw[bk].is_array())
            for (const auto& e : raw[bk]) arr.push_back(E::from(e).to_json());
        else if (raw[bk].is_object())
            arr.push_back(E::from(raw[bk]).to_json());
    }
    result[bk] = std::move(arr);
    return result;
}

// Runtime dispatch table: entity string -> compile-time entity type.
// Adding an entity = one line in AllEntities; everything else follows.
using SearchFn = json (*)(const json&);
using LookupFn = json (*)(const json&);
using BrowseFn = json (*)(const json&);

struct EntityOps {
    bool searchable = false, lookable = false, browsable = false;
    SearchFn search = nullptr;
    LookupFn lookup = nullptr;
    BrowseFn browse = nullptr;
    std::string list_key, browse_key;
};

template <typename E>
void add_ops(std::map<std::string, EntityOps>& m) {
    EntityOps o;
    o.list_key = std::string(E::list_key);
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
        o.browse_key = std::string(E::browse_key);
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
// Field names mirror musicbrainz-api shapes (kebab-case).
// ---------------------------------------------------------------------------
static std::string jstr(const json& j, const char* k) {
    auto it = j.find(k);
    if (it == j.end() || it->is_null() || !it->is_string()) return {};
    return it->get<std::string>();
}

static std::string ms_format(long long ms) {
    if (ms <= 0) return "";
    long long s = ms / 1000;
    char b[32];
    snprintf(b, sizeof b, "%lld:%02lld", s / 60, s % 60);
    return b;
}

static std::string credit_string(const json& e) {
    std::string r;
    auto it = e.find("artist-credit");
    if (it != e.end() && it->is_array())
        for (const auto& c : *it)
            r += jstr(c, "name") + jstr(c, "joinphrase");
    return r;
}

static std::string summary(const std::string& entity, const json& it) {
    if (entity == "artist") {
        std::string r = jstr(it, "name");
        std::string t = jstr(it, "type"), c = jstr(it, "country");
        if (!t.empty()) r += " [" + t + "]";
        if (!c.empty()) r += " (" + c + ")";
        return r;
    }
    if (entity == "release") {
        std::string r = jstr(it, "title"), d = jstr(it, "date"), s = jstr(it, "status");
        if (!d.empty()) r += " (" + d + ")";
        if (!s.empty()) r += " [" + s + "]";
        std::string ac = credit_string(it);
        if (!ac.empty()) r += " \xe2\x80\x94 " + ac;
        return r;
    }
    if (entity == "recording") {
        std::string r = jstr(it, "title");
        auto f = it.find("length");
        if (f != it.end() && f->is_number())
            r += " (" + ms_format(f->get<long long>()) + ")";
        std::string ac = credit_string(it);
        if (!ac.empty()) r += " \xe2\x80\x94 " + ac;
        return r;
    }
    if (entity == "label") {
        std::string r = jstr(it, "name"), t = jstr(it, "type"), c = jstr(it, "label-code");
        if (!t.empty()) r += " [" + t + "]";
        if (!c.empty()) r += " (LC " + c + ")";
        return r;
    }
    if (entity == "release-group") {
        std::string r = jstr(it, "title"), d = jstr(it, "first-release-date"),
                    p = jstr(it, "primary-type");
        if (!d.empty()) r += " (" + d + ")";
        if (!p.empty()) r += " [" + p + "]";
        return r;
    }
    if (entity == "work") {
        std::string r = jstr(it, "title"), t = jstr(it, "type"), l = jstr(it, "language");
        if (!t.empty()) r += " [" + t + "]";
        if (!l.empty()) r += " (" + l + ")";
        return r;
    }
    if (entity == "cdstub") {
        std::string r = jstr(it, "title"), a = jstr(it, "artist");
        if (!a.empty()) r += " \xe2\x80\x94 " + a;
        return r;
    }
    if (entity == "url") return jstr(it, "resource");
    std::string r = jstr(it, "name");
    if (r.empty()) r = jstr(it, "title");
    std::string t = jstr(it, "type");
    if (!t.empty()) r += " [" + t + "]";
    return r;
}

static void meta_row(std::ostringstream& o, const char* label, const std::string& v) {
    if (!v.empty()) o << label << ": " << v << "\n";
}

static void render_detail(std::ostringstream& o, const std::string& entity, const json& e);

static void render_tags_genres_links(std::ostringstream& o, const json& e) {
    auto ti = e.find("tags");
    if (ti != e.end() && ti->is_array() && !ti->empty()) {
        o << "\nTags (" << ti->size() << ")\n";
        for (const auto& t : *ti) {
            o << "- " << jstr(t, "name");
            auto c = t.find("count");
            if (c != t.end() && c->is_number()) o << " (" << c->get<long long>() << ")";
            o << "\n";
        }
    }
    auto gi = e.find("genres");
    if (gi != e.end() && gi->is_array() && !gi->empty()) {
        o << "\nGenres (" << gi->size() << ")\n";
        for (const auto& g : *gi) {
            o << "- " << jstr(g, "name");
            auto c = g.find("count");
            if (c != g.end() && c->is_number()) o << " (" << c->get<long long>() << ")";
            o << "\n";
        }
    }
    auto li = e.find("sameAs");
    if (li != e.end() && li->is_array() && !li->empty()) {
        o << "\nLinks (" << li->size() << ")\n";
        for (const auto& l : *li)
            o << "- [" << jstr(l, "type") << "] " << jstr(l, "url") << "\n";
    }
}

static void render_credit(std::ostringstream& o, const json& e) {
    std::string ac = credit_string(e);
    if (!ac.empty()) o << "Artists: " << ac << "\n";
}

static void render_lifespan(std::ostringstream& o, const json& e) {
    auto it = e.find("life-span");
    if (it != e.end() && it->is_object()) {
        std::string b = jstr(*it, "begin"), en = jstr(*it, "end");
        if (!b.empty() || !en.empty())
            o << "\nLife Span\nBegin: " << b << "\nEnd: " << en << "\n";
    }
}

static void render_releases(std::ostringstream& o, const json& e) {
    auto it = e.find("releases");
    if (it != e.end() && it->is_array() && !it->empty()) {
        o << "\nReleases (" << it->size() << ")\n";
        for (const auto& r : *it)
            o << "- " << jstr(r, "title") << "\n  " << jstr(r, "id") << "\n";
    }
}

static void render_detail(std::ostringstream& o, const std::string& entity, const json& e) {
    meta_row(o, "ID", jstr(e, "id"));
    if (entity == "artist") {
        meta_row(o, "Type", jstr(e, "type"));
        meta_row(o, "Country", jstr(e, "country"));
        meta_row(o, "Sort Name", jstr(e, "sort-name"));
        meta_row(o, "Disambiguation", jstr(e, "disambiguation"));
        render_lifespan(o, e);
        if (auto a = e.find("area"); a != e.end() && a->is_object())
            meta_row(o, "Area", jstr(*a, "name"));
    } else if (entity == "release") {
        meta_row(o, "Title", jstr(e, "title"));
        meta_row(o, "Status", jstr(e, "status"));
        meta_row(o, "Quality", jstr(e, "quality"));
        meta_row(o, "Packaging", jstr(e, "packaging"));
        meta_row(o, "Date", jstr(e, "date"));
        meta_row(o, "Country", jstr(e, "country"));
        meta_row(o, "Barcode", jstr(e, "barcode"));
        meta_row(o, "ASIN", jstr(e, "asin"));
        render_credit(o, e);
        if (auto g = e.find("release-group");
            g != e.end() && g->is_object())
            o << "Group: " << jstr(*g, "title") << " [" << jstr(*g, "primary-type")
              << "]\n";
        if (auto li = e.find("label-info");
            li != e.end() && li->is_array() && !li->empty()) {
            o << "\nLabels (" << li->size() << ")\n";
            for (const auto& l : *li) {
                std::string nm, cat = jstr(l, "catalog-number");
                if (auto lb = l.find("label"); lb != l.end() && lb->is_object())
                    nm = jstr(*lb, "name");
                o << "- " << nm;
                if (!cat.empty()) o << " (" << cat << ")";
                o << "\n";
            }
        }
        if (auto ev = e.find("release-events");
            ev != e.end() && ev->is_array() && !ev->empty()) {
            o << "\nEvents (" << ev->size() << ")\n";
            for (const auto& v : *ev) {
                o << "- " << jstr(v, "date");
                if (auto ar = v.find("area"); ar != v.end() && ar->is_object())
                    o << " (" << jstr(*ar, "name") << ")";
                o << "\n";
            }
        }
        if (auto m = e.find("media"); m != e.end() && m->is_array())
            for (const auto& med : *m) {
                o << "\n[" << jstr(med, "format") << "]\n";
                if (auto t = med.find("tracks"); t != med.end() && t->is_array())
                    for (const auto& tr : *t) {
                        auto ln = tr.find("length");
                        long long ms = (ln != tr.end() && ln->is_number())
                                           ? ln->get<long long>()
                                           : 0;
                        o << "  " << jstr(tr, "number") << ". " << jstr(tr, "title")
                          << " (" << ms_format(ms) << ")\n";
                        if (auto rc = tr.find("recording");
                            rc != tr.end() && rc->is_object())
                            o << "      " << jstr(*rc, "id") << "\n";
                    }
            }
    } else if (entity == "recording") {
        meta_row(o, "Title", jstr(e, "title"));
        meta_row(o, "Length", ms_format(as_ll(e, "length")));
        auto vd = e.find("video");
        o << "Video: " << ((vd != e.end() && vd->is_boolean() && vd->get<bool>()) ? "yes" : "no")
          << "\n";
        render_credit(o, e);
        if (auto is = e.find("isrcs"); is != e.end() && is->is_array() && !is->empty()) {
            o << "ISRCs: ";
            bool first = true;
            for (const auto& s : *is) {
                if (!s.is_string()) continue;
                if (!first) o << ", ";
                o << s.get<std::string>();
                first = false;
            }
            o << "\n";
        }
        meta_row(o, "First release", jstr(e, "first-release-date"));
        render_releases(o, e);
    } else if (entity == "discid") {
        auto sc = e.find("sectors");
        if (sc != e.end() && sc->is_number())
            o << "Sectors: " << sc->get<long long>() << "\n";
        render_releases(o, e);
    } else if (entity == "label") {
        meta_row(o, "Name", jstr(e, "name"));
        meta_row(o, "Type", jstr(e, "type"));
        meta_row(o, "Country", jstr(e, "country"));
        meta_row(o, "Sort Name", jstr(e, "sort-name"));
        meta_row(o, "Label Code", jstr(e, "label-code"));
        meta_row(o, "Disambiguation", jstr(e, "disambiguation"));
        if (auto a = e.find("area"); a != e.end() && a->is_object())
            meta_row(o, "Area", jstr(*a, "name"));
        render_lifespan(o, e);
    } else if (entity == "release-group") {
        meta_row(o, "Title", jstr(e, "title"));
        meta_row(o, "Type", jstr(e, "type"));
        meta_row(o, "Disambiguation", jstr(e, "disambiguation"));
        meta_row(o, "First date", jstr(e, "first-release-date"));
        meta_row(o, "Primary", jstr(e, "primary-type"));
        render_credit(o, e);
        render_releases(o, e);
    } else if (entity == "work") {
        meta_row(o, "Title", jstr(e, "title"));
        meta_row(o, "Type", jstr(e, "type"));
        meta_row(o, "Disambiguation", jstr(e, "disambiguation"));
        meta_row(o, "Language", jstr(e, "language"));
        render_credit(o, e);
    } else {
        // Generic fallback: scalar fields, then shared sections.
        for (auto it = e.begin(); it != e.end(); ++it) {
            if (it->is_string() && !it->get<std::string>().empty())
                o << it.key() << ": " << it->get<std::string>() << "\n";
            else if (it->is_number())
                o << it.key() << ": " << it->dump() << "\n";
            else if (it->is_boolean())
                o << it.key() << ": " << (it->get<bool>() ? "true" : "false") << "\n";
        }
        render_lifespan(o, e);
    }
    // Shared sections (ratings/aliases where the model has them).
    if (auto r = e.find("rating"); r != e.end() && r->is_object()) {
        auto v = r->find("value"), c = r->find("votes-count");
        if (v != r->end() && v->is_number())
            o << "Rating: " << v->get<double>() << " ("
              << (c != r->end() && c->is_number() ? std::to_string(c->get<long long>())
                                                 : "?")
              << " votes)\n";
    }
    if (auto al = e.find("aliases"); al != e.end() && al->is_array() && !al->empty()) {
        o << "\nAliases (" << al->size() << ")\n";
        for (const auto& a : *al) {
            o << "- " << jstr(a, "name");
            std::string lc = jstr(a, "locale"), ty = jstr(a, "type");
            if (!lc.empty()) o << " [" << lc << "]";
            if (!ty.empty()) o << " (" << ty << ")";
            o << "\n";
        }
    }
    render_tags_genres_links(o, e);
}

// ---------------------------------------------------------------------------
// CLI argument parsing.
//   musicbrainz search <entity> --query Q [--limit N] [--offset N] [--inc I] [--json]
//   musicbrainz lookup <entity> <mbid> [--inc I] [--json]
//   musicbrainz browse <entity> --<link> <mbid> [--limit N] [--offset N] [--inc I] [--json]
//   musicbrainz query --entity E [--id ID] [--param k=v ...]   (always JSON)
// ---------------------------------------------------------------------------
struct Args {
    std::string op, entity, id, query, resource, inc;
    int limit = 10, offset = 0;
    bool json = false, help = false;
    PMap params;
    PMap extra; // browse link key -> value
};

static const char* kUsage =
    "usage:\n"
    "  musicbrainz search <entity> --query Q [--limit N] [--offset N] [--inc I] [--json]\n"
    "  musicbrainz lookup <entity> <mbid> [--inc I] [--json]\n"
    "  musicbrainz browse <entity> --<link> <mbid> [--limit N] [--offset N] [--inc I] [--json]\n"
    "  musicbrainz query --entity E [--id ID] [--param k=v ...]\n"
    "entities:\n"
    "  artist release recording label release-group work area place\n"
    "  event series instrument collection url annotation tag cdstub discid\n";

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
            else if (k == "resource") a.resource = v;
            else if (k == "limit") a.limit = parse_int_flag("--limit", v);
            else if (k == "offset") a.offset = parse_int_flag("--offset", v);
            else if (k == "inc") a.inc = v;
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
        a.help = true; // bare `musicbrainz` behaves like --help
        return a;
    }
    a.op = pos[0];
    if (a.op == "search" || a.op == "lookup" || a.op == "browse") {
        if (pos.size() < 2) throw CliError(2, a.op + " needs an entity\n" + kUsage);
        a.entity = pos[1];
        if (a.op == "lookup") {
            if (pos.size() < 3 && a.id.empty())
                throw CliError(2, "lookup needs an mbid");
            if (!a.id.empty() && pos.size() >= 3 && pos[2] != a.id)
                throw CliError(2, "conflicting mbids");
            if (a.id.empty()) a.id = pos[2];
        } else if (pos.size() > 2)
            throw CliError(2, "unexpected argument '" + pos[2] + "'");
    } else if (a.op != "query")
        throw CliError(2, "unknown subcommand '" + a.op + "'\n" + kUsage);
    return a;
}

} // namespace musicbrainz

using namespace musicbrainz;

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
            if (a.entity.empty()) throw CliError(2, "query needs --entity");
            PMap pm = a.params;
            pm["fmt"] = "json";
            std::cout << json::parse(fetch(bpath(a.entity, a.id, a.resource, pm))).dump(2)
                      << "\n";
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
            if (!a.inc.empty()) params["inc"] = a.inc;
            res = eo.search(params);
            list_key = eo.list_key;
        } else if (a.op == "lookup") {
            if (!eo.lookable) throw CliError(2, "entity '" + a.entity + "' is not lookable");
            params = {{"id", a.id}};
            if (!a.inc.empty()) params["inc"] = a.inc;
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
            if (!a.inc.empty()) params["inc"] = a.inc;
            res = eo.browse(params);
            list_key = eo.browse_key;
        } else
            throw CliError(2, "unknown subcommand '" + a.op + "'");
        if (a.json) {
            std::cout << res.dump(2) << "\n";
            return 0;
        }
        std::ostringstream o;
        std::string desc = a.op == "browse" ? ("by " + a.extra.begin()->first + " " + a.extra.begin()->second)
                                            : ("\"" + a.query + "\"");
        auto li = res.find(list_key);
        size_t n = (li != res.end() && li->is_array()) ? li->size() : 0;
        auto ci = res.find("count");
        if (ci == res.end())
            ci = res.find(a.entity + "-count");
        long long total = (ci != res.end() && ci->is_number()) ? ci->get<long long>() : (long long)n;
        o << a.entity << " " << desc << " — " << n << " of " << total << "\n";
        int i = 1;
        if (li != res.end() && li->is_array())
            for (const auto& e : *li) {
                auto id = e.find("id");
                std::string sid = (id != e.end() && id->is_string()) ? id->get<std::string>() : "";
                o << "  " << i++ << ". " << summary(a.entity, e) << "\n";
                if (!sid.empty()) o << "      " << sid << "\n";
            }
        std::cout << o.str();
        return 0;
    } catch (const CliError& e) {
        std::cerr << "musicbrainz: error: " << e.what() << "\n";
        return e.code;
    } catch (const std::exception& e) {
        std::cerr << "musicbrainz: error: " << e.what() << "\n";
        return 1;
    }
}
