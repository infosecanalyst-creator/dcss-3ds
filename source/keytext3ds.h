// keytext3ds.h -- the 3DS wording for keys named in Crawl's hint, tutorial
// and help text.
//
// Commands are already named by command_to_string() (patched in macro.cc):
// a bottom-screen button as "[items]", a hardware button as "A" / "B",
// anything else as "[keys] x" (the keys button, then that key). This filter
// fixes what the text names literally: Enter, Escape, Space, the number pad
// and vi-keys tables, Shift-running and single keys like <w>?</w>.
//
// Plain std::string so it is tested on a desktop (tools/test_keytext.cpp).
#pragma once
#include <string>
#include <vector>

namespace keytext3ds {

inline void replace_all_in(std::string &s, const std::string &a, const std::string &b)
{
    if (a.empty()) return;
    for (size_t p = s.find(a); p != std::string::npos; p = s.find(a, p + b.size()))
        s.replace(p, a.size(), b);
}

inline std::string lower(std::string s)
{
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

// What a key named in the text is on the 3DS ("" = leave it alone).
inline std::string map_key(const std::string &raw)
{
    const std::string k = lower(raw);
    if (k == "enter" || k == "return") return "A";
    if (k == "escape" || k == "esc") return "B";
    if (k == "space") return "A";
    if (k == "tab") return "[fight]";
    if (k == "arrow keys" || k == "number pad" || k == "numpad" || k == "vi keys" || k == "vi\nkeys"
        || k == "vi-keys")
        return "Circle Pad";
    if (k == "shift-direction" || k == "shift + direction" || k == "/ direction")
        return "[keys] / + D-Pad";
    if (k == "shift-tab") return "[keys] p";
    if (k == "shift-numpad 5" || k == "shift-numpad-5" || k == "5") return "[rest]";
    if (k == "shift") return "[keys] Shift";
    return "";
}

inline bool is_single_key(const std::string &k)
{
    return k.size() == 1 || k == "<<";
}

// Lines of the numpad / vi-keys movement diagrams.
inline bool diagram_line(const std::string &l)
{
    return l.find("vi-keys") != std::string::npos || l.find("Numpad:") != std::string::npos
        || l.find("\\|/") != std::string::npos || l.find("/|\\") != std::string::npos
        || l.find("<w>4</w>-<w>.</w>-<w>6</w>") != std::string::npos
        || l.find("<w>1 2 3</w>") != std::string::npos;
}

// Before $cmd[...] is expanded: commands used as the first letter of a word
// ("<w>$cmd[CMD_FIRE]</w>ired") become the plain word.
inline void fix_before_commands(std::string &s)
{
    replace_all_in(s, "<w>$cmd[CMD_WEAR_JEWELLERY]</w>ut", "put");
    replace_all_in(s, "<w>$cmd[CMD_FIRE]</w>ired", "fired");
    replace_all_in(s, "To use your abilities press <w>a</w>.", "To use your abilities press <w>$cmd[CMD_USE_ABILITY]</w>.");
    replace_all_in(s, "<w>5</w> or <w>Shift-numpad 5</w>", "<w>$cmd[CMD_REST]</w>");
    replace_all_in(s, " or <w>shift-numpad-5</w>", "");
}

inline void fix(std::string &s)
{
    // Phrases first.
    replace_all_in(s, "an uppercase <w>[keys] Y</w>", "the <w>Yes</w> button");
    replace_all_in(s, "an uppercase <w>[Y]</w>", "the <w>Yes</w> button");
    replace_all_in(s, "<w>[pick up]</w> or <w>[g]</w>", "<w>[pick up]</w>");
    replace_all_in(s, " (try Numlock on/off)", "");
    replace_all_in(s, "<w>[shift-tab]</w> or <w>[p]</w>", "<w>[keys] p</w>");

    // Movement diagrams: one line instead.
    {
        std::string out, line;
        bool replaced = false;
        size_t i = 0;
        while (i <= s.size())
        {
            size_t e = s.find('\n', i);
            if (e == std::string::npos) e = s.size();
            line = s.substr(i, e - i);
            if (diagram_line(line))
            {
                if (!replaced)
                    out += "  <w>Circle Pad</w>  move, diagonals too.  <w>D-Pad</w>  move four ways.\n";
                replaced = true;
            }
            else
                out += line + (e < s.size() ? "\n" : "");
            i = e + 1;
        }
        s = out;
    }

    // <w>X</w> and <w>[X]</w> naming a key.
    size_t p = 0;
    while ((p = s.find("<w>", p)) != std::string::npos)
    {
        const size_t q = s.find("</w>", p + 3);
        if (q == std::string::npos) break;
        std::string inner = s.substr(p + 3, q - p - 3);
        const bool bracket = inner.size() >= 2 && inner.front() == '[' && inner.back() == ']';
        std::string key = bracket ? inner.substr(1, inner.size() - 2) : inner;
        const std::string after = s.substr(q + 4, 8);
        const bool mnemonic = !after.empty() && isalpha((unsigned char)after[0]);   // <w>i</w>nventory
        const bool symbol = s.substr(q + 4, 30).find("symbol") != std::string::npos; // the <w>^</w> symbol
        std::string rep;
        if (key.compare(0, 6, "[keys]") == 0 || key == "[items]" || key == "A" || key == "B")
            rep = "";
        else if (!(rep = map_key(key)).empty())
            ;
        else if (mnemonic && key.size() == 1)
        {
            s.replace(p, q + 4 - p, key);      // just the letter, no key hint
            p += key.size();
            continue;
        }
        else if (is_single_key(key) && !symbol)
            rep = "[keys] " + key;
        if (!rep.empty())
        {
            if (rep[0] != '[' && bracket) rep = "[" + rep + "]";
            const std::string w = "<w>" + rep + "</w>";
            s.replace(p, q + 4 - p, w);
            p += w.size();
        }
        else
            p = q + 4;
    }

    // two commands in a row: "[keys] x[keys] v" -> "[keys] x then v"
    for (size_t k = s.find("[keys] ", 1); k != std::string::npos; k = s.find("[keys] ", k + 1))
    {
        const char c = s[k - 1];
        if (c != ' ' && c != '>' && c != '(' && c != '[' && c != '\n' && c != '\'')
            s.replace(k, 7, " then ");
    }

    // "the Circle Pad (...) or the Circle Pad" after the diagram text
    replace_all_in(s, "<w>[Circle Pad]</w>, or the <w>[Circle Pad]</w>", "<w>Circle Pad</w>");
    replace_all_in(s, "<w>[Circle Pad]</w> or the <w>[Circle Pad]</w>.", "<w>Circle Pad</w>.");
    replace_all_in(s, "<w>[Circle Pad]</w> or <w>[Circle Pad]</w>", "<w>Circle Pad</w>");
    replace_all_in(s, "<w>[keys] Shift</w>\nalong with the <w>[Circle Pad]</w> or <w>[Circle Pad]</w>",
                   "<w>[keys] /</w> and then a direction");
    replace_all_in(s, "<w>[keys] Shift</w>\nalong with the <w>Circle Pad</w>",
                   "<w>[keys] /</w> and then a direction");
}

} // namespace keytext3ds
