#ifndef _LAMBDACHINE_LOADER_H
#define _LAMBDACHINE_LOADER_H

#include "Common.h"
#include "HashTable.h"
#include "InfoTables.h"

#define STR_SEC_HDR_MAGIC       MSB_u4('B','C','S','T')
#define CLOS_SEC_HDR_MAGIC      MSB_u4('B','C','C','L')
#define INFO_MAGIC              MSB_u4('I','T','B','L')
#define CLOSURE_MAGIC           MSB_u4('C','L','O','S')

typedef struct _StringTabEntry {
  u4 len;
  char *str;
} StringTabEntry;

typedef struct _BasePathEntry BasePathEntry;  // Defined in Loader.c

typedef struct _GlobalLoaderState {
  HashTable       *loadedModules;
  HashTable       *infoTables;
  HashTable       *closures;
  BasePathEntry   *basepaths;   // list of search paths for bytecode files
} GlobalLoaderState;

typedef struct _Module {
  char        *name;
  u4           numInfoTables;
  u4           numClosures;
  u4           numStrings;
  u4           numImports;

  StringTabEntry *strings;
  const char    **imports;
} Module;

Closure *lookupClosure(const char *name);
void initLoader();
void loadModule(const char *moduleName);
void printLoaderState();

#endif
