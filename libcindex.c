//============================================================================

// Copyright (c) 2013 Taketsuru <taketsuru11@gmail.com>.
// All rights reserved.

// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:

// 1. Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimer.

// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.

// THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
// OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
// DAMAGE.

//============================================================================

#include <tcl.h>
#include <tclTomMath.h>
#include <clang-c/Index.h>

#include <assert.h>
#include <string.h>
#include <strings.h>

enum {
   TCL_RECURSE = 5
};

//------------------------------------------------------------------ utilities

struct cindexSubcommand
{
   const char  *name;
   int	      (*proc)(ClientData, Tcl_Interp *, int, Tcl_Obj *const []);
};

static int
cindexDispatchSubcommand(ClientData                     clientData,
                         Tcl_Interp                    *interp,
                         int                            objc,
                         Tcl_Obj *const                 objv[],
                         int                            n,
                         const struct cindexSubcommand *subcommands)
{
   const char *cmd = Tcl_GetStringFromObj(objv[1], NULL);

   for (int i = 0; i < n; ++i) {
      if (strcmp(subcommands[i].name, cmd) == 0) {
         return subcommands[i].proc(clientData, interp, objc - 1, objv + 1);
      }
   }

   Tcl_Obj *result = Tcl_ObjPrintf("unknown subcommand \"%s\": must be ", cmd);
   Tcl_SetObjResult(interp, result);

   for (int i = 0; i < n; ++i) {
      if (i != 0) {
         if (i < n - 1) {
            Tcl_AppendToObj(result, ", ", -1);
         } else {
            Tcl_AppendToObj(result, ", or ", -1);
         }
      }
      Tcl_AppendToObj(result, subcommands[i].name, -1);
   }

   return TCL_ERROR;
}

//----------------------------------------------------------------- enum label

struct cindexEnumLabels
{
   Tcl_Obj    **labels;
   int	        n;
   const char  *names[];
};

static int cindexGetEnumLabel(Tcl_Interp                *interp,
                              struct cindexEnumLabels *labels,
                              int                      value)
{
   if (labels->labels == NULL) {
      int n;
      for (n = 0; labels->names[n] != NULL; ++n) {
      }
      labels->n      = n;
      labels->labels = (Tcl_Obj **)Tcl_Alloc(sizeof(Tcl_Obj *) * n);
      for (int i = 0; i < n; ++i) {
         labels->labels[i] = Tcl_NewStringObj(labels->names[i], -1);
         Tcl_IncrRefCount(labels->labels[i]);
      }
   }

   if (value < 0 || labels->n <= value) {
      Tcl_SetObjResult(interp, Tcl_ObjPrintf("unknown value: %d", value));
      return TCL_ERROR;
   }

   Tcl_SetObjResult(interp, labels->labels[value]);

   return TCL_OK;
}

//--------------------------------------------------------------- availability

static struct cindexEnumLabels cindexAvailabilityLabels = {
   .names = {
      "Available",
      "Deprecated",
      "NotAvailable",
      "NotAccessible",
      NULL
   }
};

static int cindexGetAvailabilityObj(Tcl_Interp                *interp,
                                    enum CXAvailabilityKind  kind)
{
   return cindexGetEnumLabel(interp, &cindexAvailabilityLabels, kind);
}

//-------------------------------------------------------- bit mask operations

struct cindexBitMask
{
   const char *name;
   unsigned    mask;
};

static int cindexParseBitMask(Tcl_Interp                 *interp,
                              int                         noptions1,
                              const char                 *options1[],
                              int                         noptions2,
                              const struct cindexBitMask *options2,
                              const char                 *none,
                              int                         objc,
                              Tcl_Obj *const              objv[],
                              unsigned                   *output)
{
   unsigned value = 0;
   for (int i = 1; i < objc; ++i) {

      const char *obj = Tcl_GetStringFromObj(objv[i], NULL);

      unsigned mask = 0;

      if (none != NULL && strcmp(obj, none) == 0) {
         continue;
      }

      for (int j = 0; j < noptions1; ++j) {
         if (strcmp(options1[j], obj) == 0) {
            mask = 1U << j;
            goto found;
         }
      }

      for (int j = 0; j < noptions2; ++j) {
         if (strcmp(options2[j].name, obj) == 0) {
            mask = options2[j].mask;
            goto found;
         }
      }

      Tcl_SetObjResult(interp, Tcl_ObjPrintf("unknown option: \"%s\"", obj));

      return TCL_ERROR;

   found:
      value |= mask;
   }

   *output = value;

   return TCL_OK;
}

static int cindexBitMaskToString(Tcl_Interp                 *interp,
                                 int                         noptions1,
                                 const char                 *options1[],
                                 int                         noptions2,
                                 const struct cindexBitMask *options2,
                                 const char                 *none,
                                 unsigned                    mask)
{
   if (mask == 0) {
      if (none != NULL) {
         Tcl_SetObjResult(interp, Tcl_NewStringObj(none, -1));
      }

      return TCL_OK;
   }

   unsigned value = mask;

   for (int i = 0; i < noptions2; ++i) {
      if ((value & options2[i].mask) == options2[i].mask) {
         value &= ~options2[i].mask;
         Tcl_AppendElement(interp, options2[i].name);
      }
   }

   while (value != 0) {
      int i = ffs(value) - 1;
      if (noptions1 <= i) {
         Tcl_SetObjResult(interp,
                          Tcl_ObjPrintf("unknown mask value: 0x%x", 1U << i));
         return TCL_ERROR;
      }
      Tcl_AppendElement(interp, options1[i]);
      value &= ~(1U << i);
   }

   return TCL_OK;
}

//------------------------------------------------------- child list operations

static void
cindexAddChild(Tcl_Interp *interp, Tcl_Obj **list, Tcl_Obj *child)
{
   if (Tcl_IsShared(*list)) {
      *list = Tcl_DuplicateObj(*list);
   }

   int status = Tcl_ListObjAppendElement(interp, *list, child);

   if (status != TCL_OK) {
      Tcl_BackgroundException(interp, status);
   }
}

static void
cindexRemoveChild(Tcl_Interp *interp, Tcl_Obj **list, Tcl_Obj *child)
{
   if (Tcl_IsShared(*list)) {
      *list = Tcl_DuplicateObj(*list);
   }

   int n       = 0;
   int status  = Tcl_ListObjLength(interp, *list, &n);
   if (status != TCL_OK) {
      goto end;
   }

   int         child_strlen = 0;
   const char *child_str    = Tcl_GetStringFromObj(child, &child_strlen);

   for (int i = n - 1; 0 <= i; --i) {

      Tcl_Obj *elm = NULL;
      status       = Tcl_ListObjIndex(interp, *list, i, &elm);
      if (status != TCL_OK) {
         break;
      }

      int         elm_strlen = 0;
      const char *elm_str    = Tcl_GetStringFromObj(elm, &elm_strlen);

      if (elm_str == child_str
          || (elm_strlen == child_strlen
              && memcmp(elm_str, child_str, elm_strlen) == 0)) {
         status = Tcl_ListObjReplace(interp, *list, i, 1, 0, NULL);
         break;
      }

   }

 end:
   if (status != TCL_OK) {
      Tcl_BackgroundException(interp, status);
   }
}

//-------------------------------------------------------------- index mapping

/** The information associated to an index Tcl command.
 */
struct cindexIndexInfo
{
   Tcl_Interp *interp;
   Tcl_Obj    *children;
   CXIndex     index;
};

static void
cindexIndexAddChild(struct cindexIndexInfo *info, Tcl_Obj *child)
{
   cindexAddChild(info->interp, &info->children, child);
}

static void
cindexIndexRemoveChild(struct cindexIndexInfo *info, Tcl_Obj *child)
{
   cindexRemoveChild(info->interp, &info->children, child);
}

/** A callback function called when an index Tcl command is deleted.
 * 
 * \param clientData pointer to struct cindexIndexInfo
 */
static void cindexIndexDeleteProc(ClientData clientData)
{
   int status = TCL_OK;

   struct cindexIndexInfo *info = (struct cindexIndexInfo *)clientData;

   // Until info->children becomes empty, delete the last command in the list.
   for (;;) {
      int n = 0;
      status = Tcl_ListObjLength(info->interp, info->children, &n);
      if (status != TCL_OK || n <= 0) {
         break;
      }

      Tcl_Obj *child = NULL;
      status = Tcl_ListObjIndex(info->interp, info->children, n - 1, &child);
      if (status != TCL_OK) {
         break;
      }

      Tcl_IncrRefCount(child);
      Tcl_DeleteCommand(info->interp, Tcl_GetStringFromObj(child, NULL));
      Tcl_DecrRefCount(child);
   }

   if (status != TCL_OK) {
      Tcl_BackgroundException(info->interp, status);
   }

   clang_disposeIndex(info->index);
   Tcl_DecrRefCount(info->children);
   Tcl_Free((char *)info);
}

//--------------------------------------------------- translation unit mapping

struct cindexTranslationUnitInfo
{
   struct cindexTranslationUnitInfo *next;
   struct cindexIndexInfo           *parent;
   Tcl_Obj                          *name;
   CXTranslationUnit                 translationUnit;
   int                               sequenceNumber;
};

static struct cindexTranslationUnitInfo *translationUnitHashTable[256];

static int cindexTranslationUnitNextSequenceNumber = 0;

static struct cindexTranslationUnitInfo *
cindexLookupTranslationUnit(int sequenceNumber)
{
   int hashTableSize =
      sizeof translationUnitHashTable / sizeof translationUnitHashTable[0];
   int hash = sequenceNumber % hashTableSize;
   for (struct cindexTranslationUnitInfo *p = translationUnitHashTable[hash];
        p != NULL;
        p = p->next) {
      if (p->sequenceNumber == sequenceNumber) {
         return p;
      }
   }

   return NULL;
}

static int cindexIsValidTranslationUnit(int sequenceNumber)
{
   return cindexLookupTranslationUnit(sequenceNumber) != NULL;
}

static void cindexTranslationUnitDeleteProc(ClientData clientData)
{
   struct cindexTranslationUnitInfo *info
      = (struct cindexTranslationUnitInfo *)clientData;

   cindexIndexRemoveChild(info->parent, info->name);

   Tcl_DecrRefCount(info->name);
   clang_disposeTranslationUnit(info->translationUnit);

   int hashTableSize =
      sizeof translationUnitHashTable / sizeof translationUnitHashTable[0];
   int hash = info->sequenceNumber % hashTableSize;
   struct cindexTranslationUnitInfo **prev = &translationUnitHashTable[hash];
   while (*prev != info) {
      prev = &info->next;
   }
   *prev = info->next;

   Tcl_Free((char *)info);
}

//------------------------------------------------------------- cursor mapping

struct cindexCursorInfo
{
   CXCursor cursor;
   int      tuSequenceNumber;
};

static int cindexValidateCursor(Tcl_Interp		*interp,
                                Tcl_Obj                 *cursorObj,
                                struct cindexCursorInfo *info)
{
   int            infoSize = 0;
   unsigned char *infoPtr  = Tcl_GetByteArrayFromObj(cursorObj, &infoSize);

   if (infoSize != sizeof *info) {
      Tcl_SetObjResult(interp, Tcl_NewStringObj("invalid cursor", -1));
      return TCL_ERROR;
   }

   memcpy(info, infoPtr, sizeof *info);

   if (! cindexIsValidTranslationUnit(info->tuSequenceNumber)) {
      Tcl_SetObjResult(interp,
                       Tcl_NewStringObj("translation unit of the cursor "
                                        "has been deleted.", -1));
      return TCL_ERROR;
   }

   return TCL_OK;
}

//---------------------------------------------------- source location mapping

static Tcl_Obj *cindexKeyObjFile;
static Tcl_Obj *cindexKeyObjLine;
static Tcl_Obj *cindexKeyObjColumn;
static Tcl_Obj *cindexKeyObjOffset;

static void cindexInitializeKeyObjs(void)
{
   if (cindexKeyObjFile != NULL) {
      return;
   }

   cindexKeyObjFile   = Tcl_NewStringObj("file", -1);
   cindexKeyObjLine   = Tcl_NewStringObj("line", -1);
   cindexKeyObjColumn = Tcl_NewStringObj("column", -1);
   cindexKeyObjOffset = Tcl_NewStringObj("offset", -1);
}

static Tcl_Obj *cindexNewSourceLocationObj(CXSourceLocation location,
                                           void (*proc)(CXSourceLocation,
                                                        CXFile *,
                                                        unsigned *,
                                                        unsigned *,
                                                        unsigned *))
{
   cindexInitializeKeyObjs();

   CXFile   file;
   unsigned line;
   unsigned column;
   unsigned offset;

   proc(location, &file, &line, &column, &offset);

   Tcl_Obj *result = Tcl_NewObj();
   Tcl_AppendObjToObj(result, cindexKeyObjFile);

   // XXX need to consider sharing tcl objects with the same file name
   CXString cxFileName = clang_getFileName(file);
   const char *filename = clang_getCString(cxFileName);
   Tcl_AppendObjToObj(result, Tcl_NewStringObj(filename, -1));
   clang_disposeString(cxFileName);

   Tcl_AppendObjToObj(result, cindexKeyObjLine);
   Tcl_AppendObjToObj(result, Tcl_NewLongObj(line));

   Tcl_AppendObjToObj(result, cindexKeyObjColumn);
   Tcl_AppendObjToObj(result, Tcl_NewLongObj(column));
                      
   Tcl_AppendObjToObj(result, cindexKeyObjOffset);
   Tcl_AppendObjToObj(result, Tcl_NewLongObj(offset));

   return result;
}

//---------------------------------- cindex::<translationUnit instance> save

static int cindexTranslationUnitSaveObjCmd(ClientData     clientData,
                                           Tcl_Interp    *interp,
                                           int            objc,
                                           Tcl_Obj *const objv[])
{
   if (objc < 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "filename");
      return TCL_ERROR;
   }

   const char *filename = Tcl_GetStringFromObj(objv[1], NULL);

   struct cindexTranslationUnitInfo *info
      = (struct cindexTranslationUnitInfo *)clientData;

   int status = clang_saveTranslationUnit(info->translationUnit, filename, 0);
   switch (status) {

   case CXSaveError_None:
      break;

   case CXSaveError_Unknown:
      Tcl_SetObjResult(interp,
                       Tcl_ObjPrintf("an unknown error occurred "
                                     "while attempting to save to \"%s\"",
                                     filename));
      return TCL_ERROR;

   case CXSaveError_TranslationErrors:
      Tcl_SetObjResult(interp,
                       Tcl_ObjPrintf("errors during translation prevented "
                                     "the attempt to save \"%s\"",
                                     Tcl_GetStringFromObj(info->name, NULL)));
      return TCL_ERROR;

   case CXSaveError_InvalidTU:
      Tcl_SetObjResult(interp,
                       Tcl_ObjPrintf("invalid translation unit \"%s\"",
                                     Tcl_GetStringFromObj(info->name, NULL)));
      return TCL_ERROR;

   default:
      Tcl_SetObjResult(interp,
                       Tcl_ObjPrintf("unknown error code is returned by "
                                     "clang_saveTranslationUnit()"));
      return TCL_ERROR;
   }

   return TCL_OK;
}

//------------------------------- cindex::<translationUnit instance> reparse

static int cindexTranslationUnitReparseObjCmd(ClientData     clientData,
                                              Tcl_Interp    *interp,
                                              int            objc,
                                              Tcl_Obj *const objv[])
{
   if (objc != 1) {
      Tcl_WrongNumArgs(interp, 1, objv, "");
      return TCL_ERROR;
   }

   struct cindexTranslationUnitInfo *info
      = (struct cindexTranslationUnitInfo *)clientData;

   int status = clang_reparseTranslationUnit(info->translationUnit, 0, NULL, 0);
   if (status != 0) {
      Tcl_SetObjResult(interp,
                       Tcl_ObjPrintf("translation unit \"%s\" is not valid",
                                     Tcl_GetStringFromObj(info->name, NULL)));
      return TCL_ERROR;
   }

   return TCL_OK;
}

//------------------------- cindex::<translationUnit instance> resourceUsage

static int
cindexTranslationUnitResourceUsageObjCmd(ClientData     clientData,
                                         Tcl_Interp    *interp,
                                         int            objc,
                                         Tcl_Obj *const objv[])
{
   if (objc != 1) {
      Tcl_WrongNumArgs(interp, 1, objv, "");
      return TCL_ERROR;
   }

   struct cindexTranslationUnitInfo *info
      = (struct cindexTranslationUnitInfo *)clientData;

   CXTUResourceUsage usage = clang_getCXTUResourceUsage(info->translationUnit);

   Tcl_Obj *result = Tcl_NewDictObj();
   Tcl_SetObjResult(interp, result);

   for (int i = 0; i < usage.numEntries; ++i) {
      const char *name = clang_getTUResourceUsageName(usage.entries[i].kind);
      Tcl_Obj *kind = Tcl_NewStringObj(name, -1);

      mp_int value_hi;
      mp_init_set_int(&value_hi, (usage.entries[i].amount >> 32) & 0xffffffff);
      mp_lshd(&value_hi, 32);
      mp_int value_lo;
      mp_init_set_int(&value_lo, usage.entries[i].amount & 0xffffffff);
      mp_or(&value_hi, &value_lo, &value_lo);
      Tcl_Obj *value = Tcl_NewBignumObj(&value_lo);
      mp_clear(&value_hi);
      mp_clear(&value_lo);

      Tcl_IncrRefCount(kind);
      Tcl_IncrRefCount(value);
      Tcl_DictObjPut(interp, result, kind, value);
      Tcl_DecrRefCount(kind);
      Tcl_DecrRefCount(value);
   }

   clang_disposeCXTUResourceUsage(usage);

   return TCL_OK;
}

//-------------------------------- cindex::<translationUnit instance> cursor

static int cindexTranslationUnitCursorObjCmd(ClientData     clientData,
                                             Tcl_Interp    *interp,
                                             int            objc,
                                             Tcl_Obj *const objv[])
{
   // cursor
   //	returns the cursor pointing the root AST node
   //
   // cursor -file file -line line -column column
   //   returns the cursor at the specified file, line, and column
   //
   // cursor -file file -offset offset
   //   returns the cursor at the specified file and offset

   if (objc != 1) {
      Tcl_WrongNumArgs(interp, 1, objv, "");
      return TCL_ERROR;
   }

   struct cindexTranslationUnitInfo *info
      = (struct cindexTranslationUnitInfo *)clientData;

   struct cindexCursorInfo cursorInfo;
   cursorInfo.cursor = clang_getTranslationUnitCursor(info->translationUnit);
   cursorInfo.tuSequenceNumber = info->sequenceNumber;

   Tcl_SetObjResult(interp, Tcl_NewByteArrayObj((unsigned char *)&cursorInfo,
                                                sizeof cursorInfo));

   return TCL_OK;
}

//--------------------------------------- cindex::<translationUnit instance>

static int cindexTranslationUnitInstanceObjCmd(ClientData     clientData,
                                               Tcl_Interp    *interp,
                                               int            objc,
                                               Tcl_Obj *const objv[])
{
   if (objc < 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "subcommand");
      return TCL_ERROR;
   }

   static struct cindexSubcommand subcommands[] = {
      { "save",		 cindexTranslationUnitSaveObjCmd },
      { "reparse",	 cindexTranslationUnitReparseObjCmd },
      { "resourceUsage", cindexTranslationUnitResourceUsageObjCmd },
      { "cursor",	 cindexTranslationUnitCursorObjCmd },
   };
   int n = sizeof subcommands / sizeof subcommands[0];

   return cindexDispatchSubcommand(clientData, interp, objc, objv,
                                   n, subcommands);
}

//----------------------------------------- cindex::<index instance> options

static int cindexIndexOptionsObjCmd(ClientData     clientData,
                                    Tcl_Interp    *interp,
                                    int            objc,
                                    Tcl_Obj *const objv[])
{
   static const char *options1[] = {
      "-threadBackgroundPriorityForIndexing",
      "-threadBackgroundPriorityForEditing",
   };
   int noptions1 = sizeof options1 / sizeof options1[0];

   static struct cindexBitMask options2[] = {
      { "-threadBackgroundPriorityForAll",
        CXGlobalOpt_ThreadBackgroundPriorityForAll }
   };
   int noptions2 = sizeof options2 / sizeof options2[0];

   struct cindexIndexInfo *info = (struct cindexIndexInfo *)clientData;

   if (objc == 1) {
      unsigned value = clang_CXIndex_getGlobalOptions(info->index);

      return cindexBitMaskToString(interp,
                                   noptions1, options1,
                                   noptions2, options2,
                                   "-none", value);
   }

   unsigned value = 0;
   int status = cindexParseBitMask(interp,
                                   noptions1, options1,
                                   noptions2, options2,
                                   "-none", objc, objv, &value);
   if (status == TCL_OK) {
      clang_CXIndex_setGlobalOptions(info->index, value);
   }

   return status;
}

//------------------------------------------------------ cindex::<index> parse

static const char *cindexParseOptions[] = {
   "-detailPreprocessingRecord",
   "-incomplete",
   "-precompiledPreamble",
   "-cacheCompletionResults",
   "-forSerialization",
   "-cxxChainedPCH",
   "-skipFunctionBodies",
   "-includeBriefCommentsInCodeCompletion",
};

static int cindexNumParseOptions
= sizeof cindexParseOptions / sizeof cindexParseOptions[0];

static int cindexIndexParseObjCmd(ClientData     clientData,
                                  Tcl_Interp    *interp,
                                  int            objc,
                                  Tcl_Obj *const objv[])
{
   if (objc < 2) {
      Tcl_WrongNumArgs(interp, 1, objv,
                       "tuName ?-detailPreprocessingRecord? ... "
                       "?--? commandline...");
      return TCL_ERROR;
   }

   const char *commandName = Tcl_GetStringFromObj(objv[1], NULL);

   int commandLineStart = objc;
   int optionsEnd = 0;
   for (int i = 2; i < objc; ++i, optionsEnd = i) {
      const char *str = Tcl_GetStringFromObj(objv[i], NULL);
      if (str[0] != '-') {
         commandLineStart = i;
         break;
      }
      if (strcmp(str, "--") == 0) {
         commandLineStart = i + 1;
         break;
      }
   }

   unsigned flags = 0;
   int status = cindexParseBitMask(interp,
                                   cindexNumParseOptions,
                                   cindexParseOptions,
                                   0, NULL,
                                   "-none", optionsEnd, objv, &flags);

   int nargs = objc - commandLineStart;
   char **args = (char **)Tcl_Alloc(nargs * sizeof *args);
   for (int i = commandLineStart; i < objc; ++i) {
      args[i - commandLineStart] = Tcl_GetStringFromObj(objv[i], NULL);
   }

   struct cindexIndexInfo *parent = (struct cindexIndexInfo *)clientData;
   CXTranslationUnit tu
      = clang_parseTranslationUnit(parent->index, NULL,
                                   (const char *const *)args, nargs,
                                   NULL, 0, flags);
   if (tu == NULL) {
      Tcl_SetObjResult(interp,
                       Tcl_NewStringObj("failed to create translation unit.",
                                        -1));
      return TCL_ERROR;
   }

   struct cindexTranslationUnitInfo *info
      = (struct cindexTranslationUnitInfo *)Tcl_Alloc(sizeof *info);
   info->parent          = parent;
   info->name            = objv[1];
   info->translationUnit = tu;
   info->sequenceNumber  = cindexTranslationUnitNextSequenceNumber++;

   int hashTableSize =
      sizeof translationUnitHashTable / sizeof translationUnitHashTable[0];
   int hash = info->sequenceNumber % hashTableSize;
   info->next = translationUnitHashTable[hash];
   translationUnitHashTable[hash] = info;

   Tcl_IncrRefCount(info->name);

   Tcl_CreateObjCommand(interp, commandName,
                        cindexTranslationUnitInstanceObjCmd,
                        (ClientData)info, cindexTranslationUnitDeleteProc);

   return TCL_OK;
}

//------------------------------------------------- cindex::<index instance>

static int cindexIndexInstanceObjCmd(ClientData clientData,
                                     Tcl_Interp *interp,
                                     int objc,
                                     Tcl_Obj *const objv[])
{
   if (objc < 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "subcommand");
      return TCL_ERROR;
   }

   static struct cindexSubcommand subcommands[] = {
      { "options",	cindexIndexOptionsObjCmd },
      { "parse",	cindexIndexParseObjCmd }
   };
   int n = sizeof subcommands / sizeof subcommands[0];

   return cindexDispatchSubcommand(clientData, interp, objc, objv,
                                   n, subcommands);
}

//------------------------------------------------------------ cindex::index

/*
 * cindex::index ?-excludeDeclFromPCH? ?-displayDiagnostics? index
 */

static int cindexIndexObjCmd(ClientData     clientData,
                             Tcl_Interp    *interp,
                             int            objc,
                             Tcl_Obj *const objv[])
{
   const char *commandName = NULL;

   static struct {
      const char *option;
      int         flag;
   } options[] = {
      { "-excludeDeclFromPCH", 0 },
      { "-displayDiagnostics", 0 }
   };

   for (int i = 1; i < objc; ++i) {
      const char *obj = Tcl_GetStringFromObj(objv[i], NULL);

      for (int j = 0; j < sizeof options / sizeof options[0]; ++j) {
         if (strcmp(options[j].option, obj) == 0) {
            options[j].flag = 1;
            goto continue_i;
         }
      }

      if (obj[0] == '-') {
         Tcl_SetObjResult(interp,
                          Tcl_ObjPrintf("unknown option: \"%s\"", obj));
         return TCL_ERROR;
      }

      if (commandName != NULL) {
         commandName = NULL;
         break;
      }

      commandName = obj;

   continue_i:
      ;
   }

   if (commandName == NULL) {
      Tcl_WrongNumArgs(interp, 1, objv,
                       "?-excludeDeclFromPCH? ?-displayDiagnostics? index");
      return TCL_ERROR;
   }

   CXIndex index = clang_createIndex(options[0].flag, options[1].flag);
   if (index == NULL) {
      Tcl_SetObjResult(interp, Tcl_NewStringObj("index creation failed", -1));
      return TCL_ERROR;
   }

   struct cindexIndexInfo *info
      = (struct cindexIndexInfo *)Tcl_Alloc(sizeof *info);
   info->interp = interp;
   info->index = index;
   info->children = Tcl_NewObj();
   Tcl_IncrRefCount(info->children);

   Tcl_CreateObjCommand(interp, commandName, cindexIndexInstanceObjCmd,
			(ClientData)info, cindexIndexDeleteProc);

   return TCL_OK;
}

//----------------------------------------------------- cindex::foreachChild

struct cindexForeachChildInfo
{
   Tcl_Interp *interp;
   Tcl_Obj    *childName;
   Tcl_Obj    *scriptObj;
   int         returnCode;
   int         tuSequenceNumber;
};

static enum CXChildVisitResult
cindexForeachChildHelper(CXCursor     cursor,
                         CXCursor     parent,
                         CXClientData clientData)
{
   struct cindexForeachChildInfo *visitInfo
      = (struct cindexForeachChildInfo *)clientData;

   struct cindexCursorInfo cursorInfo;
   cursorInfo.cursor           = cursor;
   cursorInfo.tuSequenceNumber = visitInfo->tuSequenceNumber;

   if (Tcl_ObjSetVar2(visitInfo->interp,
                      visitInfo->childName,
                      NULL,
                      Tcl_NewByteArrayObj((unsigned char *)&cursorInfo,
                                          sizeof cursorInfo),
                      TCL_LEAVE_ERR_MSG) == NULL) {
      return TCL_ERROR;
   }

   int status = Tcl_EvalObjEx(visitInfo->interp, visitInfo->scriptObj, 0);
   switch (status) {

   case TCL_OK:
   case TCL_CONTINUE:
      return CXChildVisit_Continue;

   case TCL_BREAK:
      return CXChildVisit_Break;

   case TCL_RECURSE:
      return CXChildVisit_Recurse;

   default:
      visitInfo->returnCode = status;
      return CXChildVisit_Break;
   }
}

static int cindexForeachChildObjCmd(ClientData clientData,
                                    Tcl_Interp *interp,
                                    int objc,
                                    Tcl_Obj *const objv[])
{
   if (objc != 4) {
      Tcl_WrongNumArgs(interp, 1, objv, "childName cursor script");
      return TCL_ERROR;
   }

   struct cindexCursorInfo cursorInfo;
   int status = cindexValidateCursor(interp, objv[2], &cursorInfo);
   if (status != TCL_OK) {
      return status;
   }

   struct cindexForeachChildInfo visitInfo = {
      .interp           = interp,
      .childName        = objv[1],
      .scriptObj        = objv[3],
      .returnCode       = TCL_OK,
      .tuSequenceNumber = cursorInfo.tuSequenceNumber,
   };

   if (clang_visitChildren(cursorInfo.cursor,
                           cindexForeachChildHelper,
                           (CXClientData)&visitInfo)) {
      return visitInfo.returnCode;
   }

   return TCL_OK;
}

//---------------------------------------------------------- cindex::recurse

static int cindexRecurseObjCmd(ClientData clientData,
                               Tcl_Interp *interp,
                               int objc,
                               Tcl_Obj *const objv[])
{
   return TCL_RECURSE;
}

//--------------------------------------------------- cindex::cursor::equals

static int cindexCursorEqualsObjCmd(ClientData clientData,
                                    Tcl_Interp *interp,
                                    int objc,
                                    Tcl_Obj *const objv[])
{
   if (objc != 3) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor1 cursor2");
      return TCL_ERROR;
   }

   struct cindexCursorInfo cursorInfo[2];
   for (int i = 2; i <= 3; ++i) {
      int status = cindexValidateCursor(interp, objv[i], &cursorInfo[i - 2]);
      if (status != TCL_OK) {
         return status;
      }
   }

   unsigned eq
      = clang_equalCursors(cursorInfo[0].cursor, cursorInfo[1].cursor);
   Tcl_SetObjResult(interp, Tcl_NewIntObj(eq));

   return TCL_OK;
}

//----------------------------------------------------- cindex::cursor::hash

static int cindexCursorHashObjCmd(ClientData clientData,
                                  Tcl_Interp *interp,
                                  int objc,
                                  Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor");
      return TCL_ERROR;
   }

   struct cindexCursorInfo cursorInfo;
   int status = cindexValidateCursor(interp, objv[1], &cursorInfo);
   if (status != TCL_OK) {
      return status;
   }

   unsigned hash = clang_hashCursor(cursorInfo.cursor);
   Tcl_SetObjResult(interp, Tcl_NewLongObj(hash));

   return TCL_OK;
}

//----------------------------------------------------- cindex::cursor::kind

static int cindexCursorKindObjCmd(ClientData clientData,
                                  Tcl_Interp *interp,
                                  int objc,
                                  Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor");
      return TCL_ERROR;
   }

   struct cindexCursorInfo cursorInfo;
   int status = cindexValidateCursor(interp, objv[1], &cursorInfo);
   if (status != TCL_OK) {
      return status;
   }

   static Tcl_Obj *name = NULL;
   if (name == NULL) {
      name = Tcl_NewStringObj("cindex::cursorKind", -1);
      Tcl_IncrRefCount(name);
   }

   enum CXCursorKind kind = clang_getCursorKind(cursorInfo.cursor);
   Tcl_SetObjResult(interp,
                    Tcl_ObjGetVar2(interp, name, Tcl_NewIntObj(kind), 0));

   return TCL_OK;
}

//------------------------------------------------- cindex::cursor::is::null

static int cindexCursorIsNullObjCmd(ClientData clientData,
                                    Tcl_Interp *interp,
                                    int objc,
                                    Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor");
      return TCL_ERROR;
   }

   struct cindexCursorInfo cursorInfo;
   int status = cindexValidateCursor(interp, objv[1], &cursorInfo);
   if (status != TCL_OK) {
      return status;
   }

   int result = clang_Cursor_isNull(cursorInfo.cursor);
   Tcl_SetObjResult(interp, Tcl_NewIntObj(result));

   return TCL_OK;
}

//-------------------------------------------- cindex::cursor::is::<generic>

static int cindexCursorIsGenericObjCmd(ClientData clientData,
                                       Tcl_Interp *interp,
                                       int objc,
                                       Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor");
      return TCL_ERROR;
   }

   struct cindexCursorInfo cursorInfo;
   int status = cindexValidateCursor(interp, objv[1], &cursorInfo);
   if (status != TCL_OK) {
      return status;
   }

   typedef unsigned (*ProcType)(enum CXCursorKind);

   ProcType proc = (ProcType)clientData;

   enum CXCursorKind kind = clang_getCursorKind(cursorInfo.cursor);
   unsigned result = proc(kind);
   Tcl_SetObjResult(interp, Tcl_NewLongObj(result));

   return TCL_OK;
}

//------------------------------------------- cindex::cursor::<generic enum>

typedef int (*cindexCursorGenericEnumProc)(CXCursor);

struct cindexCursorGenericEnumInfo
{
   struct cindexEnumLabels     *labels;
   cindexCursorGenericEnumProc  proc;
};

static int cindexCursorGenericEnumObjCmd(ClientData clientData,
                                         Tcl_Interp *interp,
                                         int objc,
                                         Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor");
      return TCL_ERROR;
   }

   struct cindexCursorInfo cursorInfo;
   int status = cindexValidateCursor(interp, objv[1], &cursorInfo);
   if (status != TCL_OK) {
      return status;
   }

   struct cindexCursorGenericEnumInfo *info
      = (struct cindexCursorGenericEnumInfo *)clientData;

   cindexCursorGenericEnumProc proc
      = (cindexCursorGenericEnumProc)clientData;
   int value = proc(cursorInfo.cursor);

   return cindexGetEnumLabel(interp, info->labels, value);
}

//------------------------------------------ cindex::cursor::translationUnit

static int cindexCursorTranslationUnitObjCmd(ClientData clientData,
                                             Tcl_Interp *interp,
                                             int objc,
                                             Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor");
      return TCL_ERROR;
   }

   int infoSize = 0;
   unsigned char *infoPtr = Tcl_GetByteArrayFromObj(objv[1], &infoSize);

   struct cindexCursorInfo cursorInfo;

   if (infoSize != sizeof cursorInfo) {
      Tcl_SetObjResult(interp, Tcl_NewStringObj("invalid cursor", -1));
      return TCL_ERROR;
   }

   memcpy(&cursorInfo, infoPtr, sizeof cursorInfo);

   struct cindexTranslationUnitInfo *tuInfo
      = cindexLookupTranslationUnit(cursorInfo.tuSequenceNumber);

   if (tuInfo == NULL) {
      Tcl_SetObjResult(interp,
                       Tcl_NewStringObj("translation unit of the cursor "
                                        "has been deleted.", -1));
      return TCL_ERROR;
   }

   Tcl_SetObjResult(interp, tuInfo->name);

   return TCL_OK;
}

//------------------------------------------- cindex::cursor::semanticParent

static int cindexCursorSemanticParentObjCmd(ClientData     clientData,
                                            Tcl_Interp    *interp,
                                            int            objc,
                                            Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor");
      return TCL_ERROR;
   }

   struct cindexCursorInfo cursorInfo;
   int status = cindexValidateCursor(interp, objv[1], &cursorInfo);
   if (status != TCL_OK) {
      return status;
   }

   cursorInfo.cursor = clang_getCursorSemanticParent(cursorInfo.cursor);

   Tcl_SetObjResult(interp, Tcl_NewByteArrayObj((unsigned char *)&cursorInfo,
                                                sizeof cursorInfo));

   return TCL_OK;
}

//------------------------------------------- cindex::cursor::lexicalParent

static int cindexCursorLexicalParentObjCmd(ClientData     clientData,
                                           Tcl_Interp    *interp,
                                           int            objc,
                                           Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor");
      return TCL_ERROR;
   }

   struct cindexCursorInfo cursorInfo;
   int status = cindexValidateCursor(interp, objv[1], &cursorInfo);
   if (status != TCL_OK) {
      return status;
   }

   cursorInfo.cursor = clang_getCursorLexicalParent(cursorInfo.cursor);

   Tcl_SetObjResult(interp,
                    Tcl_NewByteArrayObj((unsigned char *)&cursorInfo,
                                        sizeof cursorInfo));

   return TCL_OK;
}

//----------------------------------------- cindex::cursor::overridenCursors

static int cindexCursorOverriddenCursorsObjCmd(ClientData     clientData,
                                               Tcl_Interp    *interp,
                                               int            objc,
                                               Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor");
      return TCL_ERROR;
   }

   struct cindexCursorInfo cursorInfo;
   int status = cindexValidateCursor(interp, objv[1], &cursorInfo);
   if (status != TCL_OK) {
      return status;
   }

   CXCursor *overridden = NULL;
   unsigned numOverridden = 0;
   clang_getOverriddenCursors(cursorInfo.cursor, &overridden, &numOverridden);

   Tcl_Obj *result = Tcl_GetObjResult(interp);
   for (int i = 0; i < numOverridden; ++i) {
      struct cindexCursorInfo newCursorInfo;
      newCursorInfo.cursor = overridden[i];
      newCursorInfo.tuSequenceNumber = cursorInfo.tuSequenceNumber;

      Tcl_AppendObjToObj(result,
                         Tcl_NewByteArrayObj((unsigned char *)&newCursorInfo,
                                             sizeof newCursorInfo));
   }

   clang_disposeOverriddenCursors(overridden);

   return TCL_OK;
}

//--------------------------------------------- cindex::cursor::includedFile

static int cindexCursorIncludedFileObjCmd(ClientData     clientData,
                                          Tcl_Interp    *interp,
                                          int            objc,
                                          Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor");
      return TCL_ERROR;
   }

   struct cindexCursorInfo cursorInfo;
   int status = cindexValidateCursor(interp, objv[1], &cursorInfo);
   if (status != TCL_OK) {
      return status;
   }

   CXFile cxfile = clang_getIncludedFile(cursorInfo.cursor);
   CXString filenameStr = clang_getFileName(cxfile);
   const char *filenameCStr = clang_getCString(filenameStr);
   Tcl_SetObjResult(interp, Tcl_NewStringObj(filenameCStr, -1));
   clang_disposeString(filenameStr);

   return TCL_OK;
}

//---------------------------------------- cindex::cursor::expansionLocation

static int cindexCursorRangeObjCmd(ClientData     clientData,
                                   Tcl_Interp    *interp,
                                   int            objc,
                                   Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor");
      return TCL_ERROR;
   }

   struct cindexCursorInfo cursorInfo;
   int status = cindexValidateCursor(interp, objv[1], &cursorInfo);
   if (status != TCL_OK) {
      return status;
   }

   typedef void (*cursorProc)(CXSourceLocation,
                              CXFile*, unsigned *, unsigned *, unsigned *);

   CXSourceRange range = clang_getCursorExtent(cursorInfo.cursor);
   cursorProc proc = (cursorProc)clientData;

   Tcl_Obj *result = Tcl_NewObj();
   Tcl_SetObjResult(interp, result);

   CXSourceLocation start = clang_getRangeStart(range);
   Tcl_Obj *startObj = cindexNewSourceLocationObj(start, proc);
   Tcl_AppendObjToObj(result, startObj);

   CXSourceLocation end = clang_getRangeEnd(range);
   Tcl_Obj *endObj = cindexNewSourceLocationObj(end, proc);
   Tcl_AppendObjToObj(result, endObj);

   return TCL_OK;
}

//---------------------------------------- cindex::cursor::is:inSystemHeader

static int cindexCursorIsInSystemHeaderObjCmd(ClientData     clientData,
                                              Tcl_Interp    *interp,
                                              int            objc,
                                              Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor");
      return TCL_ERROR;
   }

   struct cindexCursorInfo cursorInfo;
   int status = cindexValidateCursor(interp, objv[1], &cursorInfo);
   if (status != TCL_OK) {
      return status;
   }

   CXSourceLocation pos = clang_getCursorLocation(cursorInfo.cursor);
   int value = clang_Location_isInSystemHeader(pos);

   Tcl_SetObjResult(interp, Tcl_NewIntObj(value));

   return TCL_OK;
}

//-------------------------------------------- cindex::cursor::is:inMainFile

static int cindexCursorIsInMainFileObjCmd(ClientData     clientData,
                                          Tcl_Interp    *interp,
                                          int            objc,
                                          Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor");
      return TCL_ERROR;
   }

   struct cindexCursorInfo cursorInfo;
   int status = cindexValidateCursor(interp, objv[1], &cursorInfo);
   if (status != TCL_OK) {
      return status;
   }

   CXSourceLocation pos = clang_getCursorLocation(cursorInfo.cursor);
   int value = clang_Location_isFromMainFile(pos);

   Tcl_SetObjResult(interp, Tcl_NewIntObj(value));

   return TCL_OK;
}

//------------------------------------------------------------- initialization

int cindex_createCursorKindTable(Tcl_Interp *interp)
{
   struct {
      const char *name;
      int         kind;
   } table[] = {
      { "UnexposedDecl",
        CXCursor_UnexposedDecl },
      { "StructDecl",
        CXCursor_StructDecl },
      { "UnionDecl",
        CXCursor_UnionDecl },
      { "ClassDecl",
        CXCursor_ClassDecl },
      { "EnumDecl",
        CXCursor_EnumDecl },
      { "FieldDecl",
        CXCursor_FieldDecl },
      { "EnumConstantDecl",
        CXCursor_EnumConstantDecl },
      { "FunctionDecl",
        CXCursor_FunctionDecl },
      { "VarDecl",
        CXCursor_VarDecl },
      { "ParmDecl",
        CXCursor_ParmDecl },
      { "ObjCInterfaceDecl",
        CXCursor_ObjCInterfaceDecl },
      { "ObjCCategoryDecl",
        CXCursor_ObjCCategoryDecl },
      { "ObjCProtocolDecl",
        CXCursor_ObjCProtocolDecl },
      { "ObjCPropertyDecl",
        CXCursor_ObjCPropertyDecl },
      { "ObjCIvarDecl",
        CXCursor_ObjCIvarDecl },
      { "ObjCInstanceMethodDecl",
        CXCursor_ObjCInstanceMethodDecl },
      { "ObjCClassMethodDecl",
        CXCursor_ObjCClassMethodDecl },
      { "ObjCImplementationDecl",
        CXCursor_ObjCImplementationDecl },
      { "ObjCCategoryImplDecl",
        CXCursor_ObjCCategoryImplDecl },
      { "TypedefDecl",
        CXCursor_TypedefDecl },
      { "CXXMethod",
        CXCursor_CXXMethod },
      { "Namespace",
        CXCursor_Namespace },
      { "LinkageSpec",
        CXCursor_LinkageSpec },
      { "Constructor",
        CXCursor_Constructor },
      { "Destructor",
        CXCursor_Destructor },
      { "ConversionFunction",
        CXCursor_ConversionFunction },
      { "TemplateTypeParameter",
        CXCursor_TemplateTypeParameter },
      { "NonTypeTemplateParameter",
        CXCursor_NonTypeTemplateParameter },
      { "TemplateTemplateParameter",
        CXCursor_TemplateTemplateParameter },
      { "FunctionTemplate",
        CXCursor_FunctionTemplate },
      { "ClassTemplate",
        CXCursor_ClassTemplate },
      { "ClassTemplatePartialSpecialization",
        CXCursor_ClassTemplatePartialSpecialization },
      { "NamespaceAlias",
        CXCursor_NamespaceAlias },
      { "UsingDirective",
        CXCursor_UsingDirective },
      { "UsingDeclaration",
        CXCursor_UsingDeclaration },
      { "TypeAliasDecl",
        CXCursor_TypeAliasDecl },
      { "ObjCSynthesizeDecl",
        CXCursor_ObjCSynthesizeDecl },
      { "ObjCDynamicDecl",
        CXCursor_ObjCDynamicDecl },
      { "CXXAccessSpecifier",
        CXCursor_CXXAccessSpecifier },
      { "FirstRef",
        CXCursor_FirstRef },
      { "ObjCSuperClassRef",
        CXCursor_ObjCSuperClassRef },
      { "ObjCProtocolRef",
        CXCursor_ObjCProtocolRef },
      { "ObjCClassRef",
        CXCursor_ObjCClassRef },
      { "TypeRef",
        CXCursor_TypeRef },
      { "CXXBaseSpecifier",
        CXCursor_CXXBaseSpecifier },
      { "TemplateRef",
        CXCursor_TemplateRef },
      { "NamespaceRef",
        CXCursor_NamespaceRef },
      { "MemberRef",
        CXCursor_MemberRef },
      { "LabelRef",
        CXCursor_LabelRef },
      { "OverloadedDeclRef",
        CXCursor_OverloadedDeclRef },
      { "VariableRef",
        CXCursor_VariableRef },
      { "InvalidFile",
        CXCursor_InvalidFile },
      { "NoDeclFound",
        CXCursor_NoDeclFound },
      { "NotImplemented",
        CXCursor_NotImplemented },
      { "InvalidCode",
        CXCursor_InvalidCode },
      { "UnexposedExpr",
        CXCursor_UnexposedExpr },
      { "DeclRefExpr",
        CXCursor_DeclRefExpr },
      { "MemberRefExpr",
        CXCursor_MemberRefExpr },
      { "CallExpr",
        CXCursor_CallExpr },
      { "ObjCMessageExpr",
        CXCursor_ObjCMessageExpr },
      { "BlockExpr",
        CXCursor_BlockExpr },
      { "IntegerLiteral",
        CXCursor_IntegerLiteral },
      { "FloatingLiteral",
        CXCursor_FloatingLiteral },
      { "ImaginaryLiteral",
        CXCursor_ImaginaryLiteral },
      { "StringLiteral",
        CXCursor_StringLiteral },
      { "CharacterLiteral",
        CXCursor_CharacterLiteral },
      { "ParenExpr",
        CXCursor_ParenExpr },
      { "UnaryOperator",
        CXCursor_UnaryOperator },
      { "ArraySubscriptExpr",
        CXCursor_ArraySubscriptExpr },
      { "BinaryOperator",
        CXCursor_BinaryOperator },
      { "CompoundAssignOperator",
        CXCursor_CompoundAssignOperator },
      { "ConditionalOperator",
        CXCursor_ConditionalOperator },
      { "CStyleCastExpr",
        CXCursor_CStyleCastExpr },
      { "CompoundLiteralExpr",
        CXCursor_CompoundLiteralExpr },
      { "InitListExpr",
        CXCursor_InitListExpr },
      { "AddrLabelExpr",
        CXCursor_AddrLabelExpr },
      { "StmtExpr",
        CXCursor_StmtExpr },
      { "GenericSelectionExpr",
        CXCursor_GenericSelectionExpr },
      { "GNUNullExpr",
        CXCursor_GNUNullExpr },
      { "CXXStaticCastExpr",
        CXCursor_CXXStaticCastExpr },
      { "CXXDynamicCastExpr",
        CXCursor_CXXDynamicCastExpr },
      { "CXXReinterpretCastExpr",
        CXCursor_CXXReinterpretCastExpr },
      { "CXXConstCastExpr",
        CXCursor_CXXConstCastExpr },
      { "CXXFunctionalCastExpr",
        CXCursor_CXXFunctionalCastExpr },
      { "CXXTypeidExpr",
        CXCursor_CXXTypeidExpr },
      { "CXXBoolLiteralExpr",
        CXCursor_CXXBoolLiteralExpr },
      { "CXXNullPtrLiteralExpr",
        CXCursor_CXXNullPtrLiteralExpr },
      { "CXXThisExpr",
        CXCursor_CXXThisExpr },
      { "CXXThrowExpr",
        CXCursor_CXXThrowExpr },
      { "CXXNewExpr",
        CXCursor_CXXNewExpr },
      { "CXXDeleteExpr",
        CXCursor_CXXDeleteExpr },
      { "UnaryExpr",
        CXCursor_UnaryExpr },
      { "ObjCStringLiteral",
        CXCursor_ObjCStringLiteral },
      { "ObjCEncodeExpr",
        CXCursor_ObjCEncodeExpr },
      { "ObjCSelectorExpr",
        CXCursor_ObjCSelectorExpr },
      { "ObjCProtocolExpr",
        CXCursor_ObjCProtocolExpr },
      { "ObjCBridgedCastExpr",
        CXCursor_ObjCBridgedCastExpr },
      { "PackExpansionExpr",
        CXCursor_PackExpansionExpr },
      { "SizeOfPackExpr",
        CXCursor_SizeOfPackExpr },
      { "LambdaExpr",
        CXCursor_LambdaExpr },
      { "ObjCBoolLiteralExpr",
        CXCursor_ObjCBoolLiteralExpr },
      { "ObjCSelfExpr",
        CXCursor_ObjCSelfExpr },
      { "UnexposedStmt",
        CXCursor_UnexposedStmt },
      { "LabelStmt",
        CXCursor_LabelStmt },
      { "CompoundStmt",
        CXCursor_CompoundStmt },
      { "CaseStmt",
        CXCursor_CaseStmt },
      { "DefaultStmt",
        CXCursor_DefaultStmt },
      { "IfStmt",
        CXCursor_IfStmt },
      { "SwitchStmt",
        CXCursor_SwitchStmt },
      { "WhileStmt",
        CXCursor_WhileStmt },
      { "DoStmt",
        CXCursor_DoStmt },
      { "ForStmt",
        CXCursor_ForStmt },
      { "GotoStmt",
        CXCursor_GotoStmt },
      { "IndirectGotoStmt",
        CXCursor_IndirectGotoStmt },
      { "ContinueStmt",
        CXCursor_ContinueStmt },
      { "BreakStmt",
        CXCursor_BreakStmt },
      { "ReturnStmt",
        CXCursor_ReturnStmt },
      { "AsmStmt",
        CXCursor_AsmStmt },
      { "ObjCAtTryStmt",
        CXCursor_ObjCAtTryStmt },
      { "ObjCAtCatchStmt",
        CXCursor_ObjCAtCatchStmt },
      { "ObjCAtFinallyStmt",
        CXCursor_ObjCAtFinallyStmt },
      { "ObjCAtThrowStmt",
        CXCursor_ObjCAtThrowStmt },
      { "ObjCAtSynchronizedStmt",
        CXCursor_ObjCAtSynchronizedStmt },
      { "ObjCAutoreleasePoolStmt",
        CXCursor_ObjCAutoreleasePoolStmt },
      { "ObjCForCollectionStmt",
        CXCursor_ObjCForCollectionStmt },
      { "CXXCatchStmt",
        CXCursor_CXXCatchStmt },
      { "CXXTryStmt",
        CXCursor_CXXTryStmt },
      { "CXXForRangeStmt",
        CXCursor_CXXForRangeStmt },
      { "SEHTryStmt",
        CXCursor_SEHTryStmt },
      { "SEHExceptStmt",
        CXCursor_SEHExceptStmt },
      { "SEHFinallyStmt",
        CXCursor_SEHFinallyStmt },
      { "MSAsmStmt",
        CXCursor_MSAsmStmt },
      { "NullStmt",
        CXCursor_NullStmt },
      { "DeclStmt",
        CXCursor_DeclStmt },
      { "OMPParallelDirective",
        CXCursor_OMPParallelDirective },
      { "TranslationUnit",
        CXCursor_TranslationUnit },
      { "UnexposedAttr",
        CXCursor_UnexposedAttr },
      { "IBActionAttr",
        CXCursor_IBActionAttr },
      { "IBOutletAttr",
        CXCursor_IBOutletAttr },
      { "IBOutletCollectionAttr",
        CXCursor_IBOutletCollectionAttr },
      { "CXXFinalAttr",
        CXCursor_CXXFinalAttr },
      { "CXXOverrideAttr",
        CXCursor_CXXOverrideAttr },
      { "AnnotateAttr",
        CXCursor_AnnotateAttr },
      { "AsmLabelAttr",
        CXCursor_AsmLabelAttr },
      { "PackedAttr",
        CXCursor_PackedAttr },
      { "PreprocessingDirective",
        CXCursor_PreprocessingDirective },
      { "MacroDefinition",
        CXCursor_MacroDefinition },
      { "MacroExpansion",
        CXCursor_MacroExpansion },
      { "InclusionDirective",
        CXCursor_InclusionDirective },
      { "ModuleImportDecl",
        CXCursor_ModuleImportDecl },
   };
   int numEntries = sizeof table / sizeof table[0];

   Tcl_Obj *name = Tcl_NewStringObj("cindex::cursorKind", -1);
   for (int i = 0; i < numEntries; ++i) {
      if (Tcl_ObjSetVar2(interp, name, Tcl_NewIntObj(table[i].kind),
                         Tcl_NewStringObj(table[i].name, -1),
                         TCL_LEAVE_ERR_MSG) == NULL) {
         return TCL_ERROR;
      }
   }

   return TCL_OK;
}

int cindex_createCXTypeTable(Tcl_Interp *interp)
{
   struct {
      const char *name;
      int         kind;
   } table[] = {
      { "Invalid", CXType_Invalid },
      { "Unexposed", CXType_Unexposed },
      { "Void", CXType_Void },
      { "Bool", CXType_Bool },
      { "Char_U", CXType_Char_U },
      { "UChar", CXType_UChar },
      { "Char16", CXType_Char16 },
      { "Char32", CXType_Char32 },
      { "UShort", CXType_UShort },
      { "UInt", CXType_UInt },
      { "ULong", CXType_ULong },
      { "ULongLong", CXType_ULongLong },
      { "UInt128", CXType_UInt128 },
      { "Char_S", CXType_Char_S },
      { "SChar", CXType_SChar },
      { "WChar", CXType_WChar },
      { "Short", CXType_Short },
      { "Int", CXType_Int },
      { "Long", CXType_Long },
      { "LongLong", CXType_LongLong },
      { "Int128", CXType_Int128 },
      { "Float", CXType_Float },
      { "Double", CXType_Double },
      { "LongDouble", CXType_LongDouble },
      { "NullPtr", CXType_NullPtr },
      { "Overload", CXType_Overload },
      { "Dependent", CXType_Dependent },
      { "ObjCId", CXType_ObjCId },
      { "ObjCClass", CXType_ObjCClass },
      { "ObjCSel", CXType_ObjCSel },
      { "Complex", CXType_Complex },
      { "Pointer", CXType_Pointer },
      { "BlockPointer", CXType_BlockPointer },
      { "LValueReference", CXType_LValueReference },
      { "RValueReference", CXType_RValueReference },
      { "Record", CXType_Record },
      { "Enum", CXType_Enum },
      { "Typedef", CXType_Typedef },
      { "ObjCInterface", CXType_ObjCInterface },
      { "ObjCObjectPointer", CXType_ObjCObjectPointer },
      { "FunctionNoProto", CXType_FunctionNoProto },
      { "FunctionProto", CXType_FunctionProto },
      { "ConstantArray", CXType_ConstantArray },
      { "Vector", CXType_Vector },
      { "IncompleteArray", CXType_IncompleteArray },
      { "VariableArray", CXType_VariableArray },
      { "DependentSizedArray", CXType_DependentSizedArray },
      { "MemberPointer", CXType_MemberPointer },
   };
   int numEntries = sizeof table / sizeof table[0];

   Tcl_Obj *name = Tcl_NewStringObj("cindex::typeKind", -1);
   for (int i = 0; i < numEntries; ++i) {
      if (Tcl_ObjSetVar2(interp, name, Tcl_NewIntObj(table[i].kind),
                         Tcl_NewStringObj(table[i].name, -1),
                         TCL_LEAVE_ERR_MSG) == NULL) {
         return TCL_ERROR;
      }
   }

   return TCL_OK;
}

int cindex_createCallingConvTable(Tcl_Interp *interp)
{
   struct {
      const char *name;
      int         kind;
   } table[] = {
      { "Default", CXCallingConv_Default },
      { "C", CXCallingConv_C },
      { "X86StdCall", CXCallingConv_X86StdCall },
      { "X86FastCall", CXCallingConv_X86FastCall },
      { "X86ThisCall", CXCallingConv_X86ThisCall },
      { "X86Pascal", CXCallingConv_X86Pascal },
      { "AAPCS", CXCallingConv_AAPCS },
      { "AAPCS_VFP", CXCallingConv_AAPCS_VFP },
      { "PnaclCall", CXCallingConv_PnaclCall },
      { "IntelOclBicc", CXCallingConv_IntelOclBicc },
      { "X86_64Win64", CXCallingConv_X86_64Win64 },
      { "X86_64SysV", CXCallingConv_X86_64SysV },
      { "Invalid", CXCallingConv_Invalid },
      { "Unexposed", CXCallingConv_Unexposed },
   };
   int numEntries = sizeof table / sizeof table[0];

   Tcl_Obj *name = Tcl_NewStringObj("cindex::callingConv", -1);
   for (int i = 0; i < numEntries; ++i) {
      if (Tcl_ObjSetVar2(interp, name, Tcl_NewIntObj(table[i].kind),
                         Tcl_NewStringObj(table[i].name, -1),
                         TCL_LEAVE_ERR_MSG) == NULL) {
         return TCL_ERROR;
      }
   }

   return TCL_OK;
}

int Cindex_Init(Tcl_Interp *interp)
{
   if (Tcl_InitStubs(interp, "8.6", 0) == NULL) {
      return TCL_ERROR;
   }

   Tcl_Namespace *cindexNs
      = Tcl_CreateNamespace(interp, "cindex", NULL, NULL);

   int status = cindex_createCursorKindTable(interp);
   if (status != TCL_OK) {
      return status;
   }

   Tcl_CreateObjCommand(interp, "cindex::index",
			cindexIndexObjCmd, (ClientData)NULL, NULL);
   Tcl_Export(interp, cindexNs, "index", 0);
   Tcl_CreateObjCommand(interp, "cindex::foreachChild",
			cindexForeachChildObjCmd, (ClientData)NULL, NULL);
   Tcl_Export(interp, cindexNs, "foreachChild", 0);
   Tcl_CreateObjCommand(interp, "cindex::recurse",
			cindexRecurseObjCmd, (ClientData)NULL, NULL);
   Tcl_Export(interp, cindexNs, "recurse", 0);

   Tcl_Namespace *cursorNs
      = Tcl_CreateNamespace(interp, "cindex::cursor", NULL, NULL);

   Tcl_Command cursorCmd
      = Tcl_CreateEnsemble(interp, "::cindex::cursor", cursorNs, 0);
   Tcl_Export(interp, cindexNs, "cursor", 0);

   static struct cindexEnumLabels linkageLables = {
      .names = {
         "Invalid",
         "NoLinkage",
         "Internal",
         "UniqueExternal",
         "External",
         NULL
      }
   };

   static struct cindexCursorGenericEnumInfo cursorLinkageInfo = {
      .proc   = (int (*)(CXCursor))clang_getCursorLinkage,
      .labels = &linkageLables
   };

   static struct cindexCursorGenericEnumInfo cursorAvailabilityInfo = {
      .proc   = (int (*)(CXCursor))clang_getCursorAvailability,
      .labels = &cindexAvailabilityLabels
   };

   static struct cindexEnumLabels languageLables = {
      .names = {
         "Invalid",
         "C",
         "ObjC",
         "CPlusPlus",
         NULL
      }
   };

   static struct cindexCursorGenericEnumInfo cursorLanguageInfo = {
      .proc   = (int (*)(CXCursor))clang_getCursorLanguage,
      .labels = &languageLables
   };

   struct {
      const char  *name;
      int	 (*proc)(ClientData, Tcl_Interp *, int, Tcl_Obj *const []);
      ClientData   clientData;
   } cursorCmdTable[] = {
      { "equals", cindexCursorEqualsObjCmd },
      { "hash", cindexCursorHashObjCmd },
      { "kind", cindexCursorKindObjCmd },
      { "linkage",
        cindexCursorGenericEnumObjCmd, &cursorLinkageInfo },
      { "availability",
        cindexCursorGenericEnumObjCmd, &cursorAvailabilityInfo },
      { "language",
        cindexCursorGenericEnumObjCmd, &cursorLanguageInfo },
      { "translationUnit", cindexCursorTranslationUnitObjCmd },
      { "semanticParent", cindexCursorSemanticParentObjCmd },
      { "lexicalParent", cindexCursorLexicalParentObjCmd },
      { "overriddenCursors", cindexCursorOverriddenCursorsObjCmd },
      { "includedFile", cindexCursorIncludedFileObjCmd },
      { "expansionLocation",
        cindexCursorRangeObjCmd,
        (ClientData)clang_getExpansionLocation },
      { "presumedLocation",
        cindexCursorRangeObjCmd,
        (ClientData)clang_getPresumedLocation },
      { "spellingLocation",
        cindexCursorRangeObjCmd,
        (ClientData)clang_getSpellingLocation },
      { "fileLocation",
        cindexCursorRangeObjCmd,
        (ClientData)clang_getSpellingLocation },
   };

   for (int i = 0; i < sizeof cursorCmdTable / sizeof cursorCmdTable[0]; ++i) {
      char buffer[80];

      snprintf(buffer, sizeof buffer, "cindex::cursor::%s",
               cursorCmdTable[i].name);
      Tcl_CreateObjCommand(interp, buffer, cursorCmdTable[i].proc,
                           cursorCmdTable[i].clientData, NULL);
      Tcl_Export(interp, cursorNs, cursorCmdTable[i].name, 0);
   }

   Tcl_Namespace *cursorIsNs
      = Tcl_CreateNamespace(interp, "cindex::cursor::is", NULL, NULL);

   Tcl_Command cursorIsCmd
      = Tcl_CreateEnsemble(interp, "::cindex::cursor::is", cursorIsNs, 0);

   Tcl_CreateObjCommand(interp, "cindex::cursor::is::null",
			cindexCursorIsNullObjCmd, (ClientData)NULL, NULL);
   Tcl_Export(interp, cursorIsNs, "null", 0);

   struct {
      const char *name;
      unsigned	(*proc)(enum CXCursorKind);
   } isGenericTable[] = {
      { "declaration",		clang_isDeclaration },
      { "reference",		clang_isReference },
      { "expression",		clang_isExpression },
      { "statement",		clang_isStatement },
      { "attribute",		clang_isAttribute },
      { "invalid",		clang_isInvalid },
      { "translationUnit",	clang_isTranslationUnit },
      { "preprocessing",	clang_isPreprocessing },
      { "unexposed",		clang_isUnexposed },
   };
   int numIsGenericTable = sizeof isGenericTable / sizeof isGenericTable[0];

   for (int i = 0; i < numIsGenericTable; ++i) {
      char buffer[80];

      snprintf(buffer, sizeof buffer,
               "cindex::cursor::is::%s", isGenericTable[i].name);
      Tcl_CreateObjCommand(interp, buffer, cindexCursorIsGenericObjCmd,
                           (ClientData)isGenericTable[i].proc, NULL);
      Tcl_Export(interp, cursorIsNs, isGenericTable[i].name, 0);
   }

   struct {
      const char  *name;
      int	 (*proc)(ClientData, Tcl_Interp *, int, Tcl_Obj *const []);
      ClientData   clientData;
   } cursorIsSubCmdTable[] = {
      { "inSystemHeader", cindexCursorIsInSystemHeaderObjCmd },
      { "inMainFile", cindexCursorIsInMainFileObjCmd },
   };

   {
      Tcl_Obj *name =
         Tcl_NewStringObj("cindex::defaultEditingTranslationUnitOptions", -1);
      Tcl_IncrRefCount(name);

      unsigned mask = clang_defaultEditingTranslationUnitOptions();
      int status = cindexBitMaskToString(interp,
                                         cindexNumParseOptions,
                                         cindexParseOptions,
                                         0, NULL, "-none", mask);
      if (status != TCL_OK) {
         Tcl_DecrRefCount(name);
         return status;
      }

      Tcl_Obj *value = Tcl_GetObjResult(interp);
      Tcl_IncrRefCount(value);

      Tcl_ObjSetVar2(interp, name, NULL, value, 0);
      Tcl_Export(interp, cindexNs, "defaultEditingTranslationUnitOptions", 0);

      Tcl_DecrRefCount(name);
      Tcl_DecrRefCount(value);
   }

   Tcl_PkgProvide(interp, "cindex", "1.0");

   return TCL_OK;
}

// Local Variables:
// tab-width: 8
// fill-column: 78
// mode: c
// c-basic-offset: 3
// indent-tabs-mode: nil
// End:
