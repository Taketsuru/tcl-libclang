//============================================================================

// Copyright (c) 2014 Taketsuru <taketsuru11@gmail.com>.
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

static unsigned long cstringHash(const char *str)
{
   unsigned long        hash = 0;
   const unsigned char *cp   = (const unsigned char *)str;
   int                  c;

   while ((c = *cp++) != 0) {
      hash = c + (hash << 6) + (hash << 16) - hash;
   }

   return hash;
}

static Tcl_Obj *convertCXStringToObj(CXString str)
{
   const char *cstr   = clang_getCString(str);
   Tcl_Obj    *result = Tcl_NewStringObj(cstr, -1);
   clang_disposeString(str);
   return result;
}

typedef struct Command
{
   const char     *name;
   Tcl_ObjCmdProc *proc;
   ClientData      clientData;
} Command;

static void createAndExportCommands(Tcl_Interp *interp,
                                    const char *commandNameFormat,
                                    Command    *command)
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

typedef struct NameValuePair
{
   const char *name;
   int         value;
} NameValuePair;

// Create two dictionaries from an array of name and value pairs.
static void
createNameValueTable(Tcl_Obj             **valueToNameDictPtr,
                     Tcl_Obj             **nameToValueDictPtr,
                     const NameValuePair  *table)
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

static Tcl_Obj *newBignumObj(uintmax_t value, int negative)
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

static Tcl_Obj *newUintmaxObj(uintmax_t value)
{
   if (value <= LONG_MAX) {
      return Tcl_NewLongObj(value);
   }

   return newBignumObj(value, 0);
}

static Tcl_Obj *newIntmaxObj(intmax_t value)
{
   if (LONG_MIN <= value && value <= LONG_MAX) {
      return Tcl_NewLongObj(value);
   }

   return 0 <= value
      ? newBignumObj(value, 0)
      : newBignumObj(-value, 1);
}

static Tcl_Obj *newPointerObj(const void *ptr)
{
   return newUintmaxObj((uintptr_t)ptr);
}

static int getIntmaxFromBignumObj(Tcl_Interp *interp, Tcl_Obj *obj,
                                  int isSigned, void *bigPtr)
{
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

static int getIntmaxFromObj(Tcl_Interp *interp, Tcl_Obj *obj,
                            int isSigned, void *bigPtr)
{
   long longv;
   if (Tcl_GetLongFromObj(NULL, obj, &longv) == TCL_OK) {
      if (longv < 0 && ! isSigned) {
         Tcl_SetObjResult(interp, Tcl_NewStringObj("out of range", -1));
         return TCL_ERROR;
      }
      *(intmax_t *)bigPtr = longv;
      return TCL_OK;
   }

   return getIntmaxFromBignumObj(interp, obj, isSigned, bigPtr);
}

static int
getPointerFromObj(Tcl_Interp *interp, Tcl_Obj *obj, void **ptrOut)
{
   uintmax_t intvalue;
   int status = getIntmaxFromObj(interp, obj, 0, &intvalue);
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

static int
getUnsignedFromObj(Tcl_Interp *interp, Tcl_Obj *obj, unsigned *valuePtr)
{
   long value;
   int status = Tcl_GetLongFromObj(interp, obj, &value);
   if (status != TCL_OK) {
      return status;
   }

   if (value < 0 || UINT_MAX < value) {
      if (interp != NULL) {
         Tcl_SetObjResult(interp,
                          Tcl_ObjPrintf("%s is out of range",
                                        Tcl_GetStringFromObj(obj, NULL)));
      }
      return TCL_ERROR;
   }

   *valuePtr = value;

   return TCL_OK;
}

//--------------------------------------------- long long or layout error code

static Tcl_Obj *layoutErrorNames;
static Tcl_Obj *layoutErrorValues;

static void createLayoutErrorTable(void)
{
   static NameValuePair table[] = {
      { "Invalid", CXTypeLayoutError_Invalid },
      { "Incomplete", CXTypeLayoutError_Incomplete },
      { "Dependent", CXTypeLayoutError_Dependent },
      { "NotConstantSize", CXTypeLayoutError_NotConstantSize },
      { "InvalidFieldName", CXTypeLayoutError_InvalidFieldName },
      { NULL }
   };

   createNameValueTable(&layoutErrorNames, &layoutErrorValues, table);

   Tcl_IncrRefCount(layoutErrorNames);
   Tcl_IncrRefCount(layoutErrorValues);
}

static Tcl_Obj *newLayoutLongLongObj(long long value)
{
   if (0 <= value) {
      return newUintmaxObj(value);
   }

   if (value < INT_MIN) {
      Tcl_Panic("%s: unknown layout error: %lld", __func__, value);
   }

   Tcl_Obj *valueObj = Tcl_NewIntObj(value);
   Tcl_IncrRefCount(valueObj);

   Tcl_Obj *resultObj = NULL;
   int status = Tcl_DictObjGet(NULL, layoutErrorNames, valueObj, &resultObj);

   Tcl_DecrRefCount(valueObj);

   if (status != TCL_OK) {
      Tcl_Panic("%s: unknown layout error: %lld", __func__, value);
   }

   return resultObj;
}

//---------------------------------------------------------------------- enum

typedef struct EnumConsts
{
   Tcl_Obj    **labels;
   int	        n;
   const char  *names[];
} EnumConsts;

static Tcl_Obj *getEnum(EnumConsts *labels, int value)
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

//------------------------------------------------------------------ CXVersion

static Tcl_Obj *newVersionObj(CXVersion version)
{
   return Tcl_ObjPrintf("%d.%d.%d",
                        version.Major, version.Minor, version.Subminor);
}

//-------------------------------------------------------- bit mask operations

static Tcl_Obj *noneTagObj;        // = "-none"

typedef struct BitMask
{
   const char *name;
   unsigned    mask;
   Tcl_Obj    *nameObj;
} BitMask;

static Tcl_Obj *getBitMaskNameObj(BitMask *bitMask)
{
   Tcl_Obj *result = bitMask->nameObj;

   if (result == NULL) {
      result = bitMask->nameObj
             = Tcl_NewStringObj(bitMask->name, -1);
      Tcl_IncrRefCount(result);
   }

   return result;
}

static int bitMaskToString(Tcl_Interp *interp,
                           BitMask    *options,
                           Tcl_Obj    *none,
                           unsigned    mask)
{
   if (mask == 0) {
      if (none != NULL) {
         Tcl_SetObjResult(interp, noneTagObj);
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
         Tcl_Obj *nameObj = getBitMaskNameObj(&options[i]);
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
      Tcl_Obj *nameObj = getBitMaskNameObj(&options[i]);
      status = Tcl_ListObjAppendElement(interp, result, nameObj);
      value &= ~(1U << i);
   }

   return status;
}

//----------------------------------------------------------- CXSourceLocation

static Tcl_Obj *fileNameCache[64];
static Tcl_Obj *locationTagObj;
static Tcl_Obj *rangeTagObj;
static Tcl_Obj *filenameNullObj;

static Tcl_Obj *newFileNameObj(const char *filenameCstr)
{
   int hash = cstringHash(filenameCstr)
      % (sizeof fileNameCache / sizeof fileNameCache[0]);

   Tcl_Obj *candidate = fileNameCache[hash];
   if (candidate != NULL) {
      if (strcmp(Tcl_GetStringFromObj(candidate, NULL), filenameCstr) == 0) {
         return candidate;
      }
      Tcl_DecrRefCount(candidate);
   }

   Tcl_Obj *resultObj  = Tcl_NewStringObj(filenameCstr, -1);
   fileNameCache[hash] = resultObj;
   Tcl_IncrRefCount(resultObj);

   return resultObj;
}

static int getFileFromObj(Tcl_Interp        *interp,
                          CXTranslationUnit  tu,
                          Tcl_Obj           *obj,
                          CXFile            *file)
{
   const char *filename = Tcl_GetStringFromObj(obj, NULL);
   CXFile      output   = clang_getFile(tu, filename);
   if (file == NULL) {
      if (interp != NULL) {
         Tcl_SetObjResult
            (interp,
             Tcl_ObjPrintf("file %s is not a part of the translation unit.",
                           filename));
      }
      return TCL_ERROR;
   }

   *file = output;

   return TCL_OK;
}

static Tcl_Obj *newLocationObj(CXSourceLocation location)
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
   elms[tag_ix] = locationTagObj;
   for (int i = 0; i < nptrs; ++i) {
      elms[ptr_data_ix + i] = newPointerObj(location.ptr_data[i]);
   }
   elms[int_data_ix] = Tcl_NewLongObj(location.int_data);

   return Tcl_NewListObj(nelms, elms);
}

static int getLocationFromObj(Tcl_Interp       *interp,
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
       || (elms[tag_ix] != locationTagObj
           && strcmp(Tcl_GetStringFromObj(elms[tag_ix], NULL),
                     Tcl_GetStringFromObj(locationTagObj, NULL))
           != 0)) {
      goto invalid;
   }

   for (int i = 0; i < nptrs; ++i) {
      status = getPointerFromObj
         (interp, elms[ptr_data_ix + i], (void **)&location->ptr_data[i]);
      if (status != TCL_OK) {
         return status;
      }
   }

   unsigned value;
   status = getUnsignedFromObj(NULL, elms[int_data_ix], &value);
   if (status != TCL_OK) {
      goto invalid;
   }

   location->int_data = value;

   return status;

 invalid:
   if (interp != NULL) {
      Tcl_SetObjResult(interp,
                       Tcl_NewStringObj("invalid source location", -1));
   }

   return TCL_ERROR;
}

static Tcl_Obj *newRangeObj(CXSourceRange range)
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

   elms[tag_ix] = rangeTagObj;

   for (int i = 0; i < nptrs; ++i) {
      elms[ptr_data_ix + i] = newPointerObj(range.ptr_data[i]);
   }

   elms[begin_int_data_ix] = Tcl_NewLongObj(range.begin_int_data);
   elms[end_int_data_ix]   = Tcl_NewLongObj(range.end_int_data);

   return Tcl_NewListObj(nelms, elms);
}

static int getRangeFromObj(Tcl_Interp    *interp,
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
       || (elms[tag_ix] != rangeTagObj
           && strcmp(Tcl_GetStringFromObj(elms[tag_ix], NULL),
                     Tcl_GetStringFromObj(rangeTagObj, NULL)) != 0)) {
      goto invalid;
   }

   for (int i = 0; i < nptrs; ++i) {
      status = getPointerFromObj
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
      unsigned value;
      status = getUnsignedFromObj(NULL, elms[begin_int_data_ix + i], &value);
      if (status != TCL_OK) {
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

static Tcl_Obj *newPresumedLocationObj(CXSourceLocation location)
{
   enum {
      filename_ix,
      line_ix,
      column_ix,
      nelms
   };
   Tcl_Obj *elms[nelms];

   if (clang_equalLocations(location, clang_getNullLocation())) {

      elms[filename_ix]  = filenameNullObj;
      elms[line_ix]      = 
         elms[column_ix] = Tcl_NewIntObj(0);

   } else {

      CXString filename;
      unsigned line;
      unsigned column;
      clang_getPresumedLocation(location, &filename, &line, &column);


      const char *filenameCstr = clang_getCString(filename);
      elms[filename_ix]        = newFileNameObj(filenameCstr);
      clang_disposeString(filename);

      elms[line_ix]   = Tcl_NewLongObj(line);
      elms[column_ix] = Tcl_NewLongObj(column);

   }

   return Tcl_NewListObj(nelms, elms);
}

static Tcl_Obj *newDecodedLocationObj(CXFile   file,
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
      elms[filename_ix] = filenameNullObj;
   } else {
      CXString    filename     = clang_getFileName(file);
      const char *filenameCstr = clang_getCString(filename);
      elms[filename_ix]        = newFileNameObj(filenameCstr);
      clang_disposeString(filename);
   }

   elms[line_ix]   = Tcl_NewLongObj(line);
   elms[column_ix] = Tcl_NewLongObj(column);
   elms[offset_ix] = Tcl_NewLongObj(offset);

   return Tcl_NewListObj(nelms, elms);
}

//---------------------------------------------------------------------- index

/** The information associated to an index Tcl command.
 */
typedef struct IndexInfo
{
   Tcl_Interp *interp;
   Tcl_Obj    *children;        // Tcl list of child, such as a translation
                                // unit, command names
   CXIndex     index;
} IndexInfo;

static IndexInfo *createIndexInfo(Tcl_Interp *interp, CXIndex index)
{
   IndexInfo *info = (IndexInfo *)Tcl_Alloc(sizeof *info);
   info->interp    = interp;
   info->children  = Tcl_NewObj();
   info->index     = index;

   Tcl_IncrRefCount(info->children);

   return info;
}

/** A callback function called when an index Tcl command is deleted.
 * 
 * \param clientData pointer to IndexInfo
 */
static void indexDeleteProc(ClientData clientData)
{
   int status = TCL_OK;

   IndexInfo *info = (IndexInfo *)clientData;

   Tcl_Interp *interp = info->interp;

   // Until info->children becomes empty, delete the last command in the list.
   for (;;) {
      int n;
      status = Tcl_ListObjLength(NULL, info->children, &n);
      if (status != TCL_OK) {
         goto corrupted;
      }

      if (n <= 0) {
         break;
      }

      Tcl_Obj *child;
      status = Tcl_ListObjIndex(NULL, info->children, n - 1, &child);
      if (status != TCL_OK) {
         goto corrupted;
      }

      Tcl_IncrRefCount(child);
      const char *childName = Tcl_GetStringFromObj(child, NULL);
      Tcl_DeleteCommand(interp, childName);
      Tcl_DecrRefCount(child);
   }

   clang_disposeIndex(info->index);
   Tcl_DecrRefCount(info->children);
   Tcl_Free((char *)info);

   return;

 corrupted:
   Tcl_Panic("%s: info->children corrupted", __func__);
}

static void indexAddChild(IndexInfo *info, Tcl_Obj *child)
{
   Tcl_Obj *children = info->children;
   if (Tcl_IsShared(children)) {
      Tcl_Obj *newChildren = Tcl_DuplicateObj(children);
      Tcl_DecrRefCount(children);
      Tcl_IncrRefCount(newChildren);
      info->children = children = newChildren;
   }

   int status = Tcl_ListObjAppendElement(NULL, children, child);
   if (status != TCL_OK) {
      Tcl_Panic("%s: children list corrupted", __func__);
   }
}

static void indexRemoveChild(IndexInfo *info, Tcl_Obj *child)
{
   Tcl_Obj *children = info->children;
   if (Tcl_IsShared(children)) {
      Tcl_Obj *newChildren = Tcl_DuplicateObj(children);
      Tcl_DecrRefCount(children);
      Tcl_IncrRefCount(newChildren);
      info->children = children = newChildren;
   }

   int n;
   int status  = Tcl_ListObjLength(NULL, children, &n);
   if (status != TCL_OK) {
      Tcl_Panic("%s: children list corrupted", __func__);
   }

   int         child_strlen = 0;
   const char *child_str    = Tcl_GetStringFromObj(child, &child_strlen);
   int         found        = 0;
   for (int i = n - 1; 0 <= i; --i) {

      Tcl_Obj *elm;
      status = Tcl_ListObjIndex(NULL, children, i, &elm);
      if (status != TCL_OK) {
         break;
      }

      int         elm_strlen;
      const char *elm_str = Tcl_GetStringFromObj(elm, &elm_strlen);

      if (elm_str == child_str
          || (elm_strlen == child_strlen
              && memcmp(elm_str, child_str, elm_strlen) == 0)) {
         found = 1;
         status = Tcl_ListObjReplace(NULL, children, i, 1, 0, NULL);
         break;
      }

   }

   if (status == TCL_OK && ! found) {
      Tcl_Panic("child %s doesn't exist in the children list\n", child_str);
   }
}

//----------------------------------------------------------- translation unit

typedef struct TUInfo
{
   struct TUInfo     *next;
   IndexInfo         *parent;
   Tcl_Obj           *name;
   CXTranslationUnit  translationUnit;
} TUInfo;

static TUInfo *tuHashTable[32];

static int tuHash(CXTranslationUnit tu)
{
   int hashTableSize = sizeof tuHashTable / sizeof tuHashTable[0];
   return ((uintptr_t)tu / (sizeof(void *) * 4)) % hashTableSize;
}

static TUInfo * createTUInfo(IndexInfo         *parent,
                             Tcl_Obj           *tuName,
                             CXTranslationUnit  tu)
{
   TUInfo *info = (TUInfo *)Tcl_Alloc(sizeof *info);

   info->parent          = parent;
   info->name            = tuName;
   info->translationUnit = tu;

   Tcl_IncrRefCount(tuName);

   int hash          = tuHash(tu);
   info->next        = tuHashTable[hash];
   tuHashTable[hash] = info;

   indexAddChild(parent, tuName);

   return info;
}

static void tuDeleteProc(ClientData clientData)
{
   TUInfo *info = (TUInfo *)clientData;

   indexRemoveChild(info->parent, info->name);

   Tcl_DecrRefCount(info->name);
   clang_disposeTranslationUnit(info->translationUnit);

   int      hash = tuHash(info->translationUnit);
   TUInfo **prev = &tuHashTable[hash];
   while (*prev != info) {
      prev = &info->next;
   }
   *prev = info->next;

   Tcl_Free((char *)info);
}

static TUInfo * lookupTranslationUnit(CXTranslationUnit tu)
{
   int hash = tuHash(tu);
   for (TUInfo *p = tuHashTable[hash]; p != NULL; p = p->next) {
      if (p->translationUnit == tu) {
         return p;
      }
   }

   return NULL;
}

//----------------------------------------------------------------- diagnostic

static EnumConsts diagnosticSeverityLabels = {
   .names = {
      "ignored",
      "note",
      "warning",
      "error",
      "fatal"
   }
};

static Tcl_Obj *diagnosticSeverityTagObj;
static Tcl_Obj *diagnosticLocationTagObj;
static Tcl_Obj *diagnosticSpellingTagObj;
static Tcl_Obj *diagnosticEnableTagObj;
static Tcl_Obj *diagnosticDisableTagObj;
static Tcl_Obj *diagnosticCategoryTagObj;
static Tcl_Obj *diagnosticRangesTagObj;
static Tcl_Obj *diagnosticFixItsTagObj;

static Tcl_Obj *newDiagnosticObj(CXDiagnostic diagnostic)
{
   enum {
      severity_tag_ix,
      severity_ix,
      location_tag_ix,
      location_ix,
      spelling_tag_ix,
      spelling_ix,
      enable_tag_ix,
      enable_ix,
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

   resultArray[severity_tag_ix] = diagnosticSeverityTagObj;
   resultArray[location_tag_ix] = diagnosticLocationTagObj;
   resultArray[spelling_tag_ix] = diagnosticSpellingTagObj;
   resultArray[enable_tag_ix]   = diagnosticEnableTagObj;
   resultArray[disable_tag_ix]  = diagnosticDisableTagObj;
   resultArray[category_tag_ix] = diagnosticCategoryTagObj;
   resultArray[ranges_tag_ix]   = diagnosticRangesTagObj;
   resultArray[fixits_tag_ix]   = diagnosticFixItsTagObj;

   enum CXDiagnosticSeverity severity
      = clang_getDiagnosticSeverity(diagnostic);
   resultArray[severity_ix]
      = getEnum(&diagnosticSeverityLabels, severity);

   CXSourceLocation location = clang_getDiagnosticLocation(diagnostic);
   resultArray[location_ix]  = newLocationObj(location);

   CXString spelling        = clang_getDiagnosticSpelling(diagnostic);
   resultArray[spelling_ix] = convertCXStringToObj(spelling);

   CXString disable;
   CXString option         = clang_getDiagnosticOption(diagnostic, &disable);
   resultArray[enable_ix]  = convertCXStringToObj(option);
   resultArray[disable_ix] = convertCXStringToObj(disable);

   CXString category        = clang_getDiagnosticCategoryText(diagnostic);
   resultArray[category_ix] = convertCXStringToObj(category);

   unsigned   numRanges = clang_getDiagnosticNumRanges(diagnostic);
   Tcl_Obj  **ranges    = (Tcl_Obj **)Tcl_Alloc(sizeof(Tcl_Obj *) * numRanges);
   for (unsigned i = 0; i < numRanges; ++i) {
      CXSourceRange range = clang_getDiagnosticRange(diagnostic, i);
      ranges[i]           = newRangeObj(range);
   }
   resultArray[ranges_ix] = Tcl_NewListObj(numRanges, ranges);
   Tcl_Free((char *)ranges);

   unsigned   numFixIts = clang_getDiagnosticNumFixIts(diagnostic);
   Tcl_Obj  **fixits    = (Tcl_Obj **)Tcl_Alloc(sizeof(Tcl_Obj *) * numFixIts);
   for (unsigned i = 0; i < numFixIts; ++i) {
      CXSourceRange range;
      CXString      fixitStr = clang_getDiagnosticFixIt(diagnostic, i, &range);

      Tcl_Obj *fixit[2];
      fixit[0] = newRangeObj(range);
      fixit[1] = convertCXStringToObj(fixitStr);

      fixits[i] = Tcl_NewListObj(2, fixit);
   }
   resultArray[fixits_ix] = Tcl_NewListObj(numFixIts, fixits);
   Tcl_Free((char *)fixits);

   return Tcl_NewListObj(nelms, resultArray);
}

//--------------------------------------------------------------------- cursor

static Tcl_Obj *cursorKindNames;
static Tcl_Obj *cursorKindValues;

static void createCursorKindTable(void)
{
   static NameValuePair table[] = {
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
      /* { "CXCursor_FirstDecl", */
      /*   CXCursor_FirstDecl }, */
      /* { "CXCursor_LastDecl", */
      /*   CXCursor_LastDecl }, */
      /* { "FirstRef", */
      /*   CXCursor_FirstRef }, */
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
      /* { "LastRef", */
      /*   CXCursor_LastRef }, */
      /* { "FirstInvalid", */
      /*   CXCursor_FirstInvalid }, */
      { "InvalidFile",
        CXCursor_InvalidFile },
      { "NoDeclFound",
        CXCursor_NoDeclFound },
      { "NotImplemented",
        CXCursor_NotImplemented },
      { "InvalidCode",
        CXCursor_InvalidCode },
      /* { "LastInvalid", */
      /*   CXCursor_LastInvalid }, */
      /* { "FirstExpr", */
      /*   CXCursor_FirstExpr }, */
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
#if CINDEX_VERSION_MINOR >= 31
      { "OMPArraySectionExpr",
        CXCursor_OMPArraySectionExpr },
#endif
#if CINDEX_VERSION_MINOR >= 36
      { "ObjCAvailabilityCheckExpr",
        CXCursor_ObjCAvailabilityCheckExpr },
#endif
      /* { "LastExpr", */
      /*   CXCursor_LastExpr }, */
      /* { "FirstStmt", */
      /*   CXCursor_FirstStmt }, */
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
      /* { "GCCAsmStmt", */
      /*   CXCursor_GCCAsmStmt }, */
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
#if CINDEX_VERSION_MINOR >= 25
      { "OMPSimdDirective",
        CXCursor_OMPSimdDirective },
#endif
#if CINDEX_VERSION_MINOR >= 28
      { "OMPForDirective",
        CXCursor_OMPForDirective },
      { "OMPSectionsDirective",
        CXCursor_OMPSectionsDirective },
      { "OMPSectionDirective",
        CXCursor_OMPSectionDirective },
      { "OMPSingleDirective",
        CXCursor_OMPSingleDirective },
      { "OMPParallelForDirective",
        CXCursor_OMPParallelForDirective },
      { "OMPParallelSectionsDirective",
        CXCursor_OMPParallelSectionsDirective },
      { "OMPTaskDirective",
        CXCursor_OMPTaskDirective },
      { "OMPMasterDirective",
        CXCursor_OMPMasterDirective },
      { "OMPCriticalDirective",
        CXCursor_OMPCriticalDirective },
      { "OMPTaskyieldDirective",
        CXCursor_OMPTaskyieldDirective },
      { "OMPBarrierDirective",
        CXCursor_OMPBarrierDirective },
      { "OMPTaskwaitDirective",
        CXCursor_OMPTaskwaitDirective },
      { "OMPFlushDirective",
        CXCursor_OMPFlushDirective },
      { "SEHLeaveStmt",
        CXCursor_SEHLeaveStmt },
      { "OMPOrderedDirective",
        CXCursor_OMPOrderedDirective },
      { "OMPAtomicDirective",
        CXCursor_OMPAtomicDirective },
#endif
#if CINDEX_VERSION_MINOR >= 29
      { "OMPForSimdDirective",
        CXCursor_OMPForSimdDirective },
      { "OMPParallelForSimdDirective",
        CXCursor_OMPParallelForSimdDirective },
      { "OMPTargetDirective",
        CXCursor_OMPTargetDirective },
      { "OMPTeamsDirective",
        CXCursor_OMPTeamsDirective },
#endif
#if CINDEX_VERSION_MINOR >= 31
      { "OMPTaskgroupDirective",
        CXCursor_OMPTaskgroupDirective },
      { "OMPCancellationPointDirective",
        CXCursor_OMPCancellationPointDirective },
      { "OMPCancelDirective",
        CXCursor_OMPCancelDirective },
      { "OMPTargetDataDirective",
        CXCursor_OMPTargetDataDirective },
#endif
#if CINDEX_VERSION_MINOR >= 32
      { "OMPTaskLoopDirective",
        CXCursor_OMPTaskLoopDirective },
      { "OMPTaskLoopSimdDirective",
        CXCursor_OMPTaskLoopSimdDirective },
#endif
#if CINDEX_VERSION_MINOR >= 33
      { "OMPDistributeDirective",
        CXCursor_OMPDistributeDirective },
#endif
#if CINDEX_VERSION_MINOR >= 34
      { "OMPTargetEnterDataDirective",
        CXCursor_OMPTargetEnterDataDirective },
      { "OMPTargetExitDataDirective",
        CXCursor_OMPTargetExitDataDirective },
      { "OMPTargetParallelDirective",
        CXCursor_OMPTargetParallelDirective },
      { "OMPTargetParallelForDirective",
        CXCursor_OMPTargetParallelForDirective },
#endif
#if CINDEX_VERSION_MINOR >= 35
      { "OMPTargetUpdateDirective",
        CXCursor_OMPTargetUpdateDirective },
#endif
#if CINDEX_VERSION_MINOR >= 36
      { "OMPDistributeParallelForDirective",
        CXCursor_OMPDistributeParallelForDirective },
      { "OMPDistributeSimdDirective",
        CXCursor_OMPDistributeSimdDirective },
      { "OMPTargetParallelForSimdDirective",
        CXCursor_OMPTargetParallelForSimdDirective },
      { "OMPTargetSimdDirective",
        CXCursor_OMPTargetSimdDirective },
      { "OMPTeamsDistributeDirective",
        CXCursor_OMPTeamsDistributeDirective },
      { "OMPTeamsDistributeSimdDirective",
        CXCursor_OMPTeamsDistributeSimdDirective },
#endif
      /* { "LastStmt", */
      /*   CXCursor_LastStmt }, */
      { "TranslationUnit",
        CXCursor_TranslationUnit },
      /* { "FirstAttr", */
      /*   CXCursor_FirstAttr }, */
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
#if CINDEX_VERSION_MINOR >= 26
      { "PureAttr",
        CXCursor_PureAttr },
      { "ConstAttr",
        CXCursor_ConstAttr },
      { "NoDuplicateAttr",
        CXCursor_NoDuplicateAttr },
#endif
#if CINDEX_VERSION_MINOR >= 28
      { "CUDAConstantAttr",
        CXCursor_CUDAConstantAttr },
      { "CUDADeviceAttr",
        CXCursor_CUDADeviceAttr },
      { "CUDAGlobalAttr",
        CXCursor_CUDAGlobalAttr },
      { "CUDAHostAttr",
        CXCursor_CUDAHostAttr },
      { "CUDASharedAttr",
        CXCursor_CUDASharedAttr },
#endif
#if CINDEX_VERSION_MINOR >= 31
      { "VisibilityAttr",
        CXCursor_VisibilityAttr },
#endif
#if CINDEX_VERSION_MINOR >= 32
      { "DLLExport",
        CXCursor_DLLExport },
      { "DLLImport",
        CXCursor_DLLImport },
#endif
      /* { "LastAttr", */
      /*   CXCursor_LastAttr }, */
      /* { "FirstPreprocessing", */
      /*   CXCursor_FirstPreprocessing }, */
      { "PreprocessingDirective",
        CXCursor_PreprocessingDirective },
      { "MacroDefinition",
        CXCursor_MacroDefinition },
      { "MacroExpansion",
        CXCursor_MacroExpansion },
      { "MacroInstantiation",
        CXCursor_MacroInstantiation },
      { "InclusionDirective",
        CXCursor_InclusionDirective },
      /* { "LastPreprocessing", */
      /*   CXCursor_LastPreprocessing }, */
      /* { "FirstExtraDecl", */
      /*   CXCursor_FirstExtraDecl }, */
      { "ModuleImportDecl",
        CXCursor_ModuleImportDecl },
#if CINDEX_VERSION_MINOR >= 32
      { "TypeAliasTemplateDecl",
        CXCursor_TypeAliasTemplateDecl },
#endif
#if CINDEX_VERSION_MINOR >= 36
      { "StaticAssert",
        CXCursor_StaticAssert },
      { "FriendDecl",
        CXCursor_FriendDecl },
#endif
      /* { "LastExtraDecl", */
      /*   CXCursor_LastExtraDecl }, */
#if CINDEX_VERSION_MINOR >= 30
      { "OverloadCandidate",
        CXCursor_OverloadCandidate },
#endif
      { NULL }
   };

   createNameValueTable(&cursorKindNames, &cursorKindValues, table);

   Tcl_IncrRefCount(cursorKindNames);
   Tcl_IncrRefCount(cursorKindValues);
}

static Tcl_Obj *newCursorObj(CXCursor cursor)
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
   if (Tcl_DictObjGet(NULL, cursorKindNames, kind, &kindName) != TCL_OK) {
      Tcl_Panic("cursor kind %d is not valid", cursor.kind);
   }
   Tcl_DecrRefCount(kind);
   elms[kind_ix]  = kindName;
   elms[xdata_ix] = Tcl_NewLongObj(cursor.xdata);

   for (int i = 0; i < ndata; ++i) {
      elms[data_ix + i] = newPointerObj(cursor.data[i]);
   }

   return Tcl_NewListObj(nelms, elms);
}

static int
getCursorFromObj(Tcl_Interp *interp, Tcl_Obj *obj, CXCursor *cursor)
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
   if (Tcl_DictObjGet(NULL, cursorKindValues, elms[kind_ix], &kindObj)
       != TCL_OK) {
      Tcl_Panic("cursorKindValues corrupted");
   }

   if (kindObj == NULL) {
      const char *kind = Tcl_GetStringFromObj(elms[kind_ix], NULL);
      Tcl_SetObjResult(interp,
                       Tcl_ObjPrintf("invalid cursor kind: %s", kind));
      return TCL_ERROR;
   }

   int kind;
   status = Tcl_GetIntFromObj(NULL, kindObj, &kind);
   if (status != TCL_OK) {
      Tcl_Panic("cursorKindValues corrupted");
   }
   result.kind = kind;

   status = Tcl_GetIntFromObj(NULL, elms[xdata_ix], &result.xdata);
   if (status != TCL_OK) {
      goto invalid_cursor;
   }

   for (int i = 0; i < ndata; ++i) {
      status = getPointerFromObj(NULL, elms[data_ix + i],
                                 (void **)&result.data[i]);
      if (status != TCL_OK) {
         goto invalid_cursor;
      }
   }

   CXTranslationUnit tu = clang_Cursor_getTranslationUnit(result);
   if (lookupTranslationUnit(tu) == NULL) {
      goto invalid_cursor;
   }

   *cursor = result;

   return TCL_OK;

 invalid_cursor:
   Tcl_SetObjResult(interp, Tcl_NewStringObj("invalid cursor object", -1));

   return TCL_ERROR;
}

//----------------------------------------------------------------------- type

static Tcl_Obj *typeKindValues;
static Tcl_Obj *typeKindNames;

static void createCXTypeTable(void)
{
   static NameValuePair table[] = {
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
#if CINDEX_VERSION_MINOR >= 35
      { "Float128",
        CXType_Float128 },
#endif
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
#if CINDEX_VERSION_MINOR >= 32
      { "Auto",
        CXType_Auto },
#endif
#if CINDEX_VERSION_MINOR >= 35
      { "Elaborated",
        CXType_Elaborated },
#endif
      { NULL }
   };

   createNameValueTable(&typeKindNames, &typeKindValues, table);

   Tcl_IncrRefCount(typeKindNames);
   Tcl_IncrRefCount(typeKindValues);
}

static Tcl_Obj *newCXTypeObj(CXType type)
{
   Tcl_Obj *kind = Tcl_NewIntObj(type.kind);
   Tcl_IncrRefCount(kind);
   Tcl_Obj *kindName;
   if (Tcl_DictObjGet(NULL, typeKindNames, kind, &kindName) != TCL_OK
       || kindName == NULL) {
      Tcl_Panic("typeKindNames(%d) corrupted", type.kind);
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
      elements[data_ix + i] = newPointerObj(type.data[i]);
   }

   return Tcl_NewListObj(nelms, elements);
}

static int getTypeFromObj(Tcl_Interp *interp, Tcl_Obj *obj, CXType *output)
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
   if (Tcl_DictObjGet(NULL, typeKindValues, elms[kind_ix], &kindObj)
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
      if (getPointerFromObj(NULL, elms[data_ix + i], &result.data[i])
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

//--------------------------------------------------------- type equal command

static int typeEqualObjCmd(ClientData     clientData,
                           Tcl_Interp    *interp,
                           int            objc,
                           Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      type1_ix,
      type2_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "type1 type2");
      return TCL_ERROR;
   }

   CXType types[2];
   for (int i = 0; i < 2; ++i) {
      int status = getTypeFromObj(interp, objv[type1_ix + i], &types[i]);
      if (status != TCL_OK) {
         return status;
      }
   }

   int      result    = clang_equalTypes(types[0], types[1]) != 0;
   Tcl_Obj *resultObj = Tcl_NewIntObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//--------------------------------- type functionTypeCallingConvention command

static Tcl_Obj *callingConvValues;
static Tcl_Obj *callingConvNames;

static void createCallingConvTable(void)
{
   static NameValuePair table[] = {
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
#if CINDEX_VERSION_MINOR >= 36
      { "X86RegCall",
        CXCallingConv_X86RegCall },
#elif CINDEX_VERSION_MINOR >= 30
      { "Unused", 8},
#else
      { "PnaclCall",
        CXCallingConv_PnaclCall },
#endif
      { "IntelOclBicc",
        CXCallingConv_IntelOclBicc },
      { "X86_64Win64",
        CXCallingConv_X86_64Win64 },
      { "X86_64SysV",
        CXCallingConv_X86_64SysV },
#if CINDEX_VERSION_MINOR >= 30
      { "X86VectorCall",
        CXCallingConv_X86VectorCall },
#endif
#if CINDEX_VERSION_MINOR >= 34
      { "Swift",
        CXCallingConv_Swift },
#endif
#if CINDEX_VERSION_MINOR >= 35
      { "PreserveMost",
        CXCallingConv_PreserveMost },
      { "PreserveAll",
        CXCallingConv_PreserveAll },
#endif
      { "Invalid",
        CXCallingConv_Invalid },
      { "Unexposed",
        CXCallingConv_Unexposed },
      { NULL }
   };

   createNameValueTable(&callingConvNames, &callingConvValues, table);

   Tcl_IncrRefCount(callingConvNames);
   Tcl_IncrRefCount(callingConvValues);
}

//------------------------------------------------------ type offsetof command

static int typeOffsetOfObjCmd(ClientData     clientData,
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
   int status = getTypeFromObj(interp, objv[type_ix], &cxtype);
   if (status != TCL_OK) {
      return status;
   }

   const char *field     = Tcl_GetStringFromObj(objv[field_ix], NULL);
   long long   result    = clang_Type_getOffsetOf(cxtype, field);
   Tcl_Obj    *resultObj = newLayoutLongLongObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//-------------------------------------------------------- type -> int command

static int typeToIntObjCmd(ClientData     clientData,
                           Tcl_Interp    *interp,
                           int            objc,
                           Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      type_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "type");
      return TCL_ERROR;
   }

   CXType type;
   int status = getTypeFromObj(interp, objv[type_ix], &type);
   if (status != TCL_OK) {
      return status;
   }

   int      result    = ((int (*)(CXType))clientData)(type);
   Tcl_Obj *resultObj = Tcl_NewIntObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//------------------------------------------------------- type -> bool command

static int typeToBoolObjCmd(ClientData     clientData,
                            Tcl_Interp    *interp,
                            int            objc,
                            Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      type_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "type");
      return TCL_ERROR;
   }

   CXType type;
   int status = getTypeFromObj(interp, objv[type_ix], &type);
   if (status != TCL_OK) {
      return status;
   }

   int      result    = ((unsigned (*)(CXType))clientData)(type) != 0;
   Tcl_Obj *resultObj = Tcl_NewIntObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//-------------------------------------------------- type -> long long command

static int typeToLongLongObjCmd(ClientData     clientData,
                                Tcl_Interp    *interp,
                                int            objc,
                                Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      type_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "type");
      return TCL_ERROR;
   }

   CXType type;
   int status = getTypeFromObj(interp, objv[type_ix], &type);
   if (status != TCL_OK) {
      return status;
   }

   long long  result    = ((long long (*)(CXType))clientData)(type);
   Tcl_Obj   *resultObj = newIntmaxObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//----------------------------------------------------- type -> string command

static int typeToStringObjCmd(ClientData     clientData,
                              Tcl_Interp    *interp,
                              int            objc,
                              Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      type_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "type");
      return TCL_ERROR;
   }

   CXType type;
   int status = getTypeFromObj(interp, objv[type_ix], &type);
   if (status != TCL_OK) {
      return status;
   }

   CXString (*proc)(CXType) = (CXString (*)(CXType))clientData;
   CXString    result       = proc(type);
   const char *resultCstr   = clang_getCString(result);
   Tcl_Obj    *resultObj    = Tcl_NewStringObj(resultCstr, -1);
   Tcl_SetObjResult(interp, resultObj);
   clang_disposeString(result);

   return TCL_OK;
}

//------------------------------------------------------- type -> enum command

typedef int (*TypeToEnumProc)(CXType);

typedef struct TypeToEnumInfo
{
   EnumConsts     *labels;
   TypeToEnumProc  proc;
} TypeToEnumInfo;

static int typeToEnumObjCmd(ClientData     clientData,
                              Tcl_Interp    *interp,
                              int            objc,
                              Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      type_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "type");
      return TCL_ERROR;
   }

   CXType type;
   int status = getTypeFromObj(interp, objv[type_ix], &type);
   if (status != TCL_OK) {
      return status;
   }

   TypeToEnumInfo *info = (TypeToEnumInfo *)clientData;

   int      result    = (info->proc)(type);
   Tcl_Obj *resultObj = getEnum(info->labels, result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//------------------------------------------------ type -> named value command

typedef unsigned (*TypeToNamedValueProc)(CXType);

typedef struct TypeToNamedValueInfo
{
   Tcl_Obj              *names;
   TypeToNamedValueProc  proc;
} TypeToNamedValueInfo;

static int typeToNamedValueObjCmd(ClientData     clientData,
                                  Tcl_Interp    *interp,
                                  int            objc,
                                  Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      type_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "type");
      return TCL_ERROR;
   }

   CXType type;
   int status = getTypeFromObj(interp, objv[type_ix], &type);
   if (status != TCL_OK) {
      return status;
   }

   TypeToNamedValueInfo *info = (TypeToNamedValueInfo *)clientData;

   unsigned  value    = info->proc(type);
   Tcl_Obj  *valueObj = Tcl_NewLongObj(value);
   Tcl_IncrRefCount(valueObj);

   Tcl_Obj *resultObj;
   status = Tcl_DictObjGet(NULL, info->names, valueObj, &resultObj);
   Tcl_DecrRefCount(valueObj);
   if (status != TCL_OK) {
      Tcl_Panic("%s: unknown value: %d", __func__, value);
   }

   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//----------------------------------------------------- type -> cursor command

static int typeToCursorObjCmd(ClientData     clientData,
                              Tcl_Interp    *interp,
                              int            objc,
                              Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      type_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "type");
      return TCL_ERROR;
   }

   CXType type;
   int status = getTypeFromObj(interp, objv[type_ix], &type);
   if (status != TCL_OK) {
      return status;
   }

   CXCursor  result    = ((CXCursor (*)(CXType))clientData)(type);
   Tcl_Obj  *resultObj = newCursorObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//------------------------------------------------------- type -> type command

static int typeToTypeObjCmd(ClientData     clientData,
                            Tcl_Interp    *interp,
                            int            objc,
                            Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      type_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "type");
      return TCL_ERROR;
   }

   CXType type;
   int status = getTypeFromObj(interp, objv[type_ix], &type);
   if (status != TCL_OK) {
      return status;
   }

   CXType   result    = ((CXType (*)(CXType))clientData)(type);
   Tcl_Obj *resultObj = newCXTypeObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//--------------------------------------------- type, unsigned -> type command

static int typeUnsignedToTypeObjCmd(ClientData     clientData,
                                    Tcl_Interp    *interp,
                                    int            objc,
                                    Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      type_ix,
      number_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "type unsigned");
      return TCL_ERROR;
   }

   CXType type;
   int status = getTypeFromObj(interp, objv[type_ix], &type);
   if (status != TCL_OK) {
      return status;
   }

   unsigned number;
   status = getUnsignedFromObj(interp, objv[number_ix], &number);
   if (status != TCL_OK) {
      return status;
   }

   typedef CXType (*ProcType)(CXType, unsigned);

   CXType   result    = ((ProcType)clientData)(type, number);
   Tcl_Obj *resultObj = newCXTypeObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//------------------------------------------- type -> layout long long command

static int typeToLayoutLongLongObjCmd(ClientData     clientData,
                                      Tcl_Interp    *interp,
                                      int            objc,
                                      Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      type_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "type");
      return TCL_ERROR;
   }

   CXType cxtype;
   int status = getTypeFromObj(interp, objv[type_ix], &cxtype);
   if (status != TCL_OK) {
      return status;
   }

   long long  result    = ((long long (*)(CXType))clientData)(cxtype);
   Tcl_Obj   *resultObj = newLayoutLongLongObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//------------------------------------------------------ cursor::equal command

static int cursorEqualObjCmd(ClientData     clientData,
                             Tcl_Interp    *interp,
                             int            objc,
                             Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      cursor1_ix,
      cursor2_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "cursor1 cursor2");
      return TCL_ERROR;
   }

   CXCursor cursors[2];
   for (int i = 0; i < 2; ++i) {
      int status = getCursorFromObj(interp, objv[cursor1_ix + i], &cursors[i]);
      if (status != TCL_OK) {
         return status;
      }
   }

   int      result    = clang_equalCursors(cursors[0], cursors[1]) != 0;
   Tcl_Obj *resultObj = Tcl_NewIntObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//-------------------------------------- cursor::enumConstantDeclValue command

static int cursorEnumConstantDeclValueObjCmd(ClientData     clientData,
                                             Tcl_Interp    *interp,
                                             int            objc,
                                             Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      cursor_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = getCursorFromObj(interp, objv[cursor_ix], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   if (cursor.kind != CXCursor_EnumConstantDecl) {
      Tcl_SetObjResult
         (interp,
          Tcl_NewStringObj("cursor kind must be EnumConstantDecl", -1));
      return TCL_ERROR;
   }

   Tcl_Obj *result = NULL;

   CXCursor parent  = clang_getCursorSemanticParent(cursor);
   CXType   inttype = clang_getEnumDeclIntegerType(parent);
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
      unsigned long long value
         = clang_getEnumConstantDeclUnsignedValue(cursor);
      result = newUintmaxObj(value);
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
      result = newIntmaxObj(value);
      break;
   }

   default:
      Tcl_Panic("clang_getEnumDeclIntegerType returns unexpected type: %d",
                inttype.kind);
   }

   Tcl_SetObjResult(interp, result);

   return TCL_OK;
}

//------------------------------------------------------- cursor::null command

static int cursorNullObjCmd(ClientData     clientData,
                            Tcl_Interp    *interp,
                            int            objc,
                            Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "");
      return TCL_ERROR;
   }

   CXCursor  result    = clang_getNullCursor();
   Tcl_Obj  *resultObj = newCursorObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//------------------------------------------ cursor overridenCursors command

static int cursorOverriddenCursorsObjCmd(ClientData     clientData,
                                         Tcl_Interp    *interp,
                                         int            objc,
                                         Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      cursor_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = getCursorFromObj(interp, objv[cursor_ix], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   CXCursor *overridden    = NULL;
   unsigned  numOverridden = 0;
   clang_getOverriddenCursors(cursor, &overridden, &numOverridden);

   Tcl_Obj **results
      = (Tcl_Obj **)Tcl_Alloc(sizeof(Tcl_Obj *) * numOverridden);
   for (int i = 0; i < numOverridden; ++i) {
      results[i] = newCursorObj(overridden[i]);
   }

   clang_disposeOverriddenCursors(overridden);

   Tcl_Obj *resultObj = Tcl_NewListObj(numOverridden, results);
   Tcl_SetObjResult(interp, resultObj);

   Tcl_Free((char *)results);

   return TCL_OK;
}

//-------------------------------------- cursor platformAvailability command

static Tcl_Obj *alwaysDeprecatedTagObj;
static Tcl_Obj *deprecatedMessageTagObj;
static Tcl_Obj *alwaysUnavailableTagObj;
static Tcl_Obj *unavailableMessageTagObj;
static Tcl_Obj *availabilityTagObj;
static Tcl_Obj *availabilityPlatformTagObj;
static Tcl_Obj *availabilityIntroducedTagObj;
static Tcl_Obj *availabilityDeprecatedTagObj;
static Tcl_Obj *availabilityObsoletedTagObj;
static Tcl_Obj *availabilityUnavailableTagObj;
static Tcl_Obj *availabilityMessageTagObj;

static int cursorPlatformAvailabilityObjCmd(ClientData     clientData,
                                            Tcl_Interp    *interp,
                                            int            objc,
                                            Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      cursor_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = getCursorFromObj(interp, objv[cursor_ix], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   int availability_size
      = clang_getCursorPlatformAvailability
          (cursor, NULL, NULL, NULL, NULL, NULL, 0);

   CXPlatformAvailability *availability
      = (CXPlatformAvailability *)Tcl_Alloc(sizeof(CXPlatformAvailability)
                                            * availability_size);

   int      always_deprecated;
   CXString deprecated_message;
   int      always_unavailable;
   CXString unavailable_message;
   clang_getCursorPlatformAvailability(cursor,
                                       &always_deprecated,
                                       &deprecated_message,
                                       &always_unavailable,
                                       &unavailable_message,
                                       availability,
                                       availability_size);

   enum {
      always_deprecated_tag_ix,
      always_deprecated_ix,
      deprecated_message_tag_ix,
      deprecated_message_ix,
      always_unavailable_tag_ix,
      always_unavailable_ix,
      unavailable_message_tag_ix,
      unavailable_message_ix,
      availability_tag_ix,
      availability_ix,
      nelms
   };

   Tcl_Obj *resultElms[nelms];

   resultElms[always_deprecated_tag_ix]
      = alwaysDeprecatedTagObj;
   resultElms[always_deprecated_ix]
      = Tcl_NewIntObj(always_deprecated);

   resultElms[deprecated_message_tag_ix]
      = deprecatedMessageTagObj;
   resultElms[deprecated_message_ix]
      = convertCXStringToObj(deprecated_message);

   resultElms[always_unavailable_tag_ix]
      = alwaysUnavailableTagObj;
   resultElms[always_unavailable_ix]
      = Tcl_NewIntObj(always_unavailable);

   resultElms[unavailable_message_tag_ix]
      = unavailableMessageTagObj;
   resultElms[unavailable_message_ix]
      = convertCXStringToObj(unavailable_message);

   resultElms[availability_tag_ix]
      = availabilityTagObj;
   resultElms[availability_ix]
      = Tcl_NewObj();
   for (int i = 0; i < availability_size; ++i) {
      enum {
         platform_tag_ix,
         platform_ix,
         introduced_tag_ix,
         introduced_ix,
         deprecated_tag_ix,
         deprecated_ix,
         obsoleted_tag_ix,
         obsoleted_ix,
         unavailable_tag_ix,
         unavailable_ix,
         message_tag_ix,
         message_ix,
         nelms
      };

      Tcl_Obj *elms[nelms];

      elms[platform_tag_ix]
         = availabilityPlatformTagObj;
      elms[platform_ix]
         = convertCXStringToObj(availability[i].Platform);

      elms[introduced_tag_ix]
         = availabilityIntroducedTagObj;
      elms[introduced_ix]
         = newVersionObj(availability[i].Introduced);

      elms[deprecated_tag_ix]
         = availabilityDeprecatedTagObj;
      elms[deprecated_ix]
         = newVersionObj(availability[i].Deprecated);

      elms[obsoleted_tag_ix]
         = availabilityObsoletedTagObj;
      elms[obsoleted_ix]
         = newVersionObj(availability[i].Obsoleted);

      elms[unavailable_tag_ix]
         = availabilityUnavailableTagObj;
      elms[unavailable_ix]
         = Tcl_NewIntObj(availability[i].Unavailable != 0);

      elms[message_tag_ix]
         = availabilityMessageTagObj;
      elms[message_ix]
         = convertCXStringToObj(availability[i].Message);

      Tcl_Obj *elm = Tcl_NewListObj(nelms, elms);
      Tcl_ListObjAppendElement(NULL, resultElms[i], elm);

      clang_disposeCXPlatformAvailability(availability + i);
   }

   Tcl_Free((char *)availability);

   Tcl_Obj *resultObj = Tcl_NewListObj(nelms, resultElms);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//---------------------------------------- cursor referenceNameRange command

static int cursorReferenceNameRangeObjCmd(ClientData     clientData,
                                          Tcl_Interp    *interp,
                                          int            objc,
                                          Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      options_ix
   };

   static const char *options[] = {
      "-wantQualifier",
      "-wantTemplateArgs",
      "-wantSinglePiece",
      NULL
   };

   unsigned flags = 0;
   int i;
   for (i = 1; i < objc; ++i) {
      const char *arg = Tcl_GetStringFromObj(objv[i], NULL);
      if (arg[0] != '-') {
         break;
      }

      int optionNumber;
      int status = Tcl_GetIndexFromObj(interp, objv[i], options,
                                       "option", 0, &optionNumber);
      if (status != TCL_OK) {
         return TCL_ERROR;
      }

      flags |= 1 << optionNumber;
   }

   Tcl_Obj *cursorObj     = objv[i];
   Tcl_Obj *pieceIndexObj = objv[i + 1];
   if (i + 2 != objc) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv,
                       "?options...? cursor pieceIndex");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = getCursorFromObj(interp, cursorObj, &cursor);
   if (status != TCL_OK) {
      return status;
   }

   unsigned pieceIndex;
   status = getUnsignedFromObj(interp, pieceIndexObj, &pieceIndex);
   if (status != TCL_OK) {
      return status;
   }

   CXSourceRange result
      = clang_getCursorReferenceNameRange(cursor, flags, pieceIndex);
   Tcl_Obj *resultObj = newRangeObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//------------------------------------------ cursor::translationUnit command

static int cursorTranslationUnitObjCmd(ClientData     clientData,
                                       Tcl_Interp    *interp,
                                       int            objc,
                                       Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      cursor_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = getCursorFromObj(interp, objv[cursor_ix], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   CXTranslationUnit  tu   = clang_Cursor_getTranslationUnit(cursor);
   TUInfo            *info = lookupTranslationUnit(tu);
   if (info == NULL) {
      Tcl_Panic("invalid cursor");
   }

   Tcl_SetObjResult(interp, info->name);

   return TCL_OK;
}

//------------------------------------------------------ cursor -> int command

static int cursorToIntObjCmd(ClientData     clientData,
                             Tcl_Interp    *interp,
                             int            objc,
                             Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      cursor_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = getCursorFromObj(interp, objv[cursor_ix], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   int      result    = ((int (*)(CXCursor))clientData)(cursor);
   Tcl_Obj *resultObj = Tcl_NewIntObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//------------------------------------------------- cursor -> unsigned command

static int cursorToUnsignedObjCmd(ClientData     clientData,
                                  Tcl_Interp    *interp,
                                  int            objc,
                                  Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      cursor_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = getCursorFromObj(interp, objv[cursor_ix], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   unsigned  result    = ((unsigned (*)(CXCursor))clientData)(cursor);
   Tcl_Obj  *resultObj = Tcl_NewLongObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//----------------------------------------------------- cursor -> bool command

static int cursorToBoolObjCmd(ClientData     clientData,
                              Tcl_Interp    *interp,
                              int            objc,
                              Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      cursor_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = getCursorFromObj(interp, objv[cursor_ix], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   unsigned  value     = ((unsigned (*)(CXCursor))clientData)(cursor);
   int       result    = value != 0;
   Tcl_Obj  *resultObj = Tcl_NewIntObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//----------------------------------------------------- cursor -> enum command

typedef int (*CursorToEnumProc)(CXCursor);

typedef struct CursorToEnumInfo
{
   EnumConsts       *labels;
   CursorToEnumProc  proc;
} CursorToEnumInfo;

static int cursorToEnumObjCmd(ClientData     clientData,
                              Tcl_Interp    *interp,
                              int            objc,
                              Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      cursor_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = getCursorFromObj(interp, objv[cursor_ix], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   CursorToEnumInfo *info = (CursorToEnumInfo *)clientData;

   int      result    = (info->proc)(cursor);
   Tcl_Obj *resultObj = getEnum(info->labels, result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//-------------------------------------------------- cursor -> bitmask command

typedef unsigned (*CursorToBitMaskProc)(CXCursor);

typedef struct CursorToBitMaskInfo
{
   BitMask             *masks;
   Tcl_Obj             *none;
   CursorToBitMaskProc  proc;
} CursorToBitMaskInfo;

static int cursorToBitMaskObjCmd(ClientData     clientData,
                                 Tcl_Interp    *interp,
                                 int            objc,
                                 Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      cursor_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = getCursorFromObj(interp, objv[cursor_ix], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   CursorToBitMaskInfo *info = (CursorToBitMaskInfo *)clientData;

   unsigned value = (info->proc)(cursor);
   return bitMaskToString(interp, info->masks, info->none, value);
}

static unsigned getObjCPropertyAttributes(CXCursor cursor)
{
   return clang_Cursor_getObjCPropertyAttributes(cursor, 0);
}

//--------------------------------------------------- cursor -> string command

static int cursorToStringObjCmd(ClientData     clientData,
                                Tcl_Interp    *interp,
                                int            objc,
                                Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      cursor_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = getCursorFromObj(interp, objv[cursor_ix], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   CXString  result    = ((CXString (*)(CXCursor))clientData)(cursor);
   Tcl_Obj  *resultObj = convertCXStringToObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//----------------------------------------------------- cursor -> file command

static int cursorToFileObjCmd(ClientData     clientData,
                              Tcl_Interp    *interp,
                              int            objc,
                              Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      cursor_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = getCursorFromObj(interp, objv[cursor_ix], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   CXFile    result    = ((CXFile (*)(CXCursor))clientData)(cursor);
   CXString  resultStr = clang_getFileName(result);
   Tcl_Obj  *resultObj = convertCXStringToObj(resultStr);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//------------------------------------------------- cursor -> location command

static int cursorToLocationObjCmd(ClientData     clientData,
                                  Tcl_Interp    *interp,
                                  int            objc,
                                  Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      cursor_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = getCursorFromObj(interp, objv[cursor_ix], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   typedef CXSourceLocation (*ProcType)(CXCursor);

   CXSourceLocation  result    = ((ProcType)clientData)(cursor);
   Tcl_Obj          *resultObj = newLocationObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//---------------------------------------------------- cursor -> range command

static int cursorToRangeObjCmd(ClientData     clientData,
                               Tcl_Interp    *interp,
                               int            objc,
                               Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      cursor_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = getCursorFromObj(interp, objv[cursor_ix], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   typedef CXSourceRange (*ProcType)(CXCursor);

   CXSourceRange  result    = ((ProcType)clientData)(cursor);
   Tcl_Obj       *resultObj = newRangeObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//--------------------------------------------------- cursor -> cursor command

static int cursorToCursorObjCmd(ClientData     clientData,
                                Tcl_Interp    *interp,
                                int            objc,
                                Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      cursor_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = getCursorFromObj(interp, objv[cursor_ix], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   CXCursor  result    = ((CXCursor (*)(CXCursor))clientData)(cursor);
   Tcl_Obj  *resultObj = newCursorObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//----------------------------------------- cursor, unsigned -> cursor command

static int cursorUnsignedToCursorObjCmd(ClientData     clientData,
                                        Tcl_Interp    *interp,
                                        int            objc,
                                        Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      cursor_ix,
      number_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "cursor number");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = getCursorFromObj(interp, objv[cursor_ix], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   unsigned number;
   status = getUnsignedFromObj(interp, objv[number_ix], &number);
   if (status != TCL_OK) {
      return status;
   }

   typedef CXCursor (*ProcType)(CXCursor, unsigned);

   CXCursor  result    = ((ProcType)clientData)(cursor, number);
   Tcl_Obj  *resultObj = newCursorObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//------------------------------------------ cursor, unsigned -> range command

static int cursorUnsignedToRangeObjCmd(ClientData     clientData,
                                       Tcl_Interp    *interp,
                                       int            objc,
                                       Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      cursor_ix,
      number_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "cursor number");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = getCursorFromObj(interp, objv[cursor_ix], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   unsigned number;
   status = getUnsignedFromObj(interp, objv[number_ix], &number);
   if (status != TCL_OK) {
      return status;
   }

   typedef CXSourceRange (*ProcType)(CXCursor, unsigned);

   CXSourceRange  result    = ((ProcType)clientData)(cursor, number);
   Tcl_Obj       *resultObj = newRangeObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//----------------------------------------------------- cursor -> type command

static int cursorToTypeObjCmd(ClientData     clientData,
                              Tcl_Interp    *interp,
                              int            objc,
                              Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      cursor_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = getCursorFromObj(interp, objv[cursor_ix], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   CXType   result    = ((CXType (*)(CXCursor))clientData)(cursor);
   Tcl_Obj *resultObj = newCXTypeObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//----------------------------------------------------- cursor -> kind command

static int cursorToKindObjCmd(ClientData     clientData,
                              Tcl_Interp    *interp,
                              int            objc,
                              Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      cursor_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = getCursorFromObj(interp, objv[cursor_ix], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   typedef unsigned (*ProcType)(CXCursor);

   ProcType           proc    = (ProcType)clientData;
   enum CXCursorKind  kind    = proc(cursor);
   Tcl_Obj           *kindObj = Tcl_NewIntObj(kind);

   Tcl_IncrRefCount(kindObj);

   Tcl_Obj *resultObj;
   if (Tcl_DictObjGet(NULL, cursorKindNames, kindObj, &resultObj) != TCL_OK) {
      Tcl_Panic("cursor kind %d is not valid", kind);
   }

   Tcl_DecrRefCount(kindObj);

   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//--------------------------------------------- cursor -> kind -> bool command

static int cursorToKindToBoolObjCmd(ClientData     clientData,
                                    Tcl_Interp    *interp,
                                    int            objc,
                                    Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      cursor_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = getCursorFromObj(interp, objv[cursor_ix], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   typedef unsigned (*ProcType)(enum CXCursorKind);

   ProcType           proc      = (ProcType)clientData;
   enum CXCursorKind  kind      = clang_getCursorKind(cursor);
   int                result    = proc(kind) != 0;
   Tcl_Obj           *resultObj = Tcl_NewIntObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//---------------------- translation unit instance's diagnostic decode command

static int tuDiagnosticDecodeObjCmd(ClientData     clientData,
                                    Tcl_Interp    *interp,
                                    int            objc,
                                    Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      index_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "");
      return TCL_ERROR;
   }

   int index = 0;
   int status = Tcl_GetIntFromObj(interp, objv[index_ix], &index);
   if (status != TCL_OK) {
      return status;
   }

   TUInfo *info = (TUInfo *)clientData;

   unsigned numDiags = clang_getNumDiagnostics(info->translationUnit);
   if (0 > index || numDiags <= index) {
      Tcl_SetObjResult(interp,
                       Tcl_ObjPrintf("index %d is out of range", index));
      return TCL_ERROR;
   }

   CXDiagnostic  diagnostic = clang_getDiagnostic(info->translationUnit,
                                                  index);
   Tcl_Obj      *resultObj  = newDiagnosticObj(diagnostic);
   Tcl_SetObjResult(interp, resultObj);
   clang_disposeDiagnostic(diagnostic);

   return TCL_OK;
}

//---------------------- translation unit instance's diagnostic format command

static BitMask diagnosticFormatOptions[] = {
   { "-displaySourceLocation" },
   { "-displayColumn" },
   { "-displaySourceRanges" },
   { "-displayOption" },
   { "-displayCategoryId" },
   { "-displayCategoryName" },
   { NULL }
};

static int tuDiagnosticFormatObjCmd(ClientData     clientData,
                                    Tcl_Interp    *interp,
                                    int            objc,
                                    Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      index_ix,
      options_ix
   };

   if (objc < index_ix + 1) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "index ?option?...");
      return TCL_ERROR;
   }

   int index = 0;
   int status = Tcl_GetIntFromObj(interp, objv[index_ix], &index);
   if (status != TCL_OK) {
      return status;
   }

   TUInfo *info = (TUInfo *)clientData;

   unsigned flags = 0;
   if (objc == options_ix) {
      flags = clang_defaultDiagnosticDisplayOptions();
   } else if (objc == options_ix + 1
              && (objv[options_ix] == noneTagObj
                  || strcmp(Tcl_GetStringFromObj(objv[options_ix], NULL),
                            Tcl_GetStringFromObj(noneTagObj, NULL)) == 0)) {
   } else {
      for (int i = options_ix; i < objc; ++i) {
         int optionNumber;
         status = Tcl_GetIndexFromObjStruct
            (interp, objv[options_ix + i],
             diagnosticFormatOptions,
             sizeof diagnosticFormatOptions[0],
             "option", 0, &optionNumber);
         if (status != TCL_OK) {
            return status;
         }

         flags |= diagnosticFormatOptions[optionNumber].mask == 0
            ? 1 << optionNumber
            : diagnosticFormatOptions[optionNumber].mask;
      }

      if ((flags & CXDiagnostic_DisplayColumn) != 0
          || (flags & CXDiagnostic_DisplaySourceRanges) != 0) {
         flags |= CXDiagnostic_DisplaySourceLocation;
      }
   }

   unsigned numDiags = clang_getNumDiagnostics(info->translationUnit);
   if (index < 0 || numDiags <= index) {
      Tcl_SetObjResult(interp,
                       Tcl_ObjPrintf("index %d is out of range", index));
      return TCL_ERROR;
   }

   CXDiagnostic  diagnostic = clang_getDiagnostic(info->translationUnit,
                                                  index);
   CXString      result     = clang_formatDiagnostic(diagnostic, flags);
   Tcl_Obj      *resultObj  = convertCXStringToObj(result);
   Tcl_SetObjResult(interp, resultObj);
   clang_disposeDiagnostic(diagnostic);

   return TCL_OK;
}

//---------------------- translation unit instance's diagnostic number command

static int tuDiagnosticNumberObjCmd(ClientData     clientData,
                                    Tcl_Interp    *interp,
                                    int            objc,
                                    Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "");
      return TCL_ERROR;
   }

   TUInfo *info = (TUInfo *)clientData;

   unsigned  result    = clang_getNumDiagnostics(info->translationUnit);
   Tcl_Obj  *resultObj = Tcl_NewLongObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//--------------------------------- translation unit instance's cursor command

static int tuCursorObjCmd(ClientData     clientData,
                          Tcl_Interp    *interp,
                          int            objc,
                          Tcl_Obj *const objv[])
{
   TUInfo *info = (TUInfo *)clientData;

   enum {
      command_ix,
      optional_ix
   };

   static const char *options[] = {
      "-location",
      "-file",
      "-line",
      "-column",
      "-offset",
      NULL,
   };

   enum {
      option_location,
      option_file,
      option_line,
      option_column,
      option_offset,
   };

   unsigned          options_found = 0;
   CXSourceLocation  location      = clang_getNullLocation();
   unsigned          line          = 0;
   unsigned          column        = 0;
   unsigned          offset        = 0;
   CXFile            file          = NULL;

   for (int i = optional_ix; i < objc; ++i) {
      int optionNumber;
      int status = Tcl_GetIndexFromObj(interp, objv[i], options,
                                       "option", 0, &optionNumber);
      if (status != TCL_OK) {
         return status;
      }

      if ((options_found & (1 << optionNumber)) != 0) {
         Tcl_SetObjResult(interp,
                          Tcl_ObjPrintf("%s is specified more than once.",
                                        Tcl_GetStringFromObj(objv[i], NULL)));
         return TCL_ERROR;
      }

      switch (optionNumber) {

      case option_location:
         if (options_found != 0) {
            goto invalid_form;
         }

         if (objc <= i + 1) {
            Tcl_SetObjResult(interp,
                             Tcl_NewStringObj("-location is not followed by a "
                                              "source location",
                                              -1));
            return TCL_ERROR;
         }

         status = getLocationFromObj(interp, objv[i + 1], &location);
         if (status != TCL_OK) {
            return status;
         }

         break;

      case option_file:
         if ((options_found & (1 << option_location)) != 0) {
            goto invalid_form;
         }

         if (objc <= i + 1) {
            Tcl_SetObjResult(interp,
                             Tcl_NewStringObj("-file is not followed by a "
                                              "filename",
                                              -1));
            return TCL_ERROR;
         }

         const char *filename = Tcl_GetStringFromObj(objv[i + 1], NULL);
         file = clang_getFile(info->translationUnit, filename);
         if (file == NULL) {
            goto invalid_location;
         }

         break;

      case option_line:
      case option_column:
         if ((options_found & ((1 << option_location)
                               | (1 << option_offset))) != 0) {
            goto invalid_form;
         }

         if (objc <= i + 1) {
            Tcl_SetObjResult(interp,
                             Tcl_ObjPrintf("%s is not followed by a %s",
                                           Tcl_GetStringFromObj(objv[i], NULL),
                                           optionNumber == option_line
                                           ? "line number"
                                           : "column number"));
            return TCL_ERROR;
         }
         
         status = getUnsignedFromObj(interp, objv[i + 1],
                                     optionNumber == option_line
                                     ? &line
                                     : &column);
         if (status != TCL_OK) {
            return status;
         }

         break;

      case option_offset:
         if ((options_found & ((1 << option_location)
                               | (1 << option_line)
                               | (1 << option_column))) != 0) {
            goto invalid_form;
         }

         if (objc <= i + 1) {
            Tcl_SetObjResult(interp,
                             Tcl_NewStringObj("-offset is not "
                                              "followed by an offset",
                                              -1));
            return TCL_ERROR;
         }
         
         status = getUnsignedFromObj(interp, objv[i + 1], &offset);
         if (status != TCL_OK) {
            return status;
         }

         break;

      default:
         Tcl_Panic("unknown option number");

      }

      options_found |= 1 << optionNumber;
   }

   CXCursor cursor;
   unsigned line_and_column_form = (1 << option_file)
      | (1 << option_line)
      | (1 << option_column);
   unsigned offset_form = (1 << option_file) | (1 << option_offset);

   if (options_found == 0) {

      cursor = clang_getTranslationUnitCursor(info->translationUnit);

   } else {

      if ((options_found & line_and_column_form) == line_and_column_form) {

         location = clang_getLocation(info->translationUnit,
                                      file, line, column);

      } else if ((options_found & offset_form) == offset_form) {

         location = clang_getLocationForOffset(info->translationUnit,
                                               file, offset);

      } else {

         goto invalid_form;

      }

      if (clang_equalLocations(location, clang_getNullLocation())) {
         goto invalid_location;
      }

      cursor = clang_getCursor(info->translationUnit, location);

   }

   if (clang_Cursor_isNull(cursor)) {
      goto invalid_location;
   }

   Tcl_Obj *cursorObj = newCursorObj(cursor);
   Tcl_SetObjResult(interp, cursorObj);

   return TCL_OK;

 invalid_form:
   Tcl_SetObjResult(interp,
                    Tcl_NewStringObj("the specified location is not valid.",
                                     -1));
   return TCL_ERROR;

 invalid_location:
   Tcl_SetObjResult(interp,
                    Tcl_NewStringObj("the specified location is "
                                     "not a part of the translation unit.",
                                     -1));
   return TCL_ERROR;
}

//----------------------------- translation unit instance's diagnostic command

static int tuDiagnosticObjCmd(ClientData     clientData,
                              Tcl_Interp    *interp,
                              int            objc,
                              Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      subcommand_ix,
      numMandatoryArgs
   };

   if (objc < numMandatoryArgs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "subcommand");
      return TCL_ERROR;
   }

   static Command subcommands[] = {
      { "decode",
        tuDiagnosticDecodeObjCmd },
      { "format",
        tuDiagnosticFormatObjCmd },
      { "number",
        tuDiagnosticNumberObjCmd },
      { NULL },
   };

   int commandNumber;
   int status = Tcl_GetIndexFromObjStruct(interp, objv[subcommand_ix],
                                          subcommands, sizeof subcommands[0],
                                          "subcommand", 0, &commandNumber);
   if (status != TCL_OK) {
      return status;
   }

   return subcommands[commandNumber].proc(clientData, interp,
                                          objc - subcommand_ix,
                                          objv + subcommand_ix);
}

//--------------- translation unit instance's isMultipleIncludeGuarded command

static int tuIsMultipleIncludeGuardedObjCmd(ClientData     clientData,
                                            Tcl_Interp    *interp,
                                            int            objc,
                                            Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      filename_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "filename");
      return TCL_ERROR;
   }

   TUInfo *info = (TUInfo *)clientData;

   CXFile file;
   int status = getFileFromObj(interp, info->translationUnit,
                               objv[filename_ix], &file);
   if (status != TCL_OK) {
      return status;
   }

   unsigned result
      = clang_isFileMultipleIncludeGuarded(info->translationUnit, file);
   Tcl_Obj *resultObj = Tcl_NewLongObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//------------------------------- translation unit instance's location command

static int tuLocationObjCmd(ClientData     clientData,
                            Tcl_Interp    *interp,
                            int            objc,
                            Tcl_Obj *const objv[])
{
   TUInfo *info = (TUInfo *)clientData;

   enum {
      command_ix,
      option_ix
   };

   static const char *options[] = {
      "-file",
      "-line",
      "-column",
      "-offset",
      NULL,
   };

   enum {
      option_file,
      option_line,
      option_column,
      option_offset,
   };

   unsigned options_found = 0;
   unsigned line          = 0;
   unsigned column        = 0;
   unsigned offset        = 0;
   CXFile   file          = NULL;

   for (int i = option_ix; i < objc; ++i) {
      int optionNumber;
      int status = Tcl_GetIndexFromObj(interp, objv[i], options,
                                       "option", 0, &optionNumber);
      if (status != TCL_OK) {
         return status;
      }

      if ((options_found & (1 << optionNumber)) != 0) {
         Tcl_SetObjResult(interp,
                          Tcl_ObjPrintf("%s is specified more than once.",
                                        Tcl_GetStringFromObj(objv[i], NULL)));
         return TCL_ERROR;
      }

      switch (optionNumber) {

      case option_file:
         if (objc <= i + 1) {
            Tcl_SetObjResult(interp,
                             Tcl_NewStringObj("-file is not followed by a "
                                              "filename",
                                              -1));
            return TCL_ERROR;
         }

         const char *filename = Tcl_GetStringFromObj(objv[i + 1], NULL);
         file = clang_getFile(info->translationUnit, filename);
         if (file == NULL) {
            goto invalid_location;
         }

         break;

      case option_line:
      case option_column:
         if ((options_found & (1 << option_offset)) != 0) {
            goto invalid_form;
         }

         if (objc <= i + 1) {
            const char *option = Tcl_GetStringFromObj(objv[i], NULL);
            Tcl_SetObjResult(interp,
                             Tcl_ObjPrintf("%s is not followed by a %s number",
                                           option, option + 1));
            return TCL_ERROR;
         }

         status = getUnsignedFromObj(interp, objv[i + 1],
                                     optionNumber == option_line
                                     ? &line
                                     : &column);
         if (status != TCL_OK) {
            return status;
         }

         break;

      case option_offset:
         if ((options_found & ((1 << option_line) | (1 << option_column)))
             != 0) {
            goto invalid_form;
         }

         if (objc <= i + 1) {
            Tcl_SetObjResult(interp,
                             Tcl_NewStringObj("-offset is not "
                                              "followed by an offset",
                                              -1));
            return TCL_ERROR;
         }
         
         status = getUnsignedFromObj(interp, objv[i + 1], &offset);
         if (status != TCL_OK) {
            return status;
         }

         break;

      default:
         Tcl_Panic("unknown option number");

      }

      options_found |= 1 << optionNumber;
   }

   CXSourceLocation location;
   unsigned line_and_column_form = (1 << option_file)
      | (1 << option_line)
      | (1 << option_column);
   unsigned offset_form = (1 << option_file) | (1 << option_offset);
   if ((options_found & line_and_column_form) == line_and_column_form) {

      location = clang_getLocation(info->translationUnit,
                                   file, line, column);

   } else if ((options_found & offset_form) == offset_form) {

      location = clang_getLocationForOffset(info->translationUnit,
                                            file, offset);

   } else {

      goto invalid_form;

   }

   if (clang_equalLocations(location, clang_getNullLocation())) {
      goto invalid_location;
   }

   Tcl_Obj *resultObj = newLocationObj(location);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;

 invalid_form:
   Tcl_SetObjResult(interp,
                    Tcl_NewStringObj("the specified location is not valid.",
                                     -1));
   return TCL_ERROR;

 invalid_location:
   Tcl_SetObjResult(interp,
                    Tcl_NewStringObj("the specified location is "
                                     "not a part of the translation unit.",
                                     -1));
   return TCL_ERROR;
}

//----------------------- translation unit instance's modificationTime command

static int tuModificationTimeObjCmd(ClientData     clientData,
                                    Tcl_Interp    *interp,
                                    int            objc,
                                    Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      filename_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "filename");
      return TCL_ERROR;
   }

   TUInfo *info = (TUInfo *)clientData;

   CXFile file;
   int status = getFileFromObj(interp, info->translationUnit,
                               objv[filename_ix], &file);
   if (status != TCL_OK) {
      return status;
   }

   time_t   result    = clang_getFileTime(file);
   Tcl_Obj *resultObj = newUintmaxObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//-------------------------------- translation unit instance's reparse command

static struct CXUnsavedFile *createUnsavedFileArray(Tcl_Obj *unsavedFileList)
{
   int       length;
   Tcl_Obj **unsavedFileListElements;
   Tcl_ListObjGetElements(NULL, unsavedFileList,
                          &length, &unsavedFileListElements);

   int numUnsavedFiles = length >> 1;

   struct CXUnsavedFile *unsavedFiles
      = (struct CXUnsavedFile *)
        Tcl_Alloc(numUnsavedFiles * sizeof(struct CXUnsavedFile));

   for (int i = 0; i < numUnsavedFiles; ++i) {
      unsavedFiles[i].Filename
         = Tcl_GetStringFromObj(unsavedFileListElements[i * 2], NULL);

      int size;
      unsavedFiles[i].Contents
         = Tcl_GetStringFromObj(unsavedFileListElements[i * 2 + 1], &size);
      unsavedFiles[i].Length = size;
   }

   return unsavedFiles;
}

static int tuReparseObjCmd(ClientData     clientData,
                           Tcl_Interp    *interp,
                           int            objc,
                           Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      options_ix
   };

   TUInfo *info = (TUInfo *)clientData;

   enum {
      unsavedFileOption
   };

   static const char *options[] = {
      "-unsavedFile",           // -unsavedFile filename contents
      NULL
   };

   int      numUnsavedFiles = 0;
   Tcl_Obj *unsavedFileList = Tcl_NewObj();
   Tcl_IncrRefCount(unsavedFileList);

   for (int i = options_ix; i < objc; ++i) {
      int optionNumber;
      int status = Tcl_GetIndexFromObj(interp, objv[i], options,
                                       "option", 0, &optionNumber);
      if (status != TCL_OK) {
         Tcl_DecrRefCount(unsavedFileList);
         return status;
      }

      if (optionNumber == unsavedFileOption) {
         // -unsavedFile filename contents
         if (objc <= i + 2) {
            Tcl_WrongNumArgs(interp, i, objv, "filename contents ...");
            Tcl_DecrRefCount(unsavedFileList);
            return TCL_ERROR;
         }
         ++numUnsavedFiles;
         Tcl_ListObjAppendElement(NULL, unsavedFileList, objv[++i]);
         Tcl_ListObjAppendElement(NULL, unsavedFileList, objv[++i]);
      } else {
         Tcl_Panic("what?!");
      }
   }

   struct CXUnsavedFile *unsavedFiles =
      createUnsavedFileArray(unsavedFileList);
   Tcl_DecrRefCount(unsavedFileList);

   unsigned flags  = clang_defaultReparseOptions(info->translationUnit);
   int      status = clang_reparseTranslationUnit(info->translationUnit,
                                                  numUnsavedFiles,
                                                  unsavedFiles, flags);

   Tcl_Free((char *)unsavedFiles);

   if (status != 0) {
      Tcl_SetObjResult(interp,
                       Tcl_ObjPrintf("translation unit \"%s\" is not valid",
                                     Tcl_GetStringFromObj(info->name, NULL)));
      return TCL_ERROR;
   }

   return TCL_OK;
}

//-------------------------- translation unit instance's resourceUsage command

static int tuResourceUsageObjCmd(ClientData     clientData,
                                 Tcl_Interp    *interp,
                                 int            objc,
                                 Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "");
      return TCL_ERROR;
   }

   TUInfo *info = (TUInfo *)clientData;

   CXTUResourceUsage usage = clang_getCXTUResourceUsage(info->translationUnit);

   Tcl_Obj **elms
      = (Tcl_Obj **)Tcl_Alloc(2 * usage.numEntries * sizeof(Tcl_Obj *));

   for (int i = 0; i < usage.numEntries; ++i) {
      const char *name = clang_getTUResourceUsageName(usage.entries[i].kind);
      elms[i * 2]      = Tcl_NewStringObj(name, -1);
      elms[i * 2 + 1]  = newUintmaxObj(usage.entries[i].amount);
   }

   Tcl_Obj *resultObj = Tcl_NewListObj(usage.numEntries * 2, elms);

   Tcl_Free((char *)elms);
   clang_disposeCXTUResourceUsage(usage);

   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//----------------------------------- translation unit instance's save command

static int tuSaveObjCmd(ClientData     clientData,
                        Tcl_Interp    *interp,
                        int            objc,
                        Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      filename_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "filename");
      return TCL_ERROR;
   }

   TUInfo     *info     = (TUInfo *)clientData;
   const char *filename = Tcl_GetStringFromObj(objv[filename_ix], NULL);
   unsigned    flags    = clang_defaultSaveOptions(info->translationUnit);
   int         status   = clang_saveTranslationUnit(info->translationUnit,
                                                    filename, flags);
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

//----------------------------- translation unit instance's sourceFile command

static int tuSourceFileObjCmd(ClientData     clientData,
                              Tcl_Interp    *interp,
                              int            objc,
                              Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "");
      return TCL_ERROR;
   }

   TUInfo            *info      = (TUInfo *)clientData;
   CXTranslationUnit  tu        = info->translationUnit;
   CXString           result    = clang_getTranslationUnitSpelling(tu);
   Tcl_Obj           *resultObj = convertCXStringToObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//------------------------------- translation unit instance's uniqueID command

static int tuUniqueIDObjCmd(ClientData     clientData,
                            Tcl_Interp    *interp,
                            int            objc,
                            Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      filename_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "filename");
      return TCL_ERROR;
   }

   TUInfo *info = (TUInfo *)clientData;

   CXFile file;
   int status = getFileFromObj(interp, info->translationUnit,
                               objv[filename_ix], &file);
   if (status != TCL_OK) {
      return status;
   }

   CXFileUniqueID uniqueId;
   if (clang_getFileUniqueID(file, &uniqueId)) {
      Tcl_SetObjResult(interp,
                       Tcl_NewStringObj("failed to get file unique ID.", -1));
      return TCL_ERROR;
   }

   enum {
      ndata = sizeof uniqueId.data / sizeof uniqueId.data[0]
   };
   Tcl_Obj *elms[ndata];
   for (int i = 0; i < ndata; ++i) {
      elms[i] = newUintmaxObj(uniqueId.data[i]);
   }
   Tcl_Obj *resultObj = Tcl_NewListObj(ndata, elms);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//------------------------------------------ translation unit instance command

static int tuInstanceObjCmd(ClientData     clientData,
                            Tcl_Interp    *interp,
                            int            objc,
                            Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      subcommand_ix,
      numCommonArgs,
   };

   if (objc < numCommonArgs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "subcommand");
      return TCL_ERROR;
   }

   static Command subcommands[] = {
      { "cursor",
        tuCursorObjCmd },
      { "diagnostic",
        tuDiagnosticObjCmd },
      { "isMultipleIncludeGuarded",
        tuIsMultipleIncludeGuardedObjCmd },
      { "location",
        tuLocationObjCmd },
      { "modificationTime",
        tuModificationTimeObjCmd },
      { "reparse",
        tuReparseObjCmd },
      { "resourceUsage",
        tuResourceUsageObjCmd },
      { "save",
        tuSaveObjCmd },
      { "sourceFile",
        tuSourceFileObjCmd },
      { "uniqueID",
        tuUniqueIDObjCmd },
      { NULL },
   };

   int commandNumber;
   int status = Tcl_GetIndexFromObjStruct(interp, objv[subcommand_ix],
                                          subcommands, sizeof subcommands[0],
                                          "subcommand", 0, &commandNumber);
   if (status != TCL_OK) {
      return status;
   }

   return subcommands[commandNumber].proc(clientData, interp,
                                          objc - subcommand_ix,
                                          objv + subcommand_ix);
}

//-------------------------------------------------- indexName options command

static int indexNameOptionsObjCmd(ClientData     clientData,
                                  Tcl_Interp    *interp,
                                  int            objc,
                                  Tcl_Obj *const objv[])
{
   static BitMask options[] = {
      { "-backgroundIndexing" },
      { "-backgroundEditing" },
      { "-background", CXGlobalOpt_ThreadBackgroundPriorityForAll },
      { NULL }
   };

   enum {
      subcommand_ix,
      options_ix
   };

   IndexInfo *info = (IndexInfo *)clientData;

   if (objc == subcommand_ix + 1) {
      unsigned value = clang_CXIndex_getGlobalOptions(info->index);
      return bitMaskToString(interp, options, noneTagObj, value);
   }

   unsigned value = 0;

   if (objc == options_ix + 1
       && (objv[options_ix] == noneTagObj
           || strcmp(Tcl_GetStringFromObj(objv[options_ix], NULL),
                     Tcl_GetStringFromObj(noneTagObj, NULL)) == 0)) {
      // The option list is -none, or ...
   } else {
      // ... a list of options
      for (int i = 1; i < objc; ++i) {
         int number;
         int status = Tcl_GetIndexFromObjStruct(interp, objv[i],
                                                options, sizeof options[0],
                                                "option", 0, &number);
         if (status != TCL_OK) {
            return status;
         }

         value |= options[number].mask == 0
            ? 1 << number
            : options[number].mask;
      }
   }

   clang_CXIndex_setGlobalOptions(info->index, value);

   return TCL_OK;
}

//------------------------------------------ indexName translationUnit command

enum {
   parseOptions_parseLater,
   parseOptions_sourceFile,
   parseOptions_unsavedFile,
   parseOptions_firstFlag
};

static const char *parseOptions[] = {
   "-parseLater",
   "-sourceFile",
   "-unsavedFile",

   // flags
   "-detailedPreprocessingRecord",
   "-incomplete",
   "-precompiledPreamble",
   "-cacheCompletionResults",
   "-forSerialization",
   "-cxxChainedPCH",
   "-skipFunctionBodies",
   "-includeBriefCommentsInCodeCompletion",
   NULL
};

static const char * const *parseFlags
  = &parseOptions[parseOptions_firstFlag];

static int indexNameTranslationUnitObjCmd(ClientData     clientData,
                                          Tcl_Interp    *interp,
                                          int            objc,
                                          Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      options_ix
   };

   if (objc < options_ix + 1) {
      goto wrong_num_args;
   }

   int         doParse         = 1;
   unsigned    flags           = 0;
   const char *sourceFilename  = NULL;
   int         numUnsavedFiles = 0;
   Tcl_Obj    *unsavedFileList = Tcl_NewObj();
   Tcl_IncrRefCount(unsavedFileList);

   int i;
   for (i = options_ix; i < objc; ++i) {
      const char *str = Tcl_GetStringFromObj(objv[i], NULL);

      if (str[0] != '-') {
         break;
      }

      if (strcmp(str, "--") == 0) {
         ++i;
         break;
      }

      int optionNumber;
      int status = Tcl_GetIndexFromObj(interp, objv[i], parseOptions,
                                       "option", 0, &optionNumber);
      if (status != TCL_OK) {
         Tcl_DecrRefCount(unsavedFileList);
         return status;
      }

      switch (optionNumber) {
      case parseOptions_parseLater: // -parseLater
         doParse = 0;
         break;

      case parseOptions_sourceFile: // -sourceFile filename
         if (objc <= i + 1) {
            Tcl_WrongNumArgs(interp, i, objv, "filename ...");
            Tcl_DecrRefCount(unsavedFileList);
            return TCL_ERROR;
         }
         sourceFilename = Tcl_GetStringFromObj(objv[i + 1], NULL);
         ++i;
         break;

      case parseOptions_unsavedFile: // -unsavedFile filename contents
         if (objc <= i + 2) {
            Tcl_WrongNumArgs(interp, i, objv, "filename contents ...");
            Tcl_DecrRefCount(unsavedFileList);
            return TCL_ERROR;
         }
         ++numUnsavedFiles;
         Tcl_ListObjAppendElement(NULL, unsavedFileList, objv[++i]);
         Tcl_ListObjAppendElement(NULL, unsavedFileList, objv[++i]);
         break;

      default:
         flags |= 1 << (optionNumber - parseOptions_firstFlag);
      }
   }

   if (objc <= i) {
      goto wrong_num_args;
   }

   Tcl_Obj *tuNameObj = objv[i++];

   Tcl_Obj *const  *argObjs = objv + i;
   int              nargs   = objc - i;
   char           **args    = (char **)Tcl_Alloc(nargs * sizeof *args);
   for (i = 0; i < nargs; ++i) {
      args[i] = Tcl_GetStringFromObj(argObjs[i], NULL);
   }

   struct CXUnsavedFile *unsavedFiles =
      createUnsavedFileArray(unsavedFileList);
   Tcl_DecrRefCount(unsavedFileList);

   IndexInfo *parent = (IndexInfo *)clientData;
   CXTranslationUnit tu;
   if (doParse) {
      tu = clang_parseTranslationUnit(parent->index, sourceFilename,
                                      (const char *const *)args, nargs,
                                      unsavedFiles, numUnsavedFiles, flags);
   } else {
      tu = clang_createTranslationUnitFromSourceFile
         (parent->index, sourceFilename,
          nargs, (const char *const *)args,
          numUnsavedFiles, unsavedFiles);
   }

   Tcl_Free((char *)unsavedFiles);

   if (tu == NULL) {
      Tcl_SetObjResult(interp,
                       Tcl_NewStringObj("failed to create translation unit.",
                                        -1));
      return TCL_ERROR;
   }

   TUInfo     *info        = createTUInfo(parent, tuNameObj, tu);
   const char *commandName = Tcl_GetStringFromObj(tuNameObj, NULL);
   Tcl_CreateObjCommand(interp, commandName,
                        tuInstanceObjCmd, info, tuDeleteProc);

   return TCL_OK;

 wrong_num_args:
   Tcl_WrongNumArgs(interp, command_ix + 1, objv,
                    "?options? ... ?--? "
                    "translationUnitName commandLineArg...");
   return TCL_ERROR;
}

//---------------------------------------------------------- indexName command

static int indexNameObjCmd(ClientData     clientData,
                           Tcl_Interp    *interp,
                           int            objc,
                           Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      subcommand_ix,
      numCommonArgs
   };

   if (objc < numCommonArgs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "subcommand");
      return TCL_ERROR;
   }

   static Command commands[] = {
      { "options",
        indexNameOptionsObjCmd },
      { "translationUnit",
        indexNameTranslationUnitObjCmd },
      { NULL }
   };

   int commandNumber;
   int status = Tcl_GetIndexFromObjStruct(interp, objv[subcommand_ix],
                                          commands, sizeof commands[0],
                                          "subcommand", 0, &commandNumber);
   if (status != TCL_OK) {
      return status;
   }

   return commands[commandNumber].proc(clientData, interp,
                                       objc - subcommand_ix,
                                       objv + subcommand_ix);
}

//----------------------------------------------------- location equal command

static int locationEqualObjCmd(ClientData     clientData,
                               Tcl_Interp    *interp,
                               int            objc,
                               Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      location1_ix,
      location2_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "location1 location2");
      return TCL_ERROR;
   }

   CXSourceLocation locations[2];
   for (int i = 0; i < 2; ++i) {
      int status = getLocationFromObj
         (interp, objv[location1_ix + i], &locations[i]);
      if (status != TCL_OK) {
         return status;
      }
   }

   int      result    = clang_equalLocations(locations[0], locations[1]) != 0;
   Tcl_Obj *resultObj = Tcl_NewIntObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//--------------------------------------------------- location is null command

static int locationIsNullObjCmd(ClientData     clientData,
                                Tcl_Interp    *interp,
                                int            objc,
                                Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      location_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "location");
      return TCL_ERROR;
   }

   CXSourceLocation location;
   int status = getLocationFromObj(interp, objv[location_ix], &location);
   if (status != TCL_OK) {
      return status;
   }

   int      result    = clang_equalLocations(location,
                                             clang_getNullLocation()) != 0;
   Tcl_Obj *resultObj = Tcl_NewIntObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//------------------------------------------------------ location null command

static int locationNullObjCmd(ClientData     clientData,
                              Tcl_Interp    *interp,
                              int            objc,
                              Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "");
      return TCL_ERROR;
   }

   CXSourceLocation  result    = clang_getNullLocation();
   Tcl_Obj          *resultObj = newLocationObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//------------------------------------------ location presumedLocation command

static int locationPresumedLocationObjCmd(ClientData     clientData,
                                          Tcl_Interp    *interp,
                                          int            objc,
                                          Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      location_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "location");
      return TCL_ERROR;
   }

   CXSourceLocation location;
   int status = getLocationFromObj(interp, objv[location_ix], &location);
   if (status != TCL_OK) {
      return status;
   }

   Tcl_Obj *resultObj = newPresumedLocationObj(location);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//---------------------------------------------------- location decode command

typedef void (*LocationDecodeProc)
(CXSourceLocation, CXFile*, unsigned *, unsigned *, unsigned *);

static int locationDecodeObjCmd(ClientData     clientData,
                                Tcl_Interp    *interp,
                                int            objc,
                                Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      location_ix,
      nargs
   };
      
   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "location");
      return TCL_ERROR;
   }

   CXSourceLocation location;
   int status = getLocationFromObj(interp, objv[location_ix], &location);
   if (status != TCL_OK) {
      return status;
   }

   CXFile   file;
   unsigned line;
   unsigned column;
   unsigned offset;
   ((LocationDecodeProc)clientData)(location, &file, &line, &column, &offset);

   Tcl_Obj *resultObj = newDecodedLocationObj(file, line, column, offset);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//--------------------------------------------------- location -> bool command

static int locationToBoolObjCmd(ClientData     clientData,
                                Tcl_Interp    *interp,
                                int            objc,
                                Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      location_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "location");
      return TCL_ERROR;
   }

   CXSourceLocation location;
   int status = getLocationFromObj(interp, objv[location_ix], &location);
   if (status != TCL_OK) {
      return status;
   }

   typedef unsigned (*LocationToUnsignedProc)(CXSourceLocation);

   int      result    = ((LocationToUnsignedProc)clientData)(location) != 0;
   Tcl_Obj *resultObj = Tcl_NewIntObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//------------------------------------------------------- range create command

static int rangeCreateObjCmd(ClientData     clientData,
                             Tcl_Interp    *interp,
                             int            objc,
                             Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      location1_ix,
      location2_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "location1 location2");
      return TCL_ERROR;
   }

   CXSourceLocation locations[2];
   for (int i = 0; i < 2; ++i) {
      int status = getLocationFromObj
         (interp, objv[location1_ix + i], &locations[i]);
      if (status != TCL_OK) {
         return status;
      }
   }

   CXSourceRange  result    = clang_getRange(locations[0], locations[1]);
   Tcl_Obj       *resultObj = newRangeObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//-------------------------------------------------------- range equal command

static int rangeEqualObjCmd(ClientData     clientData,
                            Tcl_Interp    *interp,
                            int            objc,
                            Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      range1_ix,
      range2_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "range1 range2");
      return TCL_ERROR;
   }

   CXSourceRange ranges[2];
   for (int i = 0; i < 2; ++i) {
      int status = getRangeFromObj(interp, objv[range1_ix + i], &ranges[i]);
      if (status != TCL_OK) {
         return status;
      }
   }

   int      result    = clang_equalRanges(ranges[0], ranges[1]) != 0;
   Tcl_Obj *resultObj = Tcl_NewIntObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//--------------------------------------------------------- range null command

static int rangeNullObjCmd(ClientData     clientData,
                           Tcl_Interp    *interp,
                           int            objc,
                           Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "");
      return TCL_ERROR;
   }

   CXSourceRange  result    = clang_getNullRange();
   Tcl_Obj       *resultObj = newRangeObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//------------------------------------------------------ range is null command

static int rangeIsNullObjCmd(ClientData     clientData,
                             Tcl_Interp    *interp,
                             int            objc,
                             Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      range_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix + 1, objv, "range");
      return TCL_ERROR;
   }

   CXSourceRange range;
   int status = getRangeFromObj(interp, objv[range_ix], &range);
   if (status != TCL_OK) {
      return status;
   }

   int      result    = clang_equalRanges(range, clang_getNullRange()) != 0;
   Tcl_Obj *resultObj = Tcl_NewIntObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//-------------------------------------------------- range -> location command

static int rangeToLocationObjCmd(ClientData     clientData,
                                 Tcl_Interp    *interp,
                                 int            objc,
                                 Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "range");
      return TCL_ERROR;
   }

   CXSourceRange range;
   int status = getRangeFromObj(interp, objv[1], &range);
   if (status != TCL_OK) {
      return status;
   }

   typedef CXSourceLocation (*ProcType)(CXSourceRange); 

   CXSourceLocation  location  = ((ProcType)clientData)(range);
   Tcl_Obj          *resultObj = newLocationObj(location);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//------------------------------------------------------- foreachChild command

enum {
   TCL_RECURSE = 5
};

struct ForeachChildInfo
{
   Tcl_Interp *interp;
   Tcl_Obj    *childName;
   Tcl_Obj    *parentName;
   Tcl_Obj    *scriptObj;
   int         returnCode;
};

static enum CXChildVisitResult foreachChildHelper(CXCursor     cursor,
                                                  CXCursor     parent,
                                                  CXClientData clientData)
{
   struct ForeachChildInfo *visitInfo = (struct ForeachChildInfo *)clientData;

   Tcl_Obj *cursorObj = newCursorObj(cursor);
   if (Tcl_ObjSetVar2(visitInfo->interp, visitInfo->childName,
                      NULL, cursorObj, TCL_LEAVE_ERR_MSG) == NULL) {
      return TCL_ERROR;
   }

   if (visitInfo->parentName != NULL) {
      Tcl_Obj *parentObj = newCursorObj(parent);
      if (Tcl_ObjSetVar2(visitInfo->interp, visitInfo->parentName,
                         NULL, parentObj, TCL_LEAVE_ERR_MSG) == NULL) {
         return TCL_ERROR;
      }
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

static int foreachChildObjCmd(ClientData     clientData,
                              Tcl_Interp    *interp,
                              int            objc,
                              Tcl_Obj *const objv[])
{
   enum {
      command_ix,
      varName_ix,
      cursor_ix,
      script_ix,
      nargs
   };

   if (objc != nargs) {
      Tcl_WrongNumArgs(interp, command_ix, objv, "varName cursor script");
      return TCL_ERROR;
   }

   int       numVars;
   Tcl_Obj **varNames;
   int status
      = Tcl_ListObjGetElements(interp, objv[varName_ix], &numVars, &varNames);
   if (status != TCL_OK) {
      return status;
   }

   if (numVars != 1 && numVars != 2) {
      Tcl_SetObjResult(interp,
                       Tcl_NewStringObj("one of two variable names "
                                        "are expected", -1));
      return TCL_ERROR;
   }

   CXCursor cursor;
   status = getCursorFromObj(interp, objv[cursor_ix], &cursor);
   if (status != TCL_OK) {
      return status;
   }

   struct ForeachChildInfo visitInfo = {
      .interp     = interp,
      .childName  = varNames[0],
      .parentName = numVars == 2 ? varNames[1] : NULL,
      .scriptObj  = objv[script_ix],
      .returnCode = TCL_OK,
   };

   clang_visitChildren(cursor, foreachChildHelper, &visitInfo);

   return visitInfo.returnCode;
}

static int recurseObjCmd(ClientData     clientData,
                         Tcl_Interp    *interp,
                         int            objc,
                         Tcl_Obj *const objv[])
{
   return TCL_RECURSE;
}

//-------------------------------------------------------------- index command

static int indexObjCmd(ClientData     clientData,
                       Tcl_Interp    *interp,
                       int            objc,
                       Tcl_Obj *const objv[])
{
   static const char *options[] = {
      "-excludeDeclFromPCH",
      "-displayDiagnostics",
      NULL
   };

   const char *commandName = NULL;
   unsigned    mask        = 0;
   for (int i = 1; i < objc; ++i) {
      const char *obj = Tcl_GetStringFromObj(objv[i], NULL);

      if (obj[0] == '-') {
         int option;
         int status = Tcl_GetIndexFromObj(interp, objv[i], options,
                                         "option", 0, &option);
         if (status != TCL_OK) {
            return status;
         }

         mask |= 1 << option;

      } else {
         if (commandName != NULL) {
            goto usage_error;
         }

         commandName = Tcl_GetStringFromObj(objv[i], NULL);
      }
   }

   if (commandName == NULL) {
      goto usage_error;
   }

   CXIndex index = clang_createIndex(mask & (1 << 0), mask & (1 << 1));
   if (index == NULL) {
      Tcl_SetObjResult(interp, Tcl_NewStringObj("index creation failed", -1));
      return TCL_ERROR;
   }

   IndexInfo *info = createIndexInfo(interp, index);

   Tcl_CreateObjCommand(interp, commandName,
                        indexNameObjCmd, info, indexDeleteProc);

   return TCL_OK;

 usage_error:
   // CommandName becomes NULL when index is not specified, or more than 1
   // index are specified.
   Tcl_WrongNumArgs(interp, 1, objv,
                    "?-excludeDeclFromPCH? ?-displayDiagnostics? index");
   return TCL_ERROR;
}

//--------------------------------------------------------------- bist command

#ifdef BIST
static int test_cstringHash(Tcl_Interp *interp)
{
   // test1
   {
      unsigned long hash1 = cstringHash("This is a string");
      unsigned long hash2 = cstringHash("This is another string");
      if (hash1 == hash2) {
         Tcl_SetObjResult(interp,
                          Tcl_ObjPrintf("%s: test1, %lu, %lu",
                                        __func__, hash1, hash2));
         return TCL_ERROR;
      }
   }

   // test2
   {
      unsigned long hash1 = cstringHash("This is a string");
      unsigned long hash2 = cstringHash("This is a string");
      if (hash1 != hash2) {
         Tcl_SetObjResult(interp,
                          Tcl_ObjPrintf("%s: test2, %lu, %lu",
                                        __func__, hash1, hash2));
         return TCL_ERROR;
      }
   }

   return TCL_OK;
}

static int test_newBignumObj(Tcl_Interp *interp)
{
#define BIST_UINTMAX 18446744073709551615
#define BIST_APPEND_UL1(x) x ## UL
#define BIST_APPEND_UL(x) BIST_APPEND_UL1(x)

   // test1
   {
      Tcl_Obj *result = newBignumObj(BIST_APPEND_UL(BIST_UINTMAX), 0);
      const char *str = Tcl_GetStringFromObj(result, NULL);
      if (strcmp(STRINGIFY(BIST_UINTMAX), str) != 0) {
         Tcl_SetObjResult(interp, Tcl_ObjPrintf("%s: test1.0, %s",
                                                __func__, str));
         return TCL_ERROR;
      }

      uintmax_t result2;
      int status = getIntmaxFromBignumObj(interp, result, 0, &result2);
      if (status != TCL_OK) {
         return status;
      }

      if (result2 != BIST_APPEND_UL(BIST_UINTMAX)) {
         Tcl_SetObjResult(interp, Tcl_ObjPrintf("%s: test1.0, %" PRIuMAX,
                                                __func__, result2));
      }
   }

   // test2
   {
      Tcl_Obj *result = newBignumObj(BIST_APPEND_UL(BIST_UINTMAX), 1);
      const char *str = Tcl_GetStringFromObj(result, NULL);
      if (strcmp("-" STRINGIFY(BIST_UINTMAX), str) != 0) {
         Tcl_SetObjResult(interp, Tcl_ObjPrintf("%s: test2, %s",
                                                __func__, str));
         return TCL_ERROR;
      }
   }

#define BIST_0xfedcba9876543210 18364758544493064720
   // test3
   {
      Tcl_Obj *result =
         newBignumObj(BIST_APPEND_UL(BIST_0xfedcba9876543210), 0);
      const char *str = Tcl_GetStringFromObj(result, NULL);
      if (strcmp(STRINGIFY(BIST_0xfedcba9876543210), str) != 0) {
         Tcl_SetObjResult(interp, Tcl_ObjPrintf("%s: test3, %s",
                                                __func__, str));
         return TCL_ERROR;
      }

      uintmax_t result2;
      int status = getIntmaxFromBignumObj(interp, result, 0, &result2);
      if (status != TCL_OK) {
         return status;
      }

      if (result2 != BIST_APPEND_UL(BIST_0xfedcba9876543210)) {
         Tcl_SetObjResult(interp, Tcl_ObjPrintf("%s: test1.0, %" PRIuMAX,
                                                __func__, result2));
      }
   }

   return TCL_OK;
}

static int bistObjCmd(ClientData     clientData,
                      Tcl_Interp    *interp,
                      int            objc,
                      Tcl_Obj *const objv[])
{
   if (objc != 1) {
      Tcl_WrongNumArgs(interp, 1, objv, "");
      return TCL_ERROR;
   }

   int status;

   status = test_cstringHash(interp);
   if (status != TCL_OK) {
      return status;
   }

   status = test_newBignumObj(interp);
   if (status != TCL_OK) {
      return status;
   }

   return status;
}
#endif

//------------------------------------------------------------- initialization

static EnumConsts availabilities = {
   .names = {
      "Available",
      "Deprecated",
      "NotAvailable",
      "NotAccessible",
      NULL
   }
};

static EnumConsts cxxAccessSpecifiers = {
   .names = {
      "CXXInvalidAccessSpecifier",
      "CXXPublic",
      "CXXProtected",
      "CXXPrivate",
      NULL
   }
};

static EnumConsts languages = {
   .names = {
      "Invalid",
      "C",
      "ObjC",
      "CPlusPlus",
      NULL
   }
};

static EnumConsts linkages = {
   .names = {
      "Invalid",
      "NoLinkage",
      "Internal",
      "UniqueExternal",
      "External",
      NULL
   }
};

static BitMask objCDeclQualifiers[] = {
   { "in" },
   { "inout" },
   { "out" },
   { "bycopy" },
   { "byref" },
   { "oneway" },
   { NULL },
};

static BitMask objCPropertyAttributes[] = {
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

static EnumConsts cxxRefQualifiers = {
   .names = {
      "None",
      "LValue",
      "RValue",
      NULL
   }
};

static CXSourceRange
cursorGetSpellingNameRange(CXCursor cursor, unsigned index)
{
   return clang_Cursor_getSpellingNameRange(cursor, index, 0);
}

int Cindex_Init(Tcl_Interp *interp)
{
   if (Tcl_InitStubs(interp, "8.5", 0) == NULL) {
      return TCL_ERROR;
   }

   noneTagObj = Tcl_NewStringObj("-none", -1);
   Tcl_IncrRefCount(noneTagObj);

   locationTagObj = Tcl_NewStringObj("CXSourceLocation", -1);
   Tcl_IncrRefCount(locationTagObj);

   rangeTagObj = Tcl_NewStringObj("CXSourceRange", -1);
   Tcl_IncrRefCount(rangeTagObj);

   filenameNullObj = Tcl_NewStringObj("<null>", -1);
   Tcl_IncrRefCount(filenameNullObj);

   diagnosticSeverityTagObj
      = Tcl_NewStringObj("severity", -1);
   Tcl_IncrRefCount(diagnosticSeverityTagObj);

   diagnosticLocationTagObj
      = Tcl_NewStringObj("location", -1);
   Tcl_IncrRefCount(diagnosticLocationTagObj);

   diagnosticSpellingTagObj
      = Tcl_NewStringObj("spelling", -1);
   Tcl_IncrRefCount(diagnosticSpellingTagObj);

   diagnosticEnableTagObj
      = Tcl_NewStringObj("enable", -1);
   Tcl_IncrRefCount(diagnosticEnableTagObj);

   diagnosticDisableTagObj
      = Tcl_NewStringObj("disable", -1);
   Tcl_IncrRefCount(diagnosticDisableTagObj);

   diagnosticCategoryTagObj
      = Tcl_NewStringObj("category", -1);
   Tcl_IncrRefCount(diagnosticCategoryTagObj);

   diagnosticRangesTagObj
      = Tcl_NewStringObj("ranges", -1);
   Tcl_IncrRefCount(diagnosticRangesTagObj);

   diagnosticFixItsTagObj
      = Tcl_NewStringObj("fixits", -1);
   Tcl_IncrRefCount(diagnosticFixItsTagObj);

   alwaysDeprecatedTagObj        = Tcl_NewStringObj("alwaysDeprecated", -1);
   Tcl_IncrRefCount(alwaysDeprecatedTagObj);
   deprecatedMessageTagObj       = Tcl_NewStringObj("deprecatedMessage", -1);
   Tcl_IncrRefCount(deprecatedMessageTagObj);
   alwaysUnavailableTagObj       = Tcl_NewStringObj("alwaysUnavailable", -1);
   Tcl_IncrRefCount(alwaysUnavailableTagObj);
   unavailableMessageTagObj      = Tcl_NewStringObj("unavailableMessage", -1);
   Tcl_IncrRefCount(unavailableMessageTagObj);
   availabilityTagObj            = Tcl_NewStringObj("availability", -1);
   Tcl_IncrRefCount(availabilityTagObj);
   availabilityPlatformTagObj    = Tcl_NewStringObj("platform", -1);
   Tcl_IncrRefCount(availabilityPlatformTagObj);
   availabilityIntroducedTagObj  = Tcl_NewStringObj("introduced", -1);
   Tcl_IncrRefCount(availabilityIntroducedTagObj);
   availabilityDeprecatedTagObj  = Tcl_NewStringObj("deprecated", -1);
   Tcl_IncrRefCount(availabilityDeprecatedTagObj);
   availabilityObsoletedTagObj   = Tcl_NewStringObj("obsoleted", -1);
   Tcl_IncrRefCount(availabilityObsoletedTagObj);
   availabilityUnavailableTagObj = Tcl_NewStringObj("unavailable", -1);
   Tcl_IncrRefCount(availabilityUnavailableTagObj);
   availabilityMessageTagObj     = Tcl_NewStringObj("message", -1);
   Tcl_IncrRefCount(availabilityMessageTagObj);

   Tcl_Namespace *cindexNs
      = Tcl_CreateNamespace(interp, "cindex", NULL, NULL);

   createCursorKindTable();
   createCXTypeTable();
   createCallingConvTable();
   createLayoutErrorTable();

   //-------------------------------------------------------------------------

   static Command cmdTable[] = {
#ifdef BIST
      { "bist",
        bistObjCmd },
#endif
      { "foreachChild",
        foreachChildObjCmd },
      { "index",
        indexObjCmd },
      { "recurse",
        recurseObjCmd },
      { NULL }
   };
   createAndExportCommands(interp, "cindex::%s", cmdTable);

   //-------------------------------------------------------------------------

   Tcl_Namespace *cursorNs
      = Tcl_CreateNamespace(interp, "cindex::cursor", NULL, NULL);
   Tcl_CreateEnsemble(interp, "::cindex::cursor", cursorNs, 0);
   Tcl_Export(interp, cindexNs, "cursor", 0);

   static CursorToEnumInfo cursorAvailabilityInfo = {
      .proc   = (int (*)(CXCursor))clang_getCursorAvailability,
      .labels = &availabilities
   };

   static CursorToEnumInfo cursorCXXAccessSpecifierInfo = {
      .proc   = (int (*)(CXCursor))clang_getCXXAccessSpecifier,
      .labels = &cxxAccessSpecifiers
   };

   static CursorToEnumInfo cursorLanguageInfo = {
      .proc   = (int (*)(CXCursor))clang_getCursorLanguage,
      .labels = &languages
   };

   static CursorToEnumInfo cursorLinkageInfo = {
      .proc   = (int (*)(CXCursor))clang_getCursorLinkage,
      .labels = &linkages
   };

   static CursorToBitMaskInfo objCDeclQualifiersInfo = {
      .proc  = clang_Cursor_getObjCDeclQualifiers,
      .none  = NULL,
      .masks = objCDeclQualifiers
   };

   static CursorToBitMaskInfo objCPropertyAttributesInfo = {
      .proc  = getObjCPropertyAttributes,
      .none  = NULL,
      .masks = objCPropertyAttributes
   };

   static Command cursorCmdTable[] = {
      { "argument",
        cursorUnsignedToCursorObjCmd,
        clang_Cursor_getArgument },
      { "availability",
        cursorToEnumObjCmd,
        &cursorAvailabilityInfo },
      { "briefCommentText",
        cursorToStringObjCmd,
        clang_Cursor_getBriefCommentText },
      { "canonicalCursor",
        cursorToCursorObjCmd,
        clang_getCanonicalCursor },
      { "commentRange",
        cursorToRangeObjCmd,
        clang_Cursor_getCommentRange },
      { "cxxAccessSpecifier",
        cursorToEnumObjCmd,
        &cursorCXXAccessSpecifierInfo },
      { "definition",
        cursorToCursorObjCmd,
        clang_getCursorDefinition },
      { "displayName",
        cursorToStringObjCmd,
        clang_getCursorDisplayName },
      { "enumConstantDeclValue",
        cursorEnumConstantDeclValueObjCmd },
      { "enumDeclIntegerType",
        cursorToTypeObjCmd,
        clang_getEnumDeclIntegerType },
      { "equal",
        cursorEqualObjCmd },
      { "extent",
        cursorToRangeObjCmd,
        clang_getCursorExtent },
      { "fieldDeclBitWidth",
        cursorToIntObjCmd,
        clang_getFieldDeclBitWidth },
      { "hash",
        cursorToUnsignedObjCmd,
        clang_hashCursor },
      { "IBOutletCollectionType",
        cursorToTypeObjCmd,
        clang_getIBOutletCollectionType },
      { "includedFile",
        cursorToFileObjCmd,
        clang_getIncludedFile },
      { "language",
        cursorToEnumObjCmd,
        &cursorLanguageInfo },
      { "lexicalParent",
        cursorToCursorObjCmd,
        clang_getCursorLexicalParent },
      { "linkage",
        cursorToEnumObjCmd,
        &cursorLinkageInfo },
      { "location",
        cursorToLocationObjCmd,
        clang_getCursorLocation },
      { "null",
        cursorNullObjCmd },
      { "numArguments",
        cursorToIntObjCmd,
        clang_Cursor_getNumArguments },
      { "numOverloadedDecls",
        cursorToUnsignedObjCmd,
        clang_getNumOverloadedDecls },
      { "objCDeclQualifiers",
        cursorToBitMaskObjCmd,
        &objCDeclQualifiersInfo },
      { "objCPropertyAttributes",
        cursorToBitMaskObjCmd,
        &objCPropertyAttributesInfo },
      { "objCSelectorIndex",
        cursorToIntObjCmd,
        clang_Cursor_getObjCSelectorIndex },
      { "objCTypeEncoding",
        cursorToStringObjCmd,
        clang_getDeclObjCTypeEncoding },
      { "overloadedDecls",
        cursorUnsignedToCursorObjCmd,
        clang_getOverloadedDecl },
      { "overriddenCursors",
        cursorOverriddenCursorsObjCmd },
      { "platformAvailability",
        cursorPlatformAvailabilityObjCmd },
      { "rawCommentText",
        cursorToStringObjCmd,
        clang_Cursor_getRawCommentText },
      { "receiverType",
        cursorToTypeObjCmd,
        clang_Cursor_getReceiverType },
      { "referenced",
        cursorToCursorObjCmd,
        clang_getCursorReferenced },
      { "referenceNameRange",
        cursorReferenceNameRangeObjCmd, },
      { "resultType",
        cursorToTypeObjCmd,
        clang_getCursorResultType },
      { "semanticParent",
        cursorToCursorObjCmd,
        clang_getCursorSemanticParent },
      { "specializedTemplate",
        cursorToCursorObjCmd,
        clang_getSpecializedCursorTemplate },
      { "spelling",
        cursorToStringObjCmd,
        clang_getCursorSpelling },
      { "spellingNameRange",
        cursorUnsignedToRangeObjCmd,
        cursorGetSpellingNameRange },
      { "translationUnit",
        cursorTranslationUnitObjCmd },
      { "templateCursorKind",
        cursorToKindObjCmd,
        clang_getTemplateCursorKind },
      { "type",
        cursorToTypeObjCmd,
        clang_getCursorType },
      { "typedefDeclUnderlyingType",
        cursorToTypeObjCmd,
        clang_getTypedefDeclUnderlyingType },
      { "USR",
        cursorToStringObjCmd,
        clang_getCursorUSR },
      { NULL }
   };
   createAndExportCommands(interp, "cindex::cursor::%s", cursorCmdTable);

   Tcl_Namespace *cursorIsNs
      = Tcl_CreateNamespace(interp, "cindex::cursor::is", NULL, NULL);
   Tcl_CreateEnsemble(interp, "::cindex::cursor::is", cursorIsNs, 0);
   Tcl_Export(interp, cursorNs, "is", 0);

   static Command cursorIsCmdTable[] = {
      { "attribute",
        cursorToKindToBoolObjCmd,	clang_isAttribute },
      { "bitField",
        cursorToBoolObjCmd,		clang_Cursor_isBitField },
      { "cxxMethodPureVirtual",
        cursorToBoolObjCmd,		clang_CXXMethod_isPureVirtual },
      { "cxxMethodStatic",
        cursorToBoolObjCmd,		clang_CXXMethod_isStatic },
      { "cxxMethodVirtual",
        cursorToBoolObjCmd,		clang_CXXMethod_isVirtual },
      { "declaration",
        cursorToKindToBoolObjCmd,	clang_isDeclaration },
      { "definition",
        cursorToBoolObjCmd,		clang_isCursorDefinition },
      { "dynamicCall",
        cursorToBoolObjCmd,		clang_Cursor_isDynamicCall },
      { "unexposed",
        cursorToKindToBoolObjCmd,	clang_isUnexposed },
      { "expression",
        cursorToKindToBoolObjCmd,	clang_isExpression },
      { "invalid",
        cursorToKindToBoolObjCmd,	clang_isInvalid },
      { "null",
        cursorToBoolObjCmd,		clang_Cursor_isNull },
      { "objCOptional",
        cursorToBoolObjCmd,		clang_Cursor_isObjCOptional },
      { "preprocessing",
        cursorToKindToBoolObjCmd,	clang_isPreprocessing },
      { "reference",
        cursorToKindToBoolObjCmd,	clang_isReference },
      { "statement",
        cursorToKindToBoolObjCmd,	clang_isStatement },
      { "translationUnit",
        cursorToKindToBoolObjCmd,	clang_isTranslationUnit },
      { "variadic",
        cursorToBoolObjCmd,		clang_Cursor_isVariadic },
      { "virtualBase",
        cursorToBoolObjCmd,		clang_isVirtualBase },
      { NULL }
   };
   createAndExportCommands(interp, "cindex::cursor::is::%s", cursorIsCmdTable);

   //-------------------------------------------------------------------------

   Tcl_Namespace *locationNs
      = Tcl_CreateNamespace(interp, "cindex::location", NULL, NULL);
   Tcl_CreateEnsemble(interp, "::cindex::location", locationNs, 0);
   Tcl_Export(interp, cindexNs, "location", 0);

   static Command locationCmdTable[] = {
      { "equal",
        locationEqualObjCmd },
      { "expansionLocation",
        locationDecodeObjCmd,
        clang_getExpansionLocation },
      { "fileLocation",
        locationDecodeObjCmd,
        clang_getFileLocation },
      { "null",
        locationNullObjCmd },
      { "presumedLocation",
        locationPresumedLocationObjCmd },
      { "spellingLocation",
        locationDecodeObjCmd,
        clang_getSpellingLocation },
      { NULL }
   };
   createAndExportCommands(interp, "cindex::location::%s", locationCmdTable);

   Tcl_Namespace *locationIsNs
      = Tcl_CreateNamespace(interp, "cindex::location::is", NULL, NULL);
   Tcl_CreateEnsemble(interp, "::cindex::location::is", locationIsNs, 0);
   Tcl_Export(interp, locationNs, "is", 0);

   static Command locationIsCmdTable[] = {
      { "null",
        locationIsNullObjCmd },
      { "inSystemHeader",
        locationToBoolObjCmd,
        clang_Location_isInSystemHeader },
      { "inMainFile",
        locationToBoolObjCmd,
        clang_Location_isFromMainFile },
      { NULL }
   };
   createAndExportCommands
      (interp, "cindex::location::is::%s", locationIsCmdTable);

   //-------------------------------------------------------------------------

   Tcl_Namespace *rangeNs
      = Tcl_CreateNamespace(interp, "cindex::range", NULL, NULL);
   Tcl_CreateEnsemble(interp, "::cindex::range", rangeNs, 0);
   Tcl_Export(interp, cindexNs, "range", 0);

   static Command rangeCmdTable[] = {
      { "create",
        rangeCreateObjCmd },
      { "end",
        rangeToLocationObjCmd,
        clang_getRangeEnd },
      { "equal",
        rangeEqualObjCmd },
      { "null",
        rangeNullObjCmd },
      { "start",
        rangeToLocationObjCmd,
        clang_getRangeStart },
      { NULL }
   };
   createAndExportCommands(interp, "cindex::range::%s", rangeCmdTable);

   Tcl_Namespace *rangeIsNs
      = Tcl_CreateNamespace(interp, "cindex::range::is", NULL, NULL);
   Tcl_CreateEnsemble(interp, "::cindex::range::is", rangeIsNs, 0);
   Tcl_Export(interp, rangeNs, "is", 0);

   static Command rangeIsCmdTable[] = {
      { "null",
        rangeIsNullObjCmd },
      { NULL }
   };
   createAndExportCommands(interp, "cindex::range::is::%s", rangeIsCmdTable);

   //-------------------------------------------------------------------------

   Tcl_Namespace *typeNs
      = Tcl_CreateNamespace(interp, "cindex::type", NULL, NULL);
   Tcl_CreateEnsemble(interp, "::cindex::type", typeNs, 0);
   Tcl_Export(interp, cindexNs, "type", 0);

   static CursorToEnumInfo cxxRefQualifierInfo = {
      .proc   = (CursorToEnumProc)clang_Type_getCXXRefQualifier,
      .labels = &cxxRefQualifiers
   };

   static TypeToNamedValueInfo functionTypeCallingConvInfo;
   functionTypeCallingConvInfo.proc  = clang_getFunctionTypeCallingConv;
   functionTypeCallingConvInfo.names = callingConvNames;
   Tcl_IncrRefCount(callingConvNames);

   static Command typeCmdTable[] = {
      { "alignof",
        typeToLayoutLongLongObjCmd,	
        clang_Type_getAlignOf },
      { "argType",
        typeUnsignedToTypeObjCmd,
        clang_getArgType },
      { "arrayElementType",
        typeToTypeObjCmd,
        clang_getArrayElementType },
      { "arraySize",
        typeToLongLongObjCmd,
        clang_getArraySize },
      { "canonicalType",
        typeToTypeObjCmd,
        clang_getCanonicalType },
      { "classType",
        typeToTypeObjCmd,
        clang_Type_getClassType },
      { "cxxRefQualifier",
        typeToEnumObjCmd,
        &cxxRefQualifierInfo },
      { "declaration",
        typeToCursorObjCmd,
        clang_getTypeDeclaration },
      { "elementType",
        typeToTypeObjCmd,
        clang_getElementType },
      { "equal",
        typeEqualObjCmd },
      { "functionTypeCallingConvention",
        typeToNamedValueObjCmd,
        &functionTypeCallingConvInfo },
      { "numArgTypes",
        typeToIntObjCmd,
        clang_getNumArgTypes },
      { "numElements",
        typeToLongLongObjCmd,
        clang_getNumElements },
      { "offsetof",
        typeOffsetOfObjCmd },
      { "pointeeType",
        typeToTypeObjCmd,
        clang_getPointeeType },
      { "resultType",
        typeToTypeObjCmd,
        clang_getResultType },
      { "sizeof",
        typeToLayoutLongLongObjCmd,
        clang_Type_getSizeOf },
      { "spelling",
        typeToStringObjCmd,
        clang_getTypeSpelling },
      { NULL }
   };
   createAndExportCommands(interp, "cindex::type::%s", typeCmdTable);

   Tcl_Namespace *typeIsNs
      = Tcl_CreateNamespace(interp, "cindex::type::is", NULL, NULL);
   Tcl_CreateEnsemble(interp, "::cindex::type::is", typeIsNs, 0);
   Tcl_Export(interp, typeNs, "is", 0);

   static Command typeIsCmdTable[] = {
      { "constQualified",
        typeToBoolObjCmd,
        clang_isConstQualifiedType },
      { "functionTypeVariadic",
        typeToBoolObjCmd,
        clang_isFunctionTypeVariadic },
      { "PODType",
        typeToBoolObjCmd,
        clang_isPODType },
      { "restrictQualified",
        typeToBoolObjCmd,
        clang_isRestrictQualifiedType },
      { "volatileQualified",
        typeToBoolObjCmd,
        clang_isVolatileQualifiedType },
      { NULL }
   };
   createAndExportCommands(interp, "cindex::type::is::%s", typeIsCmdTable);

   //-------------------------------------------------------------------------

   {
      unsigned mask = clang_defaultEditingTranslationUnitOptions();

      Tcl_Obj *name =
         Tcl_NewStringObj("cindex::defaultEditingTranslationUnitOptions", -1);
      Tcl_IncrRefCount(name);

      Tcl_Obj *value = Tcl_NewObj();
      Tcl_IncrRefCount(value);

      unsigned v = mask;
      while (v != 0) {
         int b = ffs(v) - 1;
         Tcl_ListObjAppendElement
            (NULL, value, Tcl_NewStringObj(parseFlags[b], -1));
         v -= 1U << b;
      }

      Tcl_ObjSetVar2(interp, name, NULL, value, 0);
      Tcl_Export(interp, cindexNs, "defaultEditingTranslationUnitOptions", 0);

      Tcl_DecrRefCount(name);
      Tcl_DecrRefCount(value);
   }

   {
      unsigned mask = clang_defaultDiagnosticDisplayOptions();

      int status = bitMaskToString
         (interp, diagnosticFormatOptions, noneTagObj, mask);
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

   Tcl_PkgProvide(interp, "cindex", PACKAGE_VERSION);

   return TCL_OK;
}

// Local Variables:
// tab-width: 8
// fill-column: 78
// mode: c
// c-basic-offset: 3
// indent-tabs-mode: nil
// End:
