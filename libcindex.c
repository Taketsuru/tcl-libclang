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
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

//------------------------------------------------------------------ utilities

static unsigned long cindexCStringHash(const char *str)
{
   unsigned long        hash = 0;
   int                  c;
   const unsigned char *cp = (const unsigned char *)str;

   while ( (c = *cp++) ) {
      hash = c + (hash << 6) + (hash << 16) - hash;
   }

   return hash;
}

static Tcl_Obj *cindexMoveCXStringToObj(CXString str)
{
   const char *cstr   = clang_getCString(str);
   Tcl_Obj    *result = Tcl_NewStringObj(cstr, -1);
   clang_disposeString(str);
   return result;
}

struct cindexCommand
{
   const char     *name;
   Tcl_ObjCmdProc *proc;
   ClientData      clientData;
};

static int
cindexDispatchSubcommand(ClientData                  clientData,
                         Tcl_Interp                 *interp,
                         int                         objc,
                         Tcl_Obj *const              objv[],
                         const struct cindexCommand *subcommands)
{
   const char *cmd = Tcl_GetStringFromObj(objv[1], NULL);

   for (int i = 0; subcommands[i].name != NULL; ++i) {
      if (strcmp(subcommands[i].name, cmd) == 0) {
         return subcommands[i].proc(clientData, interp, objc - 1, objv + 1);
      }
   }

   Tcl_Obj *result
      = Tcl_ObjPrintf("unknown subcommand \"%s\": must be ", cmd);
   Tcl_SetObjResult(interp, result);

   for (int i = 0; subcommands[i].name != NULL; ++i) {
      if (i != 0) {
         if (subcommands[i + 1].name != NULL) {
            Tcl_AppendToObj(result, ", ", -1);
         } else {
            Tcl_AppendToObj(result, ", or ", -1);
         }
      }
      Tcl_AppendToObj(result, subcommands[i].name, -1);
   }

   return TCL_ERROR;
}

static void
cindexCreateAndExportCommands(Tcl_Interp           *interp,
                              const char           *commandNameFormat,
                              struct cindexCommand *command)
{
   for (int i = 0; command[i].name != NULL; ++i) {
      char buffer[128];
      if (sizeof buffer
          <= snprintf(buffer, sizeof buffer,
                      commandNameFormat, command[i].name)) {
         Tcl_Panic("command name buffer overflow: %s\n", buffer);
      }
      Tcl_Command token
         = Tcl_CreateObjCommand(interp, buffer, command[i].proc,
                                command[i].clientData, NULL);
      Tcl_CmdInfo info;
      if (! Tcl_GetCommandInfoFromToken(token, &info)) {
         Tcl_Panic("Tcl_GetCommandInfoFromToken failed: %s\n", buffer);
      }
      Tcl_Export(interp, info.namespacePtr, command[i].name, 0);
   }
}

struct cindexNameValuePair
{
   const char *name;
   int         value;
};

static void
cindexCreateNameValueTable
(Tcl_Obj                          **valueToNameDictPtr,
 Tcl_Obj                          **nameToValueDictPtr,
 const struct cindexNameValuePair  *table)
{
   Tcl_Obj *valueToName = *valueToNameDictPtr = Tcl_NewDictObj();
   Tcl_Obj *nameToValue = *nameToValueDictPtr = Tcl_NewDictObj();

   for (int i = 0; table[i].name != NULL; ++i) {
      Tcl_Obj *value = Tcl_NewIntObj(table[i].value);
      Tcl_Obj *name  = Tcl_NewStringObj(table[i].name, -1);

      Tcl_IncrRefCount(value);
      Tcl_IncrRefCount(name);

      Tcl_DictObjPut(NULL, valueToName, value, name);
      Tcl_DictObjPut(NULL, nameToValue, name, value);

      Tcl_DecrRefCount(value);
      Tcl_DecrRefCount(name);
   }
}

//-------------------------------------------------------------- integer types

static Tcl_Obj *cindexNewBignumObj(uintmax_t value, int negative)
{
   mp_int acc;
   mp_init(&acc);

   mp_int digit;
   mp_init(&digit);

   int       digit_bit  = 16;
   uintmax_t digit_mask = (1UL << digit_bit) - 1;

   for (int i = 0; value != 0; i += digit_bit) {
      mp_set(&digit, value & digit_mask);
      mp_mul_2d(&digit, i, &digit);
      mp_or(&digit, &acc, &acc);
      value >>= digit_bit;
   }

   if (negative) {
      mp_neg(&acc, &acc);
   }

   Tcl_Obj *result = Tcl_NewBignumObj(&acc);

   mp_clear(&acc);
   mp_clear(&digit);

   return result;
}

static Tcl_Obj *cindexNewUintmaxObj(uintmax_t value)
{
   if (value <= LONG_MAX) {
      return Tcl_NewLongObj(value);
   }

   return cindexNewBignumObj(value, 0);
}

static Tcl_Obj *cindexNewIntmaxObj(intmax_t value)
{
   if (LONG_MIN <= value && value <= LONG_MAX) {
      return Tcl_NewLongObj(value);
   }

   return 0 <= value
      ? cindexNewBignumObj(value, 0)
      : cindexNewBignumObj(-value, 1);
}

static Tcl_Obj *cindexNewPointerObj(const void *ptr)
{
   return cindexNewUintmaxObj((uintptr_t)ptr);
}

static int
cindexGetIntmaxFromObj(Tcl_Interp *interp, Tcl_Obj *obj,
                       int isSigned, void *bigPtr)
{
   long longv;
   if (Tcl_GetLongFromObj(NULL, obj, &longv) == TCL_OK) {
      if (longv < 0 && ! isSigned) {
         goto out_of_range;
      }
      *(intmax_t *)bigPtr = longv;
      return TCL_OK;
   }

   mp_int bvalue;
   int status = Tcl_GetBignumFromObj(interp, obj, &bvalue);
   if (status != TCL_OK) {
      return status;
   }

   int       digit_bit  = 16;
   uintmax_t digit_mask = (1UL << digit_bit) - 1;

   uintmax_t big = 0;
   for (int i = 0; i < sizeof big * CHAR_BIT; i += digit_bit) {
      uintmax_t digit = (uintmax_t)bvalue.dp[0] & digit_mask;
      mp_div_2d(&bvalue, digit_bit, &bvalue, NULL);
      big |= digit << i;
   }

   int sign   = bvalue.sign;
   int iszero = mp_iszero(&bvalue);

   mp_clear(&bvalue);

   if (! iszero) {
      goto out_of_range;
   }

   if (sign) {
       if (! isSigned || ((intmax_t)big < 0)) {
          goto out_of_range;
       }
       big = 0 - big;
   }

   *(uintmax_t *)bigPtr = big;

   return TCL_OK;

 out_of_range:
   if (interp != NULL) {
      Tcl_SetObjResult(interp, Tcl_NewStringObj("out of range", -1));
   }

   return TCL_ERROR;
}

static int
cindexGetPointerFromObj(Tcl_Interp *interp, Tcl_Obj *obj, void **ptrOut)
{
   uintmax_t intvalue;
   int status = cindexGetIntmaxFromObj(interp, obj, 0, &intvalue);
   if (status != TCL_OK) {
      return status;
   }

   if ((uintptr_t)intvalue != intvalue) {
      if (interp != NULL) {
         Tcl_SetObjResult(interp, Tcl_NewStringObj("out of range", -1));
      }
      return TCL_ERROR;
   }

   *ptrOut = (void *)(uintptr_t)intvalue;

   return TCL_OK;
}

//----------------------------------------------------------------- enum label

struct cindexEnumLabels
{
   Tcl_Obj    **labels;
   int	        n;
   const char  *names[];
};

static Tcl_Obj *cindexGetEnumLabel(struct cindexEnumLabels *labels, int value)
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
      Tcl_Panic("unknown value: %d", value);
   }

   return labels->labels[value];
}

//-------------------------------------------------------- bit mask operations

static Tcl_Obj *cindexNone;        // = "-none"

struct cindexBitMask
{
   const char *name;
   unsigned    mask;
   Tcl_Obj    *nameObj;
};

static Tcl_Obj *cindexBitMaskNameObj(struct cindexBitMask *bitMask)
{
   Tcl_Obj *result = bitMask->nameObj;

   if (result == NULL) {
      result = bitMask->nameObj
             = Tcl_NewStringObj(bitMask->name, -1);
      Tcl_IncrRefCount(result);
   }

   return result;
}

static int cindexParseBitMask(Tcl_Interp           *interp,
                              struct cindexBitMask *options,
                              Tcl_Obj              *none,
                              int                   objc,
                              Tcl_Obj *const        objv[],
                              unsigned             *output)
{
   unsigned value = 0;
   for (int i = 1; i < objc; ++i) {

      const char *arg = Tcl_GetStringFromObj(objv[i], NULL);

      unsigned mask = 0;

      if (none != NULL
          && strcmp(arg, Tcl_GetStringFromObj(none, NULL)) == 0) {
         continue;
      }

      for (int j = 0; options[j].name != NULL; ++j) {
         if (strcmp(options[j].name, arg) == 0) {
            mask = options[j].mask;
            if (mask == 0) {
               mask = 1U << j;
            }
            goto found;
         }
      }

      Tcl_SetObjResult(interp, Tcl_ObjPrintf("unknown option: \"%s\"", arg));

      return TCL_ERROR;

   found:
      value |= mask;
   }

   *output = value;

   return TCL_OK;
}

static int cindexBitMaskToString(Tcl_Interp           *interp,
                                 struct cindexBitMask *options,
                                 Tcl_Obj              *none,
                                 unsigned              mask)
{
   if (mask == 0) {
      if (none != NULL) {
         Tcl_SetObjResult(interp, cindexNone);
      }

      return TCL_OK;
   }

   Tcl_Obj *result = Tcl_NewObj();
   Tcl_SetObjResult(interp, result);

   unsigned value  = mask;
   int      status = TCL_OK;

   int i;
   for (i = 0; options[i].name != NULL && status == TCL_OK; ++i) {
      unsigned mask = options[i].mask;
      if (mask == 0) {
         continue;
      }

      if ((value & mask) == mask) {
         value &= ~mask;
         Tcl_Obj *nameObj = cindexBitMaskNameObj(&options[i]);
         status = Tcl_ListObjAppendElement(interp, result, nameObj);
      }
   }
   int n = i;

   while (value != 0 && status == TCL_OK) {
      int i = ffs(value) - 1;
      if (n <= i || options[i].mask != 0) {
         Tcl_SetObjResult(interp,
                          Tcl_ObjPrintf("unknown mask value: 0x%x", 1U << i));
         status = TCL_ERROR;
         break;
      }
      Tcl_Obj *nameObj = cindexBitMaskNameObj(&options[i]);
      status = Tcl_ListObjAppendElement(interp, result, nameObj);
      value &= ~(1U << i);
   }

   return status;
}

//---------------------------------------------------- source location mapping

static Tcl_Obj *cindexFileNameCache[64];
static Tcl_Obj *cindexSourceLocationTagObj;
static Tcl_Obj *cindexSourceRangeTagObj;
static Tcl_Obj *cindexFilenameNullObj;

static Tcl_Obj *cindexNewFileNameObj(const char *filenameCstr)
{
   int hash = cindexCStringHash(filenameCstr)
      % (sizeof cindexFileNameCache / sizeof cindexFileNameCache[0]);

   Tcl_Obj *candidate = cindexFileNameCache[hash];
   if (candidate != NULL) {
      if (strcmp(Tcl_GetStringFromObj(candidate, NULL), filenameCstr) == 0) {
         return candidate;
      }
      Tcl_DecrRefCount(candidate);
   }

   Tcl_Obj *resultObj = Tcl_NewStringObj(filenameCstr, -1);
   cindexFileNameCache[hash] = resultObj;
   Tcl_IncrRefCount(resultObj);

   return resultObj;
}

static Tcl_Obj *cindexNewSourceLocationObj(CXSourceLocation location)
{
   enum {
      nptrs = sizeof location.ptr_data / sizeof location.ptr_data[0]
   };

   enum {
      tag_ix,
      ptr_data_ix,
      int_data_ix = ptr_data_ix + nptrs,
      nelms
   };

   Tcl_Obj *elms[nelms];
   elms[tag_ix] = cindexSourceLocationTagObj;
   for (int i = 0; i < nptrs; ++i) {
      elms[ptr_data_ix + i] = cindexNewPointerObj(location.ptr_data[i]);
   }
   elms[int_data_ix] = Tcl_NewLongObj(location.int_data);

   return Tcl_NewListObj(nelms, elms);
}

static int cindexGetSourceLocationFromObj(Tcl_Interp       *interp,
                                          Tcl_Obj          *obj,
                                          CXSourceLocation *location)
{
   enum {
      nptrs = sizeof location->ptr_data / sizeof location->ptr_data[0]
   };

   enum {
      tag_ix,
      ptr_data_ix,
      int_data_ix = ptr_data_ix + nptrs,
      nelms
   };

   int       size;
   Tcl_Obj **elms;
   int status = Tcl_ListObjGetElements(interp, obj, &size, &elms);
   if (status != TCL_OK) {
      return status;
   }

   if (size != nelms
       || (elms[tag_ix] != cindexSourceLocationTagObj
           && strcmp(Tcl_GetStringFromObj(elms[tag_ix], NULL),
                     Tcl_GetStringFromObj(cindexSourceLocationTagObj, NULL))
           != 0)) {
      goto invalid;
   }

   for (int i = 0; i < nptrs; ++i) {
      status = cindexGetPointerFromObj
         (interp, elms[ptr_data_ix + i], (void **)&location->ptr_data[i]);
      if (status != TCL_OK) {
         return status;
      }
   }

   long value;
   status = Tcl_GetLongFromObj(interp, elms[int_data_ix], &value);
   if (status != TCL_OK) {
      return status;
   }

   if (value < 0 || UINT_MAX < value) {
      goto invalid;
   }

   location->int_data = value;

   return status;

 invalid:
   if (interp != NULL) {
      Tcl_SetObjResult(interp, Tcl_NewStringObj("invalid source location", -1));
   }

   return TCL_ERROR;
}

static Tcl_Obj *cindexNewSourceRangeObj(CXSourceRange range)
{
   enum {
      nptrs = sizeof range.ptr_data / sizeof range.ptr_data[0]
   };

   enum {
      tag_ix,
      ptr_data_ix,
      begin_int_data_ix = ptr_data_ix + nptrs,
      end_int_data_ix,
      nelms
   };

   Tcl_Obj *elms[nelms];

   elms[tag_ix] = cindexSourceRangeTagObj;

   for (int i = 0; i < nptrs; ++i) {
      elms[ptr_data_ix + i] = cindexNewPointerObj(range.ptr_data[i]);
   }

   elms[begin_int_data_ix] = Tcl_NewLongObj(range.begin_int_data);
   elms[end_int_data_ix]   = Tcl_NewLongObj(range.end_int_data);

   return Tcl_NewListObj(nelms, elms);
}

static int cindexGetSourceRangeFromObj(Tcl_Interp    *interp,
                                       Tcl_Obj       *obj,
                                       CXSourceRange *range)
{
   enum {
      nptrs = sizeof range->ptr_data / sizeof range->ptr_data[0]
   };

   enum {
      tag_ix,
      ptr_data_ix,
      begin_int_data_ix = ptr_data_ix + nptrs,
      end_int_data_ix,
      nelms
   };

   int       size;
   Tcl_Obj **elms;
   int status = Tcl_ListObjGetElements(interp, obj, &size, &elms);
   if (status != TCL_OK) {
      return status;
   }

   if (size != nelms
       || (elms[tag_ix] != cindexSourceRangeTagObj
           && strcmp(Tcl_GetStringFromObj(elms[tag_ix], NULL),
                     Tcl_GetStringFromObj(cindexSourceRangeTagObj, NULL))
           != 0)) {
      goto invalid;
   }

   for (int i = 0; i < nptrs; ++i) {
      status = cindexGetPointerFromObj
         (interp, elms[ptr_data_ix + i], (void **)&range->ptr_data[i]);
      if (status != TCL_OK) {
         return status;
      }
   }

   unsigned *intdataptr[] = {
      &range->begin_int_data,
      &range->end_int_data
   };

   for (int i = 0; i < 2; ++i) {
      long value;
      status = Tcl_GetLongFromObj(interp, elms[begin_int_data_ix + i], &value);
      if (status != TCL_OK) {
         return status;
      }

      if (value < 0 || UINT_MAX < value) {
         goto invalid;
      }

      *intdataptr[i] = value;
   }

   return status;

 invalid:
   if (interp != NULL) {
      Tcl_SetObjResult(interp, Tcl_NewStringObj("invalid source range", -1));
   }

   return TCL_ERROR;
}

static Tcl_Obj *cindexNewPresumedLocationObj(CXSourceLocation location)
{
   enum {
      filename_ix,
      line_ix,
      column_ix,
      nelms
   };
   Tcl_Obj *elms[nelms];

   if (clang_equalLocations(location, clang_getNullLocation())) {

      elms[filename_ix]  = cindexFilenameNullObj;
      elms[line_ix]      = 
         elms[column_ix] = Tcl_NewIntObj(0);

   } else {

      CXString filename;
      unsigned line;
      unsigned column;
      clang_getPresumedLocation(location, &filename, &line, &column);


      const char *filenameCstr = clang_getCString(filename);
      elms[filename_ix]        = cindexNewFileNameObj(filenameCstr);
      clang_disposeString(filename);

      elms[line_ix]   = Tcl_NewLongObj(line);
      elms[column_ix] = Tcl_NewLongObj(column);

   }

   return Tcl_NewListObj(nelms, elms);
}

static Tcl_Obj *cindexNewDecodedSourceLocationObj(CXFile   file,
                                                  unsigned line,
                                                  unsigned column,
                                                  unsigned offset)
{
   enum {
      filename_ix,
      line_ix,
      column_ix,
      offset_ix,
      nelms
   };

   Tcl_Obj *elms[nelms];

   if (file == NULL) {
      elms[filename_ix] = cindexFilenameNullObj;
   } else {
      CXString    filename     = clang_getFileName(file);
      const char *filenameCstr = clang_getCString(filename);
      elms[filename_ix]        = cindexNewFileNameObj(filenameCstr);
      clang_disposeString(filename);
   }

   elms[line_ix]   = Tcl_NewLongObj(line);
   elms[column_ix] = Tcl_NewLongObj(column);
   elms[offset_ix] = Tcl_NewLongObj(offset);

   return Tcl_NewListObj(nelms, elms);
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

static struct cindexIndexInfo *
cindexCreateIndexInfo(Tcl_Interp *interp, CXIndex index)
{
   struct cindexIndexInfo *info
      = (struct cindexIndexInfo *)Tcl_Alloc(sizeof *info);

   info->interp   = interp;
   info->children = Tcl_NewObj();
   info->index    = index;

   Tcl_IncrRefCount(info->children);

   return info;
}

/** A callback function called when an index Tcl command is deleted.
 * 
 * \param clientData pointer to struct cindexIndexInfo
 */
static void cindexIndexDeleteProc(ClientData clientData)
{
   int status = TCL_OK;

   struct cindexIndexInfo *info = (struct cindexIndexInfo *)clientData;

   Tcl_Interp *interp = info->interp;

   // Until info->children becomes empty, delete the last command in the list.
   for (;;) {
      int n = 0;
      status = Tcl_ListObjLength(interp, info->children, &n);
      if (status != TCL_OK || n <= 0) {
         break;
      }

      Tcl_Obj *child = NULL;
      status = Tcl_ListObjIndex(interp, info->children, n - 1, &child);
      if (status != TCL_OK) {
         break;
      }

      Tcl_IncrRefCount(child);
      Tcl_DeleteCommand(interp, Tcl_GetStringFromObj(child, NULL));
      Tcl_DecrRefCount(child);
   }

   if (status != TCL_OK) {
      Tcl_BackgroundException(interp, status);
   }

   clang_disposeIndex(info->index);
   Tcl_DecrRefCount(info->children);
   Tcl_Free((char *)info);
}

static void
cindexIndexAddChild(struct cindexIndexInfo *info, Tcl_Obj *child)
{
   Tcl_Interp *interp = info->interp;

   Tcl_Obj *children = info->children;
   if (Tcl_IsShared(children)) {
      Tcl_Obj *newChildren = Tcl_DuplicateObj(children);
      Tcl_DecrRefCount(children);
      Tcl_IncrRefCount(newChildren);
      info->children = children = newChildren;
   }

   int status = Tcl_ListObjAppendElement(interp, children, child);

   if (status != TCL_OK) {
      Tcl_BackgroundException(interp, status);
   }
}

static void
cindexIndexRemoveChild(struct cindexIndexInfo *info, Tcl_Obj *child)
{
   Tcl_Interp *interp = info->interp;

   Tcl_Obj *children = info->children;
   if (Tcl_IsShared(children)) {
      Tcl_Obj *newChildren = Tcl_DuplicateObj(children);
      Tcl_DecrRefCount(children);
      Tcl_IncrRefCount(newChildren);
      info->children = children = newChildren;
   }

   int n       = 0;
   int status  = Tcl_ListObjLength(interp, children, &n);
   if (status != TCL_OK) {
      goto end;
   }

   int         child_strlen = 0;
   const char *child_str    = Tcl_GetStringFromObj(child, &child_strlen);
   int         found        = 0;
   for (int i = n - 1; 0 <= i; --i) {

      Tcl_Obj *elm = NULL;
      status       = Tcl_ListObjIndex(interp, children, i, &elm);
      if (status != TCL_OK) {
         break;
      }

      int         elm_strlen = 0;
      const char *elm_str    = Tcl_GetStringFromObj(elm, &elm_strlen);

      if (elm_str == child_str
          || (elm_strlen == child_strlen
              && memcmp(elm_str, child_str, elm_strlen) == 0)) {
         found = 1;
         status = Tcl_ListObjReplace(interp, children, i, 1, 0, NULL);
         break;
      }

   }

   if (status == TCL_OK && ! found) {
      Tcl_Panic("child %s doesn't exist in the children list\n", child_str);
   }

 end:
   if (status != TCL_OK) {
      Tcl_BackgroundException(interp, status);
   }
}

//--------------------------------------------------- translation unit mapping

struct cindexTUInfo
{
   struct cindexTUInfo    *next;
   struct cindexIndexInfo *parent;
   Tcl_Obj                *name;
   CXTranslationUnit       translationUnit;
};

static struct cindexTUInfo *cindexTUHashTable[256];

static int cindexTUHash(CXTranslationUnit tu)
{
   int hashTableSize = sizeof cindexTUHashTable / sizeof cindexTUHashTable[0];
   return ((uintptr_t)tu / (sizeof(void *) * 4)) % hashTableSize;
}

static struct cindexTUInfo *
cindexCreateTUInfo(struct cindexIndexInfo *parent,
                   Tcl_Obj                *tuName,
                   CXTranslationUnit       tu)
{
   struct cindexTUInfo *info = (struct cindexTUInfo *)Tcl_Alloc(sizeof *info);

   info->parent          = parent;
   info->name            = tuName;
   info->translationUnit = tu;

   Tcl_IncrRefCount(tuName);

   int hash                = cindexTUHash(tu);
   info->next              = cindexTUHashTable[hash];
   cindexTUHashTable[hash] = info;

   cindexIndexAddChild(parent, tuName);

   return info;
}

static void cindexTUDeleteProc(ClientData clientData)
{
   struct cindexTUInfo *info = (struct cindexTUInfo *)clientData;

   cindexIndexRemoveChild(info->parent, info->name);

   Tcl_DecrRefCount(info->name);
   clang_disposeTranslationUnit(info->translationUnit);

   int                   hash = cindexTUHash(info->translationUnit);
   struct cindexTUInfo **prev = &cindexTUHashTable[hash];
   while (*prev != info) {
      prev = &info->next;
   }
   *prev = info->next;

   Tcl_Free((char *)info);
}

static struct cindexTUInfo *
cindexLookupTranslationUnit(CXTranslationUnit tu)
{
   int hash = cindexTUHash(tu);
   for (struct cindexTUInfo *p = cindexTUHashTable[hash];
        p != NULL;
        p = p->next) {
      if (p->translationUnit == tu) {
         return p;
      }
   }

   return NULL;
}

//------------------------------------------------------------- cursor mapping

static Tcl_Obj *cindexCursorKindNames;
static Tcl_Obj *cindexCursorKindValues;

static void cindexCreateCursorKindTable(void)
{
   static struct cindexNameValuePair table[] = {
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

   cindexCreateNameValueTable
      (&cindexCursorKindNames, &cindexCursorKindValues, table);

   Tcl_IncrRefCount(cindexCursorKindNames);
   Tcl_IncrRefCount(cindexCursorKindValues);
}

static Tcl_Obj *cindexNewCursorObj(CXCursor cursor)
{
   enum {
      ndata = sizeof cursor.data / sizeof cursor.data[0]
   };

   enum {
      kind_ix,
      xdata_ix,
      data_ix,
      nelms = data_ix + ndata
   };

   Tcl_Obj *elms[nelms];

   Tcl_Obj *kind = Tcl_NewIntObj(cursor.kind);
   Tcl_IncrRefCount(kind);
   Tcl_Obj *kindName;
   if (Tcl_DictObjGet(NULL, cindexCursorKindNames, kind, &kindName) != TCL_OK) {
      Tcl_Panic("cursor kind %d is not valid", cursor.kind);
   }
   Tcl_DecrRefCount(kind);
   elms[kind_ix] = kindName;
   elms[xdata_ix] = Tcl_NewLongObj(cursor.xdata);

   for (int i = 0; i < ndata; ++i) {
      elms[data_ix + i] = cindexNewPointerObj(cursor.data[i]);
   }

   return Tcl_NewListObj(nelms, elms);
}

static int
cindexGetCursorFromObj(Tcl_Interp *interp, Tcl_Obj *obj, CXCursor *cursor)
{
   CXCursor result = { 0 };

   enum {
      ndata = sizeof result.data / sizeof result.data[0]
   };

   enum {
      kind_ix,
      xdata_ix,
      data_ix,
      nelms = data_ix + ndata,
   };

   int       n;
   Tcl_Obj **elms;
   int status = Tcl_ListObjGetElements(interp, obj, &n, &elms);
   if (status != TCL_OK) {
      return status;
   }

   Tcl_Obj* kindObj = NULL;
   if (Tcl_DictObjGet(NULL, cindexCursorKindValues, elms[kind_ix], &kindObj)
       != TCL_OK) {
      Tcl_Panic("cindexCursorKindValues corrupted");
   }

   if (kindObj == NULL) {
      Tcl_SetObjResult
         (interp,
          Tcl_ObjPrintf("invalid cursor kind: %s",
                        Tcl_GetStringFromObj(elms[kind_ix], NULL)));
      return TCL_ERROR;
   }

   int kind;
   status = Tcl_GetIntFromObj(NULL, kindObj, &kind);
   if (status != TCL_OK) {
      Tcl_Panic("cindexCursorKindValues corrupted");
   }
   result.kind = kind;

   status = Tcl_GetIntFromObj(NULL, elms[xdata_ix], &result.xdata);
   if (status != TCL_OK) {
      goto invalid_cursor;
   }

   for (int i = 0; i < ndata; ++i) {
      status = cindexGetPointerFromObj
         (NULL, elms[data_ix + i], (void **)&result.data[i]);
      if (status != TCL_OK) {
         goto invalid_cursor;
      }
   }

   CXTranslationUnit tu = clang_Cursor_getTranslationUnit(result);
   if (cindexLookupTranslationUnit(tu) == NULL) {
      goto invalid_cursor;
   }

   *cursor = result;

   return TCL_OK;

 invalid_cursor:
   Tcl_SetObjResult(interp,
                    Tcl_NewStringObj("invalid cursor object", -1));
   return TCL_ERROR;
}

//----------------------------------------------------------------- diagnostic

static struct cindexEnumLabels cindexDiagnosticSeverityLabels = {
   .names = {
      "Ignored",
      "Note",
      "Warning",
      "Error",
      "Fatal"
   }
};

static Tcl_Obj *cindexDiagnosticSeverityTagObj;
static Tcl_Obj *cindexDiagnosticLocationTagObj;
static Tcl_Obj *cindexDiagnosticSpellingTagObj;
static Tcl_Obj *cindexDiagnosticOptionTagObj;
static Tcl_Obj *cindexDiagnosticDisableTagObj;
static Tcl_Obj *cindexDiagnosticCategoryTagObj;
static Tcl_Obj *cindexDiagnosticRangesTagObj;
static Tcl_Obj *cindexDiagnosticFixItsTagObj;

static Tcl_Obj *cindexNewDiagnosticObj(CXDiagnostic diagnostic)
{
   enum {
      severity_tag_ix,
      severity_ix,
      location_tag_ix,
      location_ix,
      spelling_tag_ix,
      spelling_ix,
      option_tag_ix,
      option_ix,
      disable_tag_ix,
      disable_ix,
      category_tag_ix,
      category_ix,
      ranges_tag_ix,
      ranges_ix,
      fixits_tag_ix,
      fixits_ix,
      nelms
   };
   Tcl_Obj *resultArray[nelms];

   resultArray[severity_tag_ix] = cindexDiagnosticSeverityTagObj;
   resultArray[location_tag_ix] = cindexDiagnosticLocationTagObj;
   resultArray[spelling_tag_ix] = cindexDiagnosticSpellingTagObj;
   resultArray[option_tag_ix]   = cindexDiagnosticOptionTagObj;
   resultArray[disable_tag_ix]  = cindexDiagnosticDisableTagObj;
   resultArray[category_tag_ix] = cindexDiagnosticCategoryTagObj;
   resultArray[ranges_tag_ix]   = cindexDiagnosticRangesTagObj;
   resultArray[fixits_tag_ix]   = cindexDiagnosticFixItsTagObj;

   enum CXDiagnosticSeverity severity
      = clang_getDiagnosticSeverity(diagnostic);
   resultArray[severity_ix]
      = cindexGetEnumLabel(&cindexDiagnosticSeverityLabels, severity);

   CXSourceLocation location = clang_getDiagnosticLocation(diagnostic);
   resultArray[location_ix]  = cindexNewSourceLocationObj(location);

   CXString spelling        = clang_getDiagnosticSpelling(diagnostic);
   resultArray[spelling_ix] = cindexMoveCXStringToObj(spelling);

   CXString disable;
   CXString option         = clang_getDiagnosticOption(diagnostic, &disable);
   resultArray[option_ix]  = cindexMoveCXStringToObj(option);
   resultArray[disable_ix] = cindexMoveCXStringToObj(disable);

   CXString category        = clang_getDiagnosticCategoryText(diagnostic);
   resultArray[category_ix] = cindexMoveCXStringToObj(category);

   unsigned numRanges = clang_getDiagnosticNumRanges(diagnostic);
   Tcl_Obj **ranges = (Tcl_Obj **)Tcl_Alloc(sizeof(Tcl_Obj *) * numRanges);
   for (unsigned i = 0; i < numRanges; ++i) {
      CXSourceRange range = clang_getDiagnosticRange(diagnostic, i);
      ranges[i] = cindexNewSourceRangeObj(range);
   }
   resultArray[ranges_ix] = Tcl_NewListObj(numRanges, ranges);
   Tcl_Free((char *)ranges);

   unsigned   numFixIts = clang_getDiagnosticNumFixIts(diagnostic);
   Tcl_Obj  **fixits    = (Tcl_Obj **)Tcl_Alloc(sizeof(Tcl_Obj *) * numFixIts);
   for (unsigned i = 0; i < numFixIts; ++i) {
      CXSourceRange range;
      CXString      fixitStr = clang_getDiagnosticFixIt(diagnostic, i, &range);

      Tcl_Obj *fixit[2];
      fixit[0] = cindexMoveCXStringToObj(fixitStr);
      fixit[1] = cindexNewSourceRangeObj(range);

      fixits[i] = Tcl_NewListObj(2, fixit);
   }
   resultArray[fixits_ix] = Tcl_NewListObj(numFixIts, fixits);
   Tcl_Free((char *)fixits);

   return Tcl_NewListObj(nelms, resultArray);
}

//------------------------------------------------------------- type mapping

static Tcl_Obj *cindexTypeKindValues;
static Tcl_Obj *cindexTypeKindNames;

void cindexCreateCXTypeTable(void)
{
   static struct cindexNameValuePair table[] = {
      { "Invalid",
        CXType_Invalid },
      { "Unexposed",
        CXType_Unexposed },
      { "Void",
        CXType_Void },
      { "Bool",
        CXType_Bool },
      { "Char_U",
        CXType_Char_U },
      { "UChar",
        CXType_UChar },
      { "Char16",
        CXType_Char16 },
      { "Char32",
        CXType_Char32 },
      { "UShort",
        CXType_UShort },
      { "UInt",
        CXType_UInt },
      { "ULong",
        CXType_ULong },
      { "ULongLong",
        CXType_ULongLong },
      { "UInt128",
        CXType_UInt128 },
      { "Char_S",
        CXType_Char_S },
      { "SChar",
        CXType_SChar },
      { "WChar",
        CXType_WChar },
      { "Short",
        CXType_Short },
      { "Int",
        CXType_Int },
      { "Long",
        CXType_Long },
      { "LongLong",
        CXType_LongLong },
      { "Int128",
        CXType_Int128 },
      { "Float",
        CXType_Float },
      { "Double",
        CXType_Double },
      { "LongDouble",
        CXType_LongDouble },
      { "NullPtr",
        CXType_NullPtr },
      { "Overload",
        CXType_Overload },
      { "Dependent",
        CXType_Dependent },
      { "ObjCId",
        CXType_ObjCId },
      { "ObjCClass",
        CXType_ObjCClass },
      { "ObjCSel",
        CXType_ObjCSel },
      { "Complex",
        CXType_Complex },
      { "Pointer",
        CXType_Pointer },
      { "BlockPointer",
        CXType_BlockPointer },
      { "LValueReference",
        CXType_LValueReference },
      { "RValueReference",
        CXType_RValueReference },
      { "Record",
        CXType_Record },
      { "Enum",
        CXType_Enum },
      { "Typedef",
        CXType_Typedef },
      { "ObjCInterface",
        CXType_ObjCInterface },
      { "ObjCObjectPointer",
        CXType_ObjCObjectPointer },
      { "FunctionNoProto",
        CXType_FunctionNoProto },
      { "FunctionProto",
        CXType_FunctionProto },
      { "ConstantArray",
        CXType_ConstantArray },
      { "Vector",
        CXType_Vector },
      { "IncompleteArray",
        CXType_IncompleteArray },
      { "VariableArray",
        CXType_VariableArray },
      { "DependentSizedArray",
        CXType_DependentSizedArray },
      { "MemberPointer",
        CXType_MemberPointer },
      { NULL }
   };

   cindexCreateNameValueTable
      (&cindexTypeKindNames, &cindexTypeKindValues, table);

   Tcl_IncrRefCount(cindexTypeKindNames);
   Tcl_IncrRefCount(cindexTypeKindValues);
}

Tcl_Obj *cindexNewCXTypeObj(CXType type)
{
   Tcl_Obj *kind = Tcl_NewIntObj(type.kind);
   Tcl_IncrRefCount(kind);
   Tcl_Obj *kindName;
   if (Tcl_DictObjGet(NULL, cindexTypeKindNames, kind, &kindName) != TCL_OK
       || kindName == NULL) {
      Tcl_Panic("cindexTypeKindNames(%d) corrupted", type.kind);
   }
   Tcl_DecrRefCount(kind);

   enum {
      ndata = sizeof type.data / sizeof type.data[0]
   };

   enum {
      kind_ix,
      data_ix,
      nelms = data_ix + ndata
   };

   Tcl_Obj* elements[nelms];
   elements[kind_ix] = kindName;
   for (int i = 0; i < ndata; ++i) {
      elements[data_ix + i] = cindexNewPointerObj(type.data[i]);
   }

   return Tcl_NewListObj(nelms, elements);
}

int cindexGetCXTypeObj(Tcl_Interp *interp, Tcl_Obj *obj, CXType *output)
{
   CXType result = { 0 };

   enum {
      ndata = sizeof result.data / sizeof result.data[0]
   };

   enum {
      kind_ix,
      data_ix,
      nelms = data_ix + ndata
   };

   int       nelms_actual = 0;
   Tcl_Obj **elms         = NULL;
   if (Tcl_ListObjGetElements(NULL, obj, &nelms_actual, &elms) != TCL_OK
       || nelms_actual != nelms) {
      goto invalid_type;
   }

   Tcl_Obj *kindObj;
   if (Tcl_DictObjGet(NULL, cindexTypeKindValues, elms[kind_ix], &kindObj)
       != TCL_OK
       || kindObj == NULL) {
      goto invalid_type;
   }

   int kind;
   int status = Tcl_GetIntFromObj(interp, kindObj, &kind);
   if (status != TCL_OK) {
      goto invalid_type;
   }
   result.kind = kind;

   for (int i = 0; i < ndata; ++i) {
      if (cindexGetPointerFromObj(NULL, elms[data_ix + i], &result.data[i])
          != TCL_OK) {
         goto invalid_type;
      }
   }

   *output = result;

   return TCL_OK;

 invalid_type:
   Tcl_SetObjResult(interp,
                    Tcl_NewStringObj("invalid type object", -1));
   return TCL_ERROR;
}

//----------------------------------------------------------------------------

static struct cindexEnumLabels cindexAvailabilityLabels = {
   .names = {
      "Available",
      "Deprecated",
      "NotAvailable",
      "NotAccessible",
      NULL
   }
};

static struct cindexEnumLabels cindexLinkageLables = {
   .names = {
      "Invalid",
      "NoLinkage",
      "Internal",
      "UniqueExternal",
      "External",
      NULL
   }
};

static struct cindexEnumLabels cindexLanguageLables = {
   .names = {
      "Invalid",
      "C",
      "ObjC",
      "CPlusPlus",
      NULL
   }
};

static struct cindexBitMask cindexObjCPropertyAttributes[] = {
   { "readonly" },
   { "getter" },
   { "assign" },
   { "readwrite" },
   { "retain" },
   { "copy" },
   { "nonatomic" },
   { "setter" },
   { "atomic" },
   { "weak" },
   { "strong" },
   { "unsafe_unretained" },
   { NULL },
};

static struct cindexBitMask cindexObjCDeclQualifiers[] = {
   { "in" },
   { "inout" },
   { "out" },
   { "bycopy" },
   { "byref" },
   { "oneway" },
   { NULL },
};

//-------------------------------------------------- range -> location command

typedef CXSourceLocation (*CindexRangeToLocation)(CXSourceRange); 

static int cindexGenericRangeToLocationObjCmd(ClientData     clientData,
                                              Tcl_Interp    *interp,
                                              int            objc,
                                              Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "range");
      return TCL_ERROR;
   }

   CXSourceRange range;
   int status = cindexGetSourceRangeFromObj(interp, objv[1], &range);
   if (status != TCL_OK) {
      return status;
   }

   CXSourceLocation  location  = ((CindexRangeToLocation)clientData)(range);
   Tcl_Obj          *resultObj = cindexNewSourceLocationObj(location);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//----------------------------------------- cindex::location::presumedLocation

static int cindexMaybeSourceRange(Tcl_Obj *x)
{
   Tcl_Obj *tagObj;

   return Tcl_ListObjIndex(NULL, x, 0, &tagObj) == TCL_OK
      && tagObj != cindexSourceLocationTagObj
      && (tagObj == cindexSourceRangeTagObj
          || strcmp(Tcl_GetStringFromObj(tagObj, NULL),
                    Tcl_GetStringFromObj(cindexSourceRangeTagObj, NULL)) == 0);
}

static int cindexLocationPresumedLocationObjCmd(ClientData     clientData,
                                                Tcl_Interp    *interp,
                                                int            objc,
                                                Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "location");
      return TCL_ERROR;
   }

   if (cindexMaybeSourceRange(objv[1])) {

      CXSourceRange range;
      int status = cindexGetSourceRangeFromObj(interp, objv[1], &range);
      if (status != TCL_OK) {
         return status;
      }

      CXSourceLocation start = clang_getRangeStart(range);
      CXSourceLocation end   = clang_getRangeEnd(range);

      Tcl_Obj *elms[2];
      elms[0] = cindexNewPresumedLocationObj(start);
      elms[1] = cindexNewPresumedLocationObj(end);

      Tcl_Obj *resultObj = Tcl_NewListObj(2, elms);
      Tcl_SetObjResult(interp, resultObj);

   } else {

      CXSourceLocation location;
      int status = cindexGetSourceLocationFromObj(interp, objv[1], &location);
      if (status != TCL_OK) {
         return status;
      }

      Tcl_Obj *resultObj = cindexNewPresumedLocationObj(location);
      Tcl_SetObjResult(interp, resultObj);

   }

   return TCL_OK;
}

//-------------------------------------------- generic location decode command

typedef void (*CindexLocationDecodeProc)
	(CXSourceLocation, CXFile*, unsigned *, unsigned *, unsigned *);

static Tcl_Obj *cindexNewDecodedRangeObj(CindexLocationDecodeProc proc,
                                         CXSourceRange            range)
{
   CXSourceLocation locs[2];
   locs[0] = clang_getRangeStart(range);
   locs[1] = clang_getRangeEnd(range);

   Tcl_Obj *elms[2];
   for (int i = 0; i < 2; ++i) {
      CXFile   file;
      unsigned line;
      unsigned column;
      unsigned offset;
      proc(locs[i], &file, &line, &column, &offset);

      elms[i] = cindexNewDecodedSourceLocationObj(file, line, column, offset);
   }

   return Tcl_NewListObj(2, elms);
}

static int cindexLocationGenericDecodeObjCmd(ClientData     clientData,
                                             Tcl_Interp    *interp,
                                             int            objc,
                                             Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "location/range");
      return TCL_ERROR;
   }

   if (cindexMaybeSourceRange(objv[1])) {

      CXSourceRange range;
      int status = cindexGetSourceRangeFromObj(interp, objv[1], &range);
      if (status != TCL_OK) {
         return status;
      }

      Tcl_Obj *resultObj
         = cindexNewDecodedRangeObj((CindexLocationDecodeProc)clientData,
                                    range);
      Tcl_SetObjResult(interp, resultObj);

   } else {

      CXSourceLocation location;
      int status = cindexGetSourceLocationFromObj(interp, objv[1], &location);
      if (status != TCL_OK) {
         return status;
      }

      CXFile   file;
      unsigned line;
      unsigned column;
      unsigned offset;
      ((CindexLocationDecodeProc)clientData)
         (location, &file, &line, &column, &offset);

      Tcl_Obj *resultObj
         = cindexNewDecodedSourceLocationObj(file, line, column, offset);
      Tcl_SetObjResult(interp, resultObj);

   }

   return TCL_OK;
}

//------------------------------------------------- cindex::location::is::null

static int cindexLocationIsNullObjCmd(ClientData     clientData,
                                      Tcl_Interp    *interp,
                                      int            objc,
                                      Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "location/range");
      return TCL_ERROR;
   }

   if (cindexMaybeSourceRange(objv[1])) {

      CXSourceRange range;
      int status = cindexGetSourceRangeFromObj(interp, objv[1], &range);
      if (status != TCL_OK) {
         return status;
      }

      unsigned  result    = clang_equalRanges(range, clang_getNullRange());
      Tcl_Obj  *resultObj = Tcl_NewLongObj(result);
      Tcl_SetObjResult(interp, resultObj);

   } else {

      CXSourceLocation location;
      int status = cindexGetSourceLocationFromObj(interp, objv[1], &location);
      if (status != TCL_OK) {
         return status;
      }

      unsigned  result    = clang_equalLocations(location,
                                                 clang_getNullLocation());
      Tcl_Obj  *resultObj = Tcl_NewLongObj(result);
      Tcl_SetObjResult(interp, resultObj);

   }

   return TCL_OK;
}

//----------------------------------------------- location -> unsigned command

static int cindexGenericLocationToUnsignedObjCmd(ClientData     clientData,
                                                 Tcl_Interp    *interp,
                                                 int            objc,
                                                 Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "location/range");
      return TCL_ERROR;
   }

   CXSourceLocation location;
   int status = cindexGetSourceLocationFromObj(interp, objv[1], &location);
   if (status != TCL_OK) {
      return status;
   }

   unsigned  result    = ((unsigned (*)(CXSourceLocation))clientData)(location);
   Tcl_Obj  *resultObj = Tcl_NewLongObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//---------------------------------------------------- cindex::location::equal

static int cindexLocationEqualObjCmd(ClientData     clientData,
                                     Tcl_Interp    *interp,
                                     int            objc,
                                     Tcl_Obj *const objv[])
{
   if (objc != 3) {
      Tcl_WrongNumArgs(interp, 1, objv, "location1 location2");
      return TCL_ERROR;
   }

   CXSourceLocation locations[2];
   for (int i = 0; i < 2; ++i) {
      int status
         = cindexGetSourceLocationFromObj(interp, objv[1 + i], &locations[i]);
      if (status != TCL_OK) {
         return status;
      }
   }

   unsigned  result    = clang_equalLocations(locations[0], locations[1]);
   Tcl_Obj  *resultObj = Tcl_NewLongObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//------------------------------------------------------- cindex::range::equal

static int cindexRangeEqualObjCmd(ClientData     clientData,
                                  Tcl_Interp    *interp,
                                  int            objc,
                                  Tcl_Obj *const objv[])
{
   if (objc != 3) {
      Tcl_WrongNumArgs(interp, 1, objv, "range1 range2");
      return TCL_ERROR;
   }

   CXSourceRange ranges[2];
   for (int i = 0; i < 2; ++i) {
      int status = cindexGetSourceRangeFromObj(interp, objv[1 + i], &ranges[i]);
      if (status != TCL_OK) {
         return status;
      }
   }

   unsigned  result    = clang_equalRanges(ranges[0], ranges[1]);
   Tcl_Obj  *resultObj = Tcl_NewLongObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//------------------------------------ cindex::<translationUnit instance> save

static int cindexTUSaveObjCmd(ClientData     clientData,
                              Tcl_Interp    *interp,
                              int            objc,
                              Tcl_Obj *const objv[])
{
   if (objc < 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "filename");
      return TCL_ERROR;
   }

   const char *filename = Tcl_GetStringFromObj(objv[1], NULL);

   struct cindexTUInfo *info = (struct cindexTUInfo *)clientData;

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

static int cindexTUReparseObjCmd(ClientData     clientData,
                                 Tcl_Interp    *interp,
                                 int            objc,
                                 Tcl_Obj *const objv[])
{
   if (objc != 1) {
      Tcl_WrongNumArgs(interp, 1, objv, "");
      return TCL_ERROR;
   }

   struct cindexTUInfo *info = (struct cindexTUInfo *)clientData;

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

static int cindexTUResourceUsageObjCmd(ClientData     clientData,
                                       Tcl_Interp    *interp,
                                       int            objc,
                                       Tcl_Obj *const objv[])
{
   if (objc != 1) {
      Tcl_WrongNumArgs(interp, 1, objv, "");
      return TCL_ERROR;
   }

   struct cindexTUInfo *info = (struct cindexTUInfo *)clientData;

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

static int cindexTUCursorObjCmd(ClientData     clientData,
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

   struct cindexTUInfo *info = (struct cindexTUInfo *)clientData;

   CXCursor  cursor    = clang_getTranslationUnitCursor(info->translationUnit);
   Tcl_Obj  *cursorObj = cindexNewCursorObj(cursor);
   Tcl_SetObjResult(interp, cursorObj);

   return TCL_OK;
}

//---------------- cindex::<translationUnit instance> isMultipleIncludeGuarded

static int cindexTUIsMultipleIncludeGuardedObjCmd(ClientData     clientData,
                                                  Tcl_Interp    *interp,
                                                  int            objc,
                                                  Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "filename");
      return TCL_ERROR;
   }

   struct cindexTUInfo *info = (struct cindexTUInfo *)clientData;
   CXFile file = clang_getFile(info->translationUnit,
                               Tcl_GetStringFromObj(objv[1], NULL));
   unsigned result
      = clang_isFileMultipleIncludeGuarded(info->translationUnit, file);
   Tcl_Obj *resultObj = Tcl_NewIntObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//---------------------------- cindex::<translationUnit instance> fileUniqueID

static int cindexTUFileUniqueIDObjCmd(ClientData     clientData,
                                      Tcl_Interp    *interp,
                                      int            objc,
                                      Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "filename");
      return TCL_ERROR;
   }

   struct cindexTUInfo *info = (struct cindexTUInfo *)clientData;
   CXFile file = clang_getFile(info->translationUnit,
                               Tcl_GetStringFromObj(objv[1], NULL));
   CXFileUniqueID uniqueId;
   if (clang_getFileUniqueID(file, &uniqueId)) {
      Tcl_SetObjResult(interp,
                       Tcl_NewStringObj("failed to get file unique ID.", -1));
      return TCL_ERROR;
   }

   enum {
      nelms = sizeof uniqueId.data / sizeof uniqueId.data[0]
   };
   Tcl_Obj *elms[nelms];
   for (int i = 0; i < nelms; ++i) {
      elms[i] = cindexNewUintmaxObj(uniqueId.data[i]);
   }
   Tcl_Obj *resultObj = Tcl_NewListObj(nelms, elms);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//----------------------- cindex::<translationUnit instance> diagnostic number

static int cindexTUDiagnosticNumberObjCmd(ClientData     clientData,
                                          Tcl_Interp    *interp,
                                          int            objc,
                                          Tcl_Obj *const objv[])
{
   if (objc != 1) {
      Tcl_WrongNumArgs(interp, 1, objv, "");
      return TCL_ERROR;
   }

   struct cindexTUInfo *info = (struct cindexTUInfo *)clientData;

   unsigned  result    = clang_getNumDiagnostics(info->translationUnit);
   Tcl_Obj  *resultObj = Tcl_NewLongObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//----------------------- cindex::<translationUnit instance> diagnostic decode

static int cindexTUDiagnosticDecodeObjCmd(ClientData     clientData,
                                          Tcl_Interp    *interp,
                                          int            objc,
                                          Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "");
      return TCL_ERROR;
   }

   int index = 0;
   int status = Tcl_GetIntFromObj(interp, objv[1], &index);
   if (status != TCL_OK) {
      return status;
   }

   struct cindexTUInfo *info = (struct cindexTUInfo *)clientData;

   unsigned numDiags = clang_getNumDiagnostics(info->translationUnit);
   if (numDiags <= index) {
      Tcl_SetObjResult(interp,
                       Tcl_ObjPrintf("index %d is out of range", index));
      return TCL_ERROR;
   }

   CXDiagnostic  diagnostic = clang_getDiagnostic(info->translationUnit, index);
   Tcl_Obj      *resultObj  = cindexNewDiagnosticObj(diagnostic);
   Tcl_SetObjResult(interp, resultObj);
   clang_disposeDiagnostic(diagnostic);

   return TCL_OK;
}

//----------------------- cindex::<translationUnit instance> diagnostic format

static struct cindexBitMask cindexDiagnosticFormatOptions[] = {
   { "-displaySourceLocation" },
   { "-displayColumn" },
   { "-displaySourceRanges" },
   { "-displayOption" },
   { "-displayCategoryId" },
   { "-displayCategoryName" },
   { NULL }
};

static int cindexTUDiagnosticFormatObjCmd(ClientData     clientData,
                                          Tcl_Interp    *interp,
                                          int            objc,
                                          Tcl_Obj *const objv[])
{
   if (objc < 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "");
      return TCL_ERROR;
   }

   unsigned flags  = 0;
   int      status = cindexParseBitMask(interp, cindexDiagnosticFormatOptions,
                                        cindexNone, objc - 2, objv + 1, &flags);

   int index = 0;
   status = Tcl_GetIntFromObj(interp, objv[objc - 1], &index);
   if (status != TCL_OK) {
      return status;
   }

   struct cindexTUInfo *info = (struct cindexTUInfo *)clientData;

   unsigned numDiags = clang_getNumDiagnostics(info->translationUnit);
   if (numDiags <= index) {
      Tcl_SetObjResult(interp,
                       Tcl_ObjPrintf("index %d is out of range", index));
      return TCL_ERROR;
   }

   CXDiagnostic  diagnostic = clang_getDiagnostic(info->translationUnit, index);
   CXString      result     = clang_formatDiagnostic(diagnostic, flags);
   Tcl_Obj      *resultObj  = cindexMoveCXStringToObj(result);
   Tcl_SetObjResult(interp, resultObj);
   clang_disposeDiagnostic(diagnostic);

   return TCL_OK;
}

//------------------------------ cindex::<translationUnit instance> diagnostic

static int cindexTUDiagnosticObjCmd(ClientData     clientData,
                                    Tcl_Interp    *interp,
                                    int            objc,
                                    Tcl_Obj *const objv[])
{
   if (objc < 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "subcommand");
      return TCL_ERROR;
   }

   static struct cindexCommand subcommands[] = {
      { "number",
        cindexTUDiagnosticNumberObjCmd },
      { "decode",
        cindexTUDiagnosticDecodeObjCmd },
      { "format",
        cindexTUDiagnosticFormatObjCmd },
      { NULL },
   };

   return cindexDispatchSubcommand(clientData, interp, objc, objv, subcommands);
}

//--------------------------------------- cindex::<translationUnit instance>

static int cindexTUInstanceObjCmd(ClientData     clientData,
                                  Tcl_Interp    *interp,
                                  int            objc,
                                  Tcl_Obj *const objv[])
{
   if (objc < 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "subcommand");
      return TCL_ERROR;
   }

   static struct cindexCommand subcommands[] = {
      { "save",
        cindexTUSaveObjCmd },
      { "reparse",
        cindexTUReparseObjCmd },
      { "resourceUsage",
        cindexTUResourceUsageObjCmd },
      { "cursor",
        cindexTUCursorObjCmd },
      { "isMultipleIncludeGuarded",
        cindexTUIsMultipleIncludeGuardedObjCmd },
      { "fileUniqueID",
        cindexTUFileUniqueIDObjCmd },
      { "diagnostic",
        cindexTUDiagnosticObjCmd },
      { NULL },
   };

   return cindexDispatchSubcommand(clientData, interp, objc, objv, subcommands);
}

//------------------------------------------- cindex::<index instance> options

static int cindexIndexOptionsObjCmd(ClientData     clientData,
                                    Tcl_Interp    *interp,
                                    int            objc,
                                    Tcl_Obj *const objv[])
{
   static struct cindexBitMask options[] = {
      { "-threadBackgroundPriorityForIndexing" },
      { "-threadBackgroundPriorityForEditing" },
      { "-threadBackgroundPriorityForAll",
        CXGlobalOpt_ThreadBackgroundPriorityForAll },
      { NULL }
   };

   struct cindexIndexInfo *info = (struct cindexIndexInfo *)clientData;

   if (objc == 1) {
      unsigned value = clang_CXIndex_getGlobalOptions(info->index);
      return cindexBitMaskToString(interp, options, cindexNone, value);
   }

   unsigned value = 0;
   int status
      = cindexParseBitMask(interp, options, cindexNone, objc, objv, &value);
   if (status == TCL_OK) {
      clang_CXIndex_setGlobalOptions(info->index, value);
   }

   return status;
}

//------------------------------------------------------ cindex::<index> parse

static struct cindexBitMask cindexParseOptions[] = {
   { "-detailPreprocessingRecord" },
   { "-incomplete" },
   { "-precompiledPreamble" },
   { "-cacheCompletionResults" },
   { "-forSerialization" },
   { "-cxxChainedPCH" },
   { "-skipFunctionBodies" },
   { "-includeBriefCommentsInCodeCompletion" },
   { NULL }
};

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
   int status = cindexParseBitMask(interp, cindexParseOptions, cindexNone,
                                   optionsEnd, objv, &flags);
   if (status != TCL_OK) {
      return status;
   }

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

   struct cindexTUInfo *info = cindexCreateTUInfo(parent, objv[1], tu);

   Tcl_CreateObjCommand(interp, commandName,
                        cindexTUInstanceObjCmd, info, cindexTUDeleteProc);

   return TCL_OK;
}

//------------------------------------------------- cindex::<index instance>

static int cindexIndexInstanceObjCmd(ClientData     clientData,
                                     Tcl_Interp    *interp,
                                     int            objc,
                                     Tcl_Obj *const objv[])
{
   if (objc < 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "subcommand");
      return TCL_ERROR;
   }

   static struct cindexCommand subcommands[] = {
      { "options", cindexIndexOptionsObjCmd },
      { "parse",   cindexIndexParseObjCmd },
      { NULL }
   };

   return cindexDispatchSubcommand(clientData, interp, objc, objv, subcommands);
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

   struct cindexIndexInfo *info = cindexCreateIndexInfo(interp, index);

   Tcl_CreateObjCommand(interp, commandName, cindexIndexInstanceObjCmd,
			info, cindexIndexDeleteProc);

   return TCL_OK;
}

//----------------------------------------------------- cindex::foreachChild

enum {
   TCL_RECURSE = 5
};

struct cindexForeachChildInfo
{
   Tcl_Interp          *interp;
   Tcl_Obj             *childName;
   Tcl_Obj             *scriptObj;
   int                  returnCode;
};

static enum CXChildVisitResult
cindexForeachChildHelper(CXCursor     cursor,
                         CXCursor     parent,
                         CXClientData clientData)
{
   struct cindexForeachChildInfo *visitInfo
      = (struct cindexForeachChildInfo *)clientData;

   Tcl_Obj *cursorObj = cindexNewCursorObj(cursor);

   if (Tcl_ObjSetVar2(visitInfo->interp, visitInfo->childName,
                      NULL, cursorObj, TCL_LEAVE_ERR_MSG) == NULL) {
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

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[2], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   struct cindexForeachChildInfo visitInfo = {
      .interp     = interp,
      .childName  = objv[1],
      .scriptObj  = objv[3],
      .returnCode = TCL_OK,
   };

   if (clang_visitChildren(cursor, cindexForeachChildHelper, &visitInfo)) {
      return visitInfo.returnCode;
   }

   return TCL_OK;
}

static int cindexRecurseObjCmd(ClientData     clientData,
                               Tcl_Interp    *interp,
                               int            objc,
                               Tcl_Obj *const objv[])
{
   return TCL_RECURSE;
}

//---------------------------------------------------- cindex::cursor::equal

static int cindexCursorEqualObjCmd(ClientData clientData,
                                   Tcl_Interp *interp,
                                   int objc,
                                   Tcl_Obj *const objv[])
{
   if (objc != 3) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor1 cursor2");
      return TCL_ERROR;
   }

   CXCursor cursors[2];
   for (int i = 1, ix = 0; i < 3; ++i, ++ix) {
      int status = cindexGetCursorFromObj(interp, objv[i], &cursors[ix]);
      if (status != TCL_OK) {
         return status;
      }
   }

   unsigned eq = clang_equalCursors(cursors[0], cursors[1]);
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

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   unsigned hash = clang_hashCursor(cursor);
   Tcl_SetObjResult(interp, Tcl_NewLongObj(hash));

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

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   int result = clang_Cursor_isNull(cursor);
   Tcl_SetObjResult(interp, Tcl_NewIntObj(result));

   return TCL_OK;
}

//------------------------------------------------- cursor -> kind -> unsigned

static int cindexCursoToKindToUnsignedObjCmd(ClientData clientData,
                                       Tcl_Interp *interp,
                                       int objc,
                                       Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   typedef unsigned (*ProcType)(enum CXCursorKind);

   ProcType proc = (ProcType)clientData;

   enum CXCursorKind kind = clang_getCursorKind(cursor);
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

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   struct cindexCursorGenericEnumInfo *info
      = (struct cindexCursorGenericEnumInfo *)clientData;

   int value = (info->proc)(cursor);

   Tcl_SetObjResult(interp, cindexGetEnumLabel(info->labels, value));

   return TCL_OK;
}

//-------------------------------------------------- cursor -> bitmask command

typedef unsigned (*cindexCursorToBitMaskProc)(CXCursor);

struct cindexCursorToBitMaskInfo
{
   struct cindexBitMask      *masks;
   Tcl_Obj                   *none;
   cindexCursorToBitMaskProc  proc;
};

static int cindexCursorToBitMaskObjCmd(ClientData     clientData,
                                       Tcl_Interp    *interp,
                                       int            objc,
                                       Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   struct cindexCursorToBitMaskInfo *info
      = (struct cindexCursorToBitMaskInfo *)clientData;

   unsigned value = (info->proc)(cursor);
   return cindexBitMaskToString(interp, info->masks, info->none, value);
}

static unsigned cindex_getObjCPropertyAttributes(CXCursor cursor)
{
   return clang_Cursor_getObjCPropertyAttributes(cursor, 0);
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

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   CXTranslationUnit    tu   = clang_Cursor_getTranslationUnit(cursor);
   struct cindexTUInfo *info = cindexLookupTranslationUnit(tu);
   if (info == NULL) {
      Tcl_Panic("invalid cursor");
   }

   Tcl_SetObjResult(interp, info->name);

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

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   CXCursor  parentCursor = clang_getCursorSemanticParent(cursor);
   Tcl_Obj  *parentObj    = cindexNewCursorObj(parentCursor);
   Tcl_SetObjResult(interp, parentObj);

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

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   CXCursor  parentCursor = clang_getCursorLexicalParent(cursor);
   Tcl_Obj  *parentObj    = cindexNewCursorObj(parentCursor);
   Tcl_SetObjResult(interp, parentObj);

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

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   CXCursor *overridden = NULL;
   unsigned numOverridden = 0;
   clang_getOverriddenCursors(cursor, &overridden, &numOverridden);

   Tcl_Obj **results = (Tcl_Obj **)Tcl_Alloc(sizeof(Tcl_Obj *) * numOverridden);
   for (int i = 0; i < numOverridden; ++i) {
      results[i] = cindexNewCursorObj(overridden[i]);
   }
   clang_disposeOverriddenCursors(overridden);
   Tcl_SetObjResult(interp, Tcl_NewListObj(numOverridden, results));
   Tcl_Free((char *)results);

   return status;
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

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   CXFile   cxfile   = clang_getIncludedFile(cursor);
   CXString filename = clang_getFileName(cxfile);
   Tcl_SetObjResult(interp, cindexMoveCXStringToObj(filename));

   return TCL_OK;
}

//--------------------------------------------------- cindex::cursor::location

static int cindexCursorLocationObjCmd(ClientData     clientData,
                                      Tcl_Interp    *interp,
                                      int            objc,
                                      Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   CXSourceLocation  result    = clang_getCursorLocation(cursor);
   Tcl_Obj          *resultObj = cindexNewSourceLocationObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//---------------------------------------------- cursor -> sourcerange command

static int cindexCursorToSourceRangeObjCmd(ClientData     clientData,
                                           Tcl_Interp    *interp,
                                           int            objc,
                                           Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   CXSourceRange  result    = ((CXSourceRange (*)(CXCursor))clientData)(cursor);
   Tcl_Obj       *resultObj = cindexNewSourceRangeObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//----------------------------------------- cindex::cursor::<location command>

static int cindexCursorDecodedRangeObjCmd(ClientData     clientData,
                                          Tcl_Interp    *interp,
                                          int            objc,
                                          Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   CXSourceRange range = clang_getCursorExtent(cursor);
   Tcl_Obj *resultObj
      = cindexNewDecodedRangeObj((CindexLocationDecodeProc)clientData, range);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

static int cindexCursorPresumedLocationObjCmd(ClientData     clientData,
                                              Tcl_Interp    *interp,
                                              int            objc,
                                              Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   CXSourceRange range = clang_getCursorExtent(cursor);

   enum {
      nlocs = 2
   };

   CXSourceLocation locs[nlocs];
   locs[0] = clang_getRangeStart(range);
   locs[1] = clang_getRangeEnd(range);

   Tcl_Obj *locObjs[nlocs];

   for (int i = 0; i < nlocs; ++i) {
      locObjs[i] = cindexNewPresumedLocationObj(locs[i]);
   }

   Tcl_SetObjResult(interp, Tcl_NewListObj(nlocs, locObjs));

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

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   CXSourceLocation pos = clang_getCursorLocation(cursor);
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

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   CXSourceLocation pos = clang_getCursorLocation(cursor);
   int value = clang_Location_isFromMainFile(pos);

   Tcl_SetObjResult(interp, Tcl_NewIntObj(value));

   return TCL_OK;
}

//----------------------------------------------------- cursor -> type command

static int cindexCursorToTypeObjCmd(ClientData     clientData,
                                           Tcl_Interp    *interp,
                                           int            objc,
                                           Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   CXType   cxtype = ((CXType (*)(CXCursor))clientData)(cursor);
   Tcl_Obj *result = cindexNewCXTypeObj(cxtype);
   Tcl_SetObjResult(interp, result);

   return TCL_OK;
}

//-------------------------------------- cindex::cursor::enumConstantDeclValue

static int cindexCursorEnumConstantDeclValueObjCmd(ClientData     clientData,
                                                   Tcl_Interp    *interp,
                                                   int            objc,
                                                   Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   if (cursor.kind != CXCursor_EnumConstantDecl) {
      Tcl_SetObjResult
         (interp, Tcl_NewStringObj("cursor kind must be EnumConstantDecl", -1));
      return TCL_ERROR;
   }

   Tcl_Obj *result = NULL;

   CXCursor parent = clang_getCursorSemanticParent(cursor);
   assert(parent.kind == CXCursor_EnumDecl);
   CXType inttype = clang_getEnumDeclIntegerType(parent);
   switch (inttype.kind) {

   case CXType_Bool:
   case CXType_Char_U:
   case CXType_UChar:
   case CXType_Char16:
   case CXType_Char32:
   case CXType_UShort:
   case CXType_UInt:
   case CXType_ULong:
   case CXType_ULongLong:
   case CXType_UInt128: {
      unsigned long long value = clang_getEnumConstantDeclUnsignedValue(cursor);
      result = cindexNewUintmaxObj(value);
      break;
   }

   case CXType_Char_S:
   case CXType_SChar:
   case CXType_WChar:
   case CXType_Short:
   case CXType_Int:
   case CXType_Long:
   case CXType_LongLong:
   case CXType_Int128: {
      long long value = clang_getEnumConstantDeclValue(cursor);
      result = cindexNewIntmaxObj(value);
      break;
   }

   default:
      Tcl_Panic("clang_getEnumDeclIntegerType returns unexpected type: %d",
                inttype.kind);
   }

   Tcl_SetObjResult(interp, result);

   return TCL_OK;
}

//------------------------------------------------------ cursor -> int command

static int cindexCursorToIntObjCmd(ClientData     clientData,
                                          Tcl_Interp    *interp,
                                          int            objc,
                                          Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   int      result    = ((int (*)(CXCursor))clientData)(cursor);
   Tcl_Obj *resultObj = Tcl_NewIntObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//------------------------------------------------- cursor -> unsigned command

static int cindexCursorToUnsignedObjCmd(ClientData     clientData,
                                               Tcl_Interp    *interp,
                                               int            objc,
                                               Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   unsigned  result    = ((unsigned (*)(CXCursor))clientData)(cursor);
   Tcl_Obj  *resultObj = Tcl_NewIntObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//--------------------------------------------------- cursor -> string command

static int cindexCursorToStringObjCmd(ClientData     clientData,
                                             Tcl_Interp    *interp,
                                             int            objc,
                                             Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   CXString  result    = ((CXString (*)(CXCursor))clientData)(cursor);
   Tcl_Obj  *resultObj = cindexMoveCXStringToObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//--------------------------------------------------- cursor -> cursor command

static int cindexCursorToCursorObjCmd(ClientData     clientData,
                                      Tcl_Interp    *interp,
                                      int            objc,
                                      Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   CXCursor  result    = ((CXCursor (*)(CXCursor))clientData)(cursor);
   Tcl_Obj  *resultObj = cindexNewCursorObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//----------------------------------------- cindex::cursor::cxxAccessSpecifier

static int cindexCursorCXXAccessSpecifierObjCmd(ClientData     clientData,
                                                Tcl_Interp    *interp,
                                                int            objc,
                                                Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "type");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   static struct cindexEnumLabels table = {
      .names = {
         "CXXInvalidAccessSpecifier",
         "CXXPublic",
         "CXXProtected",
         "CXXPrivate",
         NULL
      }
   };

   int      result    = clang_getCXXAccessSpecifier(cursor);
   Tcl_Obj *resultObj = cindexGetEnumLabel(&table, result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//----------------------------------------- cursor, unsigned -> cursor command

static int cindexCursorUnsignedToCursorObjCmd(ClientData     clientData,
                                              Tcl_Interp    *interp,
                                              int            objc,
                                              Tcl_Obj *const objv[])
{
   if (objc != 3) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor number");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   long number;
   status = Tcl_GetLongFromObj(interp, objv[2], &number);
   if (status != TCL_OK) {
      return status;
   }
   if (number < 0 || UINT_MAX < number) {
      Tcl_SetObjResult(interp, Tcl_NewStringObj("out of range", -1));
      return TCL_ERROR;
   }

   CXCursor  cxresult  = clang_Cursor_getArgument(cursor, number);
   Tcl_Obj  *resultObj = cindexNewCursorObj(cxresult);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//----------------------------------------------------- type -> string command

static int cindexGenericTypeToStringObjCmd(ClientData     clientData,
                                           Tcl_Interp    *interp,
                                           int            objc,
                                           Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "type");
      return TCL_ERROR;
   }

   CXType cxtype;
   int status = cindexGetCXTypeObj(interp, objv[1], &cxtype);
   if (status != TCL_OK) {
      return status;
   }

   CXString (*proc)(CXType) = (CXString (*)(CXType))clientData;
   CXString    result       = proc(cxtype);
   const char *resultCstr   = clang_getCString(result);
   Tcl_Obj    *resultObj    = Tcl_NewStringObj(resultCstr, -1);
   Tcl_SetObjResult(interp, resultObj);
   clang_disposeString(result);

   return TCL_OK;
}

//-------------------------------------------------------- cindex::type::equal

static int cindexTypeEqualObjCmd(ClientData     clientData,
                                 Tcl_Interp    *interp,
                                 int            objc,
                                 Tcl_Obj *const objv[])
{
   if (objc != 3) {
      Tcl_WrongNumArgs(interp, 1, objv, "type1 type2");
      return TCL_ERROR;
   }

   CXType cxtypes[2];
   for (int i = 0; i < 2; ++i) {
      int status = cindexGetCXTypeObj(interp, objv[1 + i], &cxtypes[i]);
      if (status != TCL_OK) {
         return status;
      }
   }

   unsigned result = clang_equalTypes(cxtypes[0], cxtypes[1]);
   Tcl_Obj *resultObj = Tcl_NewIntObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//--------------------------------------------------------------- type -> type

static int cindexGenericTypeToTypeObjCmd(ClientData     clientData,
                                         Tcl_Interp    *interp,
                                         int            objc,
                                         Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "type");
      return TCL_ERROR;
   }

   CXType cxtype;
   int status = cindexGetCXTypeObj(interp, objv[1], &cxtype);
   if (status != TCL_OK) {
      return status;
   }

   CXType (*proc)(CXType) = (CXType (*)(CXType))clientData;
   CXType   result        = proc(cxtype);
   Tcl_Obj *resultObj     = cindexNewCXTypeObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//-------------------------------------------------------- type -> int command

static int cindexGenericTypeToIntObjCmd(ClientData     clientData,
                                        Tcl_Interp    *interp,
                                        int            objc,
                                        Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "type");
      return TCL_ERROR;
   }

   CXType cxtype;
   int status = cindexGetCXTypeObj(interp, objv[1], &cxtype);
   if (status != TCL_OK) {
      return status;
   }

   int      result    = ((int (*)(CXType))clientData)(cxtype);
   Tcl_Obj *resultObj = Tcl_NewLongObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//--------------------------------------------------- type -> unsigned command

static int cindexGenericTypeToUnsignedObjCmd(ClientData     clientData,
                                             Tcl_Interp    *interp,
                                             int            objc,
                                             Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "type");
      return TCL_ERROR;
   }

   CXType cxtype;
   int status = cindexGetCXTypeObj(interp, objv[1], &cxtype);
   if (status != TCL_OK) {
      return status;
   }

   unsigned (*proc)(CXType) = (unsigned (*)(CXType))clientData;
   unsigned  result         = proc(cxtype);
   Tcl_Obj  *resultObj      = Tcl_NewLongObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//-------------------------------------------------- type -> long long command

static int cindexGenericTypeToLongLongObjCmd(ClientData     clientData,
                                             Tcl_Interp    *interp,
                                             int            objc,
                                             Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "type");
      return TCL_ERROR;
   }

   CXType cxtype;
   int status = cindexGetCXTypeObj(interp, objv[1], &cxtype);
   if (status != TCL_OK) {
      return status;
   }

   long long  result    = ((long long (*)(CXType))clientData)(cxtype);
   Tcl_Obj   *resultObj = cindexNewIntmaxObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//-------------------------------- type -> long long or a layout error command

static Tcl_Obj *cindexLayoutErrorNames;
static Tcl_Obj *cindexLayoutErrorValues;

static void cindexCreateLayoutErrorTable(void)
{
   static struct cindexNameValuePair table[] = {
      { "Invalid", CXTypeLayoutError_Invalid },
      { "Incomplete", CXTypeLayoutError_Incomplete },
      { "Dependent", CXTypeLayoutError_Dependent },
      { "NotConstantSize", CXTypeLayoutError_NotConstantSize },
      { "InvalidFieldName", CXTypeLayoutError_InvalidFieldName },
      { NULL }
   };

   cindexCreateNameValueTable
      (&cindexLayoutErrorNames, &cindexLayoutErrorValues, table);

   Tcl_IncrRefCount(cindexLayoutErrorNames);
   Tcl_IncrRefCount(cindexLayoutErrorValues);
}

static Tcl_Obj *cindexNewLayoutLongLongObj(long long value)
{
   if (0 <= value) {
      return cindexNewUintmaxObj(value);
   }

   Tcl_Obj *valueObj = Tcl_NewLongObj(value);
   Tcl_IncrRefCount(valueObj);

   Tcl_Obj *resultObj;
   int status
      = Tcl_DictObjGet(NULL, cindexLayoutErrorNames, valueObj, &resultObj);

   Tcl_DecrRefCount(valueObj);

   if (status != TCL_OK) {
      Tcl_Panic("unknown layout error: %lld", value);
   }

   return resultObj;
}

static int cindexGenericTypeToLayoutLongLongObjCmd(ClientData     clientData,
                                                   Tcl_Interp    *interp,
                                                   int            objc,
                                                   Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "type");
      return TCL_ERROR;
   }

   CXType cxtype;
   int status = cindexGetCXTypeObj(interp, objv[1], &cxtype);
   if (status != TCL_OK) {
      return status;
   }

   long long  result    = ((long long (*)(CXType))clientData)(cxtype);
   Tcl_Obj   *resultObj = cindexNewLayoutLongLongObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//----------------------------------------------------- type -> cursor command

static int cindexGenericTypeToCursorObjCmd(ClientData     clientData,
                                           Tcl_Interp    *interp,
                                           int            objc,
                                           Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "type");
      return TCL_ERROR;
   }

   CXType cxtype;
   int status = cindexGetCXTypeObj(interp, objv[1], &cxtype);
   if (status != TCL_OK) {
      return status;
   }

   CXCursor  result    = ((CXCursor (*)(CXType))clientData)(cxtype);
   Tcl_Obj  *resultObj = cindexNewCursorObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//--------------------------------------------- type, unsigned -> type command

static int cindexGenericTypeUnsignedToTypeObjCmd(ClientData     clientData,
                                                 Tcl_Interp    *interp,
                                                 int            objc,
                                                 Tcl_Obj *const objv[])
{
   if (objc != 3) {
      Tcl_WrongNumArgs(interp, 1, objv, "type unsigned");
      return TCL_ERROR;
   }

   CXType cxtype;
   int status = cindexGetCXTypeObj(interp, objv[1], &cxtype);
   if (status != TCL_OK) {
      return status;
   }

   long number;
   status = Tcl_GetLongFromObj(interp, objv[2], &number);
   if (status != TCL_OK) {
      return status;
   }
   if (number < 0 || UINT_MAX < number) {
      Tcl_SetObjResult(interp,
                       Tcl_NewStringObj("out of range", -1));
      return TCL_ERROR;
   }

   CXType result = ((CXType (*)(CXType, unsigned))clientData)(cxtype, number);
   Tcl_Obj *resultObj = cindexNewCXTypeObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//-------------------------------- cindex::type::functionTypeCallingConvention

static Tcl_Obj *cindexCallingConvValues;
static Tcl_Obj *cindexCallingConvNames;

void cindexCreateCallingConvTable(void)
{
   static struct cindexNameValuePair table[] = {
      { "Default",
        CXCallingConv_Default },
      { "C",
        CXCallingConv_C },
      { "X86StdCall",
        CXCallingConv_X86StdCall },
      { "X86FastCall",
        CXCallingConv_X86FastCall },
      { "X86ThisCall",
        CXCallingConv_X86ThisCall },
      { "X86Pascal",
        CXCallingConv_X86Pascal },
      { "AAPCS",
        CXCallingConv_AAPCS },
      { "AAPCS_VFP",
        CXCallingConv_AAPCS_VFP },
      { "PnaclCall",
        CXCallingConv_PnaclCall },
      { "IntelOclBicc",
        CXCallingConv_IntelOclBicc },
      { "X86_64Win64",
        CXCallingConv_X86_64Win64 },
      { "X86_64SysV",
        CXCallingConv_X86_64SysV },
      { "Invalid",
        CXCallingConv_Invalid },
      { "Unexposed",
        CXCallingConv_Unexposed },
      { NULL }
   };

   cindexCreateNameValueTable
      (&cindexCallingConvNames, &cindexCallingConvValues, table);

   Tcl_IncrRefCount(cindexCallingConvNames);
   Tcl_IncrRefCount(cindexCallingConvValues);
}

static int
cindexTypeFunctionTypeCallingConventionObjCmd(ClientData     clientData,
                                              Tcl_Interp    *interp,
                                              int            objc,
                                              Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "type");
      return TCL_ERROR;
   }

   CXType cxtype;
   int status = cindexGetCXTypeObj(interp, objv[1], &cxtype);
   if (status != TCL_OK) {
      return status;
   }

   enum CXCallingConv  cconv    = clang_getFunctionTypeCallingConv(cxtype);
   Tcl_Obj            *cconvObj = Tcl_NewIntObj(cconv);
   Tcl_IncrRefCount(cconvObj);

   Tcl_Obj *resultObj;
   status = Tcl_DictObjGet(NULL, cindexCallingConvNames, cconvObj, &resultObj);
   Tcl_DecrRefCount(cconvObj);
   if (status != TCL_OK) {
      Tcl_Panic("unknown calling convention: %d", cconv);
   }

   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//---------------------------------------------- cindex::type::cxxRefQualifier

static int cindexTypeCXXRefQualifierObjCmd(ClientData     clientData,
                                           Tcl_Interp    *interp,
                                           int            objc,
                                           Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "type");
      return TCL_ERROR;
   }

   CXType cxtype;
   int status = cindexGetCXTypeObj(interp, objv[1], &cxtype);
   if (status != TCL_OK) {
      return status;
   }

   static struct cindexEnumLabels table = {
      .names = {
         "None",
         "LValue",
         "RValue",
         NULL
      }
   };

   int      result    = clang_Type_getCXXRefQualifier(cxtype);
   Tcl_Obj *resultObj = cindexGetEnumLabel(&table, result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//----------------------------------------------------- cindex::type::offsetof

static int cindexTypeOffsetOfObjCmd(ClientData     clientData,
                                    Tcl_Interp    *interp,
                                    int            objc,
                                    Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      type_ix,
      field_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, 1, objv, "type field");
      return TCL_ERROR;
   }

   CXType cxtype;
   int status = cindexGetCXTypeObj(interp, objv[type_ix], &cxtype);
   if (status != TCL_OK) {
      return status;
   }

   const char *field     = Tcl_GetStringFromObj(objv[field_ix], NULL);
   long long   result    = clang_Type_getOffsetOf(cxtype, field);
   Tcl_Obj    *resultObj = cindexNewLayoutLongLongObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//------------------------------------------------------------- initialization

int Cindex_Init(Tcl_Interp *interp)
{
   if (Tcl_InitStubs(interp, "8.6", 0) == NULL) {
      return TCL_ERROR;
   }

   cindexNone = Tcl_NewStringObj("-none", -1);
   Tcl_IncrRefCount(cindexNone);

   cindexSourceLocationTagObj = Tcl_NewStringObj("CXSourceLocation", -1);
   Tcl_IncrRefCount(cindexSourceLocationTagObj);

   cindexSourceRangeTagObj = Tcl_NewStringObj("CXSourceRange", -1);
   Tcl_IncrRefCount(cindexSourceRangeTagObj);

   cindexFilenameNullObj = Tcl_NewStringObj("<null>", -1);
   Tcl_IncrRefCount(cindexFilenameNullObj);

   cindexDiagnosticSeverityTagObj
      = Tcl_NewStringObj("severity", -1);
   Tcl_IncrRefCount(cindexDiagnosticSeverityTagObj);

   cindexDiagnosticLocationTagObj
      = Tcl_NewStringObj("location", -1);
   Tcl_IncrRefCount(cindexDiagnosticLocationTagObj);

   cindexDiagnosticSpellingTagObj
      = Tcl_NewStringObj("spelling", -1);
   Tcl_IncrRefCount(cindexDiagnosticSpellingTagObj);

   cindexDiagnosticOptionTagObj
      = Tcl_NewStringObj("option", -1);
   Tcl_IncrRefCount(cindexDiagnosticOptionTagObj);

   cindexDiagnosticDisableTagObj
      = Tcl_NewStringObj("disable", -1);
   Tcl_IncrRefCount(cindexDiagnosticDisableTagObj);

   cindexDiagnosticCategoryTagObj
      = Tcl_NewStringObj("category", -1);
   Tcl_IncrRefCount(cindexDiagnosticCategoryTagObj);

   cindexDiagnosticRangesTagObj
      = Tcl_NewStringObj("ranges", -1);
   Tcl_IncrRefCount(cindexDiagnosticRangesTagObj);

   cindexDiagnosticFixItsTagObj
      = Tcl_NewStringObj("fixits", -1);
   Tcl_IncrRefCount(cindexDiagnosticFixItsTagObj);


   Tcl_Namespace *cindexNs
      = Tcl_CreateNamespace(interp, "cindex", NULL, NULL);

   cindexCreateCursorKindTable();
   cindexCreateCXTypeTable();
   cindexCreateCallingConvTable();
   cindexCreateLayoutErrorTable();

   static struct cindexCommand cmdTable[] = {
      { "index",
        cindexIndexObjCmd },
      { "foreachChild",
        cindexForeachChildObjCmd },
      { "recurse",
        cindexRecurseObjCmd },
      { NULL }
   };
   cindexCreateAndExportCommands(interp, "cindex::%s", cmdTable);

   Tcl_Namespace *locationNs
      = Tcl_CreateNamespace(interp, "cindex::location", NULL, NULL);
   Tcl_CreateEnsemble(interp, "::cindex::location", locationNs, 0);
   Tcl_Export(interp, cindexNs, "location", 0);

   static struct cindexCommand locationCmdTable[] = {
      { "presumedLocation",
        cindexLocationPresumedLocationObjCmd },
      { "expansionLocation",
        cindexLocationGenericDecodeObjCmd,
        clang_getExpansionLocation },
      { "spellingLocation",
        cindexLocationGenericDecodeObjCmd,
        clang_getSpellingLocation },
      { "fileLocation",
        cindexLocationGenericDecodeObjCmd,
        clang_getFileLocation },
      { "equal",
        cindexLocationEqualObjCmd },
      { NULL }
   };
   cindexCreateAndExportCommands
      (interp, "cindex::location::%s", locationCmdTable);

   Tcl_Namespace *locationIsNs
      = Tcl_CreateNamespace(interp, "cindex::location::is", NULL, NULL);
   Tcl_CreateEnsemble(interp, "::cindex::location::is", locationIsNs, 0);
   Tcl_Export(interp, locationNs, "is", 0);

   static struct cindexCommand locationIsCmdTable[] = {
      { "null",
        cindexLocationIsNullObjCmd },
      { "inSystemHeader",
        cindexGenericLocationToUnsignedObjCmd,
        clang_Location_isInSystemHeader },
      { "inMainFile",
        cindexGenericLocationToUnsignedObjCmd,
        clang_Location_isFromMainFile },
      { NULL }
   };
   cindexCreateAndExportCommands
      (interp, "cindex::location::is::%s", locationIsCmdTable);

   Tcl_Namespace *rangeNs
      = Tcl_CreateNamespace(interp, "cindex::range", NULL, NULL);
   Tcl_CreateEnsemble(interp, "::cindex::range", rangeNs, 0);
   Tcl_Export(interp, cindexNs, "range", 0);

   static struct cindexCommand rangeCmdTable[] = {
      { "presumedLocation",
        cindexLocationPresumedLocationObjCmd },
      { "expansionLocation",
        cindexLocationGenericDecodeObjCmd,
        clang_getExpansionLocation },
      { "spellingLocation",
        cindexLocationGenericDecodeObjCmd,
        clang_getSpellingLocation },
      { "fileLocation",
        cindexLocationGenericDecodeObjCmd,
        clang_getFileLocation },
      { "start",
        cindexGenericRangeToLocationObjCmd,
        clang_getRangeStart },
      { "end",
        cindexGenericRangeToLocationObjCmd,
        clang_getRangeEnd },
      { "equal",
        cindexRangeEqualObjCmd },
      { NULL }
   };
   cindexCreateAndExportCommands(interp, "cindex::range::%s", rangeCmdTable);

   Tcl_Namespace *rangeIsNs
      = Tcl_CreateNamespace(interp, "cindex::range::is", NULL, NULL);
   Tcl_CreateEnsemble(interp, "::cindex::range::is", rangeIsNs, 0);
   Tcl_Export(interp, rangeNs, "is", 0);

   static struct cindexCommand rangeIsCmdTable[] = {
      { "null",
        cindexLocationIsNullObjCmd },
      { NULL }
   };
   cindexCreateAndExportCommands
      (interp, "cindex::range::is::%s", rangeIsCmdTable);

   Tcl_Namespace *cursorNs
      = Tcl_CreateNamespace(interp, "cindex::cursor", NULL, NULL);
   Tcl_CreateEnsemble(interp, "::cindex::cursor", cursorNs, 0);
   Tcl_Export(interp, cindexNs, "cursor", 0);

   static struct cindexCursorGenericEnumInfo cursorLinkageInfo = {
      .proc   = (int (*)(CXCursor))clang_getCursorLinkage,
      .labels = &cindexLinkageLables
   };

   static struct cindexCursorGenericEnumInfo cursorAvailabilityInfo = {
      .proc   = (int (*)(CXCursor))clang_getCursorAvailability,
      .labels = &cindexAvailabilityLabels
   };

   static struct cindexCursorGenericEnumInfo cursorLanguageInfo = {
      .proc   = (int (*)(CXCursor))clang_getCursorLanguage,
      .labels = &cindexLanguageLables
   };

   static struct cindexCursorToBitMaskInfo objCPropertyAttributesInfo = {
      .proc  = cindex_getObjCPropertyAttributes,
      .none  = NULL,
      .masks = cindexObjCPropertyAttributes
   };

   static struct cindexCursorToBitMaskInfo objCDeclQualifiersInfo = {
      .proc  = clang_Cursor_getObjCDeclQualifiers,
      .none  = NULL,
      .masks = cindexObjCDeclQualifiers
   };

   static struct cindexCommand cursorCmdTable[] = {
      { "equal",
        cindexCursorEqualObjCmd },
      { "hash",
        cindexCursorHashObjCmd },
      { "linkage",
        cindexCursorGenericEnumObjCmd, &cursorLinkageInfo },
      { "availability",
        cindexCursorGenericEnumObjCmd, &cursorAvailabilityInfo },
      { "language",
        cindexCursorGenericEnumObjCmd, &cursorLanguageInfo },
      { "translationUnit",
        cindexCursorTranslationUnitObjCmd },
      { "semanticParent",
        cindexCursorSemanticParentObjCmd },
      { "lexicalParent",
        cindexCursorLexicalParentObjCmd },
      { "overriddenCursors",
        cindexCursorOverriddenCursorsObjCmd },
      { "includedFile",
        cindexCursorIncludedFileObjCmd },
      { "location",
        cindexCursorLocationObjCmd },
      { "extent",
        cindexCursorToSourceRangeObjCmd,
        clang_getCursorExtent },
      { "expansionLocation",
        cindexCursorDecodedRangeObjCmd,
        (ClientData)clang_getExpansionLocation },
      { "presumedLocation",
        cindexCursorPresumedLocationObjCmd },
      { "spellingLocation",
        cindexCursorDecodedRangeObjCmd,
        (ClientData)clang_getSpellingLocation },
      { "fileLocation",
        cindexCursorDecodedRangeObjCmd,
        (ClientData)clang_getFileLocation },
      { "type",
        cindexCursorToTypeObjCmd,
        clang_getCursorType },
      { "typedefDeclUnderlyingType",
        cindexCursorToTypeObjCmd,
        clang_getTypedefDeclUnderlyingType },
      { "enumDeclIntegerType",
        cindexCursorToTypeObjCmd,
        clang_getEnumDeclIntegerType },
      { "enumConstantDeclValue",
        cindexCursorEnumConstantDeclValueObjCmd },
      { "fieldDeclBitWidth",
        cindexCursorToIntObjCmd,
        clang_getFieldDeclBitWidth },
      { "numArguments",
        cindexCursorToIntObjCmd,
        clang_Cursor_getNumArguments },
      { "argument",
        cindexCursorUnsignedToCursorObjCmd,
        clang_Cursor_getArgument },
      { "objCTypeEncoding",
        cindexCursorToStringObjCmd,
        clang_getDeclObjCTypeEncoding },
      { "resultType",
        cindexCursorToTypeObjCmd,
        clang_getCursorResultType },
      { "cxxAccessSpecifier",
        cindexCursorCXXAccessSpecifierObjCmd },
      { "numOverloadedDecls",
        cindexCursorToUnsignedObjCmd,
        clang_getNumOverloadedDecls },
      { "overloadedDecls",
        cindexCursorUnsignedToCursorObjCmd,
        clang_getOverloadedDecl },
      { "IBOutletCollectionType",
        cindexCursorToTypeObjCmd,
        clang_getIBOutletCollectionType },
      { "USR",
        cindexCursorToStringObjCmd,
        clang_getCursorUSR },
      { "spelling",
        cindexCursorToStringObjCmd,
        clang_getCursorSpelling },
      { "displayName",
        cindexCursorToStringObjCmd,
        clang_getCursorDisplayName },
      { "referenced",
        cindexCursorToCursorObjCmd,
        clang_getCursorReferenced },
      { "definition",
        cindexCursorToCursorObjCmd,
        clang_getCursorDefinition },
      { "canonicalCursor",
        cindexCursorToCursorObjCmd,
        clang_getCanonicalCursor },
      { "objCSelectorIndex",
        cindexCursorToIntObjCmd,
        clang_Cursor_getObjCSelectorIndex },
      { "receiverType",
        cindexCursorToTypeObjCmd,
        clang_Cursor_getReceiverType },
      { "objCPropertyAttributes",
        cindexCursorToBitMaskObjCmd,
        &objCPropertyAttributesInfo },
      { "objCDeclQualifiers",
        cindexCursorToBitMaskObjCmd,
        &objCDeclQualifiersInfo },
      { "commentRange",
        cindexCursorToSourceRangeObjCmd,
        clang_Cursor_getCommentRange },
      { "rawCommentText",
        cindexCursorToStringObjCmd,
        clang_Cursor_getCommentRange },
      { "briefCommentText",
        cindexCursorToStringObjCmd,
        clang_Cursor_getBriefCommentText },
      { NULL }
   };
   cindexCreateAndExportCommands(interp, "cindex::cursor::%s", cursorCmdTable);

   Tcl_Namespace *cursorIsNs
      = Tcl_CreateNamespace(interp, "cindex::cursor::is", NULL, NULL);
   Tcl_CreateEnsemble(interp, "::cindex::cursor::is", cursorIsNs, 0);
   Tcl_Export(interp, cursorNs, "is", 0);

   static struct cindexCommand cursorIsCmdTable[] = {
      { "null",
        cindexCursorIsNullObjCmd },
      { "declaration",
        cindexCursoToKindToUnsignedObjCmd, clang_isDeclaration },
      { "reference",
        cindexCursoToKindToUnsignedObjCmd, clang_isReference },
      { "expression",
	cindexCursoToKindToUnsignedObjCmd, clang_isExpression },
      { "statement",
        cindexCursoToKindToUnsignedObjCmd, clang_isStatement },
      { "attribute",
        cindexCursoToKindToUnsignedObjCmd, clang_isAttribute },
      { "invalid",
        cindexCursoToKindToUnsignedObjCmd, clang_isInvalid },
      { "translationUnit",
	cindexCursoToKindToUnsignedObjCmd, clang_isTranslationUnit },
      { "preprocessing",
	cindexCursoToKindToUnsignedObjCmd, clang_isPreprocessing },
      { "unexposed",
        cindexCursoToKindToUnsignedObjCmd, clang_isUnexposed },
      { "inSystemHeader",
        cindexCursorIsInSystemHeaderObjCmd },
      { "inMainFile",
        cindexCursorIsInMainFileObjCmd },
      { "bitField",
        cindexCursorToUnsignedObjCmd,
        clang_Cursor_isBitField },
      { "virtualBase",
        cindexCursorToUnsignedObjCmd,
        clang_isVirtualBase },
      { "definition",
        cindexCursorToUnsignedObjCmd,
        clang_isCursorDefinition },
      { "dynamicCall",
        cindexCursorToIntObjCmd,
        clang_Cursor_isDynamicCall },
      { "objCOptional",
        cindexCursorToUnsignedObjCmd,
        clang_Cursor_isObjCOptional },
      { "variadic",
        cindexCursorToUnsignedObjCmd,
        clang_Cursor_isVariadic },
      { NULL }
   };
   cindexCreateAndExportCommands
      (interp, "cindex::cursor::is::%s", cursorIsCmdTable);

   Tcl_Namespace *typeNs
      = Tcl_CreateNamespace(interp, "cindex::type", NULL, NULL);
   Tcl_CreateEnsemble(interp, "::cindex::type", typeNs, 0);
   Tcl_Export(interp, cindexNs, "type", 0);

   static struct cindexCommand typeCmdTable[] = {
      { "spelling",
        cindexGenericTypeToStringObjCmd,
        clang_getTypeSpelling },
      { "equal",
        cindexTypeEqualObjCmd },
      { "canonicalType",
        cindexGenericTypeToTypeObjCmd,
        clang_getCanonicalType },
      { "pointeeType",
        cindexGenericTypeToTypeObjCmd,
        clang_getPointeeType },
      { "resultType",
        cindexGenericTypeToTypeObjCmd,
        clang_getResultType },
      { "elementType",
        cindexGenericTypeToTypeObjCmd,
        clang_getElementType },
      { "arrayElementType",
        cindexGenericTypeToTypeObjCmd,
        clang_getArrayElementType },
      { "classType",
        cindexGenericTypeToTypeObjCmd,
        clang_Type_getClassType },
      { "numElements",
        cindexGenericTypeToLongLongObjCmd,
        clang_getNumElements },
      { "arraySize",
        cindexGenericTypeToLongLongObjCmd,
        clang_getArraySize },
      { "alignof",
        cindexGenericTypeToLayoutLongLongObjCmd,
        clang_Type_getAlignOf },
      { "sizeof",
        cindexGenericTypeToLayoutLongLongObjCmd,
        clang_Type_getSizeOf },
      { "declaration",
        cindexGenericTypeToCursorObjCmd,
        clang_getTypeDeclaration },
      { "functionTypeCallingConvention",
        cindexTypeFunctionTypeCallingConventionObjCmd },
      { "functionTypeNumArgTypes",
        cindexGenericTypeToIntObjCmd,
        clang_getNumArgTypes },
      { "argType",
        cindexGenericTypeUnsignedToTypeObjCmd,
        clang_getArgType },
      { "offsetof",
        cindexTypeOffsetOfObjCmd },
      { "cxxRefQualifier",
        cindexTypeCXXRefQualifierObjCmd },
      { NULL }
   };
   cindexCreateAndExportCommands(interp, "cindex::type::%s", typeCmdTable);

   Tcl_Namespace *typeIsNs
      = Tcl_CreateNamespace(interp, "cindex::type::is", NULL, NULL);
   Tcl_CreateEnsemble(interp, "::cindex::type::is", typeIsNs, 0);
   Tcl_Export(interp, typeNs, "is", 0);

   static struct cindexCommand typeIsCmdTable[] = {
      { "constQualified",
        cindexGenericTypeToUnsignedObjCmd,
        clang_isConstQualifiedType },
      { "volatileQualified",
        cindexGenericTypeToUnsignedObjCmd,
        clang_isVolatileQualifiedType },
      { "restrictQualified",
        cindexGenericTypeToUnsignedObjCmd,
        clang_isRestrictQualifiedType },
      { "PODType",
        cindexGenericTypeToUnsignedObjCmd,
        clang_isPODType },
      { "functionTypeVariadic",
        cindexGenericTypeToUnsignedObjCmd,
        clang_isFunctionTypeVariadic },
      { NULL }
   };
   cindexCreateAndExportCommands
      (interp, "cindex::type::is::%s", typeIsCmdTable);

   {
      unsigned mask = clang_defaultEditingTranslationUnitOptions();

      int status
         = cindexBitMaskToString(interp, cindexParseOptions, cindexNone, mask);
      if (status != TCL_OK) {
         return status;
      }

      Tcl_Obj *name =
         Tcl_NewStringObj("cindex::defaultEditingTranslationUnitOptions", -1);
      Tcl_IncrRefCount(name);

      Tcl_Obj *value = Tcl_GetObjResult(interp);
      Tcl_IncrRefCount(value);

      Tcl_ObjSetVar2(interp, name, NULL, value, 0);
      Tcl_Export(interp, cindexNs, "defaultEditingTranslationUnitOptions", 0);

      Tcl_DecrRefCount(name);
      Tcl_DecrRefCount(value);
   }

   {
      unsigned mask = clang_defaultDiagnosticDisplayOptions();

      int status = cindexBitMaskToString
         (interp, cindexDiagnosticFormatOptions, cindexNone, mask);
      if (status != TCL_OK) {
         return status;
      }

      Tcl_Obj *name =
         Tcl_NewStringObj("cindex::defaultDiagnosticDisplayOptions", -1);
      Tcl_IncrRefCount(name);

      Tcl_Obj *value = Tcl_GetObjResult(interp);
      Tcl_IncrRefCount(value);

      Tcl_ObjSetVar2(interp, name, NULL, value, 0);
      Tcl_Export(interp, cindexNs, "defaultDiagnosticDisplayOptions", 0);

      Tcl_DecrRefCount(name);
      Tcl_DecrRefCount(value);
   }

   Tcl_PkgProvide(interp, "cindex", "1.0");

   return TCL_OK;
}

#if 0

TODOs

/**
 * \brief Construct a USR for a specified Objective-C class.
 */
CINDEX_LINKAGE CXString clang_constructUSR_ObjCClass(const char *class_name);

/**
 * \brief Construct a USR for a specified Objective-C category.
 */
CINDEX_LINKAGE CXString
  clang_constructUSR_ObjCCategory(const char *class_name,
                                 const char *category_name);

/**
 * \brief Construct a USR for a specified Objective-C protocol.
 */
CINDEX_LINKAGE CXString
  clang_constructUSR_ObjCProtocol(const char *protocol_name);


/**
 * \brief Construct a USR for a specified Objective-C instance variable and
 *   the USR for its containing class.
 */
CINDEX_LINKAGE CXString clang_constructUSR_ObjCIvar(const char *name,
                                                    CXString classUSR);

/**
 * \brief Construct a USR for a specified Objective-C method and
 *   the USR for its containing class.
 */
CINDEX_LINKAGE CXString clang_constructUSR_ObjCMethod(const char *name,
                                                      unsigned isInstanceMethod,
                                                      CXString classUSR);

/**
 * \brief Construct a USR for a specified Objective-C property and the USR
 *  for its containing class.
 */
CINDEX_LINKAGE CXString clang_constructUSR_ObjCProperty(const char *property,
                                                        CXString classUSR);

/**
 * \brief Retrieve a range for a piece that forms the cursors spelling name.
 * Most of the times there is only one range for the complete spelling but for
 * objc methods and objc message expressions, there are multiple pieces for each
 * selector identifier.
 * 
 * \param pieceIndex the index of the spelling name piece. If this is greater
 * than the actual number of pieces, it will return a NULL (invalid) range.
 *  
 * \param options Reserved.
 */
CINDEX_LINKAGE CXSourceRange clang_Cursor_getSpellingNameRange(CXCursor,
                                                          unsigned pieceIndex,
                                                          unsigned options);

/**
 * \brief Given a cursor that represents a documentable entity (e.g.,
 * declaration), return the associated parsed comment as a
 * \c CXComment_FullComment AST node.
 */
CINDEX_LINKAGE CXComment clang_Cursor_getParsedComment(CXCursor C);

/**
 * \brief Given a CXCursor_ModuleImportDecl cursor, return the associated module.
 */
CINDEX_LINKAGE CXModule clang_Cursor_getModule(CXCursor C);

/**
 * \param Module a module object.
 *
 * \returns the module file where the provided module object came from.
 */
CINDEX_LINKAGE CXFile clang_Module_getASTFile(CXModule Module);

/**
 * \param Module a module object.
 *
 * \returns the parent of a sub-module or NULL if the given module is top-level,
 * e.g. for 'std.vector' it will return the 'std' module.
 */
CINDEX_LINKAGE CXModule clang_Module_getParent(CXModule Module);

/**
 * \param Module a module object.
 *
 * \returns the name of the module, e.g. for the 'std.vector' sub-module it
 * will return "vector".
 */
CINDEX_LINKAGE CXString clang_Module_getName(CXModule Module);

/**
 * \param Module a module object.
 *
 * \returns the full name of the module, e.g. "std.vector".
 */
CINDEX_LINKAGE CXString clang_Module_getFullName(CXModule Module);

/**
 * \param Module a module object.
 *
 * \returns the number of top level headers associated with this module.
 */
CINDEX_LINKAGE unsigned clang_Module_getNumTopLevelHeaders(CXTranslationUnit,
                                                           CXModule Module);

/**
 * \param Module a module object.
 *
 * \param Index top level header index (zero-based).
 *
 * \returns the specified top level header associated with the module.
 */
CINDEX_LINKAGE
CXFile clang_Module_getTopLevelHeader(CXTranslationUnit,
                                      CXModule Module, unsigned Index);

#endif

// Local Variables:
// tab-width: 8
// fill-column: 78
// mode: c
// c-basic-offset: 3
// indent-tabs-mode: nil
// End:
