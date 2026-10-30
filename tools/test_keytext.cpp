// Desktop check of source/keytext3ds.h on the real hint and tutorial texts:
//   g++ -std=c++11 -Isource tools/test_keytext.cpp -o /tmp/tk && /tmp/tk <crawl-ref/source>/dat/descript/hints.txt ...
// Prints every processed entry; review for keys the 3DS does not have.
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include "keytext3ds.h"

static std::string cmd3ds(const std::string &c)
{
    static const std::map<std::string, std::string> m = {
        {"CMD_DISPLAY_INVENTORY","[items]"},{"CMD_CAST_SPELL","[cast]"},{"CMD_USE_ABILITY","[abil]"},
        {"CMD_DISPLAY_MAP","[map]"},{"CMD_DISPLAY_CHARACTER_STATUS","[char]"},{"CMD_REPLAY_MESSAGES","[msgs]"},
        {"CMD_EXPLORE","[explr]"},{"CMD_AUTOFIGHT","[fight]"},{"CMD_REST","[rest]"},{"CMD_WAIT","[wait]"},
        {"CMD_PICKUP","[pick up]"},{"CMD_GO_UPSTAIRS","[up]"},{"CMD_GO_DOWNSTAIRS","[down]"},
        {"CMD_EQUIP","[items]"},{"CMD_READ","[items]"},{"CMD_QUAFF","[items]"},{"CMD_DROP","[items]"},
        {"CMD_EVOKE","[items]"},{"CMD_WIELD_WEAPON","[items]"},{"CMD_WEAR_ARMOUR","[items]"},
    };
    auto it = m.find(c);
    if (it != m.end()) return it->second;
    if (c.compare(0, 8, "CMD_MOVE") == 0) return "Circle Pad";
    return "[keys] " + c.substr(4, 3);
}

static std::string untag(std::string s)
{
    // drop <tiles>/<localtiles>/<webtiles> blocks, keep the rest
    for (const char *t : {"localtiles", "webtiles", "tiles"})
    {
        std::string o = std::string("<") + t + ">", c = std::string("</") + t + ">";
        size_t p;
        while ((p = s.find(o)) != std::string::npos)
        {
            size_t q = s.find(c, p);
            if (q == std::string::npos) break;
            s.erase(p, q + c.size() - p);
        }
    }
    for (const char *t : {"<console>", "</console>", "<nomouse>", "</nomouse>"})
        keytext3ds::replace_all_in(s, t, "");
    return s;
}

int main(int argc, char **argv)
{
    for (int a = 1; a < argc; ++a)
    {
        std::ifstream f(argv[a]);
        std::stringstream ss; ss << f.rdbuf();
        std::string all = ss.str(), e;
        size_t i = 0;
        while (i < all.size())
        {
            size_t j = all.find("%%%%", i);
            if (j == std::string::npos) j = all.size();
            e = untag(all.substr(i, j - i));
            keytext3ds::fix_before_commands(e);
            size_t p;
            while ((p = e.find("$cmd[")) != std::string::npos)
            {
                size_t q = e.find(']', p);
                e.replace(p, q - p + 1, cmd3ds(e.substr(p + 5, q - p - 5)));
            }
            while ((p = e.find("<input>")) != std::string::npos)
            {
                size_t q = e.find("</input>", p);
                std::string in = e.substr(p + 7, q - p - 7);
                in = (!in.empty() && in[0] == '[') ? "<w>" + in + "</w>" : "<w>[" + in + "]</w>";
                e.replace(p, q - p + 8, in);
            }
            keytext3ds::fix(e);
            printf("%s\n----\n", e.c_str());
            i = j + 4;
        }
    }
}
