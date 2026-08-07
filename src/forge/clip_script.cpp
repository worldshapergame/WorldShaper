#include "forge/clip_script.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

#include "world/tags.hpp"

namespace ws {
namespace forge {

namespace {

// --- tokens ---------------------------------------------------------------------------
//
// Whitespace separated, with braces, equals and commas as tokens of their own so that
// `rgb=120,120,116` and `union { a b }` both fall out without the tokenizer knowing what either
// means. A comment runs to the end of its line.
struct Token {
    std::string text;
    u32 line = 0;
    bool starts_line = false;
};

std::vector<Token> tokenize(const std::string& source) {
    std::vector<Token> tokens;
    u32 line = 1;
    bool at_line_start = true;
    usize i = 0;
    while (i < source.size()) {
        const char c = source[i];
        if (c == '#') {
            while (i < source.size() && source[i] != '\n') ++i;
            continue;
        }
        if (c == '\n') {
            ++line;
            at_line_start = true;
            ++i;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            ++i;
            continue;
        }
        Token token;
        token.line = line;
        token.starts_line = at_line_start;
        at_line_start = false;
        if (c == '{' || c == '}' || c == '=' || c == ',') {
            token.text = std::string(1, c);
            ++i;
        } else {
            const usize begin = i;
            while (i < source.size()) {
                const char d = source[i];
                if (d == ' ' || d == '\t' || d == '\r' || d == '\n' || d == '{' || d == '}' ||
                    d == '=' || d == ',' || d == '#') {
                    break;
                }
                ++i;
            }
            token.text = source.substr(begin, i - begin);
        }
        tokens.push_back(token);
    }
    return tokens;
}

bool is_number(const std::string& s) {
    if (s.empty()) return false;
    char* end = nullptr;
    std::strtod(s.c_str(), &end);
    return end != nullptr && *end == '\0';
}

u32 axis_from(const std::string& s) {
    if (s == "x" || s == "X" || s == "0") return 0;
    if (s == "y" || s == "Y" || s == "1") return 1;
    if (s == "z" || s == "Z" || s == "2") return 2;
    return 3;
}

// --- the parser -------------------------------------------------------------------------

class Parser {
public:
    Parser(const std::vector<Token>& tokens, Script& script, VoxelTypeTable& types,
           const TagRegistry& tags)
        : tokens_(tokens), script_(script), types_(types), tags_(tags) {}

    void run() {
        while (at_ < tokens_.size()) {
            const usize before = at_;
            statement();
            if (at_ == before) ++at_;   // never spin on a token nothing consumed
        }
    }

private:
    // --- token access ---------------------------------------------------------------------
    bool done() const { return at_ >= tokens_.size(); }
    const Token& peek() const { return tokens_[at_]; }
    bool at_new_statement() const { return done() || tokens_[at_].starts_line; }
    std::string take() { return done() ? std::string() : tokens_[at_++].text; }
    u32 line() const { return done() ? (tokens_.empty() ? 0 : tokens_.back().line) : peek().line; }

    void fail(const std::string& message) {
        script_.errors.push_back(ScriptError{line(), message});
    }

    // Everything up to the start of the next line, which is one statement's worth.
    void skip_statement() {
        while (!done() && !tokens_[at_].starts_line) ++at_;
    }

    // --- values -------------------------------------------------------------------------
    //
    // A number, or the name of a parameter. Both are usable anywhere a number is, and that is
    // what lets a clip expose a dial: writing `height` instead of `2.4` changes nothing about
    // how the file reads and everything about whether it can move afterwards.
    bool value(f64& out) {
        if (done() || peek().text == "{" || peek().text == "}") return false;
        const std::string t = peek().text;
        if (is_number(t)) {
            out = std::strtod(t.c_str(), nullptr);
            ++at_;
            return true;
        }
        auto it = parameters_.find(t);
        if (it != parameters_.end()) {
            // Unless it is the left side of a `key=value`, in which case it is a key that
            // happens to share a name with a parameter — which is not a coincidence but the
            // normal case: `run=run` says "the step run is the parameter called run", and it is
            // exactly what an author writes. Without this lookahead the name is swallowed as a
            // positional argument and the equals sign that follows becomes a statement of its
            // own, which is how it announces itself.
            if (at_ + 1 < tokens_.size() && tokens_[at_ + 1].text == "=") return false;
            out = script_.field.get_parameter(t.c_str(), 0.0);
            ++at_;
            return true;
        }
        return false;
    }

    f64 value_or(f64 fallback) {
        f64 v = fallback;
        value(v);
        return v;
    }

    // key=value pairs, gathered before the expression is built so order does not matter.
    struct Keys {
        std::map<std::string, std::vector<f64>> numbers;
        std::map<std::string, std::string> words;

        f64 number(const std::string& key, f64 fallback) const {
            auto it = numbers.find(key);
            return (it != numbers.end() && !it->second.empty()) ? it->second[0] : fallback;
        }
        bool has(const std::string& key) const {
            return numbers.count(key) != 0 || words.count(key) != 0;
        }
        std::string word(const std::string& key, const std::string& fallback) const {
            auto it = words.find(key);
            return (it != words.end()) ? it->second : fallback;
        }
    };

    // Reads `key=value` and `key=a,b,c` until something that is not one.
    void keys_into(Keys& keys) {
        while (!done() && !at_new_statement()) {
            if (at_ + 1 >= tokens_.size() || tokens_[at_ + 1].text != "=") break;
            const std::string key = tokens_[at_].text;
            at_ += 2;   // the name and the equals
            std::vector<f64> numbers;
            std::string word;
            while (true) {
                f64 v = 0.0;
                if (value(v)) {
                    numbers.push_back(v);
                } else if (!done() && peek().text != "," && peek().text != "{" &&
                           peek().text != "}") {
                    word = take();
                } else {
                    break;
                }
                if (!done() && peek().text == ",") {
                    ++at_;
                    continue;
                }
                break;
            }
            if (!numbers.empty()) keys.numbers[key] = numbers;
            if (!word.empty()) keys.words[key] = word;
        }
    }

    // --- expressions -----------------------------------------------------------------------

    // A name already bound by `let`, or a fresh call.
    bool expression(u32& out) {
        if (done()) return false;
        const std::string head = peek().text;
        auto bound = bindings_.find(head);
        if (bound != bindings_.end()) {
            // A bare name, unless it is followed by arguments, in which case it was meant as a
            // call and the author has shadowed a builder — which they are allowed to do.
            ++at_;
            out = bound->second;
            return true;
        }
        return call(out);
    }

    // The children of a `{ ... }` block.
    std::vector<u32> block() {
        std::vector<u32> parts;
        if (done() || peek().text != "{") return parts;
        ++at_;
        while (!done() && peek().text != "}") {
            u32 child = 0;
            if (!expression(child)) {
                fail("expected a shape or a name inside the braces, found '" + peek().text + "'");
                ++at_;
                continue;
            }
            parts.push_back(child);
        }
        if (!done() && peek().text == "}") ++at_;
        return parts;
    }

    bool call(u32& out);

    // --- statements --------------------------------------------------------------------------
    void statement();

    const std::vector<Token>& tokens_;
    Script& script_;
    VoxelTypeTable& types_;
    const TagRegistry& tags_;
    usize at_ = 0;
    std::map<std::string, u32> bindings_;
    std::map<std::string, VoxelTypeId> materials_;
    std::map<std::string, bool> parameters_;
};

bool Parser::call(u32& out) {
    if (done()) return false;
    const std::string head = take();
    Field& f = script_.field;

    // Positional numbers first, then keys. Nearly every shape reads better with its position
    // written plainly and its size named — `box -4 0 -4 4 3 4 round=0.05`.
    std::vector<f64> args;
    f64 v = 0.0;
    while (value(v)) args.push_back(v);
    Keys keys;
    keys_into(keys);
    const auto arg = [&](usize index, f64 fallback) {
        return (index < args.size()) ? args[index] : fallback;
    };

    if (head == "sphere") {
        out = f.sphere({arg(0, 0), arg(1, 0), arg(2, 0)}, keys.number("r", arg(3, 1.0)));
        return true;
    }
    if (head == "box") {
        // Written as two opposite corners, because that is how a room is described. Converted to
        // centre and half extent here so the author never has to.
        const Vec3 lo{arg(0, 0), arg(1, 0), arg(2, 0)};
        const Vec3 hi{arg(3, 1), arg(4, 1), arg(5, 1)};
        const Vec3 centre{(lo.x + hi.x) * 0.5, (lo.y + hi.y) * 0.5, (lo.z + hi.z) * 0.5};
        const Vec3 half{std::abs(hi.x - lo.x) * 0.5, std::abs(hi.y - lo.y) * 0.5,
                        std::abs(hi.z - lo.z) * 0.5};
        out = f.box(centre, half, keys.number("round", 0.0));
        return true;
    }
    if (head == "cylinder") {
        out = f.cylinder({arg(0, 0), arg(1, 0), arg(2, 0)}, keys.number("r", 1.0),
                         keys.number("h", 1.0) * 0.5,
                         axis_from(keys.word("axis", "y")) % 3u);
        return true;
    }
    if (head == "capsule") {
        out = f.capsule({arg(0, 0), arg(1, 0), arg(2, 0)}, {arg(3, 0), arg(4, 1), arg(5, 0)},
                        keys.number("r", 0.25));
        return true;
    }
    if (head == "torus") {
        out = f.torus({arg(0, 0), arg(1, 0), arg(2, 0)}, keys.number("ring", 1.0),
                      keys.number("tube", 0.25), axis_from(keys.word("axis", "y")) % 3u);
        return true;
    }
    if (head == "cone") {
        out = f.cone({arg(0, 0), arg(1, 0), arg(2, 0)}, keys.number("r", 1.0),
                     keys.number("h", 1.0), axis_from(keys.word("axis", "y")) % 3u);
        return true;
    }
    if (head == "plane") {
        out = f.plane({arg(0, 0), arg(1, 1), arg(2, 0)}, keys.number("at", arg(3, 0.0)));
        return true;
    }
    if (head == "ellipsoid") {
        out = f.ellipsoid({arg(0, 0), arg(1, 0), arg(2, 0)},
                          {keys.number("rx", arg(3, 1)), keys.number("ry", arg(4, 1)),
                           keys.number("rz", arg(5, 1))});
        return true;
    }
    if (head == "prism") {
        out = f.prism({arg(0, 0), arg(1, 0), arg(2, 0)}, keys.number("r", 1.0),
                      keys.number("h", 1.0) * 0.5,
                      static_cast<u32>(keys.number("sides", 6.0)),
                      axis_from(keys.word("axis", "y")) % 3u, keys.number("turn", 0.0));
        return true;
    }
    if (head == "tetra" || head == "cube" || head == "octa" || head == "dodeca" ||
        head == "icosa") {
        const u32 which = (head == "tetra")    ? 0u
                          : (head == "cube")   ? 1u
                          : (head == "octa")   ? 2u
                          : (head == "dodeca") ? 3u
                                               : 4u;
        out = f.platonic({arg(0, 0), arg(1, 0), arg(2, 0)}, keys.number("r", arg(3, 1.0)), which);
        return true;
    }
    if (head == "wedge") {
        const Vec3 lo{arg(0, 0), arg(1, 0), arg(2, 0)};
        const Vec3 hi{arg(3, 1), arg(4, 1), arg(5, 1)};
        out = f.wedge({(lo.x + hi.x) * 0.5, (lo.y + hi.y) * 0.5, (lo.z + hi.z) * 0.5},
                      {std::abs(hi.x - lo.x) * 0.5, std::abs(hi.y - lo.y) * 0.5,
                       std::abs(hi.z - lo.z) * 0.5},
                      axis_from(keys.word("rise", "y")) % 3u,
                      axis_from(keys.word("run", "z")) % 3u);
        return true;
    }
    if (head == "stairs") {
        const Vec3 lo{arg(0, 0), arg(1, 0), arg(2, 0)};
        const Vec3 hi{arg(3, 1), arg(4, 1), arg(5, 1)};
        out = f.stairs({(lo.x + hi.x) * 0.5, (lo.y + hi.y) * 0.5, (lo.z + hi.z) * 0.5},
                       {std::abs(hi.x - lo.x) * 0.5, std::abs(hi.y - lo.y) * 0.5,
                        std::abs(hi.z - lo.z) * 0.5},
                       keys.number("run", 0.30), keys.number("rise", 0.18));
        return true;
    }

    // --- combining ---------------------------------------------------------------------------
    if (head == "union" || head == "difference" || head == "intersection" || head == "add" ||
        head == "multiply" || head == "min" || head == "max") {
        const std::vector<u32> parts = block();
        if (parts.empty()) {
            fail(head + " needs a { } with at least one thing in it");
            return false;
        }
        const f64 smooth = keys.number("smooth", 0.0);
        if (head == "union") out = (smooth > 0.0) ? f.smooth_unite(parts, smooth) : f.unite(parts);
        else if (head == "difference")
            out = (smooth > 0.0) ? f.smooth_subtract(parts, smooth) : f.subtract(parts);
        else if (head == "intersection")
            out = (smooth > 0.0) ? f.smooth_intersect(parts, smooth) : f.intersect(parts);
        else if (head == "add") out = f.add(parts);
        else if (head == "multiply") out = f.multiply(parts);
        else if (head == "min") out = f.minimum(parts);
        else out = f.maximum(parts);
        return true;
    }

    // --- one-child operations ------------------------------------------------------------------
    if (head == "translate" || head == "rotate" || head == "scale" || head == "mirror" ||
        head == "repeat" || head == "around" || head == "shell" || head == "round" ||
        head == "offset" || head == "twist" || head == "bend" || head == "abs" ||
        head == "negate" || head == "step" || head == "smoothstep" || head == "clamp" ||
        head == "remap" || head == "power" || head == "displace" || head == "blend") {
        std::vector<u32> parts = block();
        if (parts.empty()) {
            // Also allow `shell walls 0.1` without braces, which reads better for one child.
            u32 child = 0;
            if (expression(child)) parts.push_back(child);
        }
        if (parts.empty()) {
            fail(head + " needs something to act on");
            return false;
        }
        // Keys may follow the block as well as precede it.
        keys_into(keys);
        std::vector<f64> more;
        while (value(v)) more.push_back(v);
        for (f64 extra : more) args.push_back(extra);

        const u32 child = parts[0];
        if (head == "translate")
            out = f.translate(child, {arg(0, 0), arg(1, 0), arg(2, 0)});
        else if (head == "rotate")
            out = f.rotate(child, {keys.number("x", arg(0, 0)), keys.number("y", arg(1, 0)),
                                   keys.number("z", arg(2, 0))});
        else if (head == "scale")
            out = f.scale(child, {keys.number("x", arg(0, 1)), keys.number("y", arg(1, 1)),
                                  keys.number("z", arg(2, 1))});
        else if (head == "mirror")
            out = f.mirror(child, axis_from(keys.word("axis", "x")) % 3u);
        else if (head == "repeat")
            out = f.repeat(child,
                           {keys.number("x", 0), keys.number("y", 0), keys.number("z", 0)},
                           {keys.number("nx", 0), keys.number("ny", 0), keys.number("nz", 0)});
        else if (head == "around")
            out = f.polar_repeat(child, static_cast<u32>(keys.number("count", arg(0, 4.0))),
                                 axis_from(keys.word("axis", "y")) % 3u);
        else if (head == "shell")
            out = f.shell(child, keys.number("thickness", arg(0, 0.1)));
        else if (head == "round")
            out = f.round_off(child, keys.number("by", arg(0, 0.05)));
        else if (head == "offset")
            out = f.offset(child, keys.number("by", arg(0, 0.0)));
        else if (head == "twist")
            out = f.twist(child, keys.number("turns", arg(0, 0.25)),
                          axis_from(keys.word("axis", "y")) % 3u);
        else if (head == "bend")
            out = f.bend(child, keys.number("turns", arg(0, 0.25)),
                         axis_from(keys.word("axis", "y")) % 3u);
        else if (head == "abs") out = f.absolute(child);
        else if (head == "negate") out = f.negate(child);
        else if (head == "step") out = f.step(child, keys.number("at", arg(0, 0.0)));
        else if (head == "smoothstep")
            out = f.smoothstep(child, keys.number("from", arg(0, 0.0)),
                               keys.number("to", arg(1, 1.0)));
        else if (head == "clamp")
            out = f.clamp_to(child, keys.number("low", arg(0, 0.0)),
                             keys.number("high", arg(1, 1.0)));
        else if (head == "remap")
            out = f.remap(child, keys.number("from", arg(0, -1.0)),
                          keys.number("to", arg(1, 1.0)), keys.number("low", arg(2, 0.0)),
                          keys.number("high", arg(3, 1.0)));
        else if (head == "power") out = f.power(child, keys.number("by", arg(0, 2.0)));
        else if (head == "blend") {
            if (parts.size() < 2) {
                fail("blend needs two things");
                return false;
            }
            out = f.blend(parts[0], parts[1], keys.number("t", arg(0, 0.5)));
        } else {   // displace
            if (parts.size() < 2) {
                fail("displace needs a shape and a pattern");
                return false;
            }
            out = f.displace(parts[0], parts[1], keys.number("amount", arg(0, 0.05)));
        }
        return true;
    }

    // --- patterns ---------------------------------------------------------------------------
    if (head == "constant") { out = f.constant(arg(0, 0.0)); return true; }
    if (head == "axis") { out = f.coordinate(axis_from(keys.word("of", "y")) % 3u); return true; }
    if (head == "distance") { out = f.radius({arg(0, 0), arg(1, 0), arg(2, 0)}); return true; }
    if (head == "sine") {
        out = f.sine(axis_from(keys.word("axis", "x")) % 3u, keys.number("period", arg(0, 1.0)),
                     keys.number("phase", 0.0));
        return true;
    }
    if (head == "waves") {
        out = f.waves(axis_from(keys.word("axis", "y")) % 3u, keys.number("a", 1.0),
                      keys.number("b", 1.0), keys.number("phase", 0.0));
        return true;
    }
    if (head == "noise") {
        out = f.noise(keys.number("size", arg(0, 1.0)),
                      static_cast<u32>(keys.number("seed", 1.0)));
        return true;
    }
    if (head == "fbm" || head == "ridged") {
        const f64 size = keys.number("size", arg(0, 1.0));
        const u32 octaves = static_cast<u32>(keys.number("octaves", 4.0));
        const f64 gain = keys.number("gain", 0.5);
        const f64 lacunarity = keys.number("lacunarity", 2.0);
        const u32 seed = static_cast<u32>(keys.number("seed", 1.0));
        out = (head == "fbm") ? f.fbm(size, octaves, gain, lacunarity, seed)
                              : f.ridged(size, octaves, gain, lacunarity, seed);
        return true;
    }
    if (head == "rasp") {
        out = f.rasp(keys.number("size", arg(0, 0.05)), keys.number("depth", 1.0),
                     static_cast<u32>(keys.number("seed", 1.0)));
        return true;
    }
    if (head == "cells") {
        out = f.cells(keys.number("size", arg(0, 0.5)),
                      static_cast<u32>(keys.number("seed", 1.0)));
        return true;
    }
    if (head == "checker") {
        const f64 cell = keys.number("size", arg(0, 1.0));
        out = f.checker({keys.number("x", cell), keys.number("y", cell), keys.number("z", cell)});
        return true;
    }
    if (head == "stripes") {
        out = f.stripes(axis_from(keys.word("axis", "y")) % 3u, keys.number("period", arg(0, 1.0)),
                        keys.number("duty", 0.5));
        return true;
    }
    if (head == "bricks") {
        out = f.bricks({keys.number("length", 0.24), keys.number("height", 0.08), 0.0},
                       keys.number("mortar", 0.012),
                       axis_from(keys.word("facing", "z")) % 3u);
        return true;
    }

    fail("unknown shape or pattern '" + head + "'");
    return false;
}

void Parser::statement() {
    if (done()) return;
    const std::string head = take();
    Field& f = script_.field;

    if (head == "metre" || head == "meter") {
        script_.settings.voxels_per_metre = static_cast<i32>(value_or(kVoxelsPerMetre));
        return;
    }
    if (head == "bounds") {
        script_.settings.low = {value_or(0), value_or(0), value_or(0)};
        script_.settings.high = {value_or(1), value_or(1), value_or(1)};
        return;
    }
    if (head == "param") {
        const std::string name = take();
        const f64 initial = value_or(0.0);
        f.parameter(name.c_str(), initial);
        parameters_[name] = true;
        return;
    }
    if (head == "material") {
        const std::string name = take();
        Keys keys;
        keys_into(keys);
        VisualRecord visual;
        auto rgb = keys.numbers.find("rgb");
        if (rgb != keys.numbers.end() && rgb->second.size() >= 3) {
            visual.red = static_cast<u8>(rgb->second[0]);
            visual.green = static_cast<u8>(rgb->second[1]);
            visual.blue = static_cast<u8>(rgb->second[2]);
        }
        visual.roughness = static_cast<u8>(keys.number("rough", 200.0));
        visual.metallic = static_cast<u8>(keys.number("metal", 0.0));
        visual.opacity = static_cast<u8>(keys.number("opacity", 255.0));
        visual.emissive = static_cast<u8>(keys.number("emit", 0.0));
        visual.translucency = static_cast<u8>(keys.number("translucent", 0.0));
        if (keys.has("ior")) {
            // Written as a refractive index, stored as the offset from vacuum the record uses.
            const f64 index = keys.number("ior", 1.0);
            visual.ior = static_cast<u8>(std::min(255.0, std::max(0.0, (index - 1.0) * 128.0)));
        }
        BehaviourRecord behaviour;
        behaviour.material = static_cast<u32>(script_.material_names.size() + 1);
        (void)tags_;
        const VoxelTypeId type = types_.intern(visual, behaviour);
        materials_[name] = type;
        // The name table is indexed by type id, so a report can look one up directly.
        if (script_.material_names.size() <= type) {
            script_.material_names.resize(static_cast<usize>(type) + 1);
        }
        script_.material_names[type] = name;
        script_.material_types.push_back(type);
        return;
    }
    if (head == "let") {
        const std::string name = take();
        if (!done() && peek().text == "=") ++at_;
        u32 node = 0;
        if (!expression(node)) {
            fail("could not read the right hand side of 'let " + name + "'");
            skip_statement();
            return;
        }
        bindings_[name] = node;
        return;
    }
    if (head == "paint") {
        const std::string material = take();
        auto it = materials_.find(material);
        if (it == materials_.end()) {
            fail("paint refers to a material '" + material + "' that has not been declared");
            skip_statement();
            return;
        }
        Keys keys;
        keys_into(keys);
        PaintRule rule;
        rule.type = it->second;
        // With no test at all the rule covers everything, which is what the first coat is.
        rule.test = f.constant(0.0);
        const std::string where = keys.word("where", "");
        if (!where.empty()) {
            auto bound = bindings_.find(where);
            if (bound == bindings_.end()) {
                fail("paint where=" + where + " does not name anything");
                return;
            }
            rule.test = bound->second;
        }
        if (keys.has("above")) rule.low = keys.number("above", -1e30);
        if (keys.has("below")) rule.high = keys.number("below", 1e30);
        if (keys.has("facing")) {
            rule.facing_axis = axis_from(keys.word("facing", "y")) % 3u;
            rule.facing_min = keys.number("at", 0.5);
        }
        script_.paint.push_back(rule);
        return;
    }
    if (head == "solid") {
        u32 node = 0;
        if (!expression(node)) {
            fail("solid needs the name of a shape");
            skip_statement();
            return;
        }
        script_.solid = node;
        script_.has_solid = true;
        return;
    }
    if (head == "region") {
        u32 node = 0;
        if (!expression(node)) {
            fail("region needs the name of a shape");
            skip_statement();
            return;
        }
        script_.settings.bounds = node;
        script_.settings.has_bounds = true;
        return;
    }

    fail("unknown statement '" + head + "'");
    skip_statement();
}

}  // namespace

Script parse_clip_script(const std::string& text, VoxelTypeTable& types, const TagRegistry& tags) {
    Script script;
    const std::vector<Token> tokens = tokenize(text);
    Parser parser(tokens, script, types, tags);
    parser.run();
    // The graph is complete now, so the boxes that let a union skip its distant children can be
    // worked out. Done here rather than in the sampler because a Field can be sampled many
    // times and its shape does not change between them.
    script.field.build_bounds();
    if (!script.has_solid && script.errors.empty()) {
        script.errors.push_back(ScriptError{0, "the file never says which shape is the solid"});
    }
    return script;
}

Script load_clip_script(const std::string& path, VoxelTypeTable& types, const TagRegistry& tags) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        Script script;
        script.errors.push_back(ScriptError{0, "could not open '" + path + "'"});
        return script;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return parse_clip_script(buffer.str(), types, tags);
}

}  // namespace forge
}  // namespace ws
