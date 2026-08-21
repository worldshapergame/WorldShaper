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

// Everything else the language has a word for: the patterns and the arithmetic. With the shapes,
// the two child-taking groups and the statements above it, this is `20-clip-forge.md` §2's whole
// vocabulary — and it is here for exactly one job, which is telling a VERB from a NAME. Nothing
// about the letters in `box` and `plinth` says which of the two is the language's and which is the
// author's, so the colouring has to be told, and this is the list.
bool is_pattern_head(const std::string& head) {
    return head == "sine" || head == "waves" || head == "noise" || head == "fbm" ||
           head == "ridged" || head == "rasp" || head == "cells" || head == "cell_edge" ||
           head == "checker" || head == "stripes" || head == "bricks" || head == "axis" ||
           head == "distance" || head == "constant" || head == "curvature" ||
           head == "occlusion" || head == "facing";
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
            ClipLink link;
            link.from = parameters_[token.text];
            link.named = true;
            link.key = key;
            link.line = token.line;
            link.column = token.column;
            link.length = static_cast<u32>(token.text.size());
            node.links.push_back(link);
            node.inputs.push_back(link.from);
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
            const u32 key_column = peek().column;
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
                    if (word.names_a_part) {
                        ClipLink link;
                        link.from = bindings_[token.text];
                        link.named = true;
                        link.key = key;
                        // The whole of `where=grain`, because taking the name out and leaving
                        // `where=` behind is a document that no longer parses.
                        link.line = token.line;
                        link.column = key_column;
                        link.length = token.column + static_cast<u32>(token.text.size()) -
                                      key_column;
                        node.links.push_back(link);
                        node.inputs.push_back(link.from);
                    }
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
        node.has_block = true;
        node.block_line = peek().line;
        node.block_column = peek().column;
        ++depth_;
        ++at_;
        while (!done() && peek().text != "}") {
            const Token at_start = peek();
            u32 child = 0;
            const usize was = at_;
            if (!expression(child)) {
                ++at_;
                continue;
            }
            ClipLink link;
            link.from = child;
            // One token consumed and it named something already bound: that is a WIRE, and its
            // bytes are what a cut erases. Anything longer is a shape written out where it stands,
            // which has no name to take away.
            link.named = (at_ == was + 1);
            link.line = at_start.line;
            link.column = at_start.column;
            link.length = static_cast<u32>(at_start.text.size());
            node.links.push_back(link);
            node.inputs.push_back(child);
            if (child < out_.nodes.size()) {
                node.last_line = std::max(node.last_line, out_.nodes[child].last_line);
            }
        }
        if (!done() && peek().text == "}") {
            node.last_line = std::max(node.last_line, peek().line);
            node.close_line = peek().line;
            node.close_column = peek().column;
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
        node.word_line = head_token.line;
        node.word_column = head_token.column;

        read_positional(node);
        read_keys(node);
        if (takes_many(node.head) || takes_one(node.head)) {
            read_block(node);
            if (node.inputs.empty() && !node.has_block && takes_one(node.head)) {
                // `shell walls 0.1` without braces, which reads better for one child. Only where
                // there were no braces at all: an explicit empty pair means the author has said
                // where the children go, and taking the next word as one swallows a key.
                const Token at_start = peek();
                const usize was = at_;
                u32 child = 0;
                if (expression(child)) {
                    ClipLink link;
                    link.from = child;
                    link.named = (at_ == was + 1);
                    link.line = at_start.line;
                    link.column = at_start.column;
                    link.length = static_cast<u32>(at_start.text.size());
                    node.links.push_back(link);
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
        note_end(node);
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

    // One past the last token this node consumed, which is what a hoist cuts to and what a delete
    // removes. Taken from the token stream rather than reconstructed from the pieces, because a
    // node's last token is often a `}` that belongs to none of them.
    void note_end(ClipNode& node) {
        if (at_ == 0) return;
        const Token& last = tokens_[at_ - 1];
        node.end_line = last.line;
        node.end_column = last.column + static_cast<u32>(last.text.size());
        node.last_line = std::max(node.last_line, last.line);
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
        node.word_line = head_token.line;
        node.word_column = head_token.column;
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
        note_end(node);
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

    // Where the name a statement binds is written, so a copy can be given one of its own.
    const auto note_name = [&](u32 at, const Token& token) {
        out_.nodes[at].name_line = token.line;
        out_.nodes[at].name_column = token.column;
        out_.nodes[at].name_length = static_cast<u32>(token.text.size());
    };

    if (head == "let") {
        const Token& name = take();
        const std::string bound = name.text;
        out_.nodes[index].name = bound;
        note_name(index, name);
        const u32 name_line = name.line;
        const u32 name_column = name.column;
        const u32 name_length = static_cast<u32>(name.text.size());
        if (!done() && peek().text == "=") ++at_;
        u32 value = 0;
        if (expression(value)) {
            // The statement IS its expression: `let all = union { ... }` is one node called `all`,
            // not a `let` box wired to a union box. A view with a box for every keyword is a view
            // of the grammar rather than of the clip.
            ClipNode expr = out_.nodes[value];
            const bool tail = (value + 1 == out_.nodes.size());
            expr.name = bound;
            expr.name_line = name_line;
            expr.name_column = name_column;
            expr.name_length = name_length;
            expr.statement = true;
            expr.line = head_token.line;
            expr.column = head_token.column;
            // `expr.word_line`/`word_column` are left alone on purpose: they are where `box` is,
            // and this statement begins at `let`.
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
        const Token& name = take();
        out_.nodes[index].name = name.text;
        note_name(index, name);
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
        note_name(index, name);
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

// --- where the author dragged a node ----------------------------------------------------------
//
// `  #@ x y` at the end of a node's own line. A comment, so the parser that builds the world never
// sees it and the file still means exactly what it meant; and IN the file, because
// `23-shell-and-libraries.md` §4 forbids the game keeping state about a file that the file does not
// carry — so a clip you send somebody opens laid out the way you left it, and there is no sidecar
// to lose, to forget, or to leave behind when the file is renamed.
//
// Two numbers and nothing else after the marker, so an ordinary comment that happens to contain
// `#@` is not mistaken for one. What that costs is a layout somebody wrote by hand and got wrong,
// which is a layout that is quietly ignored rather than a document that breaks.
namespace {

constexpr const char* kPlacedMarker = "#@";
// And the document's own, on a line of its own. A different spelling, because a line that is
// nothing but a placement has to be recognisable as one — `without_placement` would otherwise
// leave an empty line behind and the file would not come back byte for byte.
constexpr const char* kDocumentMarker = "#@doc";

// Whether this line is nothing but where the document's own box sits.
static bool is_document_placement(const std::string& line, f32& x, f32& y) {
    usize at = line.find_first_not_of(" \t");
    if (at == std::string::npos) return false;
    if (line.compare(at, std::strlen(kDocumentMarker), kDocumentMarker) != 0) return false;
    const char* rest = line.c_str() + at + std::strlen(kDocumentMarker);
    f64 read_x = 0.0;
    f64 read_y = 0.0;
    int used = 0;
    if (std::sscanf(rest, "%lf %lf%n", &read_x, &read_y, &used) != 2) return false;
    // Nothing after the two numbers, or it is a comment that merely starts the same way.
    const char* tail = rest + used;
    while (*tail == ' ' || *tail == '\t' || *tail == '\r') ++tail;
    if (*tail != '\0') return false;
    x = static_cast<f32>(read_x);
    y = static_cast<f32>(read_y);
    return true;
}

bool read_placement(const std::string& line, f32& x, f32& y) {
    const usize at = line.rfind(kPlacedMarker);
    if (at == std::string::npos) return false;
    const char* from = line.c_str() + at + 2;
    char* end = nullptr;
    const f64 first = std::strtod(from, &end);
    if (end == from) return false;
    const char* second_from = end;
    const f64 second = std::strtod(second_from, &end);
    if (end == second_from) return false;
    while (*end == ' ' || *end == '\t' || *end == '\r') ++end;
    if (*end != '\0') return false;
    x = static_cast<f32>(first);
    y = static_cast<f32>(second);
    return true;
}

// The line with any placement taken off the end, and the trailing space with it.
std::string without_placement(const std::string& line) {
    f32 x = 0.0f;
    f32 y = 0.0f;
    if (!read_placement(line, x, y)) return line;
    usize at = line.rfind(kPlacedMarker);
    while (at > 0 && (line[at - 1] == ' ' || line[at - 1] == '\t')) --at;
    return line.substr(0, at);
}

// A name nothing in the document already uses.
std::string free_name(const ClipGraph& graph, const std::string& stem) {
    for (u32 nth = 1; nth < 1000; ++nth) {
        const std::string want = stem + "_" + std::to_string(nth);
        bool taken = false;
        for (const ClipNode& node : graph.nodes) {
            if (node.name == want) taken = true;
        }
        if (!taken) return want;
    }
    return stem + "_x";
}

}  // namespace

ClipGraph read_clip_graph(const std::vector<std::string>& lines) {
    ClipGraph graph;
    const std::vector<Token> tokens = tokenize(lines);
    Reader reader(tokens, lines, graph);
    reader.run();

    // Only a STATEMENT carries a position. A sub-expression is drawn beside whatever uses it, so a
    // marker on its line would be a second answer to a question its parent has already answered —
    // and two nodes can start on one line, which would make the marker ambiguous as well.
    for (ClipNode& node : graph.nodes) {
        if (!node.statement || node.line == 0 || node.line > lines.size()) continue;
        node.placed = read_placement(lines[node.line - 1], node.at_x, node.at_y);
    }
    // And where the box that stands for the FILE was put, which belongs to no statement and so has
    // a line of its own.
    for (const std::string& line : lines) {
        if (is_document_placement(line, graph.doc_x, graph.doc_y)) {
            graph.doc_placed = true;
            break;
        }
    }
    return graph;
}

bool clip_head_known(const std::string& word) {
    return is_statement_head(word) || takes_many(word) || takes_one(word) || is_shape(word) ||
           is_pattern_head(word) || is_moulding(word) || word == "spiral" || word == "branch" ||
           word == "revolve" || word == "arc";
}

// What a word of the language IS, in the three the whole interface is coloured by. Not what part of
// speech it is: `box` and `fbm` are both verbs and one of them makes matter while the other makes a
// number, and a colour scheme that put those together would be teaching the grammar rather than the
// clip.
ClipPart part_of_word(const std::string& word) {
    if (is_number(word)) return ClipPart::Value;
    if (word == "material" || word == "paint" || word == "weather" || word == "variation") {
        return ClipPart::Material;
    }
    if (word == "solid" || word == "region") return ClipPart::Shape;
    if (is_shape(word) || word == "spiral" || word == "branch" || word == "revolve" ||
        word == "arc") {
        return ClipPart::Shape;
    }
    if (is_pattern_head(word)) return ClipPart::Value;
    // What is left of the two child-taking groups: the transforms move matter about and the
    // arithmetic works on amounts.
    if (word == "translate" || word == "rotate" || word == "scale" || word == "mirror" ||
        word == "repeat" || word == "scatter" || word == "around" || word == "twist" ||
        word == "bend") {
        return ClipPart::Shape;
    }
    if (takes_many(word) || takes_one(word)) return ClipPart::Value;
    // `let`, `metre`, `bounds`, `param`, `origin`, `include` — and every name the author chose.
    return ClipPart::Name;
}

std::vector<ClipSpan> colour_clip_line(const std::string& line) {
    std::vector<ClipSpan> spans;
    const auto push = [&](usize from, usize to, ClipPart part) {
        if (to <= from) return;
        spans.push_back(ClipSpan{static_cast<u32>(from), static_cast<u32>(to - from), part});
    };

    usize i = 0;
    bool first_word = true;
    bool after_let = false;
    while (i < line.size()) {
        const char c = line[i];
        if (c == ' ' || c == '\t' || c == '\r') {
            ++i;
            continue;
        }
        if (c == '#') {
            push(i, line.size(), ClipPart::Comment);
            break;
        }
        if (c == '{' || c == '}' || c == '=' || c == ',') {
            push(i, i + 1, ClipPart::Grammar);
            ++i;
            continue;
        }
        const usize begin = i;
        while (i < line.size() && !separator(line[i])) ++i;
        const std::string word = line.substr(begin, i - begin);

        // The word right after `let`, and the one right after `material` or `paint`, is what the
        // line is ABOUT — whatever else it happens to spell. A part called `box` is still a name.
        ClipPart part = after_let ? ClipPart::Name : part_of_word(word);

        // A key names which argument it is rather than being a kind of thing, so `size=` and
        // `round=` take no hue however the word is spelled. The value after it still does.
        usize ahead = i;
        while (ahead < line.size() && (line[ahead] == ' ' || line[ahead] == '\t')) ++ahead;
        if (ahead < line.size() && line[ahead] == '=' && part != ClipPart::Value) {
            part = ClipPart::Name;
        }

        push(begin, i, part);
        after_let = first_word && (word == "let" || word == "material" || word == "param");
        first_word = false;
    }
    return spans;
}

std::string clip_without_layout(const std::string& text) {
    if (text.find(kPlacedMarker) == std::string::npos) return text;   // covers `#@doc` too
    std::string out;
    out.reserve(text.size());
    usize at = 0;
    while (at <= text.size()) {
        usize end = text.find('\n', at);
        const bool last = end == std::string::npos;
        if (last) end = text.size();
        std::string line = text.substr(at, end - at);
        // The carriage return goes with the line and comes back with it, so a file with CRLF in it
        // is not quietly rewritten by being hashed.
        std::string tail;
        while (!line.empty() && line.back() == '\r') {
            tail = "\r" + tail;
            line.pop_back();
        }
        f32 ignore_x = 0.0f;
        f32 ignore_y = 0.0f;
        if (is_document_placement(line, ignore_x, ignore_y)) {
            // The whole LINE goes, newline and all, because leaving an empty one behind is a
            // document the cache key would see as different from the one it was built from.
            if (!last) at = end + 1;
            if (last) break;
            continue;
        }
        out += without_placement(line) + tail;
        if (!last) out += '\n';
        if (last) break;
        at = end + 1;
    }
    return out;
}

bool place_clip_document(std::vector<std::string>& lines, f32 x, f32 y) {
    char marker[64];
    std::snprintf(marker, sizeof(marker), "%s %.1f %.1f", kDocumentMarker, static_cast<f64>(x),
                  static_cast<f64>(y));
    for (std::string& line : lines) {
        f32 was_x = 0.0f;
        f32 was_y = 0.0f;
        if (!is_document_placement(line, was_x, was_y)) continue;
        line = marker;
        return true;
    }
    // At the very top, above everything: it is about the file rather than about any statement in it,
    // and a reader looking for what the file IS reads the first line.
    lines.insert(lines.begin(), marker);
    return true;
}

bool place_clip_node(std::vector<std::string>& lines, const ClipNode& node, f32 x, f32 y) {
    if (node.line == 0 || node.line > lines.size()) return false;
    char marker[64];
    std::snprintf(marker, sizeof(marker), "  %s %.1f %.1f", kPlacedMarker, static_cast<f64>(x),
                  static_cast<f64>(y));
    lines[node.line - 1] = without_placement(lines[node.line - 1]) + marker;
    return true;
}

std::string connect_clip_nodes(std::vector<std::string>& lines, const ClipGraph& graph, u32 from,
                               u32 to) {
    if (from >= graph.nodes.size() || to >= graph.nodes.size()) return "nothing to join";
    if (from == to) return "a thing cannot be made of itself";
    const ClipNode& source = graph.nodes[from];
    const ClipNode& target = graph.nodes[to];

    // A wire is a NAME, so the thing being wired has to have one. Everything a `let` binds does; a
    // shape written out inside somebody else's braces does not, and naming it is a job for the
    // script view rather than a thing to do silently behind a drag.
    if (source.name.empty()) {
        return "that one has no name -- give it one in the script and it can be joined";
    }
    if (target.line == 0 || target.line > lines.size()) return "nothing to join it to";
    for (const ClipLink& link : target.links) {
        if (link.from == from) return source.name + " is already in there";
    }
    // A cycle is a document that cannot be built, and the reader would spin on it.
    std::vector<u32> stack{from};
    std::vector<bool> seen(graph.nodes.size(), false);
    while (!stack.empty()) {
        const u32 at = stack.back();
        stack.pop_back();
        if (at >= graph.nodes.size() || seen[at]) continue;
        seen[at] = true;
        if (at == to) return "that would make a ring, and a ring cannot be built";
        for (u32 input : graph.nodes[at].inputs) stack.push_back(input);
    }

    // 1. It has braces: the name goes in before the closing one.
    if (target.has_block && target.close_line > 0 && target.close_line <= lines.size()) {
        std::string& line = lines[target.close_line - 1];
        if (target.close_column > line.size() || line[target.close_column] != '}') {
            return "the document has moved under this -- try again";
        }
        line.insert(target.close_column, source.name + " ");
        return {};
    }
    // 2. It takes children and has none written down: give it a pair.
    if (takes_many(target.head) || takes_one(target.head)) {
        if (!target.inputs.empty()) {
            // The braceless one-child form, `shell walls 0.1`. Wrapping it is what makes room.
            const ClipLink& only = target.links.empty() ? ClipLink{} : target.links.front();
            if (!target.links.empty() && only.named && only.line > 0 && only.line <= lines.size()) {
                std::string& line = lines[only.line - 1];
                if (only.column + only.length > line.size()) {
                    return "the document has moved under this -- try again";
                }
                line.insert(only.column + only.length, " " + source.name + " }");
                line.insert(only.column, "{ ");
                return {};
            }
            return "write that one's braces in the script first";
        }
        std::string& line = lines[target.line - 1];
        const usize after = target.column + target.head.size();
        if (after > line.size()) return "the document has moved under this -- try again";
        line.insert(after, " { " + source.name + " }");
        return {};
    }
    // 3. `solid` and `region` name one expression, so this replaces it.
    if (target.head == "solid" || target.head == "region") {
        std::string& line = lines[target.line - 1];
        const usize after = target.column + target.head.size();
        if (after > line.size()) return "the document has moved under this -- try again";
        line = line.substr(0, after) + " " + source.name;
        return {};
    }
    // 4. A coat reads a pattern through `where=`, and a weathering through `on=`.
    if (target.head == "paint" || target.head == "weather" || target.head == "variation") {
        const char* key = (target.head == "paint")       ? "where="
                          : (target.head == "weather") ? "on="
                                                       : "by=";
        if (target.word(std::string(key).substr(0, std::strlen(key) - 1)) != nullptr) {
            return std::string("that one already reads a ") + key;
        }
        std::string& line = lines[target.line - 1];
        line = without_placement(line) + " " + key + source.name;
        return {};
    }
    return target.head + " is not made of anything -- it has no room for a wire";
}

std::string disconnect_clip_node(std::vector<std::string>& lines, const ClipGraph& graph, u32 to,
                                 u32 which) {
    if (to >= graph.nodes.size()) return "nothing to cut";
    const ClipNode& target = graph.nodes[to];
    if (which >= target.links.size()) return "nothing to cut";
    const ClipLink& link = target.links[which];
    if (!link.named) {
        return "that one is written out where it stands -- delete the shape rather than the wire";
    }
    if (link.line == 0 || link.line > lines.size()) return "nothing to cut";
    std::string& line = lines[link.line - 1];
    if (link.column + link.length > line.size()) {
        return "the document has moved under this -- try again";
    }
    usize from = link.column;
    usize length = link.length;
    // And the space that was holding it apart from its neighbour, so a block does not fill up with
    // gaps as things are taken out of it.
    while (from + length < line.size() && line[from + length] == ' ') ++length;
    if (from + length >= line.size() || line[from + length] == '}') {
        while (from > 0 && line[from - 1] == ' ') {
            --from;
            ++length;
        }
    }
    line.erase(from, length);
    return {};
}

std::string add_clip_include(std::vector<std::string>& lines, const ClipGraph& graph,
                             const std::string& file, f32 x, f32 y) {
    if (file.empty()) return "an include has to name a file";
    if (file.find('"') != std::string::npos) return "a file name cannot hold a quotation mark";
    for (const ClipNode& node : graph.nodes) {
        if (node.head == "include" && node.target == file) return file + " is already in here";
    }
    // At the top, under whatever the document already reads from elsewhere: an include brings names
    // with it, and a name has to be bound before anything reads it.
    usize at = 0;
    for (const ClipNode& node : graph.nodes) {
        if (node.head == "metre" || node.head == "meter" || node.head == "bounds" ||
            node.head == "include" || node.head == "param" || node.head == "material") {
            at = std::max<usize>(at, node.last_line);
        }
    }
    at = std::min(at, lines.size());
    char marker[64];
    std::snprintf(marker, sizeof(marker), "  %s %.1f %.1f", kPlacedMarker, static_cast<f64>(x),
                  static_cast<f64>(y));
    lines.insert(lines.begin() + static_cast<isize>(at), "include \"" + file + "\"" + marker);
    return {};
}

std::string add_clip_node(std::vector<std::string>& lines, const ClipGraph& graph,
                          const std::string& head, f32 x, f32 y, std::string& made) {
    const std::string body = clip_node_template(head);
    if (body.empty()) return "there is no " + head + " to make";

    std::string statement;
    if (head == "material" || head == "param") {
        made = free_name(graph, head == "material" ? "colour" : "number");
        statement = head + " " + made + " " + body;
    } else if (head == "paint") {
        // A coat has to paint with something, and the first material the document declares is the
        // only sensible guess. Without one there is nothing to paint with and saying so is better
        // than writing a line that cannot parse.
        std::string material;
        for (const ClipNode& node : graph.nodes) {
            if (node.head == "material" && material.empty()) material = node.name;
        }
        if (material.empty()) return "declare a material first -- a coat has to paint with one";
        made = material;
        statement = "paint " + material;
    } else if (head == "weather") {
        // A weathering names its kind before its amount, and `desert` is the one every clip that
        // has any starts with.
        made = "desert";
        statement = "weather desert " + body;
    } else if (head == "solid" || head == "region") {
        // These NAME an expression, and one written bare does not merely fail to parse — the
        // parser looks past the end of the line for its expression and finds the next statement.
        // So it is written with something to name, and refused when there is nothing: the same
        // shape as a coat needing a material.
        std::string shape;
        for (const ClipNode& node : graph.nodes) {
            if (node.statement && !node.name.empty() && node.carries == ClipCarries::Shape) {
                shape = node.name;
            }
        }
        if (shape.empty()) return "make a shape first -- " + head + " has to name one";
        made = shape;
        statement = head + " " + shape;
    } else if (head == "metre" || head == "meter" || head == "bounds" || head == "origin" ||
               head == "variation") {
        // A statement that binds no name and takes only numbers.
        made = head;
        statement = head + " " + body;
    } else {
        made = free_name(graph, head);
        statement = "let " + made + " = " + body;
    }

    // Before the first thing that could USE it, so a name is always bound before it is read. A
    // `let` after the `solid` that names it parses into nothing, which is a document that opens as
    // an empty sky — the failure this repository has chased three times.
    usize at = lines.size();
    for (const ClipNode& node : graph.nodes) {
        if (!node.statement) continue;
        if (node.head != "solid" && node.head != "region" && node.head != "paint" &&
            node.head != "weather") {
            continue;
        }
        if (node.line > 0 && node.line - 1 < at) at = node.line - 1;
    }
    if (head == "material" || head == "param" || head == "metre" || head == "meter" ||
        head == "bounds" || head == "origin") {
        // Except these, which everything else reads or which the file opens with: they go at the
        // top, under whatever is already there.
        at = 0;
        for (const ClipNode& node : graph.nodes) {
            if (node.head == "metre" || node.head == "meter" || node.head == "bounds" ||
                node.head == "include" || node.head == "param" || node.head == "material") {
                at = std::max<usize>(at, node.last_line);
            }
        }
    }
    at = std::min(at, lines.size());

    char marker[64];
    std::snprintf(marker, sizeof(marker), "  %s %.1f %.1f", kPlacedMarker, static_cast<f64>(x),
                  static_cast<f64>(y));
    lines.insert(lines.begin() + static_cast<isize>(at), statement + marker);
    return {};
}

std::string delete_clip_node(std::vector<std::string>& lines, const ClipGraph& graph, u32 node) {
    if (node >= graph.nodes.size()) return "nothing to take out";
    const ClipNode& going = graph.nodes[node];
    if (!going.statement) {
        return "that one is written inside another -- delete the line it is on in the script";
    }
    u32 used_by = 0;
    for (const ClipNode& other : graph.nodes) {
        for (u32 input : other.inputs) {
            if (input == node) ++used_by;
        }
    }
    if (used_by > 0) {
        return std::to_string(used_by) + (used_by == 1 ? " thing is" : " things are") +
               " still made of this one";
    }
    if (going.line == 0 || going.line > lines.size()) return "nothing to take out";
    const usize first = going.line - 1;
    const usize last = std::min<usize>(going.last_line, lines.size());
    lines.erase(lines.begin() + static_cast<isize>(first), lines.begin() + static_cast<isize>(last));
    if (lines.empty()) lines.push_back({});
    return {};
}

std::string delete_clip_nodes(std::vector<std::string>& lines, const ClipGraph& graph,
                              const std::vector<u32>& nodes) {
    if (nodes.empty()) return "nothing chosen";
    if (nodes.size() == 1) return delete_clip_node(lines, graph, nodes.front());

    std::vector<bool> going(graph.nodes.size(), false);
    u32 statements = 0;
    for (u32 node : nodes) {
        if (node >= graph.nodes.size()) continue;
        if (!graph.nodes[node].statement) continue;
        going[node] = true;
        ++statements;
    }
    if (statements == 0) {
        return "those are written inside other things -- delete the lines in the script";
    }

    // Used by something that is STAYING. A thing read only by others in the same selection is going
    // with them, so counting every reader — which is what asking one at a time does — would refuse
    // to take out a group that is perfectly self-contained.
    u32 held = 0;
    for (usize i = 0; i < graph.nodes.size(); ++i) {
        if (going[i]) continue;
        for (u32 input : graph.nodes[i].inputs) {
            if (input < going.size() && going[input]) ++held;
        }
    }
    if (held > 0) {
        return std::to_string(held) + (held == 1 ? " thing outside this is" : " things outside this are") +
               " still made of it";
    }

    // Bottom up, so taking one out cannot move the ones still to go.
    std::vector<u32> order;
    for (usize i = 0; i < going.size(); ++i) {
        if (going[i]) order.push_back(static_cast<u32>(i));
    }
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
        return graph.nodes[a].line > graph.nodes[b].line;
    });
    for (u32 node : order) {
        const ClipNode& one = graph.nodes[node];
        if (one.line == 0 || one.line > lines.size()) continue;
        const usize first = one.line - 1;
        const usize last = std::min<usize>(one.last_line, lines.size());
        if (last <= first) continue;
        lines.erase(lines.begin() + static_cast<isize>(first),
                    lines.begin() + static_cast<isize>(last));
    }
    if (lines.empty()) lines.push_back({});
    return {};
}

std::string duplicate_clip_nodes(std::vector<std::string>& lines, const ClipGraph& graph,
                                 const std::vector<u32>& nodes, std::vector<std::string>& made) {
    made.clear();
    if (nodes.empty()) return "nothing chosen";

    // In the order they were written, so a copied material still comes before the copied coat that
    // paints with it, and two copied coats keep their order — which is the whole of what a paint
    // stack is (`20-clip-forge.md` §2: each one paints over the last).
    std::vector<u32> order;
    for (u32 node : nodes) {
        if (node >= graph.nodes.size()) continue;
        const ClipNode& one = graph.nodes[node];
        if (!one.statement || one.name.empty() || one.name_length == 0) continue;
        order.push_back(node);
    }
    if (order.empty()) return "nothing here can be copied on its own";
    std::sort(order.begin(), order.end(),
              [&](u32 a, u32 b) { return graph.nodes[a].line < graph.nodes[b].line; });

    // A name for each, decided before any line is written, because the second copy has to be able
    // to see the first one's name to avoid it.
    std::vector<std::string> taken;
    for (const ClipNode& node : graph.nodes) {
        if (!node.name.empty()) taken.push_back(node.name);
    }
    std::map<std::string, std::string> renamed;
    for (u32 node : order) {
        const std::string& was = graph.nodes[node].name;
        std::string want;
        // An underscore and a number, because a name with a space in it is not a name.
        for (u32 nth = 2; nth < 1000; ++nth) {
            want = was + "_" + std::to_string(nth);
            if (std::find(taken.begin(), taken.end(), want) == taken.end()) break;
        }
        taken.push_back(want);
        renamed[was] = want;
        made.push_back(want);
    }

    // One replacement list per line: where to write, how much to cover, and what to put there.
    struct Patch {
        u32 line = 0;
        u32 column = 0;
        u32 length = 0;
        std::string text;
    };

    std::vector<std::string> written;
    for (u32 node : order) {
        const ClipNode& one = graph.nodes[node];
        if (one.line == 0 || one.line > lines.size()) continue;
        const u32 last = std::min<u32>(one.last_line, static_cast<u32>(lines.size()));
        if (last < one.line) continue;

        std::vector<Patch> patches;
        patches.push_back(Patch{one.name_line, one.name_column, one.name_length, renamed[one.name]});
        for (const ClipLink& link : one.links) {
            if (!link.named || link.from >= graph.nodes.size()) continue;
            const auto to = renamed.find(graph.nodes[link.from].name);
            if (to == renamed.end()) continue;   // it points outside the copy: it keeps pointing there
            // The span covers the whole of `key=name` where there is a key, so the key comes back
            // with it — erasing `where=` and leaving the name behind is a document that stops
            // parsing.
            const std::string text = link.key.empty() ? to->second : (link.key + "=" + to->second);
            patches.push_back(Patch{link.line, link.column, link.length, text});
        }

        for (u32 line = one.line; line <= last; ++line) {
            std::string text = without_placement(lines[line - 1]);
            std::vector<Patch> here;
            for (const Patch& patch : patches) {
                if (patch.line == line) here.push_back(patch);
            }
            // Right to left, so a replacement never moves the column of one still to be applied.
            std::sort(here.begin(), here.end(),
                      [](const Patch& a, const Patch& b) { return a.column > b.column; });
            for (const Patch& patch : here) {
                if (patch.column + patch.length > text.size()) continue;
                text.replace(patch.column, patch.length, patch.text);
            }
            if (line == one.line) {
                // Half a cell down and across from the original, so the copy is a thing you can see
                // and take hold of rather than a thing exactly under what you copied.
                char marker[64];
                std::snprintf(marker, sizeof(marker), "  %s %.1f %.1f", kPlacedMarker,
                              static_cast<f64>(one.at_x + 0.5f), static_cast<f64>(one.at_y + 0.5f));
                text += marker;
            }
            written.push_back(text);
        }
    }
    if (written.empty()) return "nothing here can be copied on its own";

    // After the last line of the last thing copied. Nothing outside reads the new names, so the one
    // ordering that matters is among the copies themselves, and that is the order they were written
    // in above.
    usize at = 0;
    for (u32 node : order) at = std::max<usize>(at, graph.nodes[node].last_line);
    at = std::min(at, lines.size());
    lines.insert(lines.begin() + static_cast<isize>(at), written.begin(), written.end());
    return {};
}

std::string copy_clip_nodes(const std::vector<std::string>& lines, const ClipGraph& graph,
                            const std::vector<u32>& nodes) {
    std::vector<u32> order;
    for (u32 node : nodes) {
        if (node >= graph.nodes.size()) continue;
        if (!graph.nodes[node].statement) continue;
        order.push_back(node);
    }
    if (order.empty()) return {};
    // In the order they are WRITTEN, because a paint stack is an order and a material has to come
    // before the coat that names it.
    std::sort(order.begin(), order.end(),
              [&](u32 a, u32 b) { return graph.nodes[a].line < graph.nodes[b].line; });

    std::string out;
    std::vector<bool> done(lines.size() + 1, false);
    for (u32 node : order) {
        const ClipNode& one = graph.nodes[node];
        if (one.line == 0 || one.line > lines.size()) continue;
        const u32 last = std::min<u32>(one.last_line, static_cast<u32>(lines.size()));
        for (u32 line = one.line; line <= last; ++line) {
            if (done[line]) continue;   // two statements on one line are copied once
            done[line] = true;
            out += lines[line - 1];
            out += "\n";
        }
    }
    return out;
}

std::string paste_clip_nodes(std::vector<std::string>& lines, const ClipGraph& graph,
                             const std::string& text, f32 x, f32 y,
                             std::vector<std::string>& made) {
    made.clear();
    if (text.empty()) return "there is nothing on the clipboard";

    std::vector<std::string> incoming{std::string()};
    for (char c : text) {
        if (c == '\n') {
            incoming.emplace_back();
        } else if (c != '\r') {
            incoming.back() += c;
        }
    }
    while (!incoming.empty() && incoming.back().empty()) incoming.pop_back();
    if (incoming.empty()) return "there is nothing on the clipboard";

    const ClipGraph in = read_clip_graph(incoming);
    bool any = false;
    for (const ClipNode& node : in.nodes) {
        if (node.statement) any = true;
    }
    if (!any) return "what is on the clipboard is not a statement";

    // A free name for each thing the pasted text binds. Its own name where nothing here has it —
    // pasting into an empty document should give back exactly what was copied.
    std::vector<std::string> taken;
    for (const ClipNode& node : graph.nodes) {
        if (!node.name.empty()) taken.push_back(node.name);
    }
    std::map<std::string, std::string> renamed;
    for (const ClipNode& node : in.nodes) {
        if (!node.statement || node.name.empty() || node.name_length == 0) continue;
        if (renamed.count(node.name) != 0) continue;
        std::string want = node.name;
        for (u32 nth = 2; nth < 1000; ++nth) {
            if (std::find(taken.begin(), taken.end(), want) == taken.end()) break;
            want = node.name + "_" + std::to_string(nth);
        }
        taken.push_back(want);
        renamed[node.name] = want;
        made.push_back(want);
    }

    // Where the pasted set sits now, so it can be moved to where the pointer is without losing the
    // shape it was copied in.
    f32 least_x = 0.0f;
    f32 least_y = 0.0f;
    bool placed_any = false;
    for (const ClipNode& node : in.nodes) {
        if (!node.statement || !node.placed) continue;
        if (!placed_any) {
            least_x = node.at_x;
            least_y = node.at_y;
            placed_any = true;
        }
        least_x = std::min(least_x, node.at_x);
        least_y = std::min(least_y, node.at_y);
    }

    struct Patch {
        u32 line = 0;
        u32 column = 0;
        u32 length = 0;
        std::string text;
    };
    std::vector<Patch> patches;
    std::map<u32, std::pair<f32, f32>> place_at;   // first line of a statement -> where it goes
    for (const ClipNode& node : in.nodes) {
        if (!node.statement) continue;
        if (!node.name.empty() && node.name_length > 0) {
            const auto to = renamed.find(node.name);
            if (to != renamed.end()) {
                patches.push_back(Patch{node.name_line, node.name_column, node.name_length,
                                        to->second});
            }
        }
        for (const ClipLink& link : node.links) {
            if (!link.named || link.from >= in.nodes.size()) continue;
            const auto to = renamed.find(in.nodes[link.from].name);
            if (to == renamed.end()) continue;
            const std::string text_of =
                link.key.empty() ? to->second : (link.key + "=" + to->second);
            patches.push_back(Patch{link.line, link.column, link.length, text_of});
        }
        const f32 at_x = placed_any ? (x + node.at_x - least_x) : x;
        const f32 at_y = placed_any ? (y + node.at_y - least_y) : y;
        place_at[node.line] = {at_x, at_y};
    }

    std::vector<std::string> written;
    for (u32 line = 1; line <= static_cast<u32>(incoming.size()); ++line) {
        std::string one = without_placement(incoming[line - 1]);
        std::vector<Patch> here;
        for (const Patch& patch : patches) {
            if (patch.line == line) here.push_back(patch);
        }
        // Right to left, so a replacement never moves the column of one still to be applied.
        std::sort(here.begin(), here.end(),
                  [](const Patch& a, const Patch& b) { return a.column > b.column; });
        for (const Patch& patch : here) {
            if (patch.column + patch.length > one.size()) continue;
            one.replace(patch.column, patch.length, patch.text);
        }
        const auto where = place_at.find(line);
        if (where != place_at.end()) {
            char marker[64];
            std::snprintf(marker, sizeof(marker), "  %s %.1f %.1f", kPlacedMarker,
                          static_cast<f64>(where->second.first),
                          static_cast<f64>(where->second.second));
            one += marker;
        }
        written.push_back(one);
    }

    // Before the first thing that could USE what has just arrived, so a name is bound before it is
    // read — the same rule `add_clip_node` follows and for the same reason.
    usize at = lines.size();
    for (const ClipNode& node : graph.nodes) {
        if (!node.statement) continue;
        if (node.head != "solid" && node.head != "region" && node.head != "paint" &&
            node.head != "weather") {
            continue;
        }
        if (node.line > 0 && node.line - 1 < at) at = node.line - 1;
    }
    at = std::min(at, lines.size());
    lines.insert(lines.begin() + static_cast<isize>(at), written.begin(), written.end());
    return {};
}

std::string clip_node_template(const std::string& head) {
    // Every one of these is something somebody can SEE the moment it is made: a metre-ish shape at
    // the origin rather than a point, a grain at a size that shows on a wall rather than at nought.
    // A palette that makes invisible things is a palette a player presses once.
    //
    // **It is the whole vocabulary now**, not the third of it the first pass offered. `20-clip-forge
    // .md` §2 is the list and this is that list with a default beside each word; anything missing
    // here is a word the language does not have.
    static const std::map<std::string, std::string> kTemplates = {
        // --- solids -----------------------------------------------------------------------
        {"box", "box -0.5 0 -0.5  0.5 1 0.5"},
        {"sphere", "sphere 0 0.5 0 r=0.5"},
        {"cylinder", "cylinder 0 0.5 0 r=0.4 h=1"},
        {"capsule", "capsule 0 0.2 0  0 1 0 r=0.2"},
        {"cone", "cone 0 0 0 r=0.5 h=1"},
        {"torus", "torus 0 0.5 0 ring=0.5 tube=0.15 axis=y"},
        {"arc", "arc 0 0.5 0 ring=0.5 tube=0.1 axis=y from=0 to=0.5"},
        {"prism", "prism 0 0.5 0 r=0.5 h=1 sides=6"},
        {"plane", "plane 0 1 0 0"},
        {"tetra", "tetra 0 0.5 0 r=0.5"},
        {"cube", "cube 0 0.5 0 r=0.5"},
        {"octa", "octa 0 0.5 0 r=0.5"},
        {"dodeca", "dodeca 0 0.5 0 r=0.5"},
        {"icosa", "icosa 0 0.5 0 r=0.5"},
        {"wedge", "wedge -0.5 0 -0.5  0.5 1 0.5 rise=y run=x"},
        {"stairs", "stairs 0 0 0  1 1 1 run=0.3 rise=0.18"},
        {"spiral", "spiral 0 0 0 r=0.3 tighten=0.7 tube=0.05 turns=2 axis=y"},
        {"branch", "branch 0 0 0 h=1.4 r=0.06 levels=4 count=3 spread=0.11 lean=0.3 seed=7"},
        // --- the mouldings, each a section between two corners ----------------------------
        {"fillet", "fillet 0 0  0.06 0.06 -0.5 0.5"},
        {"ovolo", "ovolo 0 0  0.06 0.06 -0.5 0.5"},
        {"cavetto", "cavetto 0 0  0.06 0.06 -0.5 0.5"},
        {"bead", "bead 0 0  0.06 0.06 -0.5 0.5"},
        {"astragal", "astragal 0 0  0.06 0.06 -0.5 0.5"},
        {"scotia", "scotia 0 0  0.08 0.1 -0.5 0.5"},
        {"cyma", "cyma 0 0  0.08 0.1 -0.5 0.5"},
        {"cyma_reversa", "cyma_reversa 0 0  0.08 0.1 -0.5 0.5"},
        // --- combining ---------------------------------------------------------------------
        {"union", "union { }"},
        {"difference", "difference { }"},
        {"intersection", "intersection { }"},
        // --- moving the point ---------------------------------------------------------------
        {"translate", "translate { } 0 0 0"},
        {"rotate", "rotate { } x=0 y=0 z=0"},
        {"scale", "scale { } 1 1 1"},
        {"mirror", "mirror { } axis=x"},
        {"repeat", "repeat { } 1 1 1"},
        {"around", "around { } count=6 axis=y"},
        {"scatter", "scatter { } x=0.15 z=0.15 nx=8 nz=8 jitter=0.4 turn=0.5"},
        {"twist", "twist { } by=0.1 axis=y"},
        {"bend", "bend { } by=0.1 axis=y"},
        // --- changing the answer --------------------------------------------------------------
        {"shell", "shell { } thickness=0.1"},
        {"round", "round { } by=0.05"},
        {"offset", "offset { } by=0"},
        {"displace", "displace { } amount=0.02"},
        // No `axis=` on this one: a key AFTER positional numbers is read by neither this reader
        // nor the parser, both of which take the keys first, and y is the default anyway.
        {"revolve", "revolve { } 0 0 0"},
        // --- patterns --------------------------------------------------------------------------
        {"fbm", "fbm size=0.2 octaves=4 seed=1"},
        {"noise", "noise size=0.2 seed=1"},
        {"ridged", "ridged size=0.25 octaves=3 seed=1"},
        {"rasp", "rasp size=0.05 depth=1 seed=1"},
        {"cells", "cells size=0.2 seed=1"},
        {"cell_edge", "cell_edge size=0.2 seed=1"},
        {"sine", "sine axis=x period=0.5"},
        {"waves", "waves axis=y period=0.5"},
        {"checker", "checker 0.25 0.25 0.25"},
        {"stripes", "stripes axis=x period=0.25 duty=0.5"},
        {"bricks", "bricks length=0.3 height=0.12 mortar=0.02 facing=z"},
        {"axis", "axis of=y"},
        {"distance", "distance 0 0 0"},
        {"constant", "constant 0"},
        // --- what the shape is doing here ------------------------------------------------------
        {"occlusion", "occlusion { } r=0.2"},
        {"curvature", "curvature { } r=0.1"},
        {"facing", "facing { } axis=y"},
        // --- arithmetic on values ---------------------------------------------------------------
        {"add", "add { }"},
        {"multiply", "multiply { }"},
        {"min", "min { }"},
        {"max", "max { }"},
        {"blend", "blend { } by=0.5"},
        {"remap", "remap { } 0 1 0 1"},
        {"abs", "abs { }"},
        {"negate", "negate { }"},
        {"step", "step { } at=0.5"},
        {"smoothstep", "smoothstep { } 0.4 0.6"},
        {"clamp", "clamp { } 0 1"},
        {"power", "power { } by=2"},
        // --- what the document says about itself ------------------------------------------------
        {"metre", "32"},
        {"bounds", "-2 0 -2  2 3 2"},
        {"origin", "0 0 0"},
        {"variation", "colour=0.03 rough=0.05 seed=1"},
        {"weather", "0.4"},
        {"material", "rgb=180,180,180 rough=200"},
        {"param", "1"},
        {"paint", ""},
        {"solid", ""},
        {"region", ""},
    };
    const auto found = kTemplates.find(head);
    if (found == kTemplates.end()) return {};
    // Three of them have no body of their own and are assembled by `add_clip_node`. Handing back
    // the head rather than nothing is what keeps "there is no such thing" a different answer from
    // "there is nothing to write after it" — trap 7, in a palette.
    if (found->second.empty()) return head;
    return found->second;
}

std::string clip_head_shown(const std::string& head) {
    if (head == "material") return "voxel type";
    return head;
}

const std::vector<ClipProperty>& clip_properties_of(const std::string& head) {
    // The material's whole surface, in the order a person thinks about one: what colour it is, how
    // it takes light, and then the two that only glass and metal care about. Every one of these was
    // already read by `forge/clip_script.cpp` and none of them were offered anywhere.
    //
    // The ranges are the ranges the RECORD holds, not invented ones: a `VisualRecord` keeps most of
    // these in a byte, so 0 to 255 is the whole of what can be said and a slider that ran further
    // would be a slider lying about what the file can hold. `ior` is the exception — it is written
    // as a refractive index and stored as the offset from vacuum — so it is spelled the way the
    // physics is and converted on the way in.
    static const std::vector<ClipProperty> kMaterial = {
        {"rgb", 3, 170.0, 0.0, 255.0, 0, "What colour it is, as red, green and blue", "colour"},
        {"rough", 1, 200.0, 0.0, 255.0, 0, "0 is a mirror, 255 is chalk", "surface"},
        {"metal", 1, 0.0, 0.0, 255.0, 0, "How much it reflects its own colour rather than white",
         "surface"},
        {"lacquer", 1, 0.0, 0.0, 15.0, 0, "A clear coat over the top of it", "surface"},
        {"sheen", 1, 0.0, 0.0, 15.0, 0, "The soft edge cloth and dust have", "surface"},
        {"brush", 1, 0.0, 0.0, 3.0, 0, "Brushed along a world axis: 0 none, 1 x, 2 y, 3 z",
         "surface"},
        {"emit", 1, 0.0, 0.0, 255.0, 0, "How much light it gives off by itself", "light"},
        {"opacity", 1, 255.0, 0.0, 255.0, 0, "255 is solid, below that light passes through",
         "light"},
        {"ior", 1, 1.0, 1.0, 3.0, 2, "Refractive index: 1 no bending, 1.33 water, 1.5 glass",
         "light"},
        {"translucent", 1, 0.0, 0.0, 255.0, 0, "How far light spreads inside it before coming back",
         "light"},
        {"absorb", 3, 0.0, 0.0, 255.0, 0, "What a thick piece takes out of the light, per metre",
         "light"},
    };
    static const std::vector<ClipProperty> kVariation = {
        {"colour", 1, 0.04, 0.0, 1.0, 2, "How far each voxel's colour wanders from the type",
         "variation"},
        {"rough", 1, 0.05, 0.0, 1.0, 2, "And how far its roughness does", "variation"},
        {"seed", 1, 1.0, 0.0, 64.0, 0, "Which set of wanderings, so two clips can differ",
         "variation"},
    };
    static const std::vector<ClipProperty> kNone;
    if (head == "material") return kMaterial;
    if (head == "variation") return kVariation;
    return kNone;
}

u32 clip_wrapper_of(const ClipGraph& graph, u32 node, const std::string& head) {
    u32 at = node;
    for (u32 depth = 0; depth < 32 && at < graph.nodes.size(); ++depth) {
        if (graph.nodes[at].head == head) return at;
        if (graph.nodes[at].inputs.size() != 1) break;
        at = graph.nodes[at].inputs.front();
    }
    return ClipGraph::kNone;
}

bool wrap_clip_node(std::vector<std::string>& lines, const ClipGraph& graph, u32 node,
                    const std::string& head, const std::string& args) {
    if (node >= graph.nodes.size()) return false;
    const ClipNode& one = graph.nodes[node];
    if (!one.statement) return false;
    if (one.word_line == 0 || one.word_line > lines.size()) return false;
    if (one.end_line == 0 || one.end_line > lines.size()) return false;
    if (one.word_column > lines[one.word_line - 1].size()) return false;
    if (one.end_column > lines[one.end_line - 1].size()) return false;

    // In front of the WORD rather than in front of the statement, or `let a = box` becomes
    // `shell { let a = box } 0.05`, which is not a document.
    //
    // The tail FIRST, because inserting the head would move the end column when both are on one
    // line — which is the ordinary right-to-left rule every writer in this file follows.
    const std::string tail = " } " + args;
    lines[one.end_line - 1].insert(one.end_column, tail);
    lines[one.word_line - 1].insert(one.word_column, head + " { ");
    return true;
}

bool unwrap_clip_node(std::vector<std::string>& lines, const ClipGraph& graph, u32 wrapper) {
    if (wrapper >= graph.nodes.size()) return false;
    const ClipNode& one = graph.nodes[wrapper];
    if (one.inputs.size() != 1) return false;
    if (!one.has_block) return false;
    const ClipNode& inside = graph.nodes[one.inputs.front()];
    if (one.word_line == 0 || one.word_line > lines.size()) return false;
    if (one.end_line == 0 || one.end_line > lines.size()) return false;
    if (inside.word_line == 0 || inside.word_line > lines.size()) return false;

    // What comes off is `head {` before what is inside, and `}` plus whatever followed it after —
    // and both spans are read from the document rather than assumed, because the author's own
    // spacing is theirs (D746).
    if (one.end_line != inside.last_line) {
        // The tail is on a later line than the thing it closes. Cutting across lines here would
        // take the author's own line breaks with it, so this one is left for the script view.
        return false;
    }
    if (one.word_line != inside.word_line) return false;
    if (inside.word_column < one.word_column) return false;
    if (one.end_column > lines[one.end_line - 1].size()) return false;

    std::string& tail_line = lines[one.end_line - 1];
    const usize close = tail_line.rfind('}', one.end_column == 0 ? 0 : one.end_column - 1);
    if (close == std::string::npos || close < inside.word_column) return false;
    // From the `}` to the end of the wrapper, and any space in front of the brace.
    usize from = close;
    while (from > inside.word_column &&
           (tail_line[from - 1] == ' ' || tail_line[from - 1] == '\t')) {
        --from;
    }
    tail_line = tail_line.substr(0, from) + tail_line.substr(one.end_column);
    // And the head and its brace, off the front.
    lines[one.word_line - 1] = lines[one.word_line - 1].substr(0, one.word_column) +
                               lines[one.word_line - 1].substr(inside.word_column);
    return true;
}

const std::vector<std::string>& clip_heads_like(const std::string& head) {
    static const std::vector<std::string> kNone;
    for (const ClipPaletteGroup& group : clip_palette()) {
        for (const std::string& one : group.heads) {
            if (one != head) continue;
            // Not every group is a set of interchangeable words. `number` holds `param` beside
            // `add`, and `document` holds `metre` beside `solid`; swapping across those is not
            // changing a node's type, it is writing a different statement.
            if (group.name == "shape" || group.name == "moulding" || group.name == "join" ||
                group.name == "move" || group.name == "change" || group.name == "pattern") {
                return group.heads;
            }
            return kNone;
        }
    }
    return kNone;
}

bool write_clip_head(std::vector<std::string>& lines, const ClipNode& node,
                     const std::string& head) {
    if (head.empty() || head == node.head) return false;
    if (node.word_line == 0 || node.word_line > lines.size()) return false;
    std::string& line = lines[node.word_line - 1];
    if (node.word_column + node.head.size() > line.size()) return false;
    // Checked against what is actually there, exactly as `write_clip_number` checks a span: a
    // graph read before an edit and used after one is how a writer comes to overwrite the wrong
    // bytes (D746).
    if (line.compare(node.word_column, node.head.size(), node.head) != 0) return false;
    line.replace(node.word_column, node.head.size(), head);
    return true;
}

bool write_clip_target(std::vector<std::string>& lines, const ClipNode& node,
                       const std::string& file) {
    if (file.empty() || file.find('"') != std::string::npos) return false;
    if (node.line == 0 || node.line > lines.size()) return false;
    std::string& line = lines[node.line - 1];
    const usize open = line.find('"', node.word_column);
    if (open == std::string::npos) return false;
    const usize close = line.find('"', open + 1);
    if (close == std::string::npos) return false;
    line = line.substr(0, open + 1) + file + line.substr(close);
    return true;
}

bool erase_clip_key(std::vector<std::string>& lines, const ClipNode& node,
                    const std::string& key) {
    if (node.line == 0 || node.line > lines.size()) return false;
    std::string& line = lines[node.line - 1];
    usize code = line.size();
    bool in_string = false;
    for (usize i = 0; i < line.size(); ++i) {
        if (line[i] == '"') in_string = !in_string;
        if (line[i] == '#' && !in_string) {
            code = i;
            break;
        }
    }
    const std::string wanted = key + "=";
    usize at = 0;
    while (at + wanted.size() <= code) {
        const usize found = line.find(wanted, at);
        if (found == std::string::npos || found >= code) return false;
        const bool own_word = found == 0 || line[found - 1] == ' ' || line[found - 1] == '\t';
        if (own_word) {
            usize end = found + wanted.size();
            while (end < code && line[end] != ' ' && line[end] != '\t') ++end;
            // The space in FRONT of it goes too, or a line loses a key and keeps its gap.
            usize begin = found;
            while (begin > 0 && (line[begin - 1] == ' ' || line[begin - 1] == '\t')) --begin;
            if (begin == 0) begin = found;   // it was the first thing on the line: keep the indent
            line = line.substr(0, begin) + line.substr(end);
            return true;
        }
        at = found + 1;
    }
    return false;
}

bool clip_default_number(const std::string& head, const std::string& key, u32 index, f64& value) {
    const std::string body = clip_node_template(head);
    if (body.empty()) return false;
    // Read as a document rather than picked apart by hand, so a template and the thing it makes are
    // read by exactly the same code and cannot drift.
    const std::vector<std::string> lines{"let it = " + body};
    const ClipGraph graph = read_clip_graph(lines);
    for (const ClipNode& node : graph.nodes) {
        if (node.head != head) continue;
        for (const ClipNumber& number : node.numbers) {
            if (number.key != key || number.index != index) continue;
            value = number.value;
            return true;
        }
    }
    return false;
}

bool write_clip_key(std::vector<std::string>& lines, const ClipNode& node, const std::string& key,
                    const std::string& value) {
    if (node.line == 0 || node.line > lines.size()) return false;
    std::string& line = lines[node.line - 1];

    // Where the statement ends and its comment begins, because a key written after a `#` is a key
    // written into a comment — and the `#@` marker that carries the layout is one of those.
    usize code = line.size();
    bool in_string = false;
    for (usize i = 0; i < line.size(); ++i) {
        if (line[i] == '"') in_string = !in_string;
        if (line[i] == '#' && !in_string) {
            code = i;
            break;
        }
    }

    // Already there: replace what it holds and nothing else on the line.
    const std::string wanted = key + "=";
    usize at = 0;
    while (at + wanted.size() <= code) {
        const usize found = line.find(wanted, at);
        if (found == std::string::npos || found >= code) break;
        const bool own_word = found == 0 || line[found - 1] == ' ' || line[found - 1] == '\t';
        if (own_word) {
            usize end = found + wanted.size();
            while (end < code && line[end] != ' ' && line[end] != '\t') ++end;
            line = line.substr(0, found + wanted.size()) + value + line.substr(end);
            return true;
        }
        at = found + 1;
    }

    // Not there: at the end of the statement, before whatever comment follows it.
    usize tail = code;
    while (tail > 0 && (line[tail - 1] == ' ' || line[tail - 1] == '\t')) --tail;
    line = line.substr(0, tail) + " " + key + "=" + value + line.substr(tail);
    return true;
}

const std::vector<ClipPaletteGroup>& clip_palette() {
    // Grouped, because the vocabulary is eighty words and a list of eighty is a list nobody reads.
    // These are `20-clip-forge.md` §2's own groupings, and **everything the language has is in one
    // of them** — the first pass offered thirty-one and the rest were reachable only by typing,
    // which is a palette that teaches a player the language is smaller than it is.
    static const std::vector<ClipPaletteGroup> kGroups = {
        {"shape",
         {"box", "sphere", "cylinder", "capsule", "cone", "torus", "arc", "prism", "plane", "tetra",
          "cube", "octa", "dodeca", "icosa", "wedge", "stairs", "spiral", "branch"}},
        {"moulding",
         {"fillet", "ovolo", "cavetto", "bead", "astragal", "scotia", "cyma", "cyma_reversa"}},
        {"join", {"union", "difference", "intersection"}},
        {"move",
         {"translate", "rotate", "scale", "mirror", "repeat", "around", "scatter", "twist", "bend"}},
        {"change", {"shell", "round", "offset", "displace", "revolve"}},
        {"pattern",
         {"fbm", "noise", "ridged", "rasp", "cells", "cell_edge", "sine", "waves", "checker",
          "stripes", "bricks", "axis", "distance"}},
        {"number",
         {"param", "constant", "add", "multiply", "min", "max", "blend", "remap", "abs", "negate",
          "step", "smoothstep", "clamp", "power", "occlusion", "curvature", "facing"}},
        // Material and paint are not "the document", they are what the thing is MADE of, and a
        // player looking for a colour was looking under a word that means the file's own settings.
        // Reported directly: *many nodes are inside the document category which makes no sense.*
        {"voxel type", {"material", "paint"}},
        {"finish", {"weather", "variation"}},
        {"document", {"metre", "bounds", "origin", "solid", "region"}},
    };
    return kGroups;
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
