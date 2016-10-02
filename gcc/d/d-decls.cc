// d-decls.cc -- D frontend for GCC.
// Copyright (C) 2011-2016 Free Software Foundation, Inc.

// GCC is free software; you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 3, or (at your option) any later
// version.

// GCC is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
// for more details.

// You should have received a copy of the GNU General Public License
// along with GCC; see the file COPYING3.  If not see
// <http://www.gnu.org/licenses/>.


#include "config.h"
#include "system.h"
#include "coretypes.h"

#include "dfrontend/mars.h"
#include "dfrontend/aggregate.h"
#include "dfrontend/attrib.h"
#include "dfrontend/enum.h"
#include "dfrontend/globals.h"
#include "dfrontend/init.h"
#include "dfrontend/module.h"
#include "dfrontend/statement.h"
#include "dfrontend/ctfe.h"
#include "dfrontend/target.h"

#include "tree.h"
#include "fold-const.h"
#include "diagnostic.h"
#include "target.h"
#include "stringpool.h"
#include "stor-layout.h"

#include "d-tree.h"
#include "d-codegen.h"
#include "d-objfile.h"
#include "id.h"

// Create the symbol with tree for struct initialisers.

tree
SymbolDeclaration::toSymbol()
{
  return dsym->toInitializer();
}


/* Generate a mangled identifier using NAME and SUFFIX, prefixed by the
   assembler name for DSYM.  */

tree
make_internal_name (Dsymbol *dsym, const char *name, const char *suffix)
{
  const char *prefix = mangle(dsym);
  unsigned namelen = strlen (name);
  unsigned buflen = (2 + strlen (prefix) + namelen + strlen (suffix)) * 2;
  char *buf = (char *) alloca (buflen);

  snprintf (buf, buflen, "_D%s%u%s%s", prefix, namelen, name, suffix);
  tree ident = get_identifier (buf);

  /* Symbol is not found in user code, but generate a readable name for it
     anyway for debug and diagnostic reporting.  */
  snprintf (buf, buflen, "%s.%s", dsym->toPrettyChars(), name);
  IDENTIFIER_PRETTY_NAME (ident) = get_identifier (buf);

  return ident;
}

tree
Dsymbol::toSymbol()
{
  gcc_unreachable();          // BUG: implement
  return NULL;
}

// Create the symbol with VAR_DECL tree for static variables.

tree
VarDeclaration::toSymbol()
{
  if (!csym)
    {
      // For field declaration, it is possible for toSymbol to be called
      // before the parent's build_ctype()
      if (isField())
	{
	  AggregateDeclaration *parent_decl = toParent()->isAggregateDeclaration();
	  gcc_assert (parent_decl);
	  build_ctype(parent_decl->type);
	  gcc_assert (csym);
	  return csym;
	}

      tree mangle = NULL_TREE;

      if (this->isDataseg ())
	{
	  if (this->mangleOverride)
	    mangle = get_identifier (this->mangleOverride);
	  else
	    {
	      mangle = get_identifier (::mangle (this));

	      // Use same symbol for VarDeclaration templates with same mangle
	      if (this->storage_class & STCextern)
		;
	      else if (!IDENTIFIER_DSYMBOL (mangle))
		IDENTIFIER_DSYMBOL (mangle) = this;
	      else
		{
		  Dsymbol *other = IDENTIFIER_DSYMBOL (mangle);

		  // Non-templated variables shouldn't be defined twice
		  bool local_p, template_p;
		  get_template_storage_info(this, &local_p, &template_p);
		  if (!template_p)
		    ScopeDsymbol::multiplyDefined(loc, this, other);

		  csym = other->toSymbol();
		  return csym;
		}
	    }
	}

      tree_code code = isParameter() ? PARM_DECL
	: !canTakeAddressOf() ? CONST_DECL
	: VAR_DECL;

      csym = build_decl (UNKNOWN_LOCATION, code,
			 get_identifier (ident->string),
			 declaration_type (this));
      DECL_LANG_SPECIFIC (csym) = build_lang_decl (this);

      DECL_CONTEXT (csym) = d_decl_context (this);
      set_decl_location (csym, this);

      if (this->alignment > 0)
	{
	  SET_DECL_ALIGN (csym, this->alignment * BITS_PER_UNIT);
	  DECL_USER_ALIGN (csym) = 1;
	}

      if (isParameter())
	{
	  // Pass non-trivial structs by invisible reference.
	  if (TREE_ADDRESSABLE (TREE_TYPE (csym)))
	    {
	      tree argtype = build_reference_type(TREE_TYPE (csym));
	      argtype = build_qualified_type(argtype, TYPE_QUAL_RESTRICT);
	      gcc_assert (!DECL_BY_REFERENCE (csym));
	      TREE_TYPE (csym) = argtype;
	      DECL_BY_REFERENCE (csym) = 1;
	      TREE_ADDRESSABLE (csym) = 0;
	      relayout_decl (csym);
	      this->storage_class |= STCref;
	    }

	  DECL_ARG_TYPE (csym) = TREE_TYPE (csym);
	  gcc_assert (TREE_CODE (DECL_CONTEXT (csym)) == FUNCTION_DECL);
	}
      else if (!canTakeAddressOf())
	{
	  // Manifest constants have no address in memory.
	  TREE_CONSTANT (csym) = 1;
	  TREE_READONLY (csym) = 1;
	  TREE_STATIC (csym) = 0;
	}
      else if (isDataseg())
	{
	  gcc_assert (mangle != NULL_TREE);

	  if (!this->mangleOverride
	      && (protection == PROTpublic
		  || storage_class & (STCstatic | STCextern)))
	    {
	      tree target_name = targetm.mangle_decl_assembler_name (csym, mangle);
	      IDENTIFIER_DSYMBOL (target_name) = IDENTIFIER_DSYMBOL (mangle);
	      mangle = target_name;
	    }

	  IDENTIFIER_PRETTY_NAME (mangle) = get_identifier (toPrettyChars (true));
	  SET_DECL_ASSEMBLER_NAME (csym, mangle);
	  setup_symbol_storage (this, csym, false);
	}

      d_keep (csym);

      // Can't set TREE_STATIC, etc. until we get to toObjFile as this could be
      // called from a variable in an imported module.
      if ((isConst() || isImmutable()) && (storage_class & STCinit)
	  && declaration_reference_p(this))
	{
	  if (!TREE_STATIC (csym))
	    TREE_READONLY (csym) = 1;
	  else
	    DECL_LANG_READONLY (csym) = true;
	}

      // Propagate shared.
      if (TYPE_SHARED (TREE_TYPE (csym)))
	TREE_ADDRESSABLE (csym) = 1;

      // Mark compiler generated temporaries as artificial.
      if (storage_class & STCtemp)
	DECL_ARTIFICIAL (csym) = 1;

#if TARGET_DLLIMPORT_DECL_ATTRIBUTES
      // Have to test for import first
      if (isImportedSymbol())
	{
	  insert_decl_attribute (csym, "dllimport");
	  DECL_DLLIMPORT_P (csym) = 1;
	}
      else if (isExport())
	insert_decl_attribute (csym, "dllexport");
#endif

      if (global.params.vtls && isDataseg() && isThreadlocal())
	{
	  char *p = loc.toChars();
	  fprintf (global.stdmsg, "%s: %s is thread local\n", p ? p : "", toChars());
	  if (p)
	    free (p);
	}
    }

  return csym;
}

// Create the symbol with tree for typeinfo decls.

tree
TypeInfoDeclaration::toSymbol()
{
  if (!csym)
    {
      gcc_assert(tinfo->ty != Terror);

      VarDeclaration::toSymbol();

      // This variable is the static initialization for the
      // given TypeInfo.  It is the actual data, not a reference
      gcc_assert (TREE_CODE (TREE_TYPE (csym)) == POINTER_TYPE);
      TREE_TYPE (csym) = TREE_TYPE (TREE_TYPE (csym));
      relayout_decl (csym);
      TREE_USED (csym) = 1;

      // Built-in typeinfo will be referenced as one-only.
      D_DECL_ONE_ONLY (csym) = 1;
      d_comdat_linkage (csym);
    }

  return csym;
}

// Create the symbol with tree for typeinfoclass decls.

tree
TypeInfoClassDeclaration::toSymbol()
{
  gcc_assert (tinfo->ty == Tclass);
  TypeClass *tc = (TypeClass *) tinfo;
  return tc->sym->toSymbol();
}


// Create the symbol with tree for function aliases.

tree
FuncAliasDeclaration::toSymbol()
{
  return funcalias->toSymbol();
}

// Create the symbol with FUNCTION_DECL tree for functions.

tree
FuncDeclaration::toSymbol()
{
  if (!csym)
    {
      TypeFunction *ftype = (TypeFunction *) (tintro ? tintro : type);
      tree fntype = NULL_TREE;
      tree vindex = NULL_TREE;

      // Run full semantic on symbols we need to know about during compilation.
      if (inferRetType && type && !type->nextOf())
	{
	  Module *old_current_module_decl = current_module_decl;
	  current_module_decl = NULL;
	  if (!functionSemantic())
	    {
	      csym = error_mark_node;
	      return csym;
	    }
	  current_module_decl = old_current_module_decl;
	}

      tree mangle;

      if (this->mangleOverride)
	mangle = get_identifier (this->mangleOverride);
      else
	{
	  mangle = get_identifier (mangleExact (this));

	  // Use same symbol for FuncDeclaration templates with same mangle
	  if (this->fbody)
	    {
	      if (!IDENTIFIER_DSYMBOL (mangle))
		IDENTIFIER_DSYMBOL (mangle) = this;
	      else
		{
		  Dsymbol *other = IDENTIFIER_DSYMBOL (mangle);

		  // Non-templated functions shouldn't be defined twice
		  bool local_p, template_p;
		  get_template_storage_info(this, &local_p, &template_p);
		  if (!template_p)
		    ScopeDsymbol::multiplyDefined(loc, this, other);

		  csym = other->toSymbol();
		  return csym;
		}
	    }
	}

      csym = build_decl (UNKNOWN_LOCATION, FUNCTION_DECL, NULL_TREE, NULL_TREE);
      DECL_LANG_SPECIFIC (csym) = build_lang_decl (this);
      DECL_CONTEXT (csym) = d_decl_context (this);

      DECL_NAME (csym) = this->isMain()
	? get_identifier (toPrettyChars (true)) : get_identifier (ident->string);

      if (!this->mangleOverride)
	{
	  tree target_name = targetm.mangle_decl_assembler_name (csym, mangle);
	  IDENTIFIER_DSYMBOL (target_name) = IDENTIFIER_DSYMBOL (mangle);
	  mangle = target_name;
	}

      IDENTIFIER_PRETTY_NAME (mangle) = get_identifier (toPrettyChars (true));
      SET_DECL_ASSEMBLER_NAME (csym, mangle);

      TREE_TYPE (csym) = build_ctype(ftype);
      d_keep (csym);

      if (isNested())
	{
	  // Add an extra argument for the frame/closure pointer,
	  // also needed to be compatible with delegates.
	  fntype = build_vthis_type(void_type_node, TREE_TYPE (csym));
	}
      else if (isThis())
	{
	  // Do this even if there is no debug info.  It is needed to make
	  // sure member functions are not called statically
	  AggregateDeclaration *agg_decl = isMember2();
	  tree handle = build_ctype(agg_decl->handleType());

	  // If handle is a pointer type, get record type.
	  if (!agg_decl->isStructDeclaration())
	    handle = TREE_TYPE (handle);

	  fntype = build_vthis_type(handle, TREE_TYPE (csym));

	  if (isVirtual() && vtblIndex != -1)
	    vindex = size_int (vtblIndex);
	}
      else if (isMain() && ftype->nextOf()->toBasetype()->ty == Tvoid)
	{
	  // void main() implicitly converted to int main().
	  fntype = build_function_type (int_type_node, TYPE_ARG_TYPES (TREE_TYPE (csym)));
	}

      if (fntype != NULL_TREE)
	{
	  TYPE_ATTRIBUTES (fntype) = TYPE_ATTRIBUTES (TREE_TYPE (csym));
	  TYPE_LANG_SPECIFIC (fntype) = TYPE_LANG_SPECIFIC (TREE_TYPE (csym));
	  TREE_ADDRESSABLE (fntype) = TREE_ADDRESSABLE (TREE_TYPE (csym));
	  TREE_TYPE (csym) = fntype;
	  d_keep (fntype);
	}

      if (vindex)
	{
	  DECL_VINDEX (csym) = vindex;
	  DECL_VIRTUAL_P (csym) = 1;
	}

      if (isMember2() || isFuncLiteralDeclaration())
	{
	  // See grokmethod in cp/decl.c
	  DECL_DECLARED_INLINE_P (csym) = 1;
	  DECL_NO_INLINE_WARNING_P (csym) = 1;
	}

      if (naked)
	{
	  DECL_NO_INSTRUMENT_FUNCTION_ENTRY_EXIT (csym) = 1;
	  DECL_UNINLINABLE (csym) = 1;
	}

      // These are always compiler generated.
      if (isArrayOp)
	{
	  DECL_ARTIFICIAL (csym) = 1;
	  D_DECL_ONE_ONLY (csym) = 1;
	}
      // So are ensure and require contracts.
      if (ident == Id::ensure || ident == Id::require)
	{
	  DECL_ARTIFICIAL (csym) = 1;
	  TREE_PUBLIC (csym) = 1;
	}

      // Storage class attributes
      if (storage_class & STCstatic)
	TREE_STATIC (csym) = 1;
      if (storage_class & STCfinal)
	DECL_FINAL_P (csym) = 1;

#if TARGET_DLLIMPORT_DECL_ATTRIBUTES
      // Have to test for import first
      if (isImportedSymbol())
	{
	  insert_decl_attribute (csym, "dllimport");
	  DECL_DLLIMPORT_P (csym) = 1;
	}
      else if (isExport())
	insert_decl_attribute (csym, "dllexport");
#endif
      set_decl_location (csym, this);
      setup_symbol_storage (this, csym, false);

      if (!ident)
	TREE_PUBLIC (csym) = 0;

      // %% Probably should be a little more intelligent about this
      TREE_USED (csym) = 1;

      maybe_set_intrinsic (this);
    }

  return csym;
}


// Create the thunk symbol functions.
// Thunk is added to class at OFFSET.

tree
FuncDeclaration::toThunkSymbol (int offset)
{
  Thunk *thunk;

  toSymbol();
  toObjFile();

  /* If the thunk is to be static (that is, it is being emitted in this
     module, there can only be one FUNCTION_DECL for it.   Thus, there
     is a list of all thunks for a given function. */
  bool found = false;

  for (size_t i = 0; i < DECL_LANG_THUNKS (csym).length(); i++)
    {
      thunk = DECL_LANG_THUNKS (csym)[i];
      if (thunk->offset == offset)
	{
	  found = true;
	  break;
	}
    }

  if (!found)
    {
      thunk = new Thunk();
      thunk->offset = offset;
      DECL_LANG_THUNKS (csym).safe_push (thunk);
    }

  if (!thunk->symbol)
    {
      tree thunk_decl = build_decl (DECL_SOURCE_LOCATION (csym),
				    FUNCTION_DECL, NULL_TREE, TREE_TYPE (csym));
      DECL_LANG_SPECIFIC (thunk_decl) = DECL_LANG_SPECIFIC (csym);

      TREE_READONLY (thunk_decl) = TREE_READONLY (csym);
      TREE_THIS_VOLATILE (thunk_decl) = TREE_THIS_VOLATILE (csym);
      TREE_NOTHROW (thunk_decl) = TREE_NOTHROW (csym);

      DECL_CONTEXT (thunk_decl) = d_decl_context (this);

      /* Thunks inherit the public access of the function they are targetting.  */
      TREE_PUBLIC (thunk_decl) = TREE_PUBLIC (csym);
      DECL_EXTERNAL (thunk_decl) = 0;

      /* Thunks are always addressable.  */
      TREE_ADDRESSABLE (thunk_decl) = 1;
      TREE_USED (thunk_decl) = 1;
      DECL_ARTIFICIAL (thunk_decl) = 1;
      DECL_DECLARED_INLINE_P (thunk_decl) = 0;

      DECL_VISIBILITY (thunk_decl) = DECL_VISIBILITY (csym);
      /* Needed on some targets to avoid "causes a section type conflict".  */
      D_DECL_ONE_ONLY (thunk_decl) = D_DECL_ONE_ONLY (csym);
      DECL_COMDAT (thunk_decl) = DECL_COMDAT (csym);
      DECL_WEAK (thunk_decl) = DECL_WEAK (csym);

      tree target_name = DECL_ASSEMBLER_NAME (csym);
      unsigned identlen = IDENTIFIER_LENGTH (target_name) + 14;
      const char *ident = XNEWVEC (const char, identlen);
      snprintf (CONST_CAST (char *, ident), identlen,
		"_DT%u%s", offset, IDENTIFIER_POINTER (target_name));

      DECL_NAME (thunk_decl) = get_identifier (ident);
      SET_DECL_ASSEMBLER_NAME (thunk_decl, DECL_NAME (thunk_decl));

      d_keep (thunk_decl);

      use_thunk (thunk_decl, csym, offset);

      thunk->symbol = thunk_decl;
    }

  return thunk->symbol;
}


// Create the "ClassInfo" symbol for classes.

tree
ClassDeclaration::toSymbol()
{
  if (!csym)
    {
      tree ident = make_internal_name (this, "__Class", "Z");

      csym = build_decl (BUILTINS_LOCATION, VAR_DECL,
			 IDENTIFIER_PRETTY_NAME (ident), unknown_type_node);
      SET_DECL_ASSEMBLER_NAME (csym, ident);
      DECL_LANG_SPECIFIC (csym) = build_lang_decl (NULL);
      d_keep (csym);

      setup_symbol_storage (this, csym, true);
      set_decl_location (csym, this);

      DECL_ARTIFICIAL (csym) = 1;
      // ClassInfo cannot be const data, because we use the monitor on it.
      TREE_READONLY (csym) = 0;
    }

  return csym;
}

// Create the "InterfaceInfo" symbol for interfaces.

tree
InterfaceDeclaration::toSymbol()
{
  if (!csym)
    {
      tree ident = make_internal_name (this, "__Interface", "Z");
      csym = build_decl (BUILTINS_LOCATION, VAR_DECL,
			 IDENTIFIER_PRETTY_NAME (ident), unknown_type_node);
      SET_DECL_ASSEMBLER_NAME (csym, ident);
      DECL_LANG_SPECIFIC (csym) = build_lang_decl (NULL);
      d_keep (csym);

      setup_symbol_storage (this, csym, true);
      set_decl_location (csym, this);

      DECL_ARTIFICIAL (csym) = 1;
      TREE_READONLY (csym) = 1;
    }

  return csym;
}

// Create the "ModuleInfo" symbol for a given module.

tree
Module::toSymbol()
{
  if (!csym)
    {
      tree ident = make_internal_name (this, "__ModuleInfo", "Z");
      csym = build_decl (BUILTINS_LOCATION, VAR_DECL,
			 IDENTIFIER_PRETTY_NAME (ident), unknown_type_node);
      SET_DECL_ASSEMBLER_NAME (csym, ident);
      DECL_LANG_SPECIFIC (csym) = build_lang_decl (NULL);
      d_keep (csym);

      setup_symbol_storage (this, csym, true);
      set_decl_location (csym, this);

      DECL_ARTIFICIAL (csym) = 1;
      // Not readonly, moduleinit depends on this.
      TREE_READONLY (csym) = 0;
    }

  return csym;
}

tree
StructLiteralExp::toSymbol()
{
  if (!sym)
    {
      // Build reference symbol.
      tree ctype = build_ctype(type);
      sym = build_artificial_decl(ctype, NULL_TREE, "S");
      set_decl_location(sym, loc);

      DECL_LANG_SPECIFIC (sym) = build_lang_decl (NULL);
      this->sinit = sym;

      DECL_INITIAL (sym) = build_expr(this, true);
      d_finish_symbol(sym);
    }

  return sym;
}

tree
ClassReferenceExp::toSymbol()
{
  if (!value->sym)
    {
      // Build reference symbol.
      tree ctype = build_ctype(value->stype);
      value->sym = build_artificial_decl(TREE_TYPE (ctype), NULL_TREE, "C");
      set_decl_location(value->sym, loc);

      DECL_LANG_SPECIFIC (value->sym) = build_lang_decl (NULL);

      toInstanceDt(&DECL_LANG_INITIAL (value->sym));
      d_finish_symbol(value->sym);

      value->sinit = value->sym;
    }

  return value->sym;
}

// Create the "vtbl" symbol for ClassDeclaration.
// This is accessible via the ClassData, but since it is frequently
// needed directly (like for rtti comparisons), make it directly accessible.

tree
ClassDeclaration::toVtblSymbol()
{
  if (!vtblsym)
    {
      /* The DECL_INITIAL value will have a different type object from the
	 VAR_DECL.  The back end seems to accept this. */
      Type *vtbltype = Type::tvoidptr->sarrayOf (vtbl.dim);

      tree ident = make_internal_name (this, "__vtbl", "Z");
      vtblsym = build_decl (UNKNOWN_LOCATION, VAR_DECL,
			    IDENTIFIER_PRETTY_NAME (ident),
			    build_ctype (vtbltype));
      SET_DECL_ASSEMBLER_NAME (vtblsym, ident);
      DECL_LANG_SPECIFIC (vtblsym) = build_lang_decl (NULL);
      d_keep (vtblsym);

      setup_symbol_storage (this, vtblsym, true);
      set_decl_location (vtblsym, this);

      TREE_READONLY(vtblsym) = 1;
      TREE_ADDRESSABLE(vtblsym) = 1;
      DECL_ARTIFICIAL(vtblsym) = 1;

      DECL_CONTEXT (vtblsym) = d_decl_context (this);
      SET_DECL_ALIGN (vtblsym, TARGET_VTABLE_ENTRY_ALIGN);
    }
  return vtblsym;
}

// Create the static initializer for the struct/class.

// Because this is called from the front end (mtype.cc:TypeStruct::defaultInit()),
// we need to hold off using back-end stuff until the toobjfile phase.

// Specifically, it is not safe create a VAR_DECL with a type from build_ctype()
// because there may be unresolved recursive references.
// StructDeclaration::toObjFile calls toInitializer without ever calling
// SymbolDeclaration::toSymbol, so we just need to keep checking if we
// are in the toObjFile phase.

tree
AggregateDeclaration::toInitializer()
{
  if (!sinit)
    sinit = make_internal_name (this, "__init", "Z");

  if (!VAR_P (sinit) && current_module_decl)
    {
      tree stype = build_ctype (type);
      if (!this->isStructDeclaration())
	stype = TREE_TYPE (stype);

      tree ident = sinit;
      sinit = build_decl (UNKNOWN_LOCATION, VAR_DECL,
			  IDENTIFIER_PRETTY_NAME (ident), stype);
      SET_DECL_ASSEMBLER_NAME (sinit, ident);
      DECL_LANG_SPECIFIC (sinit) = build_lang_decl (NULL);
      d_keep (sinit);

      setup_symbol_storage (this, sinit, true);
      set_decl_location (sinit, this);

      TREE_ADDRESSABLE (sinit) = 1;
      TREE_READONLY (sinit) = 1;
      DECL_ARTIFICIAL (sinit) = 1;
      // These initialisers are always global.
      DECL_CONTEXT (sinit) = NULL_TREE;
    }

  return sinit;
}

// Create the static initializer for the enum.

tree
EnumDeclaration::toInitializer()
{
  if (!sinit)
    {
      Identifier *ident_save = ident;
      if (!ident)
	ident = Identifier::generateId("__enum");
      sinit = make_internal_name (this, "__init", "Z");
      ident = ident_save;
    }

  if (!VAR_P (sinit) && current_module_decl)
    {
      tree ident = sinit;
      sinit = build_decl (UNKNOWN_LOCATION, VAR_DECL,
			  IDENTIFIER_PRETTY_NAME (ident),
			  build_ctype (type));
      SET_DECL_ASSEMBLER_NAME (sinit, ident);
      DECL_LANG_SPECIFIC (sinit) = build_lang_decl (NULL);
      d_keep (sinit);

      setup_symbol_storage (this, sinit, true);
      set_decl_location (sinit, this);

      TREE_READONLY (sinit) = 1;
      DECL_ARTIFICIAL (sinit) = 1;
      DECL_CONTEXT (sinit) = NULL_TREE;
    }

  return sinit;
}

