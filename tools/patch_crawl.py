#!/usr/bin/env python3
"""Apply the 3DS port modifications to a crawl checkout.

Usage: patch_crawl.py <path to crawl-ref/source>

Idempotent: each edit is skipped when its marker is already present.
Edits are anchored on short, stable upstream lines so they survive
crawl's constant churn better than a context diff would.  If an anchor
is missing, the script stops and tells you which one, so the edit can be
redone by hand (they are all a few lines).
"""
import os
import shutil
import sys

SRC = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.join(HERE, "..", "source")


def rw(fname, old, new, marker):
    """Replace `old` with `new` in fname unless `marker` is already there."""
    path = os.path.join(SRC, fname)
    text = open(path, encoding="utf-8").read()
    if marker in text:
        print("  = %-14s already patched (%s)" % (fname, marker.strip()[:40]))
        return
    if old not in text:
        raise SystemExit("ANCHOR NOT FOUND in %s -- edit by hand:\n-----\n%s\n-----"
                         % (fname, old))
    assert marker in new, "marker must appear in replacement text"
    open(path, "w", encoding="utf-8").write(text.replace(old, new, 1))
    print("  + %-14s %s" % (fname, marker.strip()[:40]))


# ------------------------------------------------------------- platform.h
# (checked first, so the desktop test harness -- a Linux build with
#  -D__3DS__ -- also takes the 3DS paths)
rw("platform.h",
   "* OS DETECTION *\n* ------------ */\n\n",
   "* OS DETECTION *\n* ------------ */\n\n"
   "/* 3DS before the hosts */\n#if !defined (OS_DETECTED)\n#if defined (__3DS__)\n"
   "#define OS_DETECTED\n#define TARGET_OS_3DS\n#endif\n#endif\n\n",
   "/* 3DS before the hosts */")

# --------------------------------------------------------------- unicode.h
# newlib (with _GNU_SOURCE) declares wcwidth(wchar_t); wchar_t is unsigned int
# on arm-none-eabi, so crawl's wcwidth(char32_t) called with an int is
# ambiguous. Forward the int case explicitly.
rw("unicode.h",
   "#ifndef UNIX\nint wcwidth(char32_t c);\n#endif\n",
   "#ifndef UNIX\nint wcwidth(char32_t c);\n#endif\n"
   "#ifdef TARGET_OS_3DS\nstatic inline int wcwidth(int c) { return wcwidth((char32_t)c); }\n#endif\n",
   "wcwidth(int c)")

# ----------------------------------------------------------------- tags.cc
# int32_t is 'long' on arm-none-eabi; the conduct maps are map<..., int>, so
# the by-reference int marshaller must take int for template deduction.
rw("tags.cc",
   "static void marshallIntReference(writer &th, const int32_t &data)",
   "static void marshallIntReference(writer &th, const int &data) // int, not int32_t: see dcss-3ds",
   "const int &data) // int, not int32_t")

# ----------------------------------------------------------- syscalls.cc
rw("syscalls.cc",
   '#include "unicode.h"\n',
   '#include "unicode.h"\n#ifdef TARGET_OS_3DS\n# include <3ds.h> // PS_GenerateRandomBytes, svcGetSystemTick\n#endif\n',
   "# include <3ds.h> // PS_GenerateRandomBytes")
# No /dev/urandom on the 3DS: seed from the PS service's hardware RNG, with a
# tick-counter mix as a fallback so the game can never fail to start.
rw("syscalls.cc",
   "#else\n    /* Try opening from various system provided (hopefully) CSPRNGs */\n",
   "#elif defined(TARGET_OS_3DS)\n"
   "    bool ok = false;\n"
   "    if (R_SUCCEEDED(psInit()))\n"
   "    {\n"
   "        ok = R_SUCCEEDED(PS_GenerateRandomBytes(buf, len));\n"
   "        psExit();\n"
   "    }\n"
   "    if (!ok)\n"
   "    {\n"
   "        uint64_t x = svcGetSystemTick() ^ (osGetTime() << 21) ^ 0x9E3779B97F4A7C15ULL;\n"
   "        for (int i = 0; i < len; ++i)\n"
   "        {\n"
   "            x ^= x << 13; x ^= x >> 7; x ^= x << 17;\n"
   "            buf[i] = (char)(x >> 24);\n"
   "        }\n"
   "    }\n"
   "    return true;\n"
   "#else\n    /* Try opening from various system provided (hopefully) CSPRNGs */\n",
   "PS_GenerateRandomBytes(buf, len)")

# ------------------------------------------------------------------- end.cc
# Show fatal errors on the 3DS and wait for START (otherwise they flash past).
rw("end.cc",
   "        databaseSystemShutdown();\n",
   "        databaseSystemShutdown();\n"
   "#ifdef TARGET_OS_3DS\n"
   "        extern void lib3ds_fatal_wait(const char *msg);\n"
   "        if (exit_code)\n"
   "            lib3ds_fatal_wait(error.c_str());\n"
   "#endif\n",
   "lib3ds_fatal_wait(error.c_str())")

# --------------------------------------------------------------- AppHdr.h
rw("AppHdr.h",
   "#elif defined(TARGET_OS_WINDOWS)\n    #if defined(TARGET_COMPILER_MINGW)",
   "#elif defined(TARGET_OS_3DS)\n"
   "    // Nintendo 3DS (devkitARM + libctru). Not UNIX: no signals, no\n"
   "    // curses, no file locking; PCRE comes from contrib.\n"
   "    #define FILE_SEPARATOR '/'\n"
   "    #ifndef REGEX_PCRE\n    #define REGEX_PCRE\n    #endif\n"
   "    #include \"lib3ds.h\"\n"
   "#elif defined(TARGET_OS_WINDOWS)\n    #if defined(TARGET_COMPILER_MINGW)",
   "#elif defined(TARGET_OS_3DS)")

# ------------------------------------------------------------ syscalls.cc
rw("syscalls.cc",
   "bool lock_file(int fd, bool write, bool wait)\n{",
   "#ifdef TARGET_OS_3DS\n"
   "// Single-process platform: file locking and fdatasync are no-ops.\n"
   "bool lock_file(int, bool, bool) { return true; }\n"
   "bool unlock_file(int) { return true; }\n"
   "int fdatasync(int) { return 0; }\n"
   "#else // !TARGET_OS_3DS\n\n"
   "bool lock_file(int fd, bool write, bool wait)\n{",
   "#else // !TARGET_OS_3DS")

rw("syscalls.cc",
   "# ifndef CRAWL_HAVE_FDATASYNC\n// At least MacOS X",
   "# if !defined(CRAWL_HAVE_FDATASYNC) && !defined(TARGET_OS_3DS)\n// At least MacOS X",
   "!defined(CRAWL_HAVE_FDATASYNC) && !defined(TARGET_OS_3DS)")

path = os.path.join(SRC, "syscalls.cc")
t = open(path, encoding="utf-8").read()
if "#endif // !TARGET_OS_3DS" not in t:
    i = t.find("bool unlock_file(int fd)\n{")
    assert i > 0, "unlock_file anchor not found"
    end = t.find("\n}\n", i) + 3
    t = t[:end] + "#endif // !TARGET_OS_3DS\n" + t[end:]
    open(path, "w", encoding="utf-8").write(t)
    print("  + syscalls.cc    #endif after unlock_file")
else:
    print("  = syscalls.cc    #endif already present")

# ---------------------------------------------------------------- main.cc
rw("main.cc",
   "#ifndef TARGET_OS_WINDOWS\n# include <langinfo.h>\n#endif",
   "#if !defined(TARGET_OS_WINDOWS) && !defined(TARGET_OS_3DS)\n"
   "# include <langinfo.h>\n#endif",
   "!defined(TARGET_OS_3DS)\n# include <langinfo.h>")

rw("main.cc",
   "int main(int argc, char *argv[])\n{\n",
   "int main(int argc, char *argv[])\n{\n"
   "#ifdef TARGET_OS_3DS\n    lib3ds_early_init();\n#endif\n",
   "lib3ds_early_init();")

# --------------------------------------------------------------- crash.cc
rw("crash.cc",
   "#ifndef TARGET_OS_WINDOWS\n# include <cerrno>\n# include <sys/wait.h>\n#endif",
   "#ifndef TARGET_OS_WINDOWS\n# include <cerrno>\n"
   "# ifndef TARGET_OS_3DS\n# include <sys/wait.h>\n# endif\n#endif",
   "# ifndef TARGET_OS_3DS\n# include <sys/wait.h>")

rw("crash.cc",
   '#include "crash.h"\n',
   '#include "crash.h"\n#ifdef TARGET_OS_3DS\n#define alarm(seconds) ((void)0) // no SIGALRM on the 3DS\n#endif\n',
   "#define alarm(seconds) ((void)0)")

rw("crash.cc",
   "#ifndef TARGET_OS_WINDOWS\n    if (crawl_state.no_gdb)",
   "#if !defined(TARGET_OS_WINDOWS) && !defined(TARGET_OS_3DS) // no fork/gdb\n    if (crawl_state.no_gdb)",
   "!defined(TARGET_OS_3DS) // no fork/gdb")

# -------------------------------------------------------------- random.cc
rw("random.cc",
   "#if defined(UNIX) || defined(TARGET_COMPILER_MINGW)\n# include <unistd.h>\n",
   "#if defined(UNIX) || defined(TARGET_COMPILER_MINGW) || defined(TARGET_OS_3DS)\n# include <unistd.h>\n",
   "defined(TARGET_COMPILER_MINGW) || defined(TARGET_OS_3DS)\n# include <unistd.h>")

# --------------------------------------------------------------- Makefile
block = open(os.path.join(PORT, "Makefile.3ds-block")).read()
rw("Makefile",
   "include Makefile.obj\n",
   block + "include Makefile.obj\n",
   "ifdef TARGET_3DS")
# keep an already-inserted block current
_mk = os.path.join(SRC, "Makefile")
_t = open(_mk, encoding="utf-8").read()
_a = _t.find("#\n# Nintendo 3DS cross build")
_b = _t.find("include Makefile.obj\n")
if 0 <= _a < _b and _t[_a:_b] != block:
    open(_mk, "w", encoding="utf-8").write(_t[:_a] + block + _t[_b:])
    print("  + Makefile       3DS build block refreshed")

rw("Makefile",
   "ifndef TILES\nifdef NEED_LIBW32C\nOBJECTS += libw32c.o\nelse\nOBJECTS += libunix.o\nendif\nendif",
   "ifndef TILES\nifdef TARGET_3DS\nOBJECTS += lib3ds.o\nelse\n"
   "ifdef NEED_LIBW32C\nOBJECTS += libw32c.o\nelse\nOBJECTS += libunix.o\nendif\nendif\nendif",
   "OBJECTS += lib3ds.o")

rw("Makefile",
   "else\nifndef ANDROID\nifneq ($(uname_S),Haiku)\nEXTRA_LIBS += -pthread\nendif\nendif\nendif",
   "else\nifndef ANDROID\nifndef TARGET_3DS\nifneq ($(uname_S),Haiku)\n"
   "EXTRA_LIBS += -pthread\nendif\nendif\nendif\nendif",
   "ifndef TARGET_3DS\nifneq ($(uname_S),Haiku)")

rw("Makefile",
   "\tifneq (,$(findstring mingw,$(CROSSHOST)))\n\t\tuname_S=MINGW32\n\tendif\n",
   "\tifneq (,$(findstring mingw,$(CROSSHOST)))\n\t\tuname_S=MINGW32\n\tendif\n"
   "\tifdef TARGET_3DS\n\t\tuname_S=3DS\n\tendif\n",
   "uname_S=3DS")

# sqlite: no fcntl() file locking on the 3DS, use the lock-free unix VFS
rw("sqldbm.cc",
   "    if (ec(sqlite3_open_v2(\n                dbfile.c_str(), &db,\n"
   "                readonly ? SQLITE_OPEN_READONLY :\n"
   "                (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE),\n"
   "                nullptr\n",
   "    if (ec(sqlite3_open_v2(\n                dbfile.c_str(), &db,\n"
   "                readonly ? SQLITE_OPEN_READONLY :\n"
   "                (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE),\n"
   "#ifdef TARGET_OS_3DS\n                \"unix-none\" // no fcntl locks on the 3DS\n#else\n"
   "                nullptr\n#endif\n",
   "unix-none")

# sqlite: device paths ("sdmc:/...") are absolute; without this SQLite glues the
# cwd in front ("sdmc:/3ds/crawl//sdmc:/3ds/...") and every DB open fails
rw("contrib/sqlite/sqlite3.c",
   "  zOut[nOut-1] = '\\0';\n  if( zPath[0]=='/' ){\n",
   "  zOut[nOut-1] = '\\0';\n  /* 3DS: \"sdmc:/...\" is absolute too -- don't prepend the cwd to it */\n"
   "  if( zPath[0]=='/' || strstr(zPath, \":/\")!=0 ){\n",
   "is absolute too")

# --- DB_DIAG_3DS: robust, explained DB creation on the 3DS ---------------------
rw("contrib/sqlite/Makefile",
   "\t\t -DSQLITE_OMIT_PAGER_PRAGMAS \\\n",
   "\t\t -DSQLITE_DISABLE_DIRSYNC \\\n",
   "SQLITE_DISABLE_DIRSYNC")
rw("sqldbm.cc",
   "    init_schema();\n    return errc;",
   "#ifdef TARGET_OS_3DS\n"
   "    // 3DS: these DBs are caches rebuilt from text -- journal in RAM, no fsync\n"
   "    sqlite3_exec(db, \"PRAGMA journal_mode=MEMORY; PRAGMA synchronous=OFF;\",\n"
   "                 nullptr, nullptr, nullptr);\n"
   "#endif\n"
   "    init_schema();\n    return errc;",
   "journal in RAM, no fsync")
rw("sqldbm.cc", "#include \"syscalls.h\"\n",
   "#include \"syscalls.h\"\n\nextern string dbm_last_error;\n", "extern string dbm_last_error;")
rw("sqldbm.cc",
   "        if (!readonly)\n            sqlite3_exec(db, \"COMMIT;\", nullptr, nullptr, nullptr);\n",
   "        if (!readonly && sqlite3_exec(db, \"COMMIT;\", nullptr, nullptr, nullptr) != SQLITE_OK)\n"
   "            dbm_last_error = string(\"commit failed: \") + sqlite3_errmsg(db);\n",
   "commit failed: ")
rw("sqldbm.cc",
   "SQL_DBM *dbm_open(const char *filename, int mode, int)\n{\n    SQL_DBM *n = new SQL_DBM(filename, mode == O_RDONLY, true);\n    if (!n->is_open())\n    {\n",
   "string dbm_last_error;\n\n"
   "SQL_DBM *dbm_open(const char *filename, int mode, int)\n{\n    SQL_DBM *n = new SQL_DBM(filename, mode == O_RDONLY, true);\n    if (!n->is_open())\n    {\n"
   "        dbm_last_error = (n->error.empty() ? string(\"open failed\") : n->error)\n"
   "                         + \" (sqlite \" + to_string(n->errc) + \")\";\n",
   "string dbm_last_error;\n\nSQL_DBM")
rw("database.cc", "#include \"unicode.h\"\n",
   "#include \"unicode.h\"\n\nextern string dbm_last_error;   // sqldbm.cc: reason for the last failed open\n",
   "extern string dbm_last_error;")
rw("database.cc",
   "    if (timestamp.empty())\n        return false;\n",
   "    if (timestamp.empty())\n    {\n        dbm_last_error = string(\"opened, but no TIMESTAMP row\")\n"
   "                         + (dbm_last_error.empty() ? string() : \"; \" + dbm_last_error);\n        return false;\n    }\n",
   "no TIMESTAMP row")
rw("database.cc",
   "    if (!_needs_update())\n        return;\n    _regenerate_db();\n",
   "    if (!_needs_update())\n        return;\n    dbm_last_error.clear();\n    _regenerate_db();\n",
   "dbm_last_error.clear();")
rw("database.cc",
   "        end(1, true, \"Failed to open DB: %s\",\n            _db_cache_path(_db_name, lang()).c_str());",
   "        end(1, false, \"Failed to open DB: %s\\n%s\",\n            _db_cache_path(_db_name, lang()).c_str(), dbm_last_error.c_str());",
   "dbm_last_error.c_str()")

# --- PROGRESS_3DS: show first-run DB / map-cache building on the 3DS console ---
for _f in ("database.cc", "maps.cc"):
    _p = os.path.join(SRC, _f)
    _t = open(_p, encoding="utf-8").read()
    _o = "#if defined(DEBUG_DIAGNOSTICS) && !(defined(TARGET_COMPILER_VC) && defined(USE_TILE))\n    printf(\"Regenerating"
    _n = "#if (defined(DEBUG_DIAGNOSTICS) && !(defined(TARGET_COMPILER_VC) && defined(USE_TILE))) || defined(TARGET_OS_3DS)\n    printf(\"Regenerating"
    if _o in _t:
        open(_p, "w", encoding="utf-8").write(_t.replace(_o, _n))
        print("  + %-14s first-run progress on the console" % _f)

# 3DS: the tile *indices* are all the frontend needs from rltiles (the art
# comes from the prebuilt atlas, 3ds/tiles16.bin), so keep generating the
# tile headers the console way -- no libpng / real PNGs needed on the host.
_mk = os.path.join(SRC, "Makefile")
_t = open(_mk, encoding="utf-8").read()
if "$(if $(TARGET_3DS),,$(WEBTILES))" not in _t:
    _t = _t.replace("TILES=$(TILES)$(WEBTILES)", "TILES=$(TILES)$(if $(TARGET_3DS),,$(WEBTILES))")
    open(_mk, "w", encoding="utf-8").write(_t)
    print("  + Makefile       rltiles: indices only on 3DS")

# webtiles' UTF-8 locale check (nl_langinfo) -- not on the 3DS
rw("main.cc",
   "#ifdef USE_TILE_WEB\n    if (strcasecmp(nl_langinfo(CODESET), \"UTF-8\"))",
   "#if defined(USE_TILE_WEB) && !defined(TARGET_OS_3DS)\n    if (strcasecmp(nl_langinfo(CODESET), \"UTF-8\"))",
   "defined(USE_TILE_WEB) && !defined(TARGET_OS_3DS)\n    if (strcasecmp")

# webtiles normally builds contrib libpng for the tile sheet generator; the
# 3DS build only needs tile indices (see above), so skip it
rw("Makefile",
   "ifdef NO_PKGCONFIG\nBUILD_LIBPNG = YesPlease # used by build-rltiles sub-make\nendif\n",
   "ifdef NO_PKGCONFIG\nifndef TARGET_3DS\nBUILD_LIBPNG = YesPlease # used by build-rltiles sub-make\nendif\nendif\n",
   "ifndef TARGET_3DS\nBUILD_LIBPNG")

# ...and the game doesn't need the webtiles client's tile PNGs either
rw("Makefile",
   "ifdef TILES_ANY\ninstall-data: $(DESTTILEFILES)\n$(GAME): $(DESTTILEFILES)\nendif\n",
   "ifdef TILES_ANY\nifndef TARGET_3DS\ninstall-data: $(DESTTILEFILES)\n$(GAME): $(DESTTILEFILES)\nendif\nendif\n",
   "ifndef TARGET_3DS\ninstall-data: $(DESTTILEFILES)")

path = os.path.join(SRC, "Makefile")
t = open(path, encoding="utf-8").read()
rules = open(os.path.join(PORT, "Makefile.3ds-rules")).read()
if "crawl.3dsx:" not in t:
    t += "\n" + rules
    open(path, "w", encoding="utf-8").write(t)
    print("  + Makefile       3dsx/cia rules appended")
else:
    _a = t.find("#############################################################################\n# Nintendo 3DS packaging")
    _b = t.find("endif # TARGET_3DS\n", _a)
    if 0 <= _a < _b and t[_a:_b + len("endif # TARGET_3DS\n")] != rules:
        t = t[:_a] + rules + t[_b + len("endif # TARGET_3DS\n"):]
        open(path, "w", encoding="utf-8").write(t)
        print("  + Makefile       3dsx rules refreshed")
    else:
        print("  = Makefile       3dsx rules already present")



# -------------------------------------------------------- romfs timestamps
# libctru's romfs reports every file's mtime as the moment romfsInit() ran,
# so crawl saw all of dat/ as "changed" on every launch and rebuilt its
# databases and level cache each time (minutes). Give romfs files one stable
# time instead: the installed .3dsx's own mtime (changes only on an update).
rw("files.cc",
   "time_t file_modtime(const string &file)\n{\n    struct stat filestat;\n",
   "#if defined(__3DS__) && !defined(__3DS_HARNESS__)\ntime_t lib3ds_romfs_mtime();\n#endif\n"
   "time_t file_modtime(const string &file)\n{\n"
   "#if defined(__3DS__) && !defined(__3DS_HARNESS__)\n"
   "    if (file.compare(0, 6, \"romfs:\") == 0)\n        return lib3ds_romfs_mtime(); // romfs mtime = launch time otherwise\n#endif\n"
   "    struct stat filestat;\n",
   "lib3ds_romfs_mtime(); // romfs mtime")


# -------------------------------------------- portable des cache (harness)
# The desktop harness can prebuild the level cache for the 3DS, but compiled
# Lua bytecode isn't portable (x86-64 size_t != ARM). Harness builds write
# the chunk's source text instead; the 3DS compiles it when the map is used.
rw("dlua.cc",
   "    if (!compiled.empty())\n    {\n        marshallByte(outf, CT_COMPILED);\n",
   "#ifdef __3DS_HARNESS__\n    if (!chunk.empty())   // portable cache: source, not x86 bytecode\n    {\n"
   "        marshallByte(outf, CT_SOURCE);\n        marshallString4(outf, chunk);\n    }\n    else\n#endif\n"
   "    if (!compiled.empty())\n    {\n        marshallByte(outf, CT_COMPILED);\n",
   "portable cache: source, not x86 bytecode")

rw("maps.cc",
   "    time_t mtime = file_modtime(dat);\n    _reset_map_parser();\n",
   "    time_t mtime = file_modtime(s); // by path, like the check (3DS: romfs stamp)\n    _reset_map_parser();\n",
   "file_modtime(s); // by path")


# -------------------------------------------------- messages for the 3DS top
# The 3DS dashboard shows messages in a bigger font (66 columns). Re-wrapping
# crawl's already-wrapped 80-column lines left ragged fragments, so hand the
# frontend whole messages (with colours) and let it wrap them once.
rw("message.cc",
   "string get_last_messages(int mcount, bool full)\n{",
   "#ifdef __3DS__\n// 3DS frontend: the newest messages, oldest first, with their colours\n"
   "vector<pair<formatted_string, int>> lib3ds_recent_messages(int mcount)\n{\n"
   "    vector<pair<formatted_string, int>> out;\n"
   "    const store_t& msgs = buffer.get_store();\n"
   "    mcount = min(mcount, NUM_STORED_MESSAGES);\n"
   "    for (int i = -1; (int)out.size() < mcount; --i)\n    {\n"
   "        const message_line msg = msgs[i];\n        if (!msg)\n            break;\n"
   "        out.insert(out.begin(), make_pair(formatted_string::parse_string(msg.full_text(),\n"
   "                       channel_to_colour(msg.channel, msg.param)), msg.turn));\n    }\n"
   "    return out;\n}\n#endif\n\n"
   "string get_last_messages(int mcount, bool full)\n{",
   "lib3ds_recent_messages(int mcount)")

# -------------------------------------------------------------- sound (3DS)
# A third sound backend next to WinMM / external command / SDL_mixer:
# lib3ds_play_sound() plays raw PCM through ndsp. The CC0 crawl-sounds pack's
# rules ship as dat/defaults/sounds3ds.txt, loaded with the other defaults.
rw("sound.h",
   "#if defined(WINMM_PLAY_SOUNDS)\n #define SOUND_BACKEND",
   "#if defined(SOUND_PLAY_3DS)\n #define SOUND_BACKEND \"Sound support (3DS)\"\n"
   "void lib3ds_play_sound(const char *file);\n#elif defined(WINMM_PLAY_SOUNDS)\n #define SOUND_BACKEND",
   "Sound support (3DS)")
rw("sound.cc",
   "    if (Options.sounds_on)\n    {\n#if defined(WINMM_PLAY_SOUNDS)\n",
   "    if (Options.sounds_on)\n    {\n#if defined(SOUND_PLAY_3DS)\n        if (file && *file)\n            lib3ds_play_sound(file); // 3DS ndsp\n#elif defined(WINMM_PLAY_SOUNDS)\n",
   "lib3ds_play_sound(file); // 3DS ndsp")
rw("initfile.cc",
   "#if !(defined(USE_SOUND) && (defined(WINMM_PLAY_SOUNDS) || defined(SOUND_PLAY_COMMAND) || defined(USE_SDL)))",
   "#if !(defined(USE_SOUND) && (defined(WINMM_PLAY_SOUNDS) || defined(SOUND_PLAY_COMMAND) || defined(USE_SDL) || defined(SOUND_PLAY_3DS)))",
   "|| defined(SOUND_PLAY_3DS)))")
rw("initfile.cc",
   "    \"defaults/consumable_shortcuts.txt\",\n};\n",
   "    \"defaults/consumable_shortcuts.txt\",\n#ifdef SOUND_PLAY_3DS\n    \"defaults/sounds3ds.txt\",   // 3DS: CC0 crawl-sounds pack\n#endif\n};\n",
   "defaults/sounds3ds.txt")
shutil.copy(os.path.join(PORT, "3ds", "sounds3ds.txt"), os.path.join(SRC, "dat", "defaults", "sounds3ds.txt"))
print("  + dat/defaults/sounds3ds.txt copied")
os.makedirs(os.path.join(SRC, "3ds", "sounds"), exist_ok=True)
_snd = os.path.join(PORT, "3ds", "sounds")
for f in os.listdir(_snd):
    shutil.copy(os.path.join(_snd, f), os.path.join(SRC, "3ds", "sounds", f))
print("  + 3ds/sounds/ (%d files) copied" % len(os.listdir(_snd)))

# -------------------------------------------------------------- new files
for f in ("lib3ds.cc", "lib3ds.h", "font5x8.h", "font6x13.h", "font6x13b.h", "manual3ds.h", "manual_text.h"):
    shutil.copy(os.path.join(PORT, f), os.path.join(SRC, f))
    print("  + %-14s copied" % f)
os.makedirs(os.path.join(SRC, "3ds"), exist_ok=True)
for f in ("crawl.rsf", "icon.png"):
    p = os.path.join(PORT, "3ds", f)
    if os.path.exists(p):
        shutil.copy(p, os.path.join(SRC, "3ds", f))
        print("  + 3ds/%-10s copied" % f)
os.makedirs(os.path.join(SRC, "3ds", "compat", "sys"), exist_ok=True)
shutil.copy(os.path.join(PORT, "3ds", "compat", "sys", "un.h"), os.path.join(SRC, "3ds", "compat", "sys", "un.h"))
print("  + 3ds/compat/sys/un.h copied")
_atl = os.path.join(PORT, "3ds", "tiles16.bin")
if os.path.exists(_atl):
    shutil.copy(_atl, os.path.join(SRC, "3ds", "tiles16.bin"))
    print("  + 3ds/tiles16.bin copied")

rv = os.path.join(SRC, "util", "release_ver")
if not os.path.exists(rv):
    open(rv, "w").write("0.34-3ds\n")
    print("  + util/release_ver (lets a shallow clone build)")
print("done")
