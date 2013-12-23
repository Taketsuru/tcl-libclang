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
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>
#include <limits.h>

enum {
   TCL_RECURSE = 5
};

//------------------------------------------------------------------ utilities

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

static int
cindexCreateNameValueTable(Tcl_Interp                        *interp,
                           const char                        *varNamePrefix,
                           Tcl_Obj                          **nameArrayNameOut,
                           Tcl_Obj                          **valueArrayNameOut,
                           const struct cindexNameValuePair  *table)
{
   Tcl_Obj *nameArrayName  = Tcl_ObjPrintf("%sNames", varNamePrefix);
   Tcl_Obj *valueArrayName = Tcl_ObjPrintf("%sValues", varNamePrefix);

   Tcl_IncrRefCount(nameArrayName);
   Tcl_IncrRefCount(valueArrayName);

   int status = TCL_OK;
   for (int i = 0; status == TCL_OK && table[i].name != NULL; ++i) {

      Tcl_Obj *value = Tcl_NewIntObj(table[i].value);
      Tcl_Obj *name  = Tcl_NewStringObj(table[i].name, -1);

      Tcl_IncrRefCount(value);
      Tcl_IncrRefCount(name);

      if (Tcl_ObjSetVar2(interp, nameArrayName,
                         value, name, TCL_LEAVE_ERR_MSG) == NULL
          || Tcl_ObjSetVar2(interp, valueArrayName,
                            name, value, TCL_LEAVE_ERR_MSG) == NULL) {
         status = TCL_ERROR;
      }

      Tcl_DecrRefCount(value);
      Tcl_DecrRefCount(name);
   }

   if (status == TCL_OK) {
      *valueArrayNameOut = valueArrayName;
      *nameArrayNameOut  = nameArrayName;
   } else {
      Tcl_DecrRefCount(valueArrayName);
      Tcl_DecrRefCount(nameArrayName);
   }

   return status;
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
cindexGetPointerFromObj(Tcl_Interp *interp, Tcl_Obj *obj, void **ptrOut)
{
   long value;
   if (Tcl_GetLongFromObj(NULL, obj, &value) == TCL_OK) {
      *ptrOut = (void *)value;
      return TCL_OK;
   }

   mp_int bvalue;
   int status = Tcl_GetBignumFromObj(interp, obj, &bvalue);
   if (status != TCL_OK) {
      return status;
   }

   int       digit_bit  = 16;
   uintptr_t digit_mask = (1UL << digit_bit) - 1;

   uintptr_t result = 0;
   for (int i = 0; i < sizeof result * CHAR_BIT; i += digit_bit) {
      uintptr_t digit = (uintptr_t)bvalue.dp[0] & digit_mask;
      mp_div_2d(&bvalue, digit_bit, &bvalue, NULL);
      result |= digit << i;
   }

   int iszero = mp_iszero(&bvalue);

   mp_clear(&bvalue);

   if (! iszero) {
      if (interp != NULL) {
         Tcl_SetObjResult(interp,
                          Tcl_NewStringObj("the given integer is too large "
                                           "as a pointer.", -1));
      }
      return TCL_ERROR;
   }

   *ptrOut = (void *)result;

   return TCL_OK;
}

//----------------------------------------------------------------- enum label

struct cindexEnumLabels
{
   Tcl_Obj    **labels;
   int	        n;
   const char  *names[];
};

static int cindexGetEnumLabel(Tcl_Interp              *interp,
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

static int cindexGetAvailabilityObj(Tcl_Interp              *interp,
                                    enum CXAvailabilityKind  kind)
{
   return cindexGetEnumLabel(interp, &cindexAvailabilityLabels, kind);
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
      result = Tcl_NewStringObj(bitMask->name, -1);
      bitMask->nameObj = result;
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

      const char *obj = Tcl_GetStringFromObj(objv[i], NULL);

      unsigned mask = 0;

      if (none != NULL && strcmp(obj, Tcl_GetStringFromObj(none, NULL)) == 0) {
         continue;
      }

      for (int j = 0; options[j].name != NULL; ++j) {
         if (strcmp(options[j].name, obj) == 0) {
            mask = options[j].mask;
            if (mask == 0) {
               mask = 1U << j;
            }
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
   int                     sequenceNumber;
};

static struct cindexTUInfo *cindexTUHashTable[256];

static struct cindexTUInfo *
cindexCreateTUInfo(struct cindexIndexInfo *parent,
                   Tcl_Obj                *tuName,
                   CXTranslationUnit       tu)
{
   static int nextNumber = 0;

   struct cindexTUInfo *info = (struct cindexTUInfo *)Tcl_Alloc(sizeof *info);

   info->parent          = parent;
   info->name            = tuName;
   info->translationUnit = tu;
   info->sequenceNumber  = nextNumber++;

   Tcl_IncrRefCount(tuName);

   int hashTableSize = sizeof cindexTUHashTable / sizeof cindexTUHashTable[0];
   int hash = info->sequenceNumber % hashTableSize;
   info->next = cindexTUHashTable[hash];
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

   int hashTableSize = sizeof cindexTUHashTable / sizeof cindexTUHashTable[0];
   int hash = info->sequenceNumber % hashTableSize;
   struct cindexTUInfo **prev = &cindexTUHashTable[hash];
   while (*prev != info) {
      prev = &info->next;
   }
   *prev = info->next;

   Tcl_Free((char *)info);
}

static struct cindexTUInfo *
cindexLookupTranslationUnit(int sequenceNumber)
{
   int hashTableSize =
      sizeof cindexTUHashTable / sizeof cindexTUHashTable[0];
   int hash = sequenceNumber % hashTableSize;
   for (struct cindexTUInfo *p = cindexTUHashTable[hash];
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

//------------------------------------------------------------- cursor mapping

static Tcl_Obj *cindexCursorKindNamesName;
static Tcl_Obj *cindexCursorKindValuesName;

static int cindexCreateCursorKindTable(Tcl_Interp *interp)
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

   return cindexCreateNameValueTable(interp, "cindex::cursorKind",
                                     &cindexCursorKindNamesName,
                                     &cindexCursorKindValuesName,
                                     table);
}

static Tcl_Obj *cindexNewCursorObj(Tcl_Interp          *interp,
                                   CXCursor             cursor,
                                   struct cindexTUInfo *tuInfo)
{
   Tcl_Obj *elms[6];
   int      ix = 0;

   Tcl_Obj *kind = Tcl_NewIntObj(cursor.kind);
   Tcl_IncrRefCount(kind);
   Tcl_Obj *kindName
      = Tcl_ObjGetVar2(interp, cindexCursorKindNamesName, kind, 0);
   Tcl_DecrRefCount(kind);
   if (kindName == NULL) {
      Tcl_Panic("%s(%d) corrupted",
                Tcl_GetStringFromObj(cindexCursorKindNamesName, NULL),
                cursor.kind);
   }
   elms[ix++] = kindName;
   elms[ix++] = Tcl_NewIntObj(cursor.xdata);

   int ndata = sizeof cursor.data / sizeof cursor.data[0];
   for (int i = 0; i < ndata; ++i) {
      elms[ix + i] = cindexNewPointerObj(cursor.data[i]);
   }
   ix += ndata;

   elms[ix++] = Tcl_NewIntObj(tuInfo->sequenceNumber);

   return Tcl_NewListObj(ix, elms);
}

static int
cindexGetCursorFromObj(Tcl_Interp *interp, Tcl_Obj *obj,
                       CXCursor *cursor, struct cindexTUInfo **infoPtr)
{
   int       n;
   Tcl_Obj **elms;
   int status = Tcl_ListObjGetElements(interp, obj, &n, &elms);
   if (status != TCL_OK) {
      return status;
   }

   int ix = 0;

   Tcl_Obj* kindValue
      = Tcl_ObjGetVar2(interp, cindexCursorKindValuesName, elms[ix], 0);
   if (kindValue == NULL) {
      Tcl_SetObjResult(interp,
                       Tcl_ObjPrintf("invalid cursor kind: %s",
                                     Tcl_GetStringFromObj(elms[ix], NULL)));
      return TCL_ERROR;
   }

   int kind;
   status = Tcl_GetIntFromObj(interp, kindValue, &kind);
   if (status != TCL_OK) {
      Tcl_SetObjResult(interp,
                       Tcl_NewStringObj("cindex::cursorKindValue "
                                        "has been corrupted", -1));
      return TCL_ERROR;
   }

   ++ix;

   int xdata;
   status = Tcl_GetIntFromObj(interp, elms[ix], &xdata);
   if (status != TCL_OK) {
      goto invalid_cursor;
   }
   ++ix;

   enum {
      ndata = sizeof cursor->data / sizeof cursor->data[0]
   };
   void *data[ndata];
   for (int i = 0; i < ndata; ++i) {
      status = cindexGetPointerFromObj(interp, elms[ix + i], &data[i]);
      if (status != TCL_OK) {
         goto invalid_cursor;
      }
   }
   ix += ndata;

   int tuSequenceNumber;
   status = Tcl_GetIntFromObj(interp, elms[ix], &tuSequenceNumber);
   if (status != TCL_OK) {
      goto invalid_cursor;
   }

   struct cindexTUInfo *info = cindexLookupTranslationUnit(tuSequenceNumber);
   if (info == NULL) {
      Tcl_SetObjResult(interp,
                       Tcl_NewStringObj("translation unit of the given cursor "
                                        "has already been deleted.", -1));
      return TCL_ERROR;
   }

   ++ix;

   cursor->kind = kind;
   cursor->xdata = xdata;
   memcpy(cursor->data, data, sizeof data);

   if (infoPtr != NULL) {
      *infoPtr = info;
   }

   return TCL_OK;

 invalid_cursor:
   Tcl_SetObjResult(interp,
                    Tcl_NewStringObj("invalid cursor object", -1));
   return TCL_ERROR;
}

//---------------------------------------------------- source location mapping

static Tcl_Obj *cindexFile;
static Tcl_Obj *cindexLine;
static Tcl_Obj *cindexColumn;
static Tcl_Obj *cindexOffset;
static Tcl_Obj *cindexFileNameCache[64];

static unsigned long cindexFileNameHash(const char *str)
{
   unsigned long        hash = 0;
   int                  c;
   const unsigned char *cp = (const unsigned char *)str;

   while ( (c = *cp++) ) {
      hash = c + (hash << 6) + (hash << 16) - hash;
   }

   return hash;
}

static Tcl_Obj *cindexNewFileNameObj(const char *filenameCstr)
{
   int hash = cindexFileNameHash(filenameCstr)
      % (sizeof cindexFileNameCache / sizeof cindexFileNameCache[0]);

   Tcl_Obj *candidate = cindexFileNameCache[hash];
   if (candidate != NULL) {
      if (strcmp(Tcl_GetStringFromObj(candidate, NULL), filenameCstr) == 0) {
         return candidate;
      }
      Tcl_DecrRefCount(candidate);
   }

   Tcl_Obj *result = Tcl_NewStringObj(filenameCstr, -1);
   cindexFileNameCache[hash] = result;
   Tcl_IncrRefCount(result);

   return result;
}

//------------------------------------------------------------- type mapping

static Tcl_Obj *cindexTypeKindValuesObj;
static Tcl_Obj *cindexTypeKindNamesObj;

int cindexCreateCXTypeTable(Tcl_Interp *interp)
{
   static struct cindexNameValuePair cxtypeKinds[] = {
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
      { NULL }
   };

   return cindexCreateNameValueTable(interp,
                                     "cindex::typeKind",
                                     &cindexTypeKindNamesObj,
                                     &cindexTypeKindValuesObj,
                                     cxtypeKinds);
}

Tcl_Obj *cindexNewCXTypeObj(Tcl_Interp *interp, CXType type)
{
   Tcl_Obj *kind = Tcl_NewIntObj(type.kind);
   Tcl_IncrRefCount(kind);
   Tcl_Obj *kindName = Tcl_ObjGetVar2(interp, cindexTypeKindNamesObj, kind, 0);
   Tcl_DecrRefCount(kind);
   if (kindName == NULL) {
      Tcl_Panic("%s(%d) corrupted",
                Tcl_GetStringFromObj(cindexTypeKindNamesObj, NULL),
                type.kind);
   }

   enum {
      ndata = sizeof type.data / sizeof type.data[0]
   };

   Tcl_Obj* elements[1 + ndata];
   elements[0] = kindName;
   for (int i = 0; i < ndata; ++i) {
      elements[i + 1] = cindexNewPointerObj(type.data[i]);
   }

   return Tcl_NewListObj(sizeof elements / sizeof elements[0], elements);
}

int cindexGetCXTypeObj(Tcl_Interp *interp, Tcl_Obj *obj, CXType *output)
{
   CXType result;
   memset(&result, 0, sizeof result);

   enum {
      n = 1 + sizeof result.data / sizeof result.data[0]
   };

   Tcl_Obj *objs[n];
   for (int i = 0; i < n; ++i) {
      int status = Tcl_ListObjIndex(interp, obj, i, objs + i);
      if (status != TCL_OK) {
         return status;
      }
   }

   Tcl_Obj *kind = Tcl_ObjGetVar2(interp, cindexTypeKindValuesObj, objs[0],
                                  TCL_LEAVE_ERR_MSG);
   if (kind == NULL) {
      return TCL_ERROR;
   }

   int kindValue;
   int status = Tcl_GetIntFromObj(interp, kind, &kindValue);
   if (status == TCL_OK) {
      output->kind = kindValue;
      for (int i = 1; i < n; ++i) {
         status = cindexGetPointerFromObj(interp, objs[i],
                                          &output->data[i - 1]);
         if (status != TCL_ERROR) {
            break;
         }
      }
   }

   return status;
}

//----------------------------------------------------------------------------

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

   struct cindexTUInfo *info
      = (struct cindexTUInfo *)clientData;

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

   struct cindexTUInfo *info
      = (struct cindexTUInfo *)clientData;

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

   struct cindexTUInfo *info
      = (struct cindexTUInfo *)clientData;

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

   struct cindexTUInfo *info = (struct cindexTUInfo *)clientData;

   CXCursor  cursor    = clang_getTranslationUnitCursor(info->translationUnit);
   Tcl_Obj  *cursorObj = cindexNewCursorObj(interp, cursor, info);
   Tcl_SetObjResult(interp, cursorObj);

   return TCL_OK;
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
      { "save",		 cindexTranslationUnitSaveObjCmd },
      { "reparse",	 cindexTranslationUnitReparseObjCmd },
      { "resourceUsage", cindexTranslationUnitResourceUsageObjCmd },
      { "cursor",	 cindexTranslationUnitCursorObjCmd },
      { NULL },
   };

   return cindexDispatchSubcommand(clientData, interp, objc, objv, subcommands);
}

//----------------------------------------- cindex::<index instance> options

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

static int cindexIndexInstanceObjCmd(ClientData clientData,
                                     Tcl_Interp *interp,
                                     int objc,
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

struct cindexForeachChildInfo
{
   Tcl_Interp          *interp;
   Tcl_Obj             *childName;
   Tcl_Obj             *scriptObj;
   int                  returnCode;
   struct cindexTUInfo *tuInfo;
};

static enum CXChildVisitResult
cindexForeachChildHelper(CXCursor     cursor,
                         CXCursor     parent,
                         CXClientData clientData)
{
   struct cindexForeachChildInfo *visitInfo
      = (struct cindexForeachChildInfo *)clientData;

   Tcl_Obj *cursorObj
      = cindexNewCursorObj(visitInfo->interp, cursor, visitInfo->tuInfo);

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

   struct cindexTUInfo *tuInfo;
   CXCursor             cursor;
   int status = cindexGetCursorFromObj(interp, objv[2], &cursor, &tuInfo);
   if (status != TCL_OK) {
      return status;
   }

   struct cindexForeachChildInfo visitInfo = {
      .interp     = interp,
      .childName  = objv[1],
      .scriptObj  = objv[3],
      .returnCode = TCL_OK,
      .tuInfo     = tuInfo,
   };

   if (clang_visitChildren(cursor, cindexForeachChildHelper, &visitInfo)) {
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
      int status = cindexGetCursorFromObj(interp, objv[i], &cursors[ix], NULL);
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
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor, NULL);
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
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor, NULL);
   if (status != TCL_OK) {
      return status;
   }

   int result = clang_Cursor_isNull(cursor);
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

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor, NULL);
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
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor, NULL);
   if (status != TCL_OK) {
      return status;
   }

   struct cindexCursorGenericEnumInfo *info
      = (struct cindexCursorGenericEnumInfo *)clientData;

   cindexCursorGenericEnumProc proc = info->proc;
   int value = proc(cursor);

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

   struct cindexTUInfo *tuInfo;
   CXCursor             cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor, &tuInfo);
   if (status != TCL_OK) {
      return status;
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

   struct cindexTUInfo *tuInfo;
   CXCursor             cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor, &tuInfo);
   if (status != TCL_OK) {
      return status;
   }

   CXCursor  parentCursor = clang_getCursorSemanticParent(cursor);
   Tcl_Obj  *parentObj    = cindexNewCursorObj(interp, parentCursor, tuInfo);
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

   struct cindexTUInfo *tuInfo;
   CXCursor             cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor, &tuInfo);
   if (status != TCL_OK) {
      return status;
   }

   CXCursor  parentCursor = clang_getCursorLexicalParent(cursor);
   Tcl_Obj  *parentObj    = cindexNewCursorObj(interp, parentCursor, tuInfo);
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

   struct cindexTUInfo *tuInfo;
   CXCursor             cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor, &tuInfo);
   if (status != TCL_OK) {
      return status;
   }

   CXCursor *overridden = NULL;
   unsigned numOverridden = 0;
   clang_getOverriddenCursors(cursor, &overridden, &numOverridden);

   Tcl_Obj **results = (Tcl_Obj **)Tcl_Alloc(sizeof(Tcl_Obj *) * numOverridden);
   for (int i = 0; i < numOverridden; ++i) {
      results[i] = cindexNewCursorObj(interp, overridden[i], tuInfo);
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
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor, NULL);
   if (status != TCL_OK) {
      return status;
   }

   CXFile cxfile = clang_getIncludedFile(cursor);
   CXString filenameStr = clang_getFileName(cxfile);
   const char *filenameCStr = clang_getCString(filenameStr);
   Tcl_SetObjResult(interp, Tcl_NewStringObj(filenameCStr, -1));
   clang_disposeString(filenameStr);

   return TCL_OK;
}

//----------------------------------------- cindex::cursor::<location command>

static int cindexCursorRangeObjCmd(ClientData     clientData,
                                   Tcl_Interp    *interp,
                                   int            objc,
                                   Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor, NULL);
   if (status != TCL_OK) {
      return status;
   }

   typedef void (*CursorProc)(CXSourceLocation,
                              CXFile*, unsigned *, unsigned *, unsigned *);

   CXSourceRange range = clang_getCursorExtent(cursor);
   CursorProc    proc  = (CursorProc)clientData;

   enum {
      nlocs = 2
   };
   CXSourceLocation locs[nlocs];
   locs[0] = clang_getRangeStart(range);
   locs[1] = clang_getRangeEnd(range);

   Tcl_Obj *locObjs[nlocs];

   for (int i = 0; i < nlocs; ++i) {

      CXFile   file;
      unsigned line;
      unsigned column;
      unsigned offset;

      proc(locs[i], &file, &line, &column, &offset);

      Tcl_Obj *elms[4];
      int      nelms = 0;

      CXString    cxFileName = clang_getFileName(file);
      const char *filename   = clang_getCString(cxFileName);
      elms[nelms]            = cindexNewFileNameObj(filename);
      clang_disposeString(cxFileName);
      ++nelms;

      elms[nelms++] = Tcl_NewLongObj(line);
      elms[nelms++] = Tcl_NewLongObj(column);
      elms[nelms++] = Tcl_NewLongObj(offset);

      assert(nelms == sizeof elms / sizeof elms[0]);

      locObjs[i] = Tcl_NewListObj(nelms, elms);
   }

   Tcl_SetObjResult(interp, Tcl_NewListObj(nlocs, locObjs));

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
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor, NULL);
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
      CXString cxFilename;
      unsigned line;
      unsigned column;
      clang_getPresumedLocation(locs[i], &cxFilename, &line, &column);

      Tcl_Obj *elms[3];
      int      nelms = 0;

      const char *filename = clang_getCString(cxFilename);
      elms[nelms++] = cindexNewFileNameObj(filename);
      clang_disposeString(cxFilename);

      elms[nelms++] = Tcl_NewLongObj(line);
      elms[nelms++] = Tcl_NewLongObj(column);

      locObjs[i] = Tcl_NewListObj(nelms, elms);
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
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor, NULL);
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
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor, NULL);
   if (status != TCL_OK) {
      return status;
   }

   CXSourceLocation pos = clang_getCursorLocation(cursor);
   int value = clang_Location_isFromMainFile(pos);

   Tcl_SetObjResult(interp, Tcl_NewIntObj(value));

   return TCL_OK;
}

//----------------------------------------------------------------------------

static int cindexCursorTypeObjCmd(ClientData     clientData,
                                  Tcl_Interp    *interp,
                                  int            objc,
                                  Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor, NULL);
   if (status != TCL_OK) {
      return status;
   }

   CXType   cxtype = clang_getCursorType(cursor);
   Tcl_Obj *result = cindexNewCXTypeObj(interp, cxtype);
   Tcl_SetObjResult(interp, result);

   return TCL_OK;
}

//---------------------------------- cindex::cursor::typedefDeclUnderlyingType

static int cindexCursorTypedefDeclUnderlyingTypeObjCmd(ClientData     clientData,
                                                       Tcl_Interp    *interp,
                                                       int            objc,
                                                       Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor, NULL);
   if (status != TCL_OK) {
      return status;
   }

   CXType   cxtype = clang_getTypedefDeclUnderlyingType(cursor);
   Tcl_Obj *result = cindexNewCXTypeObj(interp, cxtype);
   Tcl_SetObjResult(interp, result);

   return TCL_OK;
}

//---------------------------------------- cindex::cursor::enumDeclIntegerType

static int cindexCursorEnumDeclIntegerTypeObjCmd(ClientData     clientData,
                                                 Tcl_Interp    *interp,
                                                 int            objc,
                                                 Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor, NULL);
   if (status != TCL_OK) {
      return status;
   }

   if (cursor.kind != CXCursor_EnumDecl) {
      Tcl_SetObjResult
         (interp, Tcl_NewStringObj("cursor kind must be EnumDecl", -1));
      return TCL_ERROR;
   }

   CXType   cxtype = clang_getEnumDeclIntegerType(cursor);
   Tcl_Obj *result = cindexNewCXTypeObj(interp, cxtype);
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
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor, NULL);
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

//------------------------------------------ cindex::cursor::fieldDeclBitWidth

static int cindexCursorFieldDeclBitWidthObjCmd(ClientData     clientData,
                                               Tcl_Interp    *interp,
                                               int            objc,
                                               Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor, NULL);
   if (status != TCL_OK) {
      return status;
   }

   if (cursor.kind != CXCursor_FieldDecl) {
      Tcl_SetObjResult
         (interp, Tcl_NewStringObj("cursor kind must be FieldDecl.", -1));
      return TCL_ERROR;
   }

   int      result    = clang_getFieldDeclBitWidth(cursor);
   Tcl_Obj *resultObj = Tcl_NewIntObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//----------------------------------------------- cindex::cursor::numArguments

static int cindexCursorNumArgumentsObjCmd(ClientData     clientData,
                                          Tcl_Interp    *interp,
                                          int            objc,
                                          Tcl_Obj *const objv[])
{
   if (objc != 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor");
      return TCL_ERROR;
   }

   CXCursor cursor;
   int status = cindexGetCursorFromObj(interp, objv[1], &cursor, NULL);
   if (status != TCL_OK) {
      return status;
   }

   int      result    = clang_Cursor_getNumArguments(cursor);
   Tcl_Obj *resultObj = Tcl_NewIntObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//--------------------------------------------------- cindex::cursor::argument

static int cindexCursorArgumentObjCmd(ClientData     clientData,
                                      Tcl_Interp    *interp,
                                      int            objc,
                                      Tcl_Obj *const objv[])
{
   if (objc != 3) {
      Tcl_WrongNumArgs(interp, 1, objv, "cursor argumentIndex");
      return TCL_ERROR;
   }

   int argumentIndex;
   int status = Tcl_GetIntFromObj(interp, objv[2], &argumentIndex);
   if (status != TCL_OK) {
      return status;
   }

   struct cindexTUInfo *tuInfo;
   CXCursor cursor;
   status = cindexGetCursorFromObj(interp, objv[1], &cursor, &tuInfo);
   if (status != TCL_OK) {
      return status;
   }

   CXCursor  cxresult  = clang_Cursor_getArgument(cursor, argumentIndex);
   Tcl_Obj  *resultObj = cindexNewCursorObj(interp, cxresult, tuInfo);
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
   Tcl_Obj *resultObj     = cindexNewCXTypeObj(interp, result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//----------------------------------------------------- type->unsigned command

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

//----------------------------------------------------- type->long long command

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

   long long (*proc)(CXType) = (long long (*)(CXType))clientData;
   long long  result         = proc(cxtype);
   Tcl_Obj   *resultObj      = cindexNewIntmaxObj(result);
   Tcl_SetObjResult(interp, resultObj);

   return TCL_OK;
}

//------------------------------------------------------------- initialization

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
   int status = TCL_OK;

   if (Tcl_InitStubs(interp, "8.6", 0) == NULL) {
      return TCL_ERROR;
   }

   cindexNone = Tcl_NewStringObj("-none", -1);
   Tcl_IncrRefCount(cindexNone);

   cindexFile = Tcl_NewStringObj("file", -1);
   Tcl_IncrRefCount(cindexFile);

   cindexLine = Tcl_NewStringObj("line", -1);
   Tcl_IncrRefCount(cindexLine);

   cindexColumn = Tcl_NewStringObj("column", -1);
   Tcl_IncrRefCount(cindexColumn);

   cindexOffset = Tcl_NewStringObj("offset", -1);
   Tcl_IncrRefCount(cindexOffset);

   Tcl_Namespace *cindexNs
      = Tcl_CreateNamespace(interp, "cindex", NULL, NULL);

   status = cindexCreateCursorKindTable(interp);
   if (status != TCL_OK) {
      return status;
   }

   status = cindexCreateCXTypeTable(interp);
   if (status != TCL_OK) {
      return status;
   }

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

   Tcl_Namespace *cursorNs
      = Tcl_CreateNamespace(interp, "cindex::cursor", NULL, NULL);
   Tcl_Command cursorCmd
      = Tcl_CreateEnsemble(interp, "::cindex::cursor", cursorNs, 0);
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
      { "expansionLocation",
        cindexCursorRangeObjCmd,
        (ClientData)clang_getExpansionLocation },
      { "presumedLocation",
        cindexCursorPresumedLocationObjCmd },
      { "spellingLocation",
        cindexCursorRangeObjCmd,
        (ClientData)clang_getSpellingLocation },
      { "fileLocation",
        cindexCursorRangeObjCmd,
        (ClientData)clang_getFileLocation },
      { "type",
        cindexCursorTypeObjCmd },
      { "typedefDeclUnderlyingType",
        cindexCursorTypedefDeclUnderlyingTypeObjCmd },
      { "enumDeclIntegerType",
        cindexCursorEnumDeclIntegerTypeObjCmd },
      { "enumConstantDeclValue",
        cindexCursorEnumConstantDeclValueObjCmd },
      { "fieldDeclBitWidth",
        cindexCursorFieldDeclBitWidthObjCmd },
      { "numArguments",
        cindexCursorNumArgumentsObjCmd },
      { "argument",
        cindexCursorArgumentObjCmd },
      { NULL }
   };
   cindexCreateAndExportCommands(interp, "cindex::cursor::%s", cursorCmdTable);

   Tcl_Namespace *cursorIsNs
      = Tcl_CreateNamespace(interp, "cindex::cursor::is", NULL, NULL);
   Tcl_Command cursorIsCmd
      = Tcl_CreateEnsemble(interp, "::cindex::cursor::is", cursorIsNs, 0);
   Tcl_Export(interp, cursorNs, "is", 0);

   static struct cindexCommand cursorIsCmdTable[] = {
      { "null",
        cindexCursorIsNullObjCmd },
      { "declaration",
        cindexCursorIsGenericObjCmd, clang_isDeclaration },
      { "reference",
        cindexCursorIsGenericObjCmd, clang_isReference },
      { "expression",
	cindexCursorIsGenericObjCmd, clang_isExpression },
      { "statement",
        cindexCursorIsGenericObjCmd, clang_isStatement },
      { "attribute",
        cindexCursorIsGenericObjCmd, clang_isAttribute },
      { "invalid",
        cindexCursorIsGenericObjCmd, clang_isInvalid },
      { "translationUnit",
	cindexCursorIsGenericObjCmd, clang_isTranslationUnit },
      { "preprocessing",
	cindexCursorIsGenericObjCmd, clang_isPreprocessing },
      { "unexposed",
        cindexCursorIsGenericObjCmd, clang_isUnexposed },
      { "inSystemHeader",
        cindexCursorIsInSystemHeaderObjCmd },
      { "inMainFile",
        cindexCursorIsInMainFileObjCmd },
      { NULL }
   };
   cindexCreateAndExportCommands
      (interp, "cindex::cursor::is::%s", cursorIsCmdTable);

   Tcl_Namespace *typeNs
      = Tcl_CreateNamespace(interp, "cindex::type", NULL, NULL);
   Tcl_Command typeCmd
      = Tcl_CreateEnsemble(interp, "::cindex::type", typeNs, 0);
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
        cindexGenericTypeToLongLongObjCmd,
        clang_Type_getAlignOf },
      { "sizeof",
        cindexGenericTypeToLongLongObjCmd,
        clang_Type_getSizeOf },
      { NULL }
   };
   cindexCreateAndExportCommands(interp, "cindex::type::%s", typeCmdTable);

   Tcl_Namespace *typeIsNs
      = Tcl_CreateNamespace(interp, "cindex::type::is", NULL, NULL);
   Tcl_Command typeIsCmd
      = Tcl_CreateEnsemble(interp, "::cindex::type::is", typeIsNs, 0);
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
   cindexCreateAndExportCommands(interp, "cindex::type::is::%s", typeIsCmdTable);

   {
      Tcl_Obj *name =
         Tcl_NewStringObj("cindex::defaultEditingTranslationUnitOptions", -1);
      Tcl_IncrRefCount(name);

      unsigned mask = clang_defaultEditingTranslationUnitOptions();
      int status = cindexBitMaskToString(interp, cindexParseOptions,
                                         cindexNone, mask);
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
