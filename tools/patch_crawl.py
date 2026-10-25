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
rw("platform.h",
   "#if !defined (OS_DETECTED)\n#if defined (TARGET_CPU_ARM)\n",
   "#if !defined (OS_DETECTED)\n#if defined (__3DS__)\n"
   "#define OS_DETECTED\n#define TARGET_OS_3DS\n#endif\n#endif\n\n"
   "#if !defined (OS_DETECTED)\n#if defined (TARGET_CPU_ARM)\n",
   "#define TARGET_OS_3DS")

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

path = os.path.join(SRC, "Makefile")
t = open(path, encoding="utf-8").read()
if "crawl.3dsx:" not in t:
    t += "\n" + open(os.path.join(PORT, "Makefile.3ds-rules")).read()
    open(path, "w", encoding="utf-8").write(t)
    print("  + Makefile       3dsx/cia rules appended")
else:
    print("  = Makefile       3dsx rules already present")

# -------------------------------------------------------------- new files
for f in ("lib3ds.cc", "lib3ds.h", "font5x10.h"):
    shutil.copy(os.path.join(PORT, f), os.path.join(SRC, f))
    print("  + %-14s copied" % f)
os.makedirs(os.path.join(SRC, "3ds"), exist_ok=True)
for f in ("crawl.rsf", "icon.png"):
    p = os.path.join(PORT, "3ds", f)
    if os.path.exists(p):
        shutil.copy(p, os.path.join(SRC, "3ds", f))
        print("  + 3ds/%-10s copied" % f)

rv = os.path.join(SRC, "util", "release_ver")
if not os.path.exists(rv):
    open(rv, "w").write("0.34-3ds\n")
    print("  + util/release_ver (lets a shallow clone build)")
print("done")
