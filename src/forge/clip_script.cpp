#include "forge/clip_script.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
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
        // Re-binding a name replaces what it means, so the record is replaced rather than added
        // to: a part is whatever its name finally referred to.
        bool replaced = false;
        for (auto& part : script_.parts) {
            if (part.first == name) {
                part.second = node;
                replaced = true;
                break;
            }
        }
        if (!replaced) script_.parts.emplace_back(name, node);
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
    if (head == "weather") {
        const std::string kind = take();
        Keys keys;
        f64 amount = 0.0;
        value(amount);
        keys_into(keys);
        WeatherRequest request;
        request.amount = keys.number("amount", amount);
        request.scale = keys.number("scale", 1.0);
        request.seed = static_cast<u32>(keys.number("seed", 1.0));
        request.level = keys.number("level", 0.0);
        if (kind == "desert") request.kind = Weather::Desert;
        else if (kind == "overgrown") request.kind = Weather::Overgrown;
        else if (kind == "cracks") request.kind = Weather::Cracks;
        else if (kind == "burnt") request.kind = Weather::Burnt;
        else if (kind == "sea") request.kind = Weather::Sea;
        else {
            fail("unknown weathering '" + kind +
                 "' — desert, overgrown, cracks, burnt or sea");
            return;
        }
        script_.weather.push_back(request);
        return;
    }
    if (head == "variation") {
        Keys keys;
        keys_into(keys);
        script_.variation.colour = keys.number("colour", keys.number("color", 0.03));
        script_.variation.roughness = keys.number("rough", 0.05);
        script_.variation.seed = static_cast<u32>(keys.number("seed", 1.0));
        script_.variation.budget = static_cast<u32>(keys.number("budget", 1000000.0));
        const std::string by = keys.word("by", "");
        if (!by.empty()) {
            auto bound = bindings_.find(by);
            if (bound == bindings_.end()) {
                fail("variation by=" + by + " does not name anything");
                return;
            }
            script_.variation.by = bound->second;
            script_.variation.has_by = true;
        }
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

// --- weathering ---------------------------------------------------------------------------
//
// Each kind expands into two things: a deformation of the solid, and coats of paint that follow
// the same geometry the deformation followed. They are built here rather than written in the
// clip file because the expansion is long, fiddly and identical every time — an author wants to
// say "half weathered by the sea", not to re-derive what that means from curvature and cavity.

VoxelTypeId make_material(VoxelTypeTable& types, Script& script, const char* name, u8 r, u8 g,
                          u8 b, u8 rough) {
    // Reuse the author's material of the same name when there is one, so a clip that declares
    // its own `moss` keeps it and weathering paints with that rather than inventing a second.
    for (usize i = 0; i < script.material_names.size(); ++i) {
        if (script.material_names[i] == name) return static_cast<VoxelTypeId>(i);
    }
    VisualRecord visual;
    visual.red = r;
    visual.green = g;
    visual.blue = b;
    visual.roughness = rough;
    BehaviourRecord behaviour;
    behaviour.material = static_cast<u32>(script.material_names.size() + 64);
    const VoxelTypeId type = types.intern(visual, behaviour);
    if (script.material_names.size() <= type) {
        script.material_names.resize(static_cast<usize>(type) + 1);
    }
    script.material_names[type] = name;
    return type;
}

void apply_weather(Script& script, VoxelTypeTable& types) {
    if (script.weather.empty() || !script.has_solid) return;
    Field& f = script.field;

    for (const WeatherRequest& request : script.weather) {
        const f64 a = std::clamp(request.amount, 0.0, 1.0);
        if (a <= 0.0) continue;
        const f64 s = (request.scale > 0.0) ? request.scale : 1.0;
        const u32 seed = request.seed;

        // The three questions every kind asks of the shape.
        const u32 shape = script.solid;
        const u32 cavity = f.occlusion(shape, 0.22 * s);          // 0 exposed, 1 buried
        const u32 edge = f.curvature(shape, 0.10 * s);            // + on an arris, - in a hollow
        const u32 up = f.facing(shape, 1);                        // + up, - down
        const u32 height = f.coordinate(1);

        switch (request.kind) {
            case Weather::Desert: {
                // Sand settles on anything facing up and lodges in every hollow; the wind takes
                // the arrises off. Both follow the shape, so a sill collects and its nose does
                // not — which is the whole reason for doing this from geometry.
                const u32 grit = f.fbm(0.09 * s, 3u, 0.55, 2.3, seed + 3u);
                const u32 scour = f.multiply({f.smoothstep(edge, 0.2, 1.2), f.constant(a)});
                script.solid = f.displace(script.solid, scour, 0.05 * s);

                const u32 sand = make_material(types, script, "sand", 198, 176, 132, 245);
                const u32 bleach = make_material(types, script, "bleached", 208, 202, 188, 235);

                PaintRule sun_bleach;
                sun_bleach.type = bleach;
                sun_bleach.test = f.add({f.multiply({up, f.constant(0.6)}),
                                         f.multiply({grit, f.constant(0.4)})});
                sun_bleach.low = 0.55 - 0.35 * a;
                script.paint.push_back(sun_bleach);

                PaintRule drift;
                drift.type = sand;
                // Up-facing, plus low down, plus in the hollows: three ways sand arrives, added
                // rather than chosen between, because a low up-facing hollow gets the most.
                drift.test = f.add({f.multiply({up, f.constant(0.5)}),
                                    f.multiply({cavity, f.constant(0.5)}),
                                    f.smoothstep(f.negate(height), -request.level - 1.5 * s,
                                                 -request.level)});
                drift.low = 1.15 - 0.75 * a;
                script.paint.push_back(drift);
                break;
            }
            case Weather::Overgrown: {
                // Growth wants damp and shelter, so it follows the cavity term and the up-facing
                // one, and it swells the surface slightly where it takes hold.
                const u32 clumps = f.fbm(0.35 * s, 4u, 0.5, 2.1, seed + 11u);
                const u32 where = f.add({f.multiply({cavity, f.constant(0.55)}),
                                         f.multiply({up, f.constant(0.35)}),
                                         f.multiply({clumps, f.constant(0.4)})});
                script.solid =
                    f.displace(script.solid, f.multiply({f.smoothstep(where, 0.35, 0.95),
                                                         f.constant(-a)}),
                               0.045 * s);

                const u32 moss = make_material(types, script, "moss", 74, 108, 54, 250);
                const u32 lichen = make_material(types, script, "lichen", 138, 148, 108, 248);

                PaintRule pale;
                pale.type = lichen;
                pale.test = where;
                pale.low = 0.55 - 0.35 * a;
                script.paint.push_back(pale);

                PaintRule green;
                green.type = moss;
                green.test = where;
                green.low = 0.85 - 0.55 * a;
                script.paint.push_back(green);
                break;
            }
            case Weather::Cracks: {
                // A crack is not a line drawn on a surface, it is the seam between two regions,
                // which is exactly what the distance between the two nearest scattered points
                // gives — and it branches and meets itself for free, because seams do.
                const u32 seams = f.cell_edge(0.55 * s, seed + 17u);
                const u32 wander = f.fbm(0.2 * s, 3u, 0.5, 2.0, seed + 29u);
                // Narrow where the amount is low, opening as it rises.
                const u32 opened =
                    f.smoothstep(f.add({seams, f.multiply({wander, f.constant(0.05 * s)})}),
                                 0.02 * s + 0.06 * s * a, 0.0);
                // Added to the distance, not subtracted from it. Displacement moves a surface by
                // adding to how far away it says it is, so a *positive* value on the seams eats
                // into the solid — which is what a crack is. Negated, the seams stood proud of
                // the face instead, and the block came out bigger than it started.
                script.solid = f.displace(script.solid, opened, 0.09 * s * a);

                const u32 dark = make_material(types, script, "fissure", 66, 62, 58, 250);
                PaintRule inside;
                inside.type = dark;
                inside.test = opened;
                inside.low = 0.35;
                script.paint.push_back(inside);
                break;
            }
            case Weather::Burnt: {
                // Heat rounds what it touches and soot collects where nothing washes it off:
                // undersides, and the backs of hollows. So the deformation is a rounding of the
                // arrises and the paint follows the down-facing and the cavity.
                const u32 soot_grain = f.fbm(0.14 * s, 4u, 0.6, 2.4, seed + 41u);
                script.solid = f.round_off(script.solid, 0.02 * s * a);

                const u32 char_ = make_material(types, script, "charred", 44, 40, 38, 252);
                const u32 soot = make_material(types, script, "soot", 28, 26, 25, 254);
                const u32 scorch = make_material(types, script, "scorched", 96, 78, 62, 246);

                PaintRule light;
                light.type = scorch;
                light.test = f.add({soot_grain, f.multiply({cavity, f.constant(0.5)})});
                light.low = 0.5 - 0.45 * a;
                script.paint.push_back(light);

                PaintRule mid;
                mid.type = char_;
                mid.test = f.add({f.multiply({cavity, f.constant(0.7)}),
                                  f.multiply({soot_grain, f.constant(0.5)})});
                mid.low = 0.75 - 0.55 * a;
                script.paint.push_back(mid);

                PaintRule under;
                under.type = soot;
                under.test = f.add({f.negate(up), f.multiply({cavity, f.constant(0.8)})});
                under.low = 1.05 - 0.75 * a;
                script.paint.push_back(under);
                break;
            }
            case Weather::Sea: {
                // A tide line divides the whole thing. Above it, salt and bleaching; below it,
                // barnacles crusting the exposed faces and weed in everything sheltered. The
                // line is `level`, and it is the one weathering that cares where the datum is.
                const u32 crust = f.fbm(0.05 * s, 3u, 0.6, 2.6, seed + 53u);
                const u32 lumps = f.cells(0.08 * s, seed + 59u);
                const u32 below = f.smoothstep(f.negate(height), -request.level,
                                               -request.level + 0.9 * s);

                // Barnacles are added matter, not removed: they stand proud of the surface.
                const u32 growth =
                    f.multiply({below, f.smoothstep(lumps, 0.045 * s, 0.0), f.constant(-a)});
                script.solid = f.displace(script.solid, growth, 0.05 * s);

                const u32 salt = make_material(types, script, "salt", 206, 204, 196, 250);
                const u32 barnacle = make_material(types, script, "barnacle", 190, 186, 172, 244);
                const u32 weed = make_material(types, script, "weed", 58, 84, 62, 250);

                PaintRule dried;
                dried.type = salt;
                dried.test = f.add({f.multiply({f.negate(below), f.constant(0.7)}),
                                    f.multiply({crust, f.constant(0.5)})});
                dried.low = 0.75 - 0.5 * a;
                script.paint.push_back(dried);

                PaintRule shells;
                shells.type = barnacle;
                shells.test = f.multiply({below, f.smoothstep(lumps, 0.05 * s, 0.0)});
                shells.low = 0.55 - 0.4 * a;
                script.paint.push_back(shells);

                PaintRule green;
                green.type = weed;
                green.test = f.multiply({below, f.add({cavity, f.multiply({crust,
                                                                           f.constant(0.4)})})});
                green.low = 0.7 - 0.45 * a;
                script.paint.push_back(green);
                break;
            }
            default: break;
        }
    }
}

}  // namespace

Script parse_clip_script(const std::string& text, VoxelTypeTable& types, const TagRegistry& tags) {
    Script script;
    const std::vector<Token> tokens = tokenize(text);
    Parser parser(tokens, script, types, tags);
    parser.run();
    // Weathering is expanded after the whole file is read, because it acts on whatever ends up
    // being the solid and appends its coats after the author's.
    apply_weather(script, types);

    // The graph is complete now, so the boxes that let a union skip its distant children can be
    // worked out. Done here rather than in the sampler because a Field can be sampled many
    // times and its shape does not change between them.
    script.field.build_bounds();
    if (!script.has_solid && script.errors.empty()) {
        script.errors.push_back(ScriptError{0, "the file never says which shape is the solid"});
    }
    return script;
}

std::string expand_includes(const std::string& path, std::vector<SourceLine>& origin,
                            std::vector<ScriptError>& errors) {
    std::vector<std::string> open;   // the include stack, for cycle detection
    std::string out;

    // Recursive, and deliberately textual: an include splices one file's lines into another's
    // before a single token is read, so `include` costs the parser nothing and a fragment is
    // exactly the text it appears to be.
    const std::function<void(const std::string&, u32)> pull = [&](const std::string& file,
                                                                  u32 depth) {
        if (depth > 16) {
            errors.push_back(ScriptError{0, "includes nested more than sixteen deep in '" +
                                                file + "'"});
            return;
        }
        for (const std::string& already : open) {
            if (already == file) {
                errors.push_back(ScriptError{0, "'" + file + "' includes itself"});
                return;
            }
        }
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            errors.push_back(ScriptError{0, "could not open '" + file + "'"});
            return;
        }
        open.push_back(file);

        const std::filesystem::path here = std::filesystem::path(file).parent_path();
        std::string line;
        u32 number = 0;
        while (std::getline(in, line)) {
            ++number;
            if (!line.empty() && line.back() == '\r') line.pop_back();

            // `include "some/other.clip"`, resolved relative to the file doing the including, so
            // a fragment can be moved with its neighbours and still find them.
            usize at = line.find_first_not_of(" \t");
            if (at != std::string::npos && line.compare(at, 7, "include") == 0) {
                usize rest = at + 7;
                const usize quote = line.find('"', rest);
                const usize close = (quote == std::string::npos)
                                        ? std::string::npos
                                        : line.find('"', quote + 1);
                if (quote == std::string::npos || close == std::string::npos) {
                    errors.push_back(ScriptError{number,
                                                 "include needs a quoted file name, in '" + file +
                                                     "'"});
                    continue;
                }
                const std::string named = line.substr(quote + 1, close - quote - 1);
                std::filesystem::path target = std::filesystem::path(named);
                if (target.is_relative()) target = here / target;
                pull(target.lexically_normal().string(), depth + 1);
                continue;
            }

            out += line;
            out += '\n';
            origin.push_back(SourceLine{file, number});
        }
        open.pop_back();
    };

    pull(std::filesystem::path(path).lexically_normal().string(), 0);
    return out;
}

Script load_clip_script(const std::string& path, VoxelTypeTable& types, const TagRegistry& tags) {
    std::vector<SourceLine> origin;
    std::vector<ScriptError> trouble;
    const std::string text = expand_includes(path, origin, trouble);
    if (!trouble.empty() && text.empty()) {
        Script script;
        script.errors = trouble;
        return script;
    }

    Script script = parse_clip_script(text, types, tags);
    script.errors.insert(script.errors.begin(), trouble.begin(), trouble.end());

    // Errors come back numbered against the spliced text, which is a file nobody wrote. Put them
    // back where they came from — a line number that means nothing is worse than none.
    for (ScriptError& error : script.errors) {
        if (error.line == 0 || error.line > origin.size()) continue;
        const SourceLine& from = origin[error.line - 1];
        error.line = from.line;
        error.message = from.file + ": " + error.message;
    }
    script.sources = std::move(origin);
    return script;
}

}  // namespace forge
}  // namespace ws
