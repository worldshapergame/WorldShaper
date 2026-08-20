#include "game/clip_graph.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace ws {
namespace {

// --- tokens ---------------------------------------------------------------------------------
//
// The same rules `src/forge/clip_script.cpp` tokenizes by — whitespace separated, with braces,
// equals and commas as tokens of their own, and a comment running to the end of its line — plus
// the one thing that reader has no use for and this one cannot work without: **the column.**
//
// A visual edit rewrites the bytes a number occupies and nothing else, so a number that does not
// know where it is written cannot be edited without reformatting the file around it.
struct Token {
    std::string text;
    u32 line = 0;       // 1-based
    u32 column = 0;     // 0-based, in bytes
    bool starts_line = false;
};

bool separator(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '{' || c == '}' || c == '=' || c == ',' ||
           c == '#';
}

std::vector<Token> tokenize(const std::vector<std::string>& lines) {
    std::vector<Token> tokens;
    for (usize row = 0; row < lines.size(); ++row) {
        const std::string& line = lines[row];
        bool first = true;
        usize i = 0;
        while (i < line.size()) {
            const char c = line[i];
            if (c == '#') break;   // a comment runs to the end of its line
            if (c == ' ' || c == '\t' || c == '\r') {
                ++i;
                continue;
            }
            Token token;
            token.line = static_cast<u32>(row + 1);
            token.column = static_cast<u32>(i);
            token.starts_line = first;
            first = false;
            if (c == '{' || c == '}' || c == '=' || c == ',') {
                token.text = std::string(1, c);
                ++i;
            } else {
                const usize begin = i;
                while (i < line.size() && !separator(line[i])) ++i;
                token.text = line.substr(begin, i - begin);
            }
            tokens.push_back(token);
        }
    }
    return tokens;
}

bool is_number(const std::string& s) {
    if (s.empty()) return false;
    char* end = nullptr;
    std::strtod(s.c_str(), &end);
    return end != nullptr && *end == '\0';
}

// --- the two tables, copied from `Parser::call` ------------------------------------------------
//
// These are the only semantics in this file and they are here because the syntax cannot be read
// without them: `box -2 2 -0.4  2 3 0.4 grain` and `union { a b }` differ in whether the head
// takes children, and in the first `grain` belongs to whatever encloses the box rather than to the
// box. Both lists are the ones in `src/forge/clip_script.cpp`; a head in neither is a leaf that
// takes numbers and keys, which is every solid and every pattern in the language.
bool takes_many(const std::string& head) {
    return head == "union" || head == "difference" || head == "intersection" || head == "add" ||
           head == "multiply" || head == "min" || head == "max";
}

bool takes_one(const std::string& head) {
    return head == "translate" || head == "rotate" || head == "scale" || head == "mirror" ||
           head == "repeat" || head == "scatter" || head == "around" || head == "shell" ||
           head == "round" || head == "revolve" || head == "offset" || head == "twist" ||
           head == "bend" || head == "abs" || head == "negate" || head == "step" ||
           head == "smoothstep" || head == "clamp" || head == "remap" || head == "power" ||
           head == "displace" || head == "blend" || head == "occlusion" || head == "curvature" ||
           head == "facing";
}

// Whether the head hands back whatever its child was. A `translate` of a shape is a shape and a
// `translate` of a grain is a grain, so asking the child is better than a third table — and it is
// what keeps a wire's colour right through a pipeline that never says what it is carrying.
bool passes_through(const std::string& head) {
    return head == "translate" || head == "rotate" || head == "scale" || head == "mirror" ||
           head == "repeat" || head == "scatter" || head == "around" || head == "twist" ||
           head == "bend" || head == "blend";
}

bool is_moulding(const std::string& head) {
    return head == "fillet" || head == "ovolo" || head == "cavetto" || head == "bead" ||
           head == "astragal" || head == "scotia" || head == "cyma" || head == "cyma_reversa";
}

// Every head that answers with a signed distance. A head in neither this nor `passes_through` is
// read as a value, which is the safe fallback: a pattern drawn with a shape's hue would be a lie,
// and a shape drawn with the value hue is a colour that has not been decided yet.
bool is_shape(const std::string& head) {
    return head == "sphere" || head == "box" || head == "cylinder" || head == "capsule" ||
           head == "torus" || head == "arc" || head == "cone" || head == "plane" ||
           head == "ellipsoid" || head == "prism" || head == "tetra" || head == "cube" ||
           head == "octa" || head == "dodeca" || head == "icosa" || head == "wedge" ||
           head == "stairs" || head == "spiral" || head == "revolve" || head == "branch" ||
           head == "union" || head == "difference" || head == "intersection" ||
           head == "shell" || head == "round" || head == "offset" || head == "displace" ||
           is_moulding(head);
}

// The statements, which is what a document is a list of. `include` is here even though the parser
// never sees one — it is spliced away before a token is read — because it is a line the author
// wrote and the editor is about what the author wrote.
bool is_statement_head(const std::string& head) {
    return head == "metre" || head == "meter" || head == "bounds" || head == "param" ||
           head == "material" || head == "let" || head == "paint" || head == "weather" ||
           head == "variation" || head == "origin" || head == "solid" || head == "region" ||
           head == "include";
}

class Reader {
public:
    Reader(const std::vector<Token>& tokens, const std::vector<std::string>& lines, ClipGraph& out)
        : tokens_(tokens), lines_(lines), out_(out) {}

    void run() {
        while (at_ < tokens_.size()) {
            const usize before = at_;
            statement();
            if (at_ == before) ++at_;   // never spin on a token nothing consumed
        }
        flush_opaque();
        settle_unresolved();
        settle_depths();
    }

private:
    // --- tokens -----------------------------------------------------------------------------
    bool done() const { return at_ >= tokens_.size(); }
    const Token& peek() const {
        static const Token kEnd{};
        return done() ? kEnd : tokens_[at_];
    }
    const Token& peek(usize ahead) const {
        static const Token kEnd{};
        return (at_ + ahead < tokens_.size()) ? tokens_[at_ + ahead] : kEnd;
    }
    bool at_new_statement() const { return done() || tokens_[at_].starts_line; }
    const Token& take() {
        static const Token kEnd{};
        return done() ? kEnd : tokens_[at_++];
    }

    // --- what could not be read -------------------------------------------------------------
    //
    // D454. A run of lines nothing here understands becomes ONE node carrying those lines, rather
    // than a node each or — the failure this rule exists against — nothing at all. One node,
    // because a Lua mod on the mods shelf is four hundred lines and four hundred boxes is not a
    // view of anything.
    void keep_opaque(u32 first_line, u32 last_line) {
        if (opaque_open_ && opaque_last_ + 1 >= first_line) {
            opaque_last_ = std::max(opaque_last_, last_line);
            return;
        }
        flush_opaque();
        opaque_open_ = true;
        opaque_first_ = first_line;
        opaque_last_ = last_line;
    }

    void flush_opaque() {
        if (!opaque_open_) return;
        opaque_open_ = false;
        ClipNode node;
        node.head = "text";
        node.opaque = true;
        node.statement = true;
        node.line = opaque_first_;
        node.last_line = opaque_last_;
        for (u32 line = opaque_first_; line <= opaque_last_ && line <= lines_.size(); ++line) {
            if (!node.source.empty()) node.source += "\n";
            node.source += lines_[line - 1];
        }
        out_.opaque_lines += opaque_last_ - opaque_first_ + 1;
        out_.nodes.push_back(std::move(node));
        ++out_.statements;
    }

    // Everything up to the start of the next line, which is one statement's worth.
    u32 skip_statement() {
        u32 last = done() ? 0 : tokens_[at_ > 0 ? at_ - 1 : 0].line;
        while (!done() && !tokens_[at_].starts_line) {
            last = tokens_[at_].line;
            ++at_;
        }
        return last;
    }

    // --- values -----------------------------------------------------------------------------
    //
    // A number, or the name of a `param`. Both are usable anywhere a number is, which is what lets
    // a clip expose a dial — and the lookahead past a `=` is the same one `Parser::value` needs,
    // because `run=run` says "the step run is the parameter called run" and is what an author
    // actually writes.
    //
    // The two are drawn differently and deliberately so. A literal is a NUMBER, and a slider moves
    // it. A parameter is a **wire from the `param` statement that declared it** — which is what
    // `20-clip-forge.md` §3 says a slot is for, and it is the whole reason a clip exposes one:
    // `thickness=wall` drawn as a wire says the hut's walls and the shed's are the same number,
    // where drawing it as a slider would offer to change one of them on its own.
    bool at_number() const {
        if (done() || peek().text == "{" || peek().text == "}" || peek().text == "=") return false;
        return is_number(peek().text);
    }

    bool at_parameter() const {
        if (done() || peek().text == "{" || peek().text == "}" || peek().text == "=") return false;
        if (parameters_.count(peek().text) == 0) return false;
        return peek(1).text != "=";
    }

    // One value, wherever a value may be. `key` is empty for a positional one.
    bool read_value(ClipNode& node, const std::string& key, u32 index) {
        if (at_number()) {
            const Token& token = take();
            ClipNumber number;
            number.key = key;
            number.index = index;
            number.text = token.text;
            number.value = std::strtod(token.text.c_str(), nullptr);
            number.line = token.line;
            number.column = token.column;
            node.numbers.push_back(number);
            return true;
        }
        if (at_parameter()) {
            const Token& token = take();
            ClipWord word;
            word.key = key;
            word.text = token.text;
            word.line = token.line;
            word.column = token.column;
            word.names_a_part = true;
            node.words.push_back(word);
            node.inputs.push_back(parameters_[token.text]);
            return true;
        }
        return false;
    }

    // No line test, and that is `Parser::value` mirrored rather than an oversight: positional
    // numbers stop at the first token that is not one, which no statement in this language ever
    // begins with.
    void read_positional(ClipNode& node) {
        u32 index = static_cast<u32>(node.numbers.size());
        while (read_value(node, {}, index)) ++index;
    }

    // `key=value` and `key=a,b,c`, until something that is not one.
    bool read_keys(ClipNode& node) {
        bool any = false;
        while (!at_new_statement()) {
            if (peek(1).text != "=") break;
            const std::string key = peek().text;
            at_ += 2;   // the name and the equals
            any = true;
            u32 index = 0;
            for (;;) {
                if (read_value(node, key, index)) {
                    ++index;
                } else if (!done() && peek().text != "," && peek().text != "{" &&
                           peek().text != "}" && peek().text != "=") {
                    const Token& token = take();
                    ClipWord word;
                    word.key = key;
                    word.text = token.text;
                    word.line = token.line;
                    word.column = token.column;
                    word.names_a_part = bindings_.count(token.text) != 0;
                    if (word.names_a_part) node.inputs.push_back(bindings_[token.text]);
                    node.words.push_back(word);
                } else {
                    break;
                }
                if (!done() && peek().text == ",") {
                    ++at_;
                    continue;
                }
                break;
            }
        }
        return any;
    }

    // --- expressions -------------------------------------------------------------------------
    //
    // A name the document already bound, or a call. A bare name is a WIRE rather than a node: the
    // whole point of `let all = union { plinth slab }` is that `plinth` is drawn once and joined
    // twice.
    // Deliberately WITHOUT a line test, because `Parser::expression` has none either: a block
    // runs past the ends of lines and a braceless child may sit on the next one. The two readers
    // agreeing about where a statement ends is worth more here than a guard would be.
    bool expression(u32& out) {
        if (done() || peek().text == "}" || peek().text == "{") return false;
        const auto bound = bindings_.find(peek().text);
        if (bound != bindings_.end() && peek(1).text != "=") {
            ++at_;
            out = bound->second;
            return true;
        }
        if (is_number(peek().text)) return false;
        return call(out);
    }

    // How deep braces may nest. The same bound `Parser::block` carries and for the same reason: a
    // file whose braces have desynchronised nests one level for every `{` left in it, and a reader
    // whose whole contract is that it never fails must not be able to end a process.
    static constexpr u32 kMaxDepth = 64;

    void read_block(ClipNode& node) {
        if (done() || peek().text != "{") return;
        if (depth_ >= kMaxDepth) {
            at_ = tokens_.size();
            return;
        }
        ++depth_;
        ++at_;
        while (!done() && peek().text != "}") {
            u32 child = 0;
            if (!expression(child)) {
                ++at_;
                continue;
            }
            node.inputs.push_back(child);
            if (child < out_.nodes.size()) {
                node.last_line = std::max(node.last_line, out_.nodes[child].last_line);
            }
        }
        if (!done() && peek().text == "}") {
            node.last_line = std::max(node.last_line, peek().line);
            ++at_;
        }
        --depth_;
    }

    // One call: a head, its numbers, its keys, and — for the heads that take them — its children.
    //
    // The order mirrors `Parser::call`: positional values, keys, the block, then keys and values
    // again, because `union { a b } smooth=0.02` puts a key after the brace and `shell walls 0.1`
    // puts a number after a braceless child.
    bool call(u32& out) {
        if (done()) return false;
        const Token& head_token = take();
        ClipNode node;
        node.head = head_token.text;
        node.line = head_token.line;
        node.last_line = head_token.line;
        node.column = head_token.column;

        read_positional(node);
        read_keys(node);
        if (takes_many(node.head) || takes_one(node.head)) {
            read_block(node);
            if (node.inputs.empty() && takes_one(node.head)) {
                // `shell walls 0.1` without braces, which reads better for one child.
                u32 child = 0;
                if (expression(child)) {
                    node.inputs.push_back(child);
                    if (child < out_.nodes.size()) {
                        node.last_line = std::max(node.last_line, out_.nodes[child].last_line);
                    }
                }
            }
            read_keys(node);
            read_positional(node);
        } else if (!done() && peek().text == "{") {
            // A head this reader has never heard of, with braces after it. Read them as children
            // anyway rather than walking past: D454 says nothing is dropped, and a head added to
            // the language after this file was written is exactly the case that reaches here.
            read_block(node);
            read_keys(node);
        }

        for (const ClipNumber& number : node.numbers) {
            node.last_line = std::max(node.last_line, number.line);
        }
        for (const ClipWord& word : node.words) {
            node.last_line = std::max(node.last_line, word.line);
        }
        // A bare word with nothing after it and no head in the language is a name from somewhere
        // else — every part of a world is one, because a world is a manifest and `union { part_site
        // part_podium ... }` names shapes declared in the files it includes.
        node.unresolved = node.numbers.empty() && node.words.empty() && node.inputs.empty() &&
                          !takes_many(node.head) && !takes_one(node.head) && !is_shape(node.head);
        node.carries = decide_carries(node);
        out = static_cast<u32>(out_.nodes.size());
        out_.nodes.push_back(std::move(node));
        return true;
    }

    ClipCarries decide_carries(const ClipNode& node) const {
        if (passes_through(node.head) && !node.inputs.empty()) {
            const u32 first = node.inputs.front();
            if (first < out_.nodes.size()) return out_.nodes[first].carries;
            return ClipCarries::Shape;
        }
        return is_shape(node.head) ? ClipCarries::Shape : ClipCarries::Value;
    }

    // A statement node: something with no value of its own that the document does at the top
    // level. `solid`, `paint`, `metre`. It gets a node like everything else, because a view that
    // draws the expressions and not the statements has drawn half a document.
    // Returns the INDEX rather than a reference, because everything a statement does next can push
    // nodes of its own and a reference into a growing vector is a dangling reference one line
    // later. Nothing here holds one across a call that can allocate.
    u32 begin_statement(const Token& head_token) {
        flush_opaque();
        ClipNode node;
        node.head = head_token.text;
        node.statement = true;
        node.line = head_token.line;
        node.last_line = head_token.line;
        node.column = head_token.column;
        out_.nodes.push_back(std::move(node));
        ++out_.statements;
        return static_cast<u32>(out_.nodes.size() - 1);
    }

    void finish_statement(u32 index) {
        ClipNode& node = out_.nodes[index];
        for (const ClipNumber& number : node.numbers) {
            node.last_line = std::max(node.last_line, number.line);
        }
        for (const ClipWord& word : node.words) {
            node.last_line = std::max(node.last_line, word.line);
        }
        if (at_ > 0) node.last_line = std::max(node.last_line, tokens_[at_ - 1].line);
    }

    void statement();

    // A name from somewhere else takes the reading of whoever USES it.
    //
    // The document cannot say what `part_podium` is — it is declared in a file this one includes —
    // but it can see that a union is made of it, and a union is made of shapes. So an unresolved
    // name takes the carries of the first node that has it as a CHILD, which is where the reading
    // is decided: a child of a union is a shape and a child of `add` is a value.
    //
    // A child is the only way it can be reached, and that is worth stating rather than testing
    // for: an unresolved node is only ever MADE from a bare name in a child position, because a
    // `key=name` wires to a binding that already exists or to nothing at all. So this is one walk
    // over the edges and not a scan of every node per unresolved one.
    //
    // One case it gets wrong and it is worth naming: `displace { thing grain }` is a shape whose
    // SECOND child is a value, so an unresolved grain there comes out drawn as a shape. It is a
    // wire's colour on a name the document never declared, which is the smallest thing in this
    // file that can be wrong.
    void settle_unresolved() {
        std::vector<bool> settled(out_.nodes.size(), false);
        for (const ClipNode& user : out_.nodes) {
            for (u32 input : user.inputs) {
                if (input >= out_.nodes.size()) continue;
                if (!out_.nodes[input].unresolved || settled[input]) continue;
                out_.nodes[input].carries = user.carries;
                settled[input] = true;
            }
        }
    }

    // --- laying it out -------------------------------------------------------------------------
    //
    // One further right than the furthest of its inputs, worked out once here rather than by
    // whoever draws it, so that two drawings of one document cannot disagree about its shape.
    // Bounded by the node count, because a document being typed into can name itself in a cycle
    // for as long as it takes to finish the word.
    void settle_depths() {
        const usize count = out_.nodes.size();
        // Inputs are pushed before their parent everywhere except the one place a `let` puts a
        // statement in front of its own expression, so a forward pass and a second one settle
        // every real document and the loop breaks on the third. The cap is against a document
        // being typed into, which can name itself in a cycle for as long as it takes to finish
        // the word — and a graph deeper than this is not one anybody can look at anyway.
        const usize kRounds = 64;
        for (usize round = 0; round < count && round < kRounds; ++round) {
            bool moved = false;
            for (usize i = 0; i < count; ++i) {
                u32 want = 0;
                for (u32 input : out_.nodes[i].inputs) {
                    if (input >= count) continue;
                    want = std::max(want, out_.nodes[input].depth + 1);
                }
                if (want > out_.nodes[i].depth) {
                    out_.nodes[i].depth = want;
                    moved = true;
                }
            }
            if (!moved) break;
        }
    }

    const std::vector<Token>& tokens_;
    const std::vector<std::string>& lines_;
    ClipGraph& out_;
    usize at_ = 0;
    u32 depth_ = 0;
    std::map<std::string, u32> bindings_;
    std::map<std::string, u32> parameters_;
    std::map<std::string, u32> materials_;

    bool opaque_open_ = false;
    u32 opaque_first_ = 0;
    u32 opaque_last_ = 0;
};

void Reader::statement() {
    if (done()) return;
    const Token head_token = take();
    const std::string head = head_token.text;

    if (!is_statement_head(head)) {
        // Not a statement this reader knows. The whole line goes into a text node, whole, and the
        // document is still the document (D454).
        const u32 last = std::max(head_token.line, skip_statement());
        keep_opaque(head_token.line, last);
        return;
    }

    // By index and never by reference: everything below can push nodes of its own, and a
    // reference into a growing vector is a dangling reference one line later.
    const u32 index = begin_statement(head_token);

    if (head == "let") {
        const Token& name = take();
        const std::string bound = name.text;
        out_.nodes[index].name = bound;
        if (!done() && peek().text == "=") ++at_;
        u32 value = 0;
        if (expression(value)) {
            // The statement IS its expression: `let all = union { ... }` is one node called `all`,
            // not a `let` box wired to a union box. A view with a box for every keyword is a view
            // of the grammar rather than of the clip.
            ClipNode expr = out_.nodes[value];
            const bool tail = (value + 1 == out_.nodes.size());
            expr.name = bound;
            expr.statement = true;
            expr.line = head_token.line;
            expr.column = head_token.column;
            expr.last_line = std::max(expr.last_line, head_token.line);
            if (tail) {
                // The expression was built for this statement and nothing else refers to it, so
                // the statement takes its place rather than standing beside it.
                out_.nodes.pop_back();          // the expression
                out_.nodes[index] = expr;       // over the placeholder
                bindings_[bound] = index;
            } else {
                // `let b = a` — a second name for something already drawn. The statement is a node
                // of its own that carries the name and points at what it renamed.
                out_.nodes[index].inputs.push_back(value);
                out_.nodes[index].name = bound;
                out_.nodes[index].carries = out_.nodes[value].carries;
                bindings_[bound] = index;
            }
        } else {
            out_.nodes[index].name = bound;
            const u32 last = skip_statement();
            out_.nodes[index].last_line = std::max(out_.nodes[index].last_line, last);
            bindings_[bound] = index;
        }
        finish_statement(index);
        return;
    }

    if (head == "material") {
        out_.nodes[index].name = take().text;
        out_.nodes[index].carries = ClipCarries::Material;
        read_keys(out_.nodes[index]);
        materials_[out_.nodes[index].name] = index;
        finish_statement(index);
        return;
    }

    if (head == "paint") {
        const Token& material = take();
        out_.nodes[index].name = material.text;
        out_.nodes[index].carries = ClipCarries::Material;
        ClipWord word;
        word.text = material.text;
        word.line = material.line;
        word.column = material.column;
        out_.nodes[index].words.push_back(word);
        const auto known = materials_.find(material.text);
        if (known != materials_.end()) out_.nodes[index].inputs.push_back(known->second);
        read_keys(out_.nodes[index]);
        finish_statement(index);
        return;
    }

    if (head == "weather") {
        const Token& kind = take();
        out_.nodes[index].name = kind.text;
        read_positional(out_.nodes[index]);
        read_keys(out_.nodes[index]);
        finish_statement(index);
        return;
    }

    if (head == "param") {
        const Token& name = take();
        out_.nodes[index].name = name.text;
        parameters_[name.text] = index;
        read_positional(out_.nodes[index]);
        read_keys(out_.nodes[index]);
        finish_statement(index);
        return;
    }

    if (head == "include") {
        const Token& file = take();
        std::string named = file.text;
        // The quotes are part of the token, because `"` is not a separator anywhere in this
        // language. What a player reads is the name inside them.
        if (named.size() >= 2 && named.front() == '"' && named.back() == '"') {
            named = named.substr(1, named.size() - 2);
        }
        ClipWord word;
        word.text = named;
        word.line = file.line;
        word.column = file.column;
        out_.nodes[index].words.push_back(word);
        out_.nodes[index].target = named;
        // The last part of the path, because a column of boxes every one of which begins
        // `facility/` has said the folder eleven times and the file none.
        const usize slash = named.find_last_of("/\\");
        out_.nodes[index].name =
            (slash == std::string::npos) ? named : named.substr(slash + 1);
        finish_statement(index);
        return;
    }

    if (head == "solid" || head == "region") {
        u32 value = 0;
        if (expression(value)) {
            out_.nodes[index].inputs.push_back(value);
            out_.nodes[index].carries = ClipCarries::Shape;
        } else {
            const u32 last = skip_statement();
            out_.nodes[index].last_line = std::max(out_.nodes[index].last_line, last);
        }
        finish_statement(index);
        return;
    }

    // `metre`, `bounds`, `origin`, `variation` — numbers and keys, and nothing else.
    read_positional(out_.nodes[index]);
    read_keys(out_.nodes[index]);
    finish_statement(index);
}

}  // namespace

const ClipWord* ClipNode::word(const std::string& key) const {
    for (const ClipWord& one : words) {
        if (one.key == key) return &one;
    }
    return nullptr;
}

const ClipNumber* ClipNode::number(const std::string& key) const {
    for (const ClipNumber& one : numbers) {
        if (one.key == key) return &one;
    }
    return nullptr;
}

std::string ClipGraph::key_of(const ClipNode& node) {
    // Where its head is written, and the name beside it.
    //
    // **The name alone is not unique and it is not close.** `let all = displace { all grain }` is
    // named in `20-clip-forge.md` as *the form most authoring actually takes*: a clip re-binds a
    // name as a pipeline, and `clips/sampler.clip` re-binds `slab_a` and `hut` on purpose. Two
    // nodes of one name means a key that finds the wrong one, so the selection would open one
    // node's sliders over another node's numbers — and the pair are usually a box and the displace
    // that eats it, which is exactly the pair whose numbers look plausible in each other's panel.
    //
    // The position is what makes it unique, and it does not move when a number further along the
    // line changes — which is the edit a slider makes, sixty times a second, while the node is
    // selected. Inserting a LINE above it does move it, and what that costs is the selection, once,
    // on a keystroke that was not about this node.
    char at[32];
    std::snprintf(at, sizeof(at), "@%u:%u", node.line, node.column);
    return node.name + at;
}

u32 ClipGraph::find(const std::string& key) const {
    for (usize i = 0; i < nodes.size(); ++i) {
        if (key_of(nodes[i]) == key) return static_cast<u32>(i);
    }
    return kNone;
}

ClipGraph read_clip_graph(const std::vector<std::string>& lines) {
    ClipGraph graph;
    const std::vector<Token> tokens = tokenize(lines);
    Reader reader(tokens, lines, graph);
    reader.run();
    return graph;
}

u32 clip_number_decimals(const std::string& as_written) {
    const usize point = as_written.find('.');
    if (point == std::string::npos) return 0;
    usize end = as_written.find_first_of("eE", point);
    if (end == std::string::npos) end = as_written.size();
    return static_cast<u32>(std::min<usize>(end - point - 1, 6));
}

std::string spell_clip_number(f64 value, const std::string& as_written) {
    // **The author's own spelling decides.** Somebody who wrote `0.18` gets two decimals back and
    // somebody who wrote `4` gets none, because a slider that turned `sides=6` into `6.000000`
    // would have reformatted the document — which is the one thing a round trip may not do
    // (`23-shell-and-libraries.md` §5c). It is also what makes the slider's own step right: a
    // count steps by one and a fillet steps by a hundredth, and neither had to be told which.
    i32 decimals = static_cast<i32>(clip_number_decimals(as_written));

    // Wider only where the author's spelling would lose the value, which is what a TYPED number
    // does: `0.045` into a row that read `0.04` has to stay `0.045` rather than becoming what the
    // row already said. A dragged value is a multiple of the step and reaches none of this.
    for (i32 tries = decimals; tries <= 6; ++tries) {
        char probe[64];
        std::snprintf(probe, sizeof(probe), "%.*f", tries, value);
        decimals = tries;
        if (std::strtod(probe, nullptr) == value) break;
    }

    char text[64];
    std::snprintf(text, sizeof(text), "%.*f", std::clamp(decimals, 0, 6), value);
    std::string out = text;
    // Never a bare `-0`: it is the same number and it reads as a mistake.
    if (!out.empty() && out.front() == '-' && out.find_first_not_of("-0.") == std::string::npos) {
        out.erase(0, 1);
    }
    return out;
}

bool write_clip_number(std::vector<std::string>& lines, const ClipNumber& at,
                       const std::string& text) {
    if (at.line == 0 || at.line > lines.size()) return false;
    std::string& line = lines[at.line - 1];
    if (at.column + at.text.size() > line.size()) return false;
    // The span has to still hold what the graph said it did. A graph read from one document and
    // written through into another is exactly the sort of thing that produces a file nobody meant,
    // and this is one comparison.
    if (line.compare(at.column, at.text.size(), at.text) != 0) return false;
    line.replace(at.column, at.text.size(), text);
    return true;
}

}  // namespace ws
