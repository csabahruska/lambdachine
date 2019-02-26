#include "Config.h"
#include "Loader.h"
#include "HashTable.h"
#include "InfoTables.h"
#include "FileUtils.h"
#include "PrintClosure.h"
#include "StorageManager.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <limits.h>
#include "Opts.h"

#define VERSION_MAJOR  0
#define VERSION_MINOR  1

#define BUFSIZE 512

/* Debug message if debugging loader and minimum debug level. */
#ifdef LC_DEBUG_LOADER
#define LD_DBG_PR(lvl, fmt, ...) DBG_LVL(lvl, fmt, __VA_ARGS__)
#else
#define LD_DBG_PR(lvl, fmt, ...) do {} while (0)
#endif

//--------------------------------------------------------------------
// Local type definitions

// Linked list of strings
struct _BasePathEntry {
  const char    *path;
  BasePathEntry *next;
};

//--------------------------------------------------------------------
// Global variables


GlobalLoaderState *G_loader = NULL;
//char *G_basepath = NULL;

//--------------------------------------------------------------------

//
void printModule(FILE *out, Module *mdl);
void printStringTable(StringTabEntry *tbl, u4 len);
void bufferOverflow(int sz, int bufsize);

void initBasepath(BasePathEntry **basepaths, Opts *opts);

void loadStringTabEntry(FILE *, StringTabEntry */*out*/);
char *loadId(FILE *, const StringTabEntry *, const char* sep);
void loadCode(const char *filename, FILE *, LcCode */*out*/, const StringTabEntry *,
               HashTable *itbls, HashTable *closures);
void loadLiteral(const char *filename,
                 FILE *, u1 *littype /*out*/, Word *literal /*out*/,
                  const StringTabEntry *,
                  HashTable *itbls, HashTable *closures);
InfoTable *loadInfoTable(const char *filename,
                         FILE *, const StringTabEntry*,
                          HashTable *itbls, HashTable *closures);
Closure *loadClosure(const char *filename, FILE *, const StringTabEntry *,
                     HashTable *itbls, HashTable *closures);

//--------------------------------------------------------------------

void
bufferOverflow(int sz, int bufsize)
{
  fprintf(stderr, "FATAL: buffer overflow (%d, max: %d)\n",
          sz, bufsize - 1);
  exit(1);
}

// Initialise the base path from the command line options.
// The base path argument is a colon-separated list of directories
// that are searched in order (i.e., left to right).  "." is expanded
// to the home directory.
//
BasePathEntry **
addBasePathEntry(BasePathEntry **p, const char *path)
{
  BasePathEntry *b;
  size_t sz;
  char buf[PATH_MAX + 1];
  const char *real = realpath(path, buf);

  if (real == NULL) {
    fprintf(stderr, "WARNING: Could not resolve base path: %s\n", path);
    return p;
  }

  b = xmalloc(sizeof(BasePathEntry));
  b->next = NULL;
  sz = strlen(real) + 1;
  b->path = xmalloc(sz);
  memmove((void*)b->path, real, sz);

  *p = b;
  return &b->next;
}

void
initBasepath(BasePathEntry **basepaths, Opts *opts)
{
  const char *path = opts->base_path;
  const char *path_end;
  char buf[PATH_MAX + 1];

  while (1) {
    int path_len;
    path_end = strchr(path, ':');
    path_len = (path_end != NULL) ? path_end - path : strlen(path);
    int is_last_path = path_end == NULL;

    if (path_len == 0) {
      if (is_last_path) {
        // Add default path (current working directory)
        basepaths = addBasePathEntry(basepaths, ".");
      }
    } else {
      if (path_len > PATH_MAX) {
        // Skip if path is too long
        fprintf(stderr, "WARNING: Path too long - ignoring: %s\n", path);
      } else {
        memmove((void*)buf, path, path_len);
        buf[path_len] = '\0';
        basepaths = addBasePathEntry(basepaths, buf);
      }
    }

    if (is_last_path)
      break;
    else
      path = path_end + 1;
  }
}

void
printBasePaths(BasePathEntry *b)
{
  fprintf(stderr, "basepaths:\n");
  while (b) {
    fprintf(stderr, "  %s\n", b->path);
    b = b->next;
  }
}

void
initLoader(Opts *opts)
{
  G_loader = xmalloc(sizeof(*G_loader));
  G_loader->loadedModules = HashTable_create();
  G_loader->infoTables = HashTable_create();
  G_loader->closures = HashTable_create();

  G_loader->basepaths = NULL;
  initBasepath(&G_loader->basepaths, opts);
}

Closure *
lookupClosure(const char *name)
{
  return HashTable_lookup(G_loader->closures, name);
}

int
isModuleLoaded(const char *moduleName)
{
  Module *mdl;
  mdl = HashTable_lookup(G_loader->loadedModules, moduleName);

  // If the module is currently being loaded, then its string table
  // will be non-empty.

  return (mdl != NULL) && (mdl->strings == NULL);
}

/* Turn "Foo.Bar.Baz" into "Foo/Bar/Baz". Copies input string. */
char *
modulePath(const char *moduleName)
{
  char *path = strdup(moduleName);
  char *p = path;
  while (*p) {
    if (*p == '.') *p = '/';
    ++p;
  }
  return path;
}

// String arguments must be UTF-8 encoded.
/*
FILE *
openModuleFile(const char *packageName, const char *moduleName)
{
  char path[BUFSIZE];
  int res;
  char *base = G_basepath;

  if (base == NULL) {
    fprintf(stderr, "ERROR: Couldn't find base path.\n");
    exit(1);
  }

  res = snprintf(path, BUFSIZE, "%s/lib/lambdachine/packages/%s/%s.kbc",
                 base, packageName, modulePath(moduleName));
  if (res >= BUFSIZE) bufferOverflow(res, BUFSIZE);

  if (fileExists(path)) {
    return fopen(path, "rb");
  }

  fprintf(stderr, "ERROR: Could not find module `%s' in package `%s'.\n",
          moduleName, packageName);
  fprintf(stderr, "  tried: %s\n", path);
  return NULL;
}
*/

void
loadStringTabEntry(FILE *f, StringTabEntry *e /*out*/)
{
  e->len = fget_varuint(f);
  e->str = xmalloc(e->len + 1);
  fread(e->str, 1, e->len, f);
  e->str[e->len] = '\0';
}

char *
moduleNameToFile(const char *basepath, const char *name)
{
  size_t   baselen  = strlen(basepath);
  size_t   len      = strlen(name);
  size_t   rsltlen;
  char    *filename, *p;
  size_t   i;

  if (baselen == 0) {
    baselen = 1;
    basepath = ".";
  }

  rsltlen = baselen + 1 + len + 5;
  filename = xmalloc(rsltlen + 1);

  strcpy(filename, basepath);
  filename[baselen] = '/';

  p = &filename[baselen + 1];

  for (i = 0; i < len; i++) {
    if (name[i] == '.')
      *p = '/';
    else
      *p = name[i];
    ++p;
  }
  strcpy(p, ".lcbc");

  // assert(rsltlen == strlen(filename));

  return filename;
}

void loadModule_aux(const char *moduleName, u4 level);
Module *loadModuleHeader(FILE *f, const char *filename);
void loadModuleBody(const char *filename, FILE *f, Module *mdl);
void ensureNoForwardRefs();

// Load the given module and all its dependencies.
void
loadModule(const char *moduleName)
{
  loadModule_aux(moduleName, 0);
  ensureNoForwardRefs();
}

const char *wired_in_packages[] =
  { "ghc-prim", "integer-gmp", "base" };

char *
findModule(const char *moduleName)
{
  u4     i;
  char  *filename;
  char   base[PATH_MAX];
  BasePathEntry *b = G_loader->basepaths;

  while (b) {
    // 1. Try to find module in base directory
    filename = moduleNameToFile(b->path, moduleName);

    fprintf(stderr, ".. Searching for `%s' in `%s'\n", moduleName, filename);

    if (fileExists(filename)) {
      return filename;
    }
    xfree(filename);

    // TODO: wired-in packages should probably only live in one
    // directory.  Otherwise, the user could accidentally shadow them.

    for (i = 0; i < countof(wired_in_packages); i++) {
      snprintf(base, PATH_MAX, "%s/%s", b->path, wired_in_packages[i]);
      filename = moduleNameToFile(base, moduleName);
      if (fileExists(filename)) {
        return filename;
      } else {
        xfree(filename);
      }
    }

    b = b->next;
  }

  fprintf(stderr, "ERROR: Could not find module: %s\n",
          moduleName);
  exit(13);
}

// Postorder traversal
void
loadModule_aux(const char *moduleName, u4 level)
{
  char     *filename;
  Module   *mdl;
  FILE     *f;
  int       i;

  mdl = (Module*)HashTable_lookup(G_loader->loadedModules, moduleName);

  if (mdl != NULL) {
    // Module is either already loaded, or currently in process of
    // being loaded.
      return;
  }

  filename = findModule(moduleName);

  for (i = 0; i < level; i++) fputc(' ', stderr);
  fprintf(stderr, "> Loading %s ...(%s)\n", moduleName, filename);

  f = fopen(filename, "rb");
  if (f == NULL) {
    fprintf(stderr, "ERROR: Could not open file: %s\n",
            filename);
    exit(14);
  }

  mdl = loadModuleHeader(f, filename);

  HashTable_insert(G_loader->loadedModules, moduleName, mdl);

  // Load dependencies first.  This avoids creating many forward
  // references.  The downside is that we keep more file descriptors
  // open.
  for (i = 0; i < mdl->numImports; i++)
    loadModule_aux(mdl->imports[i], level + 1);

  loadModuleBody(filename, f, mdl);

  fclose(f);

  // We now don't need the string table anymore.
  xfree(mdl->strings);
  mdl->strings = NULL;

  for (i = 0; i < level; i++) fputc(' ', stderr);
  fprintf(stderr, "< DONE   (%s)\n", moduleName);
}

// Load the module and
Module *
loadModuleHeader(FILE *f, const char *filename)
{
  Module *mdl;
  char magic[5];
  u2 major, minor;
  u4 flags;
  u4 secmagic;
  u4 i;

  LC_ASSERT(f != NULL);

  fread(magic, 4, 1, f);
  magic[4] = '\0';
  if (strcmp(magic, "KHCB") != 0) {
    fprintf(stderr, "ERROR: Module '%s' is not a bytecode file. %s\n",
            filename, magic);
    exit(1);
  }

  mdl = xmalloc(sizeof(Module));

  major = fget_u2(f);
  minor = fget_u2(f);

  if (major != VERSION_MAJOR || minor != VERSION_MINOR) {
    fprintf(stderr, "ERROR: Module '%s' version mismatch.  Version: %d.%d, Expected: %d.%d\n",
            filename, major, minor, VERSION_MAJOR, VERSION_MINOR);
    exit(1);
  }

  flags = fget_u4(f);
  mdl->numStrings    = fget_u4(f);
  mdl->numInfoTables = fget_u4(f);
  mdl->numClosures   = fget_u4(f);
  mdl->numImports    = fget_u4(f);

  //printf("strings = %d, itbls = %d, closures = %d\n",
  //       mdl->numStrings, mdl->numInfoTables, mdl->numClosures);

  // String table starts with a 4 byte magic.
  secmagic = fget_u4(f);
  assert(secmagic == STR_SEC_HDR_MAGIC);

  mdl->strings = xmalloc(sizeof(StringTabEntry) * mdl->numStrings);
  for (i = 0; i < mdl->numStrings; i++) {
    loadStringTabEntry(f, &mdl->strings[i]);
  }

  //printStringTable(mdl->strings, mdl->numStrings);

  mdl->name = loadId(f, mdl->strings, ".");
  // printf("mdl name = %s\n", mdl->name);

  mdl->imports = xmalloc(sizeof(*mdl->imports) * mdl->numImports);
  for (i = 0; i < mdl->numImports; i++) {
    mdl->imports[i] = loadId(f, mdl->strings, ".");
    // printf("import: %s\n", mdl->imports[i]);
  }

  return mdl;
}

void
loadModuleBody(const char *filename, FILE *f, Module *mdl)
{
  u4 i;
  u4 secmagic;
  // Load closures
  secmagic = fget_u4(f);
  assert(secmagic == CLOS_SEC_HDR_MAGIC);

  for (i = 0; i < mdl->numInfoTables; ++i) {
    loadInfoTable(filename, f, mdl->strings,
                   G_loader->infoTables, G_loader->closures);
  }

  for (i = 0; i < mdl->numClosures; i++) {
    loadClosure(filename, f, mdl->strings,
                G_loader->infoTables, G_loader->closures);
  }
}

void
printModule(FILE *out, Module* mdl)
{
  fprintf(out, "--- Module: %s ---\n", mdl->name);
  fprintf(out, "  info tables: %d\n", mdl->numInfoTables);
  fprintf(out, "  closures:    %d\n", mdl->numClosures);
  fprintf(out, "--- Info Tables ----------------\n");
  HashTable_print(out, G_loader->infoTables, (HashValuePrinter)printInfoTable);
  fprintf(out, "--- Closures (%d) ---------------\n", HashTable_entries(G_loader->closures));
  HashTable_print(out, G_loader->closures, (HashValuePrinter)printClosure);
}

void
printClosure1(FILE *out, const char *const name, Closure *cl)
{
  fprintf(out, "%s: [%p]: ", name, cl);
  printClosure_(out, cl, true);
}

void
printLoaderState(FILE *out)
{
  fprintf(out, "--- Info Tables ----------------\n");
  HashTable_print(out, G_loader->infoTables, (HashValuePrinter)printInfoTable);
  fprintf(out, "--- Closures (%d) ---------------\n", HashTable_entries(G_loader->closures));
  HashTable_foreach(G_loader->closures,
                    (HashValueCallback)printClosure1, out);
  //  HashTable_print(G_loader->closures, (HashValuePrinter)printClosure);
}

#define MAX_PARTS  255

// Load an identifier from the file.  It is encoded as a non-empty
// sequence of references to the string table.
//
// The separator is put between each string name.
char *
loadId(FILE *f, const StringTabEntry *strings, const char* sep)
{
  u4 numparts;
  u4 parts[MAX_PARTS];
  u4 seplen = strlen(sep);
  u4 i, len = 0;
  char *ident, *p;

  numparts = fget_varuint(f);
  assert(numparts > 0);

  for (i = 0; i < numparts; ++i) {
    u4 idx = fget_varuint(f);
    len += strings[idx].len + seplen;
    parts[i] = idx;
  }
  len -= seplen;

  ident = xmalloc(sizeof(char) * len + 1);
  p = ident;
  for (i = 0; i < numparts; i++) {
    len = strings[parts[i]].len;
    memcpy(p, strings[parts[i]].str, len);
    p += len;
    if (i != numparts - 1) {
      memcpy(p, sep, seplen);
      p += seplen;
    }
  }
  *p = '\0';
  LD_DBG_PR(3, "loadId: %lx, %s, " COLOURED(COL_BLUE, "%p") "\n",
            ftell(f), ident, ident);
  return ident;
}

BCIns
loadBCIns(FILE *f)
{
  return fget_u4(f);
}

InfoTable *
loadInfoTable(const char *filename,
              FILE *f, const StringTabEntry *strings,
              HashTable *itbls, HashTable *closures)
{
  u4 magic = fget_u4(f);
  assert(magic == INFO_MAGIC);

  char *itbl_name = loadId(f, strings, ".");
  u2 cl_type = fget_varuint(f);
  InfoTable *new_itbl = NULL;
  FwdRefInfoTable *old_itbl = HashTable_lookup(itbls, itbl_name);

  if (old_itbl && old_itbl->i.type != INVALID_OBJECT) {
    fprintf(stderr, "ERROR: Duplicate info table: %s\n",
            itbl_name);
    exit(1);
  }

  switch (cl_type) {
  case CONSTR:
    // A statically allocated constructor
    {
      ConInfoTable *info = allocInfoTable(wordsof(ConInfoTable));
      info->i.type = cl_type;
      info->i.tagOrBitmap = fget_varuint(f);  // tag
      Word sz = fget_varuint(f);
      assert(sz <= 32);
      info->i.size = sz;
      info->i.layout.bitmap = sz > 0 ? fget_u4(f) : 0;
      // info->i.layout.payload.ptrs = fget_varuint(f);
      // info->i.layout.payload.nptrs = fget_varuint(f);
      info->name = loadId(f, strings, ".");
      new_itbl = (InfoTable*)info;
    }
    break;
  case FUN:
    {
      FuncInfoTable *info = allocInfoTable(wordsof(FuncInfoTable));
      info->i.type = cl_type;
      info->i.tagOrBitmap = 0; // TODO: anything useful to put in here?
      Word sz = fget_varuint(f);
      assert(sz <= 32);
      info->i.size = sz;
      info->i.layout.bitmap = sz > 0 ? fget_u4(f) : 0;
      info->name = loadId(f, strings, ".");
      loadCode(filename, f, &info->code, strings, itbls, closures);
      new_itbl = (InfoTable*)info;
    }
    break;
  case CAF:
  case THUNK:
    {
      ThunkInfoTable *info = allocInfoTable(wordsof(ThunkInfoTable));
      info->i.type = cl_type;
      info->i.tagOrBitmap = 0; // TODO: anything useful to put in here?
      Word sz = fget_varuint(f);
      assert(sz <= 32);
      info->i.size = sz;
      info->i.layout.bitmap = sz > 0 ? fget_u4(f) : 0;
      info->name = loadId(f, strings, ".");
      loadCode(filename, f, &info->code, strings, itbls, closures);
      new_itbl = (InfoTable*)info;
    }
    break;
  default:
    fprintf(stderr, "ERROR: Unknown info table type (%d)", cl_type);
    exit(1);
  }
  // new_itbl is the new info table.  There may have been forward
  // references (even during loading the code for this info table).
  if (old_itbl != NULL) {
    LD_DBG_PR(1, "Fixing itable forward reference for: %s, %p\n", itbl_name, new_itbl);
    void **p, *next;
    LC_ASSERT(old_itbl->i.type == INVALID_OBJECT);

    for (p = old_itbl->next; p != NULL; p = next) {
      next = *p;
      *p = (void*)new_itbl;
    }

    // TODO: fixup forward refs
    xfree(old_itbl);
    HashTable_update(itbls, itbl_name, new_itbl);
    xfree(itbl_name);
  } else {
    LD_DBG_PR(2, "loadInfoTable: %s " COLOURED(COL_YELLOW, "%p") "\n",
              itbl_name, new_itbl);

    HashTable_insert(itbls, itbl_name, new_itbl);
  }

  return new_itbl;
}

void
loadLiteral(const char *filename,
            FILE *f, u1 *littype /*out*/, Word *literal /*out*/,
            const StringTabEntry *strings, HashTable *itbls,
            HashTable *closures)
{
  u4 i;
  *littype = fget_u1(f);
  switch (*littype) {
  case LIT_INT:
    *literal = (Word)fget_varsint(f);
    break;
  case LIT_CHAR:
    *literal = (Word)fget_varuint(f);
    break;
  case LIT_WORD:
    *literal = (Word)fget_varuint(f);
    break;
  case LIT_FLOAT:
    *literal = (Word)fget_u4(f);
    break;
  case LIT_STRING:
    i = fget_varuint(f);
    *literal = (Word)strings[i].str;
    break;
  case LIT_CLOSURE:
    { char *clname = loadId(f, strings, ".");
      Closure *cl = HashTable_lookup(closures, clname);
      if (cl == NULL) {
        // 1st forward ref, create the link
        cl = xmalloc(sizeof(ClosureHeader) + sizeof(Word));
        setInfo(cl, NULL);
        cl->payload[0] = (Word)literal;
        *literal = (Word)NULL;
        LD_DBG_PR(2, "Creating forward reference %p for `%s', %p\n",
                  cl, clname, literal);
        HashTable_insert(closures, clname, cl);
      } else if (getInfo(cl) == NULL) {
        // forward ref (not the first), insert into linked list
        LD_DBG_PR(2, "Linking forward reference %p (%s, target: %p)\n",
                  cl, clname, literal);
        *literal = (Word)cl->payload[0];
        cl->payload[0] = (Word)literal;
        xfree(clname);
      } else {
        *literal = (Word)cl;
        xfree(clname);
      }
    }
    break;
  case LIT_INFO:
    { char *infoname = loadId(f, strings, ".");
      InfoTable *info = HashTable_lookup(itbls, infoname);
      FwdRefInfoTable *info2;
      if (info == NULL) {
        // 1st forward ref
        info2 = xmalloc(sizeof(FwdRefInfoTable));
        info2->i.type = INVALID_OBJECT;
        info2->next = (void**)literal;
        *literal = (Word)NULL;
        HashTable_insert(itbls, infoname, info2);
      } else if (info->type == INVALID_OBJECT) {
        // subsequent forward ref
        info2 = (FwdRefInfoTable*)info;
        *literal = (Word)info2->next;
        info2->next = (void**)literal;
        xfree(infoname);
      } else {
        *literal = (Word)info;
        xfree(infoname);
      }
    }
    break;
  default:
    fprintf(stderr, "ERROR: Unknown literal type (%d) "
            "when loading file: %s\n",
            *littype, filename);
    exit(1);
  }
}

Closure *
loadClosure(const char *filename,
            FILE *f, const StringTabEntry *strings,
            HashTable *itbls, HashTable *closures)
{
  u4 i;
  u4 magic = fget_u4(f);
  assert(magic == CLOSURE_MAGIC);
  char *clos_name = loadId(f, strings, ".");
  u4 payloadsize = fget_varuint(f);
  char *itbl_name = loadId(f, strings, ".");

  Closure *cl = allocStaticClosure(wordsof(ClosureHeader) + payloadsize);
  Closure *fwd_ref;
  InfoTable* info = HashTable_lookup(itbls, itbl_name);
  LC_ASSERT(info != NULL && info->type != INVALID_OBJECT);

  // Fill in closure payload.  May create forward references to
  // the current closure.
  setInfo(cl, info);
  xfree(itbl_name);
  for (i = 0; i < payloadsize; i++) {
    LD_DBG_PR(1, "Loading payload for: %s [%d]\n", clos_name, i);
    u1 dummy;
    loadLiteral(filename, f, &dummy, &cl->payload[i], strings, itbls, closures);
  }

  fwd_ref = HashTable_lookup(closures, clos_name);
  if (fwd_ref != NULL) {
    LD_DBG_PR(2, "Fixing closure forward ref: %s, %p -> %p\n", clos_name, fwd_ref, cl);
    // fixup forward refs
    void **p, *next;
    for (p = (void**)fwd_ref->payload[0]; p != NULL; p = next) {
      next = *p;
      *p = (void*)cl;
    }

    xfree(fwd_ref);
    HashTable_update(closures, clos_name, cl);
    // The key has been allocated by whoever installed the first
    // forward reference.
    xfree(clos_name);

  } else {
    LD_DBG_PR(2, "loadClosure: %s " COLOURED(COL_GREEN, "%p") "\n",
              clos_name, cl);
    HashTable_insert(closures, clos_name, cl);

  }
  return cl;
}

void
ensureNoForwardRefInfo(int *i, const char *const name, InfoTable *info)
{
  if (info->type == INVALID_OBJECT) {
    fprintf(stderr, "Unresolved info table: %s\n", name);
    (*i)++;
  }
}

void
ensureNoForwardRefClosure(int *i, const char *const name, Closure *cl)
{
  if (getInfo(cl) == NULL) {
    fprintf(stderr, "Unresolved closure: %s\n", name);
    (*i)++;
  }
}

void ensureNoForwardRefs()
{
  int i = 0;

  HashTable_foreach(G_loader->infoTables,
                    (HashValueCallback)ensureNoForwardRefInfo,
                    &i);

  HashTable_foreach(G_loader->closures,
                    (HashValueCallback)ensureNoForwardRefClosure,
                    &i);

  if (i > 0) {
    fprintf(stderr, "ERROR: There %d were unresolved references.\n", i);
    exit(15);
  }
}

void
loadCode(const char *filename,
         FILE *f, LcCode *code/*out*/,
          const StringTabEntry *strings,
          HashTable *itbls, HashTable *closures)
{
  u4 i;
  u2 *bitmaps;
  code->framesize = fget_varuint(f);
  code->arity = fget_varuint(f);
  code->sizelits = fget_varuint(f);
  code->sizecode = fget_u2(f);
  code->sizebitmaps = fget_u2(f);
  //printf("loading code: frame:%d, arity:%d, lits:%d, code:%d, bitmaps:%d\n",
  //       code->framesize, code->arity, code->sizelits, code->sizecode,
  //       code->sizebitmaps);

  code->lits = xmalloc(sizeof(*code->lits) * code->sizelits);
  code->littypes = xmalloc(sizeof(u1) * code->sizelits);
  for (i = 0; i < code->sizelits; ++i) {
    loadLiteral(filename, f, &code->littypes[i], &code->lits[i],
                strings, itbls, closures);
  }
  code->code = xmalloc(sizeof(BCIns) * code->sizecode +
                       sizeof(u2) * code->sizebitmaps);
  for (i = 0; i < code->sizecode; i++) {
    code->code[i] = loadBCIns(f);
  }
  bitmaps = (u2*)&code->code[code->sizecode];
  for (i = 0; i < code->sizebitmaps; i++) {
    *bitmaps = fget_u2(f);
    ++bitmaps;
  }
}

void
printStringTable(StringTabEntry *tbl, u4 len)
{
  u4 i;
  fprintf(stderr, "String Table:\n");
  for (i = 0; i < len; i++) {
    fprintf(stderr, "  %5d: %s\n", i, tbl[i].str);
  }
}
