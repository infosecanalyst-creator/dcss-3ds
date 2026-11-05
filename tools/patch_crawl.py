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
   "#ifdef TARGET_OS_3DS\n    lib3ds_early_init();\n"
   "    { char b[400]; snprintf(b, sizeof b, \"main: argc=%d argv0=%s argv1=%s\", argc, argv[0] ? argv[0] : \"\", argc > 1 ? argv[1] : \"\"); lib3ds_boot_mark(b); }\n"
   "    // launchers and emulators may pass extra arguments (or none at all, argc 0 and no argv);\n"
   "    // crawl would print its help and quit\n"
   "    if (argc < 1 || !argv || !argv[0]) { static char a0[] = \"sdmc:/3ds/crawl/crawl.3dsx\"; static char *av[2] = { a0, 0 }; argv = av; }\n"
   "    argc = 1;\n#endif\n",
   "lib3ds_early_init();")


# ---------------------------------------------------------------- boot trace (main.cc)
rw("main.cc",
   "    init_crash_handler();\n",
   "#ifdef TARGET_OS_3DS\n    lib3ds_boot_mark(\"main: before init_crash_handler\");\n#endif\n    init_crash_handler();\n#ifdef TARGET_OS_3DS\n    lib3ds_boot_mark(\"main: after init_crash_handler\");\n#endif\n",
   "main: before init_crash_handler")
rw("main.cc",
   "    _startup_asserts();\n",
   "#ifdef TARGET_OS_3DS\n    lib3ds_boot_mark(\"main: before _startup_asserts\");\n#endif\n    _startup_asserts();\n#ifdef TARGET_OS_3DS\n    lib3ds_boot_mark(\"main: after _startup_asserts\");\n#endif\n",
   "main: before _startup_asserts")
rw("main.cc",
   "    init_keybindings();\n",
   "#ifdef TARGET_OS_3DS\n    lib3ds_boot_mark(\"main: before init_keybindings\");\n#endif\n    init_keybindings();\n#ifdef TARGET_OS_3DS\n    lib3ds_boot_mark(\"main: after init_keybindings\");\n#endif\n",
   "main: before init_keybindings")
rw("main.cc",
   "    init_element_colours();\n",
   "#ifdef TARGET_OS_3DS\n    lib3ds_boot_mark(\"main: before init_element_colours\");\n#endif\n    init_element_colours();\n#ifdef TARGET_OS_3DS\n    lib3ds_boot_mark(\"main: after init_element_colours\");\n#endif\n",
   "main: before init_element_colours")
rw("main.cc",
   "    get_system_environment();\n",
   "#ifdef TARGET_OS_3DS\n    lib3ds_boot_mark(\"main: before get_system_environment\");\n#endif\n    get_system_environment();\n#ifdef TARGET_OS_3DS\n    lib3ds_boot_mark(\"main: after get_system_environment\");\n#endif\n",
   "main: before get_system_environment")
rw("main.cc",
   "    init_signals();\n",
   "#ifdef TARGET_OS_3DS\n    lib3ds_boot_mark(\"main: before init_signals\");\n#endif\n    init_signals();\n#ifdef TARGET_OS_3DS\n    lib3ds_boot_mark(\"main: after init_signals\");\n#endif\n",
   "main: before init_signals")
rw("main.cc",
   "    init_monsters();\n",
   "#ifdef TARGET_OS_3DS\n    lib3ds_boot_mark(\"main: before init_monsters\");\n#endif\n    init_monsters();\n#ifdef TARGET_OS_3DS\n    lib3ds_boot_mark(\"main: after init_monsters\");\n#endif\n",
   "main: before init_monsters")
rw("main.cc",
   "    init_item_name_cache();\n",
   "#ifdef TARGET_OS_3DS\n    lib3ds_boot_mark(\"main: before init_item_name_cache\");\n#endif\n    init_item_name_cache();\n#ifdef TARGET_OS_3DS\n    lib3ds_boot_mark(\"main: after init_item_name_cache\");\n#endif\n",
   "main: before init_item_name_cache")
rw("main.cc",
   "    validate_basedirs();\n",
   "#ifdef TARGET_OS_3DS\n    lib3ds_boot_mark(\"main: before validate_basedirs\");\n#endif\n    validate_basedirs();\n#ifdef TARGET_OS_3DS\n    lib3ds_boot_mark(\"main: after validate_basedirs\");\n#endif\n",
   "main: before validate_basedirs")
rw("main.cc",
   "    read_init_file();\n",
   "#ifdef TARGET_OS_3DS\n    lib3ds_boot_mark(\"main: before read_init_file\");\n#endif\n    read_init_file();\n#ifdef TARGET_OS_3DS\n    lib3ds_boot_mark(\"main: after read_init_file\");\n#endif\n",
   "main: before read_init_file")
rw("main.cc",
   "    _launch_game_loop();\n",
   "#ifdef TARGET_OS_3DS\n    lib3ds_boot_mark(\"main: before _launch_game_loop\");\n#endif\n    _launch_game_loop();\n#ifdef TARGET_OS_3DS\n    lib3ds_boot_mark(\"main: after _launch_game_loop\");\n#endif\n",
   "main: before _launch_game_loop")
rw("main.cc",
   "    // Now parse the args again, looking for everything else.\n",
   "#ifdef TARGET_OS_3DS\n    lib3ds_boot_mark(\"main: before parse_args 2\");\n#endif\n    // Now parse the args again, looking for everything else.\n",
   "main: before parse_args 2")

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


# ------------------------------------------------ save file and the HOME Menu
# The HOME Menu crashed (svcBreak in "menu") on HOME only while a game was
# loaded, i.e. while the save package held its file open read/write. The 3DS
# frontend closes that file before handing over to the HOME Menu and reopens
# it on return (every package read/write seeks first, so no state is lost).
rw("package.h",
   "    void abort();\n    void unlink();\n",
   "    void abort();\n    void unlink();\n#ifdef __3DS__\n    void lib3ds_close_fd();  // HOME Menu: release the file\n    void lib3ds_reopen_fd();\n#endif\n",
   "lib3ds_close_fd();  // HOME Menu")
rw("package.cc",
   "void package::abort()\n{",
   "#ifdef __3DS__\nvoid package::lib3ds_close_fd()\n{\n    if (fd != -1)\n    {\n        close(fd);\n        fd = -1;\n    }\n}\n\n"
   "void package::lib3ds_reopen_fd()\n{\n    if (fd == -1 && !filename.empty())\n"
   "        fd = open_u(filename.c_str(), (rw ? O_RDWR : O_RDONLY) | O_BINARY, 0666);\n}\n#endif\n\n"
   "void package::abort()\n{",
   "void package::lib3ds_close_fd()")

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

# ----------------------------------------------------- keys named in text
# Hints, the tutorial, help and menu footers name keys. On the 3DS a command
# is named by its bottom-screen button ("[items]"), a hardware button ("B",
# "L", "R") or else "[keys] x" (the keys button, then the key); literal keys
# in the text are fixed by keytext3ds.h.
_names = [
    ("CMD_DISPLAY_INVENTORY", "[items]"), ("CMD_CAST_SPELL", "[cast]"), ("CMD_USE_ABILITY", "[abil]"),
    ("CMD_DISPLAY_MAP", "[map]"), ("CMD_DISPLAY_CHARACTER_STATUS", "[char]"),
    ("CMD_REPLAY_MESSAGES", "[msgs]"), ("CMD_EXPLORE", "[explr]"), ("CMD_AUTOFIGHT", "[fight]"),
    ("CMD_REST", "[rest]"), ("CMD_WAIT", "[wait]"), ("CMD_PICKUP", "[pick up]"),
    ("CMD_GO_UPSTAIRS", "[up]"), ("CMD_GO_DOWNSTAIRS", "[down]"),
    ("CMD_MENU_EXIT", "B"), ("CMD_MENU_ACCEPT_SELECTION", "A"), ("CMD_MENU_PAGE_UP", "L"),
    ("CMD_MENU_PAGE_DOWN", "R"), ("CMD_MENU_UP", "Up"), ("CMD_MENU_DOWN", "Down"),
    ("CMD_MENU_TOGGLE_SELECTED", "tap"),
]
_items = ["CMD_EQUIP", "CMD_UNEQUIP", "CMD_WIELD_WEAPON", "CMD_UNWIELD_WEAPON", "CMD_WEAR_ARMOUR",
          "CMD_REMOVE_ARMOUR", "CMD_WEAR_JEWELLERY", "CMD_REMOVE_JEWELLERY", "CMD_QUAFF", "CMD_READ",
          "CMD_DROP", "CMD_EVOKE", "CMD_QUIVER_ITEM"]
_moves = ["CMD_MOVE_LEFT", "CMD_MOVE_RIGHT", "CMD_MOVE_UP", "CMD_MOVE_DOWN", "CMD_MOVE_UP_LEFT",
          "CMD_MOVE_UP_RIGHT", "CMD_MOVE_DOWN_LEFT", "CMD_MOVE_DOWN_RIGHT"]
_fn = ["#ifdef TARGET_OS_3DS",
       "// 3DS: the bottom-screen button (or hardware button) for a command",
       "static const char *_3ds_command_name(command_type cmd)", "{", "    switch (cmd)", "    {"]
_fn += ['    case %s: return "%s";' % (c, n) for c, n in _names]
_fn += ["    // item actions: [items], tap the item, tap the action"]
_fn += ["    case %s:" % c for c in _items] + ['        return "[items]";']
_fn += ["    case %s:" % c for c in _moves] + ['        return "Circle Pad";']
_fn += ["    default: return nullptr;", "    }", "}", "#endif", "", ""]
rw("macro.cc",
   "string command_to_string(command_type cmd, bool tutorial)\n{\n    const int key = command_to_key(cmd);\n",
   "\n".join(_fn) +
   "string command_to_string(command_type cmd, bool tutorial)\n{\n"
   "#ifdef TARGET_OS_3DS\n"
   "    if (const char *b = _3ds_command_name(cmd))\n        return b;\n"
   "#endif\n"
   "    const int key = command_to_key(cmd);\n",
   "_3ds_command_name(command_type cmd)")
rw("macro.cc",
   "    else\n        result = keycode_to_name(key, false);\n\n    return result;\n}\n",
   "    else\n        result = keycode_to_name(key, false);\n\n"
   "#ifdef TARGET_OS_3DS\n    if (!result.empty())\n        result = \"[keys] \" + result;   // the keys button, then this key\n#endif\n"
   "    return result;\n}\n",
   "result = \"[keys] \" + result;")
rw("menu.cc",
   "        if (primary != secondary_key && key_to_command(secondary_key[0], KMC_MENU) == cmd)\n",
   "#ifdef TARGET_OS_3DS\n        if (false) // 3DS: L / R are the page keys, no second key to show\n#else\n"
   "        if (primary != secondary_key && key_to_command(secondary_key[0], KMC_MENU) == cmd)\n#endif\n",
   "3DS: L / R are the page keys")
rw("hints.cc",
   "void hint_replace_cmds(string &text)\n{\n    size_t p;\n",
   "#ifdef TARGET_OS_3DS\n#include \"keytext3ds.h\"\n#endif\n\n"
   "void hint_replace_cmds(string &text)\n{\n    size_t p;\n"
   "#ifdef TARGET_OS_3DS\n    keytext3ds::fix_before_commands(text);\n#endif\n",
   "keytext3ds::fix_before_commands(text);")
rw("hints.cc",
   "        string input = text.substr(p + 7, q - p - 7);\n        input = \"<w>[\" + input;\n        input += \"]</w>\";\n        text.replace(p, q - p + 8, input);\n    }\n}\n",
   "        string input = text.substr(p + 7, q - p - 7);\n"
   "#ifdef TARGET_OS_3DS\n"
   "        if (!input.empty() && input[0] == '[')   // already \"[items]\" / \"[keys] x\"\n"
   "            input = \"<w>\" + input + \"</w>\";\n        else\n#endif\n"
   "        {\n            input = \"<w>[\" + input;\n            input += \"]</w>\";\n        }\n"
   "        text.replace(p, q - p + 8, input);\n    }\n"
   "#ifdef TARGET_OS_3DS\n    keytext3ds::fix(text);\n#endif\n}\n",
   "keytext3ds::fix(text);")

# Item / feature descriptions: the actions (read, drop, wear, go down...)
# become touch buttons on the bottom screen instead of "(r)ead" key hints.
rw("describe.cc",
   "static string _feat_action_desc(const vector<command_type>& actions,\n",
   "#ifdef TARGET_OS_3DS\n"
   "void lib3ds_set_actions(const vector<pair<string, int>> &actions);   // lib3ds.cc\n"
   "// Show the popup's actions as bottom-screen buttons (cleared by passing {}).\n"
   "static void _3ds_actions(const vector<command_type> &actions)\n{\n"
   "    static const map<command_type, pair<string, int>> t = {\n"
   "        { CMD_WIELD_WEAPON, { \"Wield\", 'w' } }, { CMD_UNWIELD_WEAPON, { \"Unwield\", 'u' } },\n"
   "        { CMD_QUIVER_ITEM, { \"Quiver\", 'q' } }, { CMD_WEAR_ARMOUR, { \"Wear\", 'w' } },\n"
   "        { CMD_REMOVE_ARMOUR, { \"Take off\", 't' } }, { CMD_EVOKE, { \"Evoke\", 'v' } },\n"
   "        { CMD_READ, { \"Read\", 'r' } }, { CMD_WEAR_JEWELLERY, { \"Put on\", 'p' } },\n"
   "        { CMD_REMOVE_JEWELLERY, { \"Remove\", 'r' } }, { CMD_QUAFF, { \"Drink\", 'q' } },\n"
   "        { CMD_PICKUP, { \"Pick up\", 'g' } }, { CMD_DROP, { \"Drop\", 'd' } },\n"
   "        { CMD_INSCRIBE_ITEM, { \"Inscribe\", 'i' } }, { CMD_ADJUST_INVENTORY, { \"Adjust\", '=' } },\n"
   "        { CMD_SET_SKILL_TARGET, { \"Skill target\", 's' } }, { CMD_MAP_GOTO_TARGET, { \"Go there\", 'g' } },\n"
   "        { CMD_GO_UPSTAIRS, { \"Go up\", '<' } }, { CMD_GO_DOWNSTAIRS, { \"Go down\", '>' } },\n"
   "        { CMD_MAP_PREV_LEVEL, { \"View level\", '[' } }, { CMD_MAP_NEXT_LEVEL, { \"View level\", ']' } },\n"
   "        { CMD_OPEN_DOOR, { \"Open\", 'o' } }, { CMD_CLOSE_DOOR, { \"Close\", 'c' } },\n"
   "    };\n"
   "    vector<pair<string, int>> v;\n"
   "    bool potion = false;\n"
   "    for (auto cmd : actions)\n    {\n"
   "        if (cmd == CMD_QUAFF)\n            potion = true;\n"
   "        if (potion && cmd == CMD_QUIVER_ITEM)\n        {\n            v.emplace_back(\"Quiver\", 'v');\n            continue;\n        }\n"
   "        auto it = t.find(cmd);\n        if (it != t.end())\n            v.push_back(it->second);\n    }\n"
   "    lib3ds_set_actions(v);\n}\n"
   "#endif\n\n"
   "static string _feat_action_desc(const vector<command_type>& actions,\n",
   "static void _3ds_actions(const vector<command_type> &actions)")
rw("describe.cc",
   "        footer_text += formatted_string(_feat_action_desc(actions, feat));\n",
   "#ifdef TARGET_OS_3DS\n        footer_text += formatted_string(\"Tap an action below.\");\n        _3ds_actions(actions);\n        (void)feat;\n#else\n"
   "        footer_text += formatted_string(_feat_action_desc(actions, feat));\n#endif\n",
   "_3ds_actions(actions);\n        (void)feat;")
rw("describe.cc",
   "    ui::run_layout(std::move(popup), done);\n    return _do_feat_action(pos, action);\n",
   "    ui::run_layout(std::move(popup), done);\n"
   "#ifdef TARGET_OS_3DS\n    _3ds_actions({});\n#endif\n"
   "    return _do_feat_action(pos, action);\n",
   "_3ds_actions({});\n#endif\n    return _do_feat_action")
rw("describe.cc",
   "        footer_text += formatted_string(_actions_desc(actions));\n",
   "#ifdef TARGET_OS_3DS\n        footer_text += formatted_string(spells.empty() ? \"Tap an action below.\" : \"tap an action below.\");\n        _3ds_actions(actions);\n#else\n"
   "        footer_text += formatted_string(_actions_desc(actions));\n#endif\n",
   "\"Tap an action below.\" : \"tap an action below.\"")
rw("describe.cc",
   "    ui::run_layout(std::move(popup), done);\n\n    return action;\n}\n",
   "    ui::run_layout(std::move(popup), done);\n"
   "#ifdef TARGET_OS_3DS\n    _3ds_actions({});\n#endif\n\n    return action;\n}\n",
   "_3ds_actions({});\n#endif\n\n    return action;")

# -------------------------------------------------------------- new files
for f in ("lib3ds.cc", "lib3ds.h", "keytext3ds.h", "font5x8.h", "font6x13.h", "font6x13b.h", "manual3ds.h", "manual_text.h"):
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
