/*-------------------------------------------------------------------------
 *
 * deparse_utility.c
 *	  Functions to convert utility commands to machine-parseable
 *	  representation
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * NOTES
 *
 * This is intended to provide JSON blobs representing DDL commands, which can
 * later be re-processed into plain strings by well-defined sprintf-like
 * expansion.  These JSON objects are intended to allow for machine-editing of
 * the commands, by replacing certain nodes within the objects.
 *
 * Much of the information in the output blob actually comes from system
 * catalogs, not from the command parse node, as it is impossible to reliably
 * construct a fully-specified command (i.e. one not dependent on search_path
 * etc) looking only at the parse node.
 *
 * IDENTIFICATION
 *	  src/backend/tcop/deparse_utility.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_language.h"
#include "catalog/pg_largeobject.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_range.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_ts_config_map.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_ts_parser.h"
#include "catalog/pg_ts_template.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/sequence.h"
#include "funcapi.h"
#include "lib/ilist.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "parser/analyze.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "rewrite/rewriteHandler.h"
#include "tcop/deparse_utility.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/json.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/syscache.h"


/*
 * Before they are turned into JSON representation, each command is represented
 * as an object tree, using the structs below.
 */
typedef enum
{
	ObjTypeNull,
	ObjTypeBool,
	ObjTypeString,
	ObjTypeArray,
	ObjTypeObject
} ObjType;

typedef struct ObjTree
{
	slist_head	params;
	int			numParams;
} ObjTree;

typedef struct ObjElem
{
	char	   *name;
	ObjType		objtype;
	bool		bool_value;
	char	   *str_value;
	ObjTree	   *obj_value;
	List	   *array_value;
	slist_node	node;
} ObjElem;

static ObjElem *new_null_object(char *name);
static ObjElem *new_bool_object(char *name, bool value);
static ObjElem *new_string_object(char *name, char *value);
static ObjElem *new_object_object(char *name, ObjTree *value);
static ObjElem *new_array_object(char *name, List *array);
static void append_null_object(ObjTree *tree, char *name);
static void append_bool_object(ObjTree *tree, char *name, bool value);
static void append_string_object(ObjTree *tree, char *name, char *value);
static void append_object_object(ObjTree *tree, char *name, ObjTree *value);
static void append_array_object(ObjTree *tree, char *name, List *array);
static inline void append_premade_object(ObjTree *tree, ObjElem *elem);

/*
 * Allocate a new object tree to store parameter values.
 */
static ObjTree *
new_objtree(void)
{
	ObjTree    *params;

	params = palloc(sizeof(ObjTree));
	params->numParams = 0;
	slist_init(&params->params);

	return params;
}

/*
 * Allocate a new object tree to store parameter values -- varargs version.
 *
 * The "fmt" argument is used to append as a "fmt" element in the output blob.
 * numobjs indicates the number of extra elements to append; for each one,
 * a name, type and value must be supplied.  Note we don't have the luxury of
 * sprintf-like compiler warnings for malformed argument lists.
 */
static ObjTree *
new_objtree_VA(char *fmt, int numobjs,...)
{
	ObjTree    *tree;
	va_list		args;
	int			i;

	/* Set up the toplevel object and its "fmt" */
	tree = new_objtree();
	append_string_object(tree, "fmt", fmt);

	/* And process the given varargs */
	va_start(args, numobjs);
	for (i = 0; i < numobjs; i++)
	{
		ObjTree    *value;
		ObjType		type;
		ObjElem	   *elem;
		char	   *name;
		char	   *strval;
		bool		boolval;
		List	   *list;

		name = va_arg(args, char *);
		type = va_arg(args, ObjType);

		/* Null params don't have a value (obviously) */
		if (type == ObjTypeNull)
		{
			append_null_object(tree, name);
			continue;
		}

		/*
		 * For all other param types there must be a value in the varargs.
		 * Fetch it and add the fully formed subobject into the main object.
		 */
		switch (type)
		{
			case ObjTypeBool:
				boolval = va_arg(args, int);
				elem = new_bool_object(name, boolval);
				break;
			case ObjTypeString:
				strval = va_arg(args, char *);
				elem = new_string_object(name, strval);
				break;
			case ObjTypeObject:
				value = va_arg(args, ObjTree *);
				elem = new_object_object(name, value);
				break;
			case ObjTypeArray:
				list = va_arg(args, List *);
				elem = new_array_object(name, list);
				break;
			default:
				elog(ERROR, "invalid parameter type %d", type);
		}

		append_premade_object(tree, elem);
	}

	va_end(args);
	return tree;
}

/* Allocate a new parameter with a NULL value */
static ObjElem *
new_null_object(char *name)
{
	ObjElem    *param;

	param = palloc0(sizeof(ObjElem));

	param->name = name ? pstrdup(name) : NULL;
	param->objtype = ObjTypeNull;

	return param;
}

/* Append a NULL object to a tree */
static void
append_null_object(ObjTree *tree, char *name)
{
	ObjElem    *param;

	param = new_null_object(name);
	append_premade_object(tree, param);
}

/* Allocate a new boolean parameter */
static ObjElem *
new_bool_object(char *name, bool value)
{
	ObjElem    *param;

	param = palloc0(sizeof(ObjElem));

	param->name = name ? pstrdup(name) : NULL;
	param->objtype = ObjTypeBool;
	param->bool_value = value;

	return param;
}

/* Append a boolean parameter to a tree */
static void
append_bool_object(ObjTree *tree, char *name, bool value)
{
	ObjElem    *param;

	param = new_bool_object(name, value);
	append_premade_object(tree, param);
}

/* Allocate a new string object */
static ObjElem *
new_string_object(char *name, char *value)
{
	ObjElem    *param;

	param = palloc0(sizeof(ObjElem));

	param->name = name ? pstrdup(name) : NULL;
	param->objtype = ObjTypeString;
	param->str_value = value;	/* XXX not duped */

	return param;
}

/*
 * Append a string parameter to a tree.
 *
 * Note: we don't pstrdup the source string.  Caller must ensure the
 * source string lives long enough.
 */
static void
append_string_object(ObjTree *tree, char *name, char *value)
{
	ObjElem	   *param;

	param = new_string_object(name, value);
	append_premade_object(tree, param);
}

/* Allocate a new object parameter */
static ObjElem *
new_object_object(char *name, ObjTree *value)
{
	ObjElem    *param;

	param = palloc0(sizeof(ObjElem));

	param->name = name ? pstrdup(name) : NULL;
	param->objtype = ObjTypeObject;
	param->obj_value = value;	/* XXX not duped */

	return param;
}

/* Append an object parameter to a tree */
static void
append_object_object(ObjTree *tree, char *name, ObjTree *value)
{
	ObjElem    *param;

	param = new_object_object(name, value);
	append_premade_object(tree, param);
}

/* Allocate a new array parameter */
static ObjElem *
new_array_object(char *name, List *array)
{
	ObjElem    *param;

	param = palloc0(sizeof(ObjElem));

	param->name = name ? pstrdup(name) : NULL;
	param->objtype = ObjTypeArray;
	param->array_value = array; /* XXX not duped */

	return param;
}

/* Append an array parameter to a tree */
static void
append_array_object(ObjTree *tree, char *name, List *array)
{
	ObjElem    *param;

	param = new_array_object(name, array);
	append_premade_object(tree, param);
}

/* Append a preallocated parameter to a tree */
static inline void
append_premade_object(ObjTree *tree, ObjElem *elem)
{
	slist_push_head(&tree->params, &elem->node);
	tree->numParams++;
}

/*
 * Create a JSON blob from our ad-hoc representation.
 *
 * Note this leaks some memory; caller is responsible for later clean up.
 *
 * XXX this implementation will fail if there are more JSON objects in the tree
 * than the maximum number of columns in a heap tuple.  To fix we would first call
 * construct_md_array and then json_object.
 */
static char *
jsonize_objtree(ObjTree *tree)
{
	TupleDesc	tupdesc;
	Datum	   *values;
	bool	   *nulls;
	slist_iter	iter;
	int			i;
	HeapTuple	htup;
	Datum		json;
	char	   *jsonstr;

	tupdesc = CreateTemplateTupleDesc(tree->numParams, false);
	values = palloc(sizeof(Datum) * tree->numParams);
	nulls = palloc(sizeof(bool) * tree->numParams);

	i = 1;
	slist_foreach(iter, &tree->params)
	{
		ObjElem    *object = slist_container(ObjElem, node, iter.cur);
		Oid			typeid;

		switch (object->objtype)
		{
			case ObjTypeNull:
			case ObjTypeString:
				typeid = TEXTOID;
				break;
			case ObjTypeBool:
				typeid = BOOLOID;
				break;
			case ObjTypeArray:
			case ObjTypeObject:
				typeid = JSONOID;
				break;
			default:
				elog(ERROR, "unable to determine type id");
				typeid = InvalidOid;
		}

		TupleDescInitEntry(tupdesc, i, object->name, typeid, -1, 0);

		nulls[i - 1] = false;
		switch (object->objtype)
		{
			case ObjTypeNull:
				nulls[i - 1] = true;
				break;
			case ObjTypeBool:
				values[i - 1] = BoolGetDatum(object->bool_value);
				break;
			case ObjTypeString:
				values[i - 1] = CStringGetTextDatum(object->str_value);
				break;
			case ObjTypeArray:
				{
					ArrayType  *arrayt;
					Datum	   *arrvals;
					Datum		jsonary;
					ListCell   *cell;
					int			length = list_length(object->array_value);
					int			j;

					/*
					 * Arrays are stored as Lists up to this point, with each
					 * element being a ObjElem; we need to construct an
					 * ArrayType with them to turn the whole thing into a JSON
					 * array.
					 */
					j = 0;
					arrvals = palloc(sizeof(Datum) * length);
					foreach(cell, object->array_value)
					{
						ObjElem    *elem = lfirst(cell);

						switch (elem->objtype)
						{
							case ObjTypeString:
								arrvals[j] =
									/*
									 * XXX need quotes around the value.  This
									 * needs to be handled differently because
									 * it will fail for values of anything but
									 * trivial complexity.
									 */
									CStringGetTextDatum(psprintf("\"%s\"",
																 elem->str_value));
								break;
							case ObjTypeObject:
								arrvals[j] =
									CStringGetTextDatum(jsonize_objtree(elem->obj_value));
								break;
							default:
								/* not worth supporting other cases */
								elog(ERROR, "unsupported object type %d",
									 elem->objtype);
						}

						j++;

					}
					arrayt = construct_array(arrvals, length,
											 JSONOID, -1, false, 'i');

					jsonary = DirectFunctionCall1(array_to_json,
												  (PointerGetDatum(arrayt)));

					values[i - 1] = jsonary;
				}
				break;
			case ObjTypeObject:
				values[i - 1] =
					CStringGetTextDatum(jsonize_objtree(object->obj_value));
				break;
		}

		i++;
	}

	BlessTupleDesc(tupdesc);
	htup = heap_form_tuple(tupdesc, values, nulls);
	json = DirectFunctionCall1(row_to_json, HeapTupleGetDatum(htup));

	jsonstr = TextDatumGetCString(json);

	return jsonstr;
}

/*
 * Release all memory used by parameters and their expansion
 */
static void
free_objtree(ObjTree *tree)
{
	/* XXX nothing here */
}

/*
 * A helper routine to setup %{}T elements.
 */
static ObjTree *
new_objtree_for_type(Oid typeId, int32 typmod)
{
	ObjTree    *typeParam;
	Oid			typnspid;
	char	   *typnsp;
	char	   *typename;
	char	   *typmodstr;
	bool		is_array;

	format_type_detailed(typeId, typmod,
						 &typnspid, &typename, &typmodstr, &is_array);

	if (!OidIsValid(typnspid))
		typnsp = pstrdup("");
	else if (isAnyTempNamespace(typnspid))
		typnsp = pstrdup("pg_temp");
	else
		typnsp = get_namespace_name(typnspid);

	/*
	 * XXX We need this kludge to support types whose typmods include extra
	 * verbiage after the parenthised value.  Really, this only applies to
	 * timestamp and timestamptz, whose typmod take the form "(N) with[out]
	 * time zone", which causes a syntax error with schema-qualified names
	 * extracted from pg_type (as opposed to specialized type names defined by
	 * the SQL standard).
	 */
	if (typmodstr)
	{
		char	*clpar;

		clpar = strchr(typmodstr, ')');
		if (clpar)
			*(clpar + 1) = '\0';
	}

	/* We don't use new_objtree_VA here because types don't have a "fmt" */
	typeParam = new_objtree();
	append_string_object(typeParam, "schemaname", typnsp);
	append_string_object(typeParam, "typename", typename);
	append_string_object(typeParam, "typmod", typmodstr);
	append_bool_object(typeParam, "is_array", is_array);

	return typeParam;
}

/*
 * A helper routine to setup %{}D and %{}O elements
 *
 * Elements "schemaname" and "objname" are set.  If the namespace OID
 * corresponds to a temp schema, that's set to "pg_temp".
 *
 * The difference between those two element types is whether the objname will
 * be quoted as an identifier or not, which is not something that this routine
 * concerns itself with; that will be up to the expand function.
 */
static ObjTree *
new_objtree_for_qualname(Oid nspid, char *name)
{
	ObjTree    *qualified;
	char	   *namespace;

	/*
	 * We don't use new_objtree_VA here because these names don't have a "fmt"
	 */
	qualified = new_objtree();
	if (isAnyTempNamespace(nspid))
		namespace = pstrdup("pg_temp");
	else
		namespace = get_namespace_name(nspid);
	append_string_object(qualified, "schemaname", namespace);
	append_string_object(qualified, "objname", pstrdup(name));

	return qualified;
}

/*
 * A helper routine to setup %{}D and %{}O elements, with the object specified
 * by classId/objId
 *
 * Elements "schemaname" and "objname" are set.  If the object is a temporary
 * object, the schema name is set to "pg_temp".
 */
static ObjTree *
new_objtree_for_qualname_id(Oid classId, Oid objectId)
{
	ObjTree    *qualified;
	Relation	catalog;
	HeapTuple	catobj;
	Datum		objnsp;
	Datum		objname;
	AttrNumber	Anum_name;
	AttrNumber	Anum_namespace;
	bool		isnull;

	catalog = heap_open(classId, AccessShareLock);

	catobj = get_catalog_object_by_oid(catalog, objectId);
	if (!catobj)
		elog(ERROR, "cache lookup failed for object %u of catalog \"%s\"",
			 objectId, RelationGetRelationName(catalog));
	Anum_name = get_object_attnum_name(classId);
	Anum_namespace = get_object_attnum_namespace(classId);

	objnsp = heap_getattr(catobj, Anum_namespace, RelationGetDescr(catalog),
						  &isnull);
	if (isnull)
		elog(ERROR, "unexpected NULL namespace");
	objname = heap_getattr(catobj, Anum_name, RelationGetDescr(catalog),
						   &isnull);
	if (isnull)
		elog(ERROR, "unexpected NULL name");

	qualified = new_objtree_for_qualname(DatumGetObjectId(objnsp),
										 NameStr(*DatumGetName(objname)));

	pfree(catobj);
	heap_close(catalog, AccessShareLock);

	return qualified;
}

/*
 * Return the string representation of the given RELPERSISTENCE value
 */
static char *
get_persistence_str(char persistence)
{
	switch (persistence)
	{
		case RELPERSISTENCE_TEMP:
			return "TEMPORARY";
		case RELPERSISTENCE_UNLOGGED:
			return "UNLOGGED";
		case RELPERSISTENCE_PERMANENT:
			return "";
		default:
			return "???";
	}
}

static ObjTree *
deparse_DefineStmt_Aggregate(Oid objectId, DefineStmt *define)
{
	HeapTuple   aggTup;
	HeapTuple   procTup;
	ObjTree	   *stmt;
	ObjTree	   *tmp;
	List	   *list;
	Datum		initval;
	bool		isnull;
	Form_pg_aggregate agg;
	Form_pg_proc proc;

	aggTup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(objectId));
	if (!HeapTupleIsValid(aggTup))
		elog(ERROR, "cache lookup failed for aggregate with OID %u", objectId);
	agg = (Form_pg_aggregate) GETSTRUCT(aggTup);

	procTup = SearchSysCache1(PROCOID, ObjectIdGetDatum(agg->aggfnoid));
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "cache lookup failed for procedure with OID %u",
			 agg->aggfnoid);
	proc = (Form_pg_proc) GETSTRUCT(procTup);

	stmt = new_objtree_VA("CREATE AGGREGATE %{identity}D (%{types:, }s) "
						  "(%{elems:, }s)", 0);

	append_object_object(stmt, "identity",
						 new_objtree_for_qualname(proc->pronamespace,
												  NameStr(proc->proname)));

	list = NIL;

	/*
	 * An aggregate may have no arguments, in which case its signature
	 * is (*), to match count(*). If it's not an ordered-set aggregate,
	 * it may have a non-zero number of arguments. Otherwise it may have
	 * zero or more direct arguments and zero or more ordered arguments.
	 * There are no defaults or table parameters, and the only mode that
	 * we need to consider is VARIADIC.
	 */

	if (proc->pronargs == 0)
		list = lappend(list, new_object_object(NULL, new_objtree_VA("*", 0)));
	else
	{
		int			i;
		int			nargs;
		Oid		   *types;
		char	   *modes;
		char	  **names;
		int			insertorderbyat = -1;

		nargs = get_func_arg_info(procTup, &types, &names, &modes);

		if (AGGKIND_IS_ORDERED_SET(agg->aggkind))
			insertorderbyat = agg->aggnumdirectargs;

		for (i = 0; i < nargs; i++)
		{
			tmp = new_objtree_VA("%{order}s%{mode}s%{name}s%{type}T", 0);

			if (i == insertorderbyat)
				append_string_object(tmp, "order", "ORDER BY ");
			else
				append_string_object(tmp, "order", "");

			if (modes)
				append_string_object(tmp, "mode",
									 modes[i] == 'v' ? "VARIADIC " : "");
			else
				append_string_object(tmp, "mode", "");

			if (names)
				append_string_object(tmp, "name", names[i]);
			else
				append_string_object(tmp, "name", " ");

			append_object_object(tmp, "type",
								 new_objtree_for_type(types[i], -1));

			list = lappend(list, new_object_object(NULL, tmp));

			/*
			 * For variadic ordered-set aggregates, we have to repeat
			 * the last argument. This nasty hack is copied from
			 * print_function_arguments in ruleutils.c
			 */
			if (i == insertorderbyat && i == nargs-1)
				list = lappend(list, new_object_object(NULL, tmp));
		}
	}

	append_array_object(stmt, "types", list);

	list = NIL;

	tmp = new_objtree_VA("SFUNC=%{procedure}D", 0);
	append_object_object(tmp, "procedure",
						 new_objtree_for_qualname_id(ProcedureRelationId,
													 agg->aggtransfn));
	list = lappend(list, new_object_object(NULL, tmp));

	tmp = new_objtree_VA("STYPE=%{type}T", 0);
	append_object_object(tmp, "type",
						 new_objtree_for_type(agg->aggtranstype, -1));
	list = lappend(list, new_object_object(NULL, tmp));

	if (agg->aggtransspace != 0)
	{
		tmp = new_objtree_VA("SSPACE=%{space}s", 1,
							 "space", ObjTypeString,
							 psprintf("%d", agg->aggtransspace));
		list = lappend(list, new_object_object(NULL, tmp));
	}

	if (OidIsValid(agg->aggfinalfn))
	{
		tmp = new_objtree_VA("FINALFUNC=%{procedure}D", 0);
		append_object_object(tmp, "procedure",
							 new_objtree_for_qualname_id(ProcedureRelationId,
														 agg->aggfinalfn));
		list = lappend(list, new_object_object(NULL, tmp));
	}

	if (agg->aggfinalextra)
	{
		tmp = new_objtree_VA("FINALFUNC_EXTRA=true", 0);
		list = lappend(list, new_object_object(NULL, tmp));
	}

	initval = SysCacheGetAttr(AGGFNOID, aggTup,
							  Anum_pg_aggregate_agginitval,
							  &isnull);
	if (!isnull)
	{
		tmp = new_objtree_VA("INITCOND=%{initval}L",
							 1, "initval", ObjTypeString,
							 TextDatumGetCString(initval));
		list = lappend(list, new_object_object(NULL, tmp));
	}

	if (OidIsValid(agg->aggmtransfn))
	{
		tmp = new_objtree_VA("MSFUNC=%{procedure}D", 0);
		append_object_object(tmp, "procedure",
							 new_objtree_for_qualname_id(ProcedureRelationId,
														 agg->aggmtransfn));
		list = lappend(list, new_object_object(NULL, tmp));
	}

	if (OidIsValid(agg->aggmtranstype))
	{
		tmp = new_objtree_VA("MSTYPE=%{type}T", 0);
		append_object_object(tmp, "type",
							 new_objtree_for_type(agg->aggmtranstype, -1));
		list = lappend(list, new_object_object(NULL, tmp));
	}

	if (agg->aggmtransspace != 0)
	{
		tmp = new_objtree_VA("SSPACE=%{space}s", 1,
							 "space", ObjTypeString,
							 psprintf("%d", agg->aggmtransspace));
		list = lappend(list, new_object_object(NULL, tmp));
	}

	if (OidIsValid(agg->aggminvtransfn))
	{
		tmp = new_objtree_VA("MINVFUNC=%{procedure}D", 0);
		append_object_object(tmp, "procedure",
							 new_objtree_for_qualname_id(ProcedureRelationId,
														 agg->aggminvtransfn));
		list = lappend(list, new_object_object(NULL, tmp));
	}

	if (OidIsValid(agg->aggmfinalfn))
	{
		tmp = new_objtree_VA("MFINALFUNC=%{procedure}D", 0);
		append_object_object(tmp, "procedure",
							 new_objtree_for_qualname_id(ProcedureRelationId,
														 agg->aggmfinalfn));
		list = lappend(list, new_object_object(NULL, tmp));
	}

	if (agg->aggmfinalextra)
	{
		tmp = new_objtree_VA("MFINALFUNC_EXTRA=true", 0);
		list = lappend(list, new_object_object(NULL, tmp));
	}

	initval = SysCacheGetAttr(AGGFNOID, aggTup,
							  Anum_pg_aggregate_aggminitval,
							  &isnull);
	if (!isnull)
	{
		tmp = new_objtree_VA("MINITCOND=%{initval}L",
							 1, "initval", ObjTypeString,
							 TextDatumGetCString(initval));
		list = lappend(list, new_object_object(NULL, tmp));
	}

	if (agg->aggkind == AGGKIND_HYPOTHETICAL)
	{
		tmp = new_objtree_VA("HYPOTHETICAL=true", 0);
		list = lappend(list, new_object_object(NULL, tmp));
	}

	if (OidIsValid(agg->aggsortop))
	{
		Oid sortop = agg->aggsortop;
		Form_pg_operator op;
		HeapTuple tup;

		tup = SearchSysCache1(OPEROID, ObjectIdGetDatum(sortop));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for operator with OID %u", sortop);
		op = (Form_pg_operator) GETSTRUCT(tup);

		tmp = new_objtree_VA("SORTOP=%{operator}O", 0);
		append_object_object(tmp, "operator",
							 new_objtree_for_qualname(op->oprnamespace,
													  NameStr(op->oprname)));
		list = lappend(list, new_object_object(NULL, tmp));

		ReleaseSysCache(tup);
	}

	append_array_object(stmt, "elems", list);

	ReleaseSysCache(procTup);
	ReleaseSysCache(aggTup);

	return stmt;
}

static ObjTree *
deparse_DefineStmt_Collation(Oid objectId, DefineStmt *define)
{
	HeapTuple   colTup;
	ObjTree	   *stmt;
	Form_pg_collation colForm;

	colTup = SearchSysCache1(COLLOID, ObjectIdGetDatum(objectId));
	if (!HeapTupleIsValid(colTup))
		elog(ERROR, "cache lookup failed for collation with OID %u", objectId);
	colForm = (Form_pg_collation) GETSTRUCT(colTup);

	stmt = new_objtree_VA("CREATE COLLATION %{identity}O "
						  "(LC_COLLATE = %{collate}L,"
						  " LC_CTYPE = %{ctype}L)", 0);

	append_object_object(stmt, "identity",
						 new_objtree_for_qualname(colForm->collnamespace,
												  NameStr(colForm->collname)));
	append_string_object(stmt, "collate", NameStr(colForm->collcollate));
	append_string_object(stmt, "ctype", NameStr(colForm->collctype));

	ReleaseSysCache(colTup);

	return stmt;
}

static ObjTree *
deparse_DefineStmt_Operator(Oid objectId, DefineStmt *define)
{
	HeapTuple   oprTup;
	ObjTree	   *stmt;
	ObjTree	   *tmp;
	List	   *list;
	Form_pg_operator oprForm;

	oprTup = SearchSysCache1(OPEROID, ObjectIdGetDatum(objectId));
	if (!HeapTupleIsValid(oprTup))
		elog(ERROR, "cache lookup failed for operator with OID %u", objectId);
	oprForm = (Form_pg_operator) GETSTRUCT(oprTup);

	stmt = new_objtree_VA("CREATE OPERATOR %{identity}O (%{elems:, }s)", 0);

	append_object_object(stmt, "identity",
						 new_objtree_for_qualname(oprForm->oprnamespace,
												  NameStr(oprForm->oprname)));

	list = NIL;

	tmp = new_objtree_VA("PROCEDURE=%{procedure}D", 0);
	append_object_object(tmp, "procedure",
						 new_objtree_for_qualname_id(ProcedureRelationId,
													 oprForm->oprcode));
	list = lappend(list, new_object_object(NULL, tmp));

	if (OidIsValid(oprForm->oprleft))
	{
		tmp = new_objtree_VA("LEFTARG=%{type}T", 0);
		append_object_object(tmp, "type",
							 new_objtree_for_type(oprForm->oprleft, -1));
		list = lappend(list, new_object_object(NULL, tmp));
	}

	if (OidIsValid(oprForm->oprright))
	{
		tmp = new_objtree_VA("RIGHTARG=%{type}T", 0);
		append_object_object(tmp, "type",
							 new_objtree_for_type(oprForm->oprright, -1));
		list = lappend(list, new_object_object(NULL, tmp));
	}

	if (OidIsValid(oprForm->oprcom))
	{
		tmp = new_objtree_VA("COMMUTATOR=%{oper}O", 0);
		append_object_object(tmp, "oper",
							 new_objtree_for_qualname_id(OperatorRelationId,
														 oprForm->oprcom));
		list = lappend(list, new_object_object(NULL, tmp));
	}

	if (OidIsValid(oprForm->oprnegate))
	{
		tmp = new_objtree_VA("NEGATOR=%{oper}O", 0);
		append_object_object(tmp, "oper",
							 new_objtree_for_qualname_id(OperatorRelationId,
														 oprForm->oprnegate));
		list = lappend(list, new_object_object(NULL, tmp));
	}

	if (OidIsValid(oprForm->oprrest))
	{
		tmp = new_objtree_VA("RESTRICT=%{procedure}D", 0);
		append_object_object(tmp, "procedure",
							 new_objtree_for_qualname_id(ProcedureRelationId,
														 oprForm->oprrest));
		list = lappend(list, new_object_object(NULL, tmp));
	}

	if (OidIsValid(oprForm->oprjoin))
	{
		tmp = new_objtree_VA("JOIN=%{procedure}D", 0);
		append_object_object(tmp, "procedure",
							 new_objtree_for_qualname_id(ProcedureRelationId,
														 oprForm->oprjoin));
		list = lappend(list, new_object_object(NULL, tmp));
	}

	if (oprForm->oprcanmerge)
		list = lappend(list, new_object_object(NULL,
											   new_objtree_VA("MERGES", 0)));
	if (oprForm->oprcanhash)
		list = lappend(list, new_object_object(NULL,
											   new_objtree_VA("HASHES", 0)));

	append_array_object(stmt, "elems", list);

	ReleaseSysCache(oprTup);

	return stmt;
}

static ObjTree *
deparse_DefineStmt_TSConfig(Oid objectId, DefineStmt *define)
{
	HeapTuple   tscTup;
	HeapTuple   tspTup;
	ObjTree	   *stmt;
	Form_pg_ts_config tscForm;
	Form_pg_ts_parser tspForm;

	tscTup = SearchSysCache1(TSCONFIGOID, ObjectIdGetDatum(objectId));
	if (!HeapTupleIsValid(tscTup))
		elog(ERROR, "cache lookup failed for text search configuration "
			 "with OID %u", objectId);
	tscForm = (Form_pg_ts_config) GETSTRUCT(tscTup);

	tspTup = SearchSysCache1(TSPARSEROID, ObjectIdGetDatum(tscForm->cfgparser));
	if (!HeapTupleIsValid(tspTup))
		elog(ERROR, "cache lookup failed for text search parser with OID %u",
			 tscForm->cfgparser);
	tspForm = (Form_pg_ts_parser) GETSTRUCT(tspTup);

	stmt = new_objtree_VA("CREATE TEXT SEARCH CONFIGURATION %{identity}D "
						  "(PARSER=%{parser}s)", 0);

	append_object_object(stmt, "identity",
						 new_objtree_for_qualname(tscForm->cfgnamespace,
												  NameStr(tscForm->cfgname)));
	append_object_object(stmt, "parser",
						 new_objtree_for_qualname(tspForm->prsnamespace,
												  NameStr(tspForm->prsname)));

	/*
	 * If this text search configuration was created by copying another
	 * one with "CREATE TEXT SEARCH CONFIGURATION x (COPY=y)", then y's
	 * PARSER selection is copied along with its mappings of tokens to
	 * dictionaries (created with ALTER … ADD MAPPING …).
	 *
	 * Unfortunately, there's no way to define these mappings in the
	 * CREATE command, so if they exist for the configuration we're
	 * deparsing, we must detect them and fail.
	 */

	{
		ScanKeyData skey;
		SysScanDesc scan;
		HeapTuple	tup;
		Relation	map;
		bool		has_mapping;

		map = heap_open(TSConfigMapRelationId, AccessShareLock);

		ScanKeyInit(&skey,
					Anum_pg_ts_config_map_mapcfg,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(objectId));

		scan = systable_beginscan(map, TSConfigMapIndexId, true,
								  NULL, 1, &skey);

		while (HeapTupleIsValid((tup = systable_getnext(scan))))
		{
			has_mapping = true;
			break;
		}

		systable_endscan(scan);
		heap_close(map, AccessShareLock);

		if (has_mapping)
			ereport(ERROR,
					(errmsg("can't recreate text search configuration with mappings")));
	}

	ReleaseSysCache(tspTup);
	ReleaseSysCache(tscTup);

	return stmt;
}

static ObjTree *
deparse_DefineStmt_TSParser(Oid objectId, DefineStmt *define)
{
	HeapTuple   tspTup;
	ObjTree	   *stmt;
	ObjTree	   *tmp;
	List	   *list;
	Form_pg_ts_parser tspForm;

	tspTup = SearchSysCache1(TSPARSEROID, ObjectIdGetDatum(objectId));
	if (!HeapTupleIsValid(tspTup))
		elog(ERROR, "cache lookup failed for text search parser with OID %u",
			 objectId);
	tspForm = (Form_pg_ts_parser) GETSTRUCT(tspTup);

	stmt = new_objtree_VA("CREATE TEXT SEARCH PARSER %{identity}D "
						  "(%{elems:, }s)", 0);

	append_object_object(stmt, "identity",
						 new_objtree_for_qualname(tspForm->prsnamespace,
												  NameStr(tspForm->prsname)));

	list = NIL;

	tmp = new_objtree_VA("START=%{procedure}D", 0);
	append_object_object(tmp, "procedure",
						 new_objtree_for_qualname_id(ProcedureRelationId,
													 tspForm->prsstart));
	list = lappend(list, new_object_object(NULL, tmp));

	tmp = new_objtree_VA("GETTOKEN=%{procedure}D", 0);
	append_object_object(tmp, "procedure",
						 new_objtree_for_qualname_id(ProcedureRelationId,
													 tspForm->prstoken));
	list = lappend(list, new_object_object(NULL, tmp));

	tmp = new_objtree_VA("END=%{procedure}D", 0);
	append_object_object(tmp, "procedure",
						 new_objtree_for_qualname_id(ProcedureRelationId,
													 tspForm->prsend));
	list = lappend(list, new_object_object(NULL, tmp));

	tmp = new_objtree_VA("LEXTYPES=%{procedure}D", 0);
	append_object_object(tmp, "procedure",
						 new_objtree_for_qualname_id(ProcedureRelationId,
													 tspForm->prslextype));
	list = lappend(list, new_object_object(NULL, tmp));

	if (OidIsValid(tspForm->prsheadline))
	{
		tmp = new_objtree_VA("HEADLINE=%{procedure}D", 0);
		append_object_object(tmp, "procedure",
							 new_objtree_for_qualname_id(ProcedureRelationId,
														 tspForm->prsheadline));
		list = lappend(list, new_object_object(NULL, tmp));
	}

	append_array_object(stmt, "elems", list);

	ReleaseSysCache(tspTup);

	return stmt;
}

static ObjTree *
deparse_DefineStmt_TSDictionary(Oid objectId, DefineStmt *define)
{
	HeapTuple   tsdTup;
	ObjTree	   *stmt;
	ObjTree	   *tmp;
	List	   *list;
	Datum		options;
	bool		isnull;
	Form_pg_ts_dict tsdForm;

	tsdTup = SearchSysCache1(TSDICTOID, ObjectIdGetDatum(objectId));
	if (!HeapTupleIsValid(tsdTup))
		elog(ERROR, "cache lookup failed for text search dictionary "
			 "with OID %u", objectId);
	tsdForm = (Form_pg_ts_dict) GETSTRUCT(tsdTup);

	stmt = new_objtree_VA("CREATE TEXT SEARCH DICTIONARY %{identity}D "
						  "(%{elems:, }s)", 0);

	append_object_object(stmt, "identity",
						 new_objtree_for_qualname(tsdForm->dictnamespace,
												  NameStr(tsdForm->dictname)));

	list = NIL;

	tmp = new_objtree_VA("TEMPLATE=%{template}D", 0);
	append_object_object(tmp, "template",
						 new_objtree_for_qualname_id(TSTemplateRelationId,
													 tsdForm->dicttemplate));
	list = lappend(list, new_object_object(NULL, tmp));

	options = SysCacheGetAttr(TSDICTOID, tsdTup,
							  Anum_pg_ts_dict_dictinitoption,
							  &isnull);
	if (!isnull)
	{
		tmp = new_objtree_VA("%{options}s", 0);
		append_string_object(tmp, "options", TextDatumGetCString(options));
		list = lappend(list, new_object_object(NULL, tmp));
	}

	append_array_object(stmt, "elems", list);

	ReleaseSysCache(tsdTup);

	return stmt;
}

static ObjTree *
deparse_DefineStmt_TSTemplate(Oid objectId, DefineStmt *define)
{
	HeapTuple   tstTup;
	ObjTree	   *stmt;
	ObjTree	   *tmp;
	List	   *list;
	Form_pg_ts_template tstForm;

	tstTup = SearchSysCache1(TSTEMPLATEOID, ObjectIdGetDatum(objectId));
	if (!HeapTupleIsValid(tstTup))
		elog(ERROR, "cache lookup failed for text search template with OID %u",
			 objectId);
	tstForm = (Form_pg_ts_template) GETSTRUCT(tstTup);

	stmt = new_objtree_VA("CREATE TEXT SEARCH TEMPLATE %{identity}D "
						  "(%{elems:, }s)", 0);

	append_object_object(stmt, "identity",
						 new_objtree_for_qualname(tstForm->tmplnamespace,
												  NameStr(tstForm->tmplname)));

	list = NIL;

	if (OidIsValid(tstForm->tmplinit))
	{
		tmp = new_objtree_VA("INIT=%{procedure}D", 0);
		append_object_object(tmp, "procedure",
							 new_objtree_for_qualname_id(ProcedureRelationId,
														 tstForm->tmplinit));
		list = lappend(list, new_object_object(NULL, tmp));
	}

	tmp = new_objtree_VA("LEXIZE=%{procedure}D", 0);
	append_object_object(tmp, "procedure",
						 new_objtree_for_qualname_id(ProcedureRelationId,
													 tstForm->tmpllexize));
	list = lappend(list, new_object_object(NULL, tmp));

	append_array_object(stmt, "elems", list);

	ReleaseSysCache(tstTup);

	return stmt;
}

static ObjTree *
deparse_DefineStmt_Type(Oid objectId, DefineStmt *define)
{
	HeapTuple   typTup;
	ObjTree	   *stmt;
	ObjTree	   *tmp;
	List	   *list;
	char	   *str;
	Datum		dflt;
	bool		isnull;
	Form_pg_type typForm;

	typTup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(objectId));
	if (!HeapTupleIsValid(typTup))
		elog(ERROR, "cache lookup failed for type with OID %u", objectId);
	typForm = (Form_pg_type) GETSTRUCT(typTup);

	/* Shortcut processing for shell types. */
	if (!typForm->typisdefined)
	{
		stmt = new_objtree_VA("CREATE TYPE %{identity}D", 0);
		append_object_object(stmt, "identity",
							 new_objtree_for_qualname(typForm->typnamespace,
													  NameStr(typForm->typname)));
		ReleaseSysCache(typTup);
		return stmt;
	}

	stmt = new_objtree_VA("CREATE TYPE %{identity}D (%{elems:, }s)", 0);

	append_object_object(stmt, "identity",
						 new_objtree_for_qualname(typForm->typnamespace,
												  NameStr(typForm->typname)));

	list = NIL;

	/* INPUT */
	tmp = new_objtree_VA("INPUT=%{procedure}D", 0);
	append_object_object(tmp, "procedure",
						 new_objtree_for_qualname_id(ProcedureRelationId,
													 typForm->typinput));
	list = lappend(list, new_object_object(NULL, tmp));

	/* OUTPUT */
	tmp = new_objtree_VA("OUTPUT=%{procedure}D", 0);
	append_object_object(tmp, "procedure",
						 new_objtree_for_qualname_id(ProcedureRelationId,
													 typForm->typoutput));
	list = lappend(list, new_object_object(NULL, tmp));

	/* RECEIVE */
	if (OidIsValid(typForm->typreceive))
	{
		tmp = new_objtree_VA("RECEIVE=%{procedure}D", 0);
		append_object_object(tmp, "procedure",
							 new_objtree_for_qualname_id(ProcedureRelationId,
														 typForm->typreceive));
		list = lappend(list, new_object_object(NULL, tmp));
	}

	/* SEND */
	if (OidIsValid(typForm->typsend))
	{
		tmp = new_objtree_VA("SEND=%{procedure}D", 0);
		append_object_object(tmp, "procedure",
							 new_objtree_for_qualname_id(ProcedureRelationId,
														 typForm->typsend));
		list = lappend(list, new_object_object(NULL, tmp));
	}

	/* TYPMOD_IN */
	if (OidIsValid(typForm->typmodin))
	{
		tmp = new_objtree_VA("TYPMOD_IN=%{procedure}D", 0);
		append_object_object(tmp, "procedure",
							 new_objtree_for_qualname_id(ProcedureRelationId,
														 typForm->typmodin));
		list = lappend(list, new_object_object(NULL, tmp));
	}

	/* TYPMOD_OUT */
	if (OidIsValid(typForm->typmodout))
	{
		tmp = new_objtree_VA("TYPMOD_OUT=%{procedure}D", 0);
		append_object_object(tmp, "procedure",
							 new_objtree_for_qualname_id(ProcedureRelationId,
														 typForm->typmodout));
		list = lappend(list, new_object_object(NULL, tmp));
	}

	/* ANALYZE */
	if (OidIsValid(typForm->typanalyze))
	{
		tmp = new_objtree_VA("ANALYZE=%{procedure}D", 0);
		append_object_object(tmp, "procedure",
							 new_objtree_for_qualname_id(ProcedureRelationId,
														 typForm->typanalyze));
		list = lappend(list, new_object_object(NULL, tmp));
	}

	/* INTERNALLENGTH */
	tmp = new_objtree_VA("INTERNALLENGTH=%{typlen}s", 0);
	if (typForm->typlen == -1)
		append_string_object(tmp, "typlen", "VARIABLE");
	else
		append_string_object(tmp, "typlen",
							 psprintf("%d", typForm->typlen));
	list = lappend(list, new_object_object(NULL, tmp));

	/* PASSEDBYVALUE */
	if (typForm->typbyval)
		list = lappend(list, new_object_object(NULL, new_objtree_VA("PASSEDBYVALUE", 0)));

	/* ALIGNMENT */
	tmp = new_objtree_VA("ALIGNMENT=%{align}s", 0);
	switch (typForm->typalign)
	{
		case 'd':
			str = "pg_catalog.float8";
			break;
		case 'i':
			str = "pg_catalog.int4";
			break;
		case 's':
			str = "pg_catalog.int2";
			break;
		case 'c':
			str = "pg_catalog.bpchar";
			break;
		default:
			elog(ERROR, "invalid alignment %c", typForm->typalign);
	}
	append_string_object(tmp, "align", str);
	list = lappend(list, new_object_object(NULL, tmp));

	tmp = new_objtree_VA("STORAGE=%{storage}s", 0);
	switch (typForm->typstorage)
	{
		case 'p':
			str = "plain";
			break;
		case 'e':
			str = "external";
			break;
		case 'x':
			str = "extended";
			break;
		case 'm':
			str = "main";
			break;
		default:
			elog(ERROR, "invalid storage specifier %c", typForm->typstorage);
	}
	append_string_object(tmp, "storage", str);
	list = lappend(list, new_object_object(NULL, tmp));

	/* CATEGORY */
	tmp = new_objtree_VA("CATEGORY=%{category}L", 0);
	append_string_object(tmp, "category",
						 psprintf("%c", typForm->typcategory));
	list = lappend(list, new_object_object(NULL, tmp));

	/* PREFERRED */
	if (typForm->typispreferred)
		list = lappend(list, new_object_object(NULL, new_objtree_VA("PREFERRED=true", 0)));

	/* DEFAULT */
	dflt = SysCacheGetAttr(TYPEOID, typTup,
						   Anum_pg_type_typdefault,
						   &isnull);
	if (!isnull)
	{
		tmp = new_objtree_VA("DEFAULT=%{default}L", 0);
		append_string_object(tmp, "default", TextDatumGetCString(dflt));
		list = lappend(list, new_object_object(NULL, tmp));
	}

	/* ELEMENT */
	if (OidIsValid(typForm->typelem))
	{
		tmp = new_objtree_VA("ELEMENT=%{elem}T", 0);
		append_object_object(tmp, "elem",
							 new_objtree_for_type(typForm->typelem, -1));
		list = lappend(list, new_object_object(NULL, tmp));
	}

	/* DELIMITER */
	tmp = new_objtree_VA("DELIMITER=%{delim}L", 0);
	append_string_object(tmp, "delim",
						 psprintf("%c", typForm->typdelim));
	list = lappend(list, new_object_object(NULL, tmp));

	/* COLLATABLE */
	if (OidIsValid(typForm->typcollation))
		list = lappend(list,
					   new_object_object(NULL,
										 new_objtree_VA("COLLATABLE=true", 0)));

	append_array_object(stmt, "elems", list);

	ReleaseSysCache(typTup);

	return stmt;
}

static char *
deparse_DefineStmt(Oid objectId, Node *parsetree)
{
	DefineStmt *define = (DefineStmt *) parsetree;
	ObjTree	   *defStmt;
	char	   *command;

	switch (define->kind)
	{
		case OBJECT_AGGREGATE:
			defStmt = deparse_DefineStmt_Aggregate(objectId, define);
			break;

		case OBJECT_COLLATION:
			defStmt = deparse_DefineStmt_Collation(objectId, define);
			break;

		case OBJECT_OPERATOR:
			defStmt = deparse_DefineStmt_Operator(objectId, define);
			break;

		case OBJECT_TSCONFIGURATION:
			defStmt = deparse_DefineStmt_TSConfig(objectId, define);
			break;

		case OBJECT_TSPARSER:
			defStmt = deparse_DefineStmt_TSParser(objectId, define);
			break;

		case OBJECT_TSDICTIONARY:
			defStmt = deparse_DefineStmt_TSDictionary(objectId, define);
			break;

		case OBJECT_TSTEMPLATE:
			defStmt = deparse_DefineStmt_TSTemplate(objectId, define);
			break;

		case OBJECT_TYPE:
			defStmt = deparse_DefineStmt_Type(objectId, define);
			break;

		default:
			elog(ERROR, "unsupported object kind");
			return NULL;
	}

	command = jsonize_objtree(defStmt);
	free_objtree(defStmt);

	return command;
}

/*
 * deparse_CreateExtensionStmt
 *		deparse a CreateExtensionStmt
 *
 * Given an extension OID and the parsetree that created it, return the JSON
 * blob representing the creation command.
 *
 * XXX the current representation makes the output command dependant on the
 * installed versions of the extension.  Is this a problem?
 */
static char *
deparse_CreateExtensionStmt(Oid objectId, Node *parsetree)
{
	CreateExtensionStmt *node = (CreateExtensionStmt *) parsetree;
	Relation    pg_extension;
	HeapTuple   extTup;
	Form_pg_extension extForm;
	ObjTree	   *extStmt;
	ObjTree	   *tmp;
	char	   *command;
	List	   *list;
	ListCell   *cell;

	pg_extension = heap_open(ExtensionRelationId, AccessShareLock);
	extTup = get_catalog_object_by_oid(pg_extension, objectId);
	if (!HeapTupleIsValid(extTup))
		elog(ERROR, "cache lookup failed for extension with OID %u",
			 objectId);
	extForm = (Form_pg_extension) GETSTRUCT(extTup);

	extStmt = new_objtree_VA("CREATE EXTENSION %{if_not_exists}s %{identity}I "
							 "%{options: }s",
							 1, "identity", ObjTypeString, node->extname);
	append_string_object(extStmt, "if_not_exists",
						 node->if_not_exists ? "IF NOT EXISTS" : "");
	list = NIL;
	foreach(cell, node->options)
	{
		DefElem *opt = (DefElem *) lfirst(cell);

		if (strcmp(opt->defname, "schema") == 0)
		{
			/* skip this one; we add one unconditionally below */
			continue;
		}
		else if (strcmp(opt->defname, "new_version") == 0)
		{
			tmp = new_objtree_VA("VERSION %{version}L", 2,
								 "type", ObjTypeString, "version",
								 "version", ObjTypeString, defGetString(opt));
			list = lappend(list, new_object_object(NULL, tmp));
		}
		else if (strcmp(opt->defname, "old_version") == 0)
		{
			tmp = new_objtree_VA("FROM %{version}L", 2,
								 "type", ObjTypeString, "from",
								 "version", ObjTypeString, defGetString(opt));
			list = lappend(list, new_object_object(NULL, tmp));
		}
		else
			elog(ERROR, "unsupported option %s", opt->defname);
	}

	tmp = new_objtree_VA("SCHEMA %{schema}I",
						 2, "type", ObjTypeString, "schema",
						 "schema", ObjTypeString,
						 get_namespace_name(extForm->extnamespace));
	list = lappend(list, new_object_object(NULL, tmp));

	append_array_object(extStmt, "options", list);

	heap_close(pg_extension, AccessShareLock);

	command = jsonize_objtree(extStmt);
	free_objtree(extStmt);

	return command;
}

static char *
deparse_AlterExtensionStmt(Oid objectId, Node *parsetree)
{
	AlterExtensionStmt *node = (AlterExtensionStmt *) parsetree;
	Relation    pg_extension;
	HeapTuple   extTup;
	Form_pg_extension extForm;
	ObjTree	   *stmt;
	char	   *command;
	char	   *version = NULL;
	ListCell   *cell;

	pg_extension = heap_open(ExtensionRelationId, AccessShareLock);
	extTup = get_catalog_object_by_oid(pg_extension, objectId);
	if (!HeapTupleIsValid(extTup))
		elog(ERROR, "cache lookup failed for extension with OID %u",
			 objectId);
	extForm = (Form_pg_extension) GETSTRUCT(extTup);

	stmt = new_objtree_VA("ALTER EXTENSION %{identity}I UPDATE%{to}s", 1,
						  "identity", ObjTypeString,
						  NameStr(extForm->extname));

	foreach(cell, node->options)
	{
		DefElem *opt = (DefElem *) lfirst(cell);

		if (strcmp(opt->defname, "new_version") == 0)
			version = defGetString(opt);
		else
			elog(ERROR, "unsupported option %s", opt->defname);
	}

	if (version)
		append_string_object(stmt, "to", psprintf(" TO '%s'", version));
	else
		append_string_object(stmt, "to", "");

	heap_close(pg_extension, AccessShareLock);

	command = jsonize_objtree(stmt);
	free_objtree(stmt);

	return command;
}

/*
 * deparse_ViewStmt
 *		deparse a ViewStmt
 *
 * Given a view OID and the parsetree that created it, return the JSON blob
 * representing the creation command.
 */
static char *
deparse_ViewStmt(Oid objectId, Node *parsetree)
{
	ObjTree    *viewStmt;
	ObjTree    *tmp;
	char	   *command;
	Relation	relation;

	relation = relation_open(objectId, AccessShareLock);

	viewStmt = new_objtree_VA("CREATE %{persistence}s VIEW %{identity}D AS %{query}s",
							  1, "persistence", ObjTypeString,
					  get_persistence_str(relation->rd_rel->relpersistence));

	tmp = new_objtree_for_qualname(relation->rd_rel->relnamespace,
								   RelationGetRelationName(relation));
	append_object_object(viewStmt, "identity", tmp);

	append_string_object(viewStmt, "query",
						 pg_get_viewdef_internal(objectId));

	command = jsonize_objtree(viewStmt);
	free_objtree(viewStmt);

	relation_close(relation, AccessShareLock);

	return command;
}

/*
 * deparse_CreateTrigStmt
 *		Deparse a CreateTrigStmt (CREATE TRIGGER)
 *
 * Given a trigger OID and the parsetree that created it, return the JSON blob
 * representing the creation command.
 */
static char *
deparse_CreateTrigStmt(Oid objectId, Node *parsetree)
{
	CreateTrigStmt *node = (CreateTrigStmt *) parsetree;
	Relation	pg_trigger;
	HeapTuple	trigTup;
	Form_pg_trigger trigForm;
	ObjTree	   *trigger;
	ObjTree	   *tmp;
	int			tgnargs;
	List	   *list;
	List	   *events;
	char	   *command;

	pg_trigger = heap_open(TriggerRelationId, AccessShareLock);

	trigTup = get_catalog_object_by_oid(pg_trigger, objectId);
	trigForm = (Form_pg_trigger) GETSTRUCT(trigTup);

	/*
	 * Some of the elements only make sense for CONSTRAINT TRIGGERs, but it
	 * seems simpler to use a single fmt string for both kinds of triggers.
	 */
	trigger =
		new_objtree_VA("CREATE %{constraint}s TRIGGER %{name}I %{time}s %{events: OR }s "
					   "ON %{relation}D %{from_table}s %{constraint_attrs: }s "
					   "FOR EACH %{for_each}s %{when}s EXECUTE PROCEDURE %{function}s",
					   2,
					   "name", ObjTypeString, node->trigname,
					   "constraint", ObjTypeString,
					   node->isconstraint ? "CONSTRAINT" : "");

	if (node->timing == TRIGGER_TYPE_BEFORE)
		append_string_object(trigger, "time", "BEFORE");
	else if (node->timing == TRIGGER_TYPE_AFTER)
		append_string_object(trigger, "time", "AFTER");
	else if (node->timing == TRIGGER_TYPE_INSTEAD)
		append_string_object(trigger, "time", "INSTEAD OF");
	else
		elog(ERROR, "unrecognized trigger timing value %d", node->timing);

	/*
	 * Decode the events that the trigger fires for.  The output is a list;
	 * in most cases it will just be a string with the even name, but when
	 * there's an UPDATE with a list of columns, we return a JSON object.
	 */
	events = NIL;
	if (node->events & TRIGGER_TYPE_INSERT)
		events = lappend(events, new_string_object(NULL, "INSERT"));
	if (node->events & TRIGGER_TYPE_DELETE)
		events = lappend(events, new_string_object(NULL, "DELETE"));
	if (node->events & TRIGGER_TYPE_TRUNCATE)
		events = lappend(events, new_string_object(NULL, "TRUNCATE"));
	if (node->events & TRIGGER_TYPE_UPDATE)
	{
		if (node->columns == NIL)
		{
			events = lappend(events, new_string_object(NULL, "UPDATE"));
		}
		else
		{
			ObjTree	   *update;
			ListCell   *cell;
			List	   *cols = NIL;

			/*
			 * Currently only UPDATE OF can be objects in the output JSON, but
			 * we add a "kind" element so that user code can distinguish
			 * possible future new event types.
			 */
			update = new_objtree_VA("UPDATE OF %{columns:, }I",
									1, "kind", ObjTypeString, "update_of");

			foreach(cell, node->columns)
			{
				char   *colname = strVal(lfirst(cell));

				cols = lappend(cols,
							   new_string_object(NULL, colname));
			}

			append_array_object(update, "columns", cols);

			events = lappend(events,
							 new_object_object(NULL, update));
		}
	}
	append_array_object(trigger, "events", events);

	tmp = new_objtree_for_qualname_id(RelationRelationId,
									  trigForm->tgrelid);
	append_object_object(trigger, "relation", tmp);

	tmp = new_objtree_VA("FROM %{relation}D", 0);
	if (trigForm->tgconstrrelid)
	{
		ObjTree	   *rel;

		rel = new_objtree_for_qualname_id(RelationRelationId,
										  trigForm->tgconstrrelid);
		append_object_object(tmp, "relation", rel);
	}
	else
		append_bool_object(tmp, "present", false);
	append_object_object(trigger, "from_table", tmp);

	list = NIL;
	if (node->deferrable)
		list = lappend(list,
					   new_string_object(NULL, "DEFERRABLE"));
	if (node->initdeferred)
		list = lappend(list,
					   new_string_object(NULL, "INITIALLY DEFERRED"));
	append_array_object(trigger, "constraint_attrs", list);

	append_string_object(trigger, "for_each",
						 node->row ? "ROW" : "STATEMENT");

	tmp = new_objtree_VA("WHEN %{clause}s", 0);
	if (node->whenClause)
	{
		Node	   *whenClause;
		Datum		value;
		bool		isnull;

		value = fastgetattr(trigTup, Anum_pg_trigger_tgqual,
							RelationGetDescr(pg_trigger), &isnull);
		if (isnull)
			elog(ERROR, "bogus NULL tgqual");

		whenClause = stringToNode(TextDatumGetCString(value));
		append_string_object(tmp, "clause",
							 pg_get_trigger_whenclause(trigForm,
													   whenClause,
													   false));
	}
	else
		append_bool_object(tmp, "present", false);
	append_object_object(trigger, "when", tmp);

	tmp = new_objtree_VA("%{funcname}D(%{args:, }L)",
						 1, "funcname", ObjTypeObject,
						 new_objtree_for_qualname_id(ProcedureRelationId,
													 trigForm->tgfoid));
	list = NIL;
	tgnargs = trigForm->tgnargs;
	if (tgnargs > 0)
	{
		bytea  *tgargs;
		char   *argstr;
		bool	isnull;
		int		findx;
		int		lentgargs;
		char   *p;

		tgargs = DatumGetByteaP(fastgetattr(trigTup,
											Anum_pg_trigger_tgargs,
											RelationGetDescr(pg_trigger),
											&isnull));
		if (isnull)
			elog(ERROR, "invalid NULL tgargs");
		argstr = (char *) VARDATA(tgargs);
		lentgargs = VARSIZE_ANY_EXHDR(tgargs);

		p = argstr;
		for (findx = 0; findx < tgnargs; findx++)
		{
			size_t	tlen;

			/* verify that the argument encoding is correct */
			tlen = strlen(p);
			if (p + tlen >= argstr + lentgargs)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid argument string (%s) for trigger \"%s\"",
								argstr, NameStr(trigForm->tgname))));

			list = lappend(list, new_string_object(NULL, p));

			p += tlen + 1;
		}
	}

	append_array_object(tmp, "args", list);		/* might be NIL */

	append_object_object(trigger, "function", tmp);

	heap_close(pg_trigger, AccessShareLock);

	command = jsonize_objtree(trigger);
	free_objtree(trigger);

	return command;
}

/*
 * deparse_ColumnDef
 *		Subroutine for CREATE TABLE deparsing
 *
 * Deparse a ColumnDef node within a regular (non typed) table creation.
 *
 * NOT NULL constraints in the column definition are emitted directly in the
 * column definition by this routine; other constraints must be emitted
 * elsewhere (the info in the parse node is incomplete anyway.)
 */
static ObjTree *
deparse_ColumnDef(Relation relation, List *dpcontext, bool composite,
				  ColumnDef *coldef)
{
	ObjTree    *column;
	ObjTree    *tmp;
	Oid			relid = RelationGetRelid(relation);
	HeapTuple	attrTup;
	Form_pg_attribute attrForm;
	Oid			typid;
	int32		typmod;
	Oid			typcollation;
	bool		saw_notnull;
	ListCell   *cell;

	/*
	 * Inherited columns without local definitions must not be emitted. XXX --
	 * maybe it is useful to have them with "present = false" or some such?
	 */
	if (!coldef->is_local)
		return NULL;

	attrTup = SearchSysCacheAttName(relid, coldef->colname);
	if (!HeapTupleIsValid(attrTup))
		elog(ERROR, "could not find cache entry for column \"%s\" of relation %u",
			 coldef->colname, relid);
	attrForm = (Form_pg_attribute) GETSTRUCT(attrTup);

	get_atttypetypmodcoll(relid, attrForm->attnum,
						  &typid, &typmod, &typcollation);

	/* Composite types use a slightly simpler format string */
	if (composite)
		column = new_objtree_VA("%{name}I %{coltype}T %{collation}s",
								3,
								"type", ObjTypeString, "column",
								"name", ObjTypeString, coldef->colname,
								"coltype", ObjTypeObject,
								new_objtree_for_type(typid, typmod));
	else
		column = new_objtree_VA("%{name}I %{coltype}T %{default}s %{not_null}s %{collation}s",
								3,
								"type", ObjTypeString, "column",
								"name", ObjTypeString, coldef->colname,
								"coltype", ObjTypeObject,
								new_objtree_for_type(typid, typmod));

	tmp = new_objtree_VA("COLLATE %{name}D", 0);
	if (OidIsValid(typcollation))
	{
		ObjTree *collname;

		collname = new_objtree_for_qualname_id(CollationRelationId,
											   typcollation);
		append_object_object(tmp, "name", collname);
	}
	else
		append_bool_object(tmp, "present", false);
	append_object_object(column, "collation", tmp);

	if (!composite)
	{
		/*
		 * Emit a NOT NULL declaration if necessary.  Note that we cannot trust
		 * pg_attribute.attnotnull here, because that bit is also set when
		 * primary keys are specified; and we must not emit a NOT NULL
		 * constraint in that case, unless explicitely specified.  Therefore,
		 * we scan the list of constraints attached to this column to determine
		 * whether we need to emit anything.
		 * (Fortunately, NOT NULL constraints cannot be table constraints.)
		 */
		saw_notnull = false;
		foreach(cell, coldef->constraints)
		{
			Constraint *constr = (Constraint *) lfirst(cell);

			if (constr->contype == CONSTR_NOTNULL)
				saw_notnull = true;
		}

		if (saw_notnull)
			append_string_object(column, "not_null", "NOT NULL");
		else
			append_string_object(column, "not_null", "");

		tmp = new_objtree_VA("DEFAULT %{default}s", 0);
		if (attrForm->atthasdef)
		{
			char *defstr;

			defstr = RelationGetColumnDefault(relation, attrForm->attnum,
											  dpcontext);

			append_string_object(tmp, "default", defstr);
		}
		else
			append_bool_object(tmp, "present", false);
		append_object_object(column, "default", tmp);
	}

	ReleaseSysCache(attrTup);

	return column;
}

/*
 * deparse_ColumnDef_Typed
 *		Subroutine for CREATE TABLE OF deparsing
 *
 * Deparse a ColumnDef node within a typed table creation.	This is simpler
 * than the regular case, because we don't have to emit the type declaration,
 * collation, or default.  Here we only return something if the column is being
 * declared NOT NULL.
 *
 * As in deparse_ColumnDef, any other constraint is processed elsewhere.
 *
 * FIXME --- actually, what about default values?
 */
static ObjTree *
deparse_ColumnDef_typed(Relation relation, List *dpcontext, ColumnDef *coldef)
{
	ObjTree    *column = NULL;
	Oid			relid = RelationGetRelid(relation);
	HeapTuple	attrTup;
	Form_pg_attribute attrForm;
	Oid			typid;
	int32		typmod;
	Oid			typcollation;
	bool		saw_notnull;
	ListCell   *cell;

	attrTup = SearchSysCacheAttName(relid, coldef->colname);
	if (!HeapTupleIsValid(attrTup))
		elog(ERROR, "could not find cache entry for column \"%s\" of relation %u",
			 coldef->colname, relid);
	attrForm = (Form_pg_attribute) GETSTRUCT(attrTup);

	get_atttypetypmodcoll(relid, attrForm->attnum,
						  &typid, &typmod, &typcollation);

	/*
	 * Search for a NOT NULL declaration.  As in deparse_ColumnDef, we rely on
	 * finding a constraint on the column rather than coldef->is_not_null.
	 */
	saw_notnull = false;
	foreach(cell, coldef->constraints)
	{
		Constraint *constr = (Constraint *) lfirst(cell);

		if (constr->contype == CONSTR_NOTNULL)
		{
			saw_notnull = true;
			break;
		}
	}

	if (saw_notnull)
		column = new_objtree_VA("%{name}I WITH OPTIONS NOT NULL", 2,
								"type", ObjTypeString, "column_notnull",
								"name", ObjTypeString, coldef->colname);

	ReleaseSysCache(attrTup);

	return column;
}

/*
 * deparseTableElements
 *		Subroutine for CREATE TABLE deparsing
 *
 * Deal with all the table elements (columns and constraints).
 *
 * Note we ignore constraints in the parse node here; they are extracted from
 * system catalogs instead.
 */
static List *
deparseTableElements(Relation relation, List *tableElements, List *dpcontext,
					 bool typed, bool composite)
{
	List	   *elements = NIL;
	ListCell   *lc;

	foreach(lc, tableElements)
	{
		Node	   *elt = (Node *) lfirst(lc);

		switch (nodeTag(elt))
		{
			case T_ColumnDef:
				{
					ObjTree	   *tree;

					tree = typed ?
						deparse_ColumnDef_typed(relation, dpcontext,
												(ColumnDef *) elt) :
						deparse_ColumnDef(relation, dpcontext,
										  composite, (ColumnDef *) elt);
					if (tree != NULL)
					{
						ObjElem    *column;

						column = new_object_object(NULL, tree);
						elements = lappend(elements, column);
					}
				}
				break;
			case T_Constraint:
				break;
			default:
				elog(ERROR, "invalid node type %d", nodeTag(elt));
		}
	}

	return elements;
}

/*
 * obtainConstraints
 *		Subroutine for CREATE TABLE/CREATE DOMAIN deparsing
 *
 * Given a table OID or domain OID, obtain its constraints and append them to
 * the given elements list.  The updated list is returned.
 *
 * This works for typed tables, regular tables, and domains.
 *
 * Note that CONSTRAINT_FOREIGN constraints are always ignored.
 */
static List *
obtainConstraints(List *elements, Oid relationId, Oid domainId)
{
	Relation	conRel;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;
	ObjTree    *tmp;

	/* only one may be valid */
	Assert(OidIsValid(relationId) ^ OidIsValid(domainId));

	/*
	 * scan pg_constraint to fetch all constraints linked to the given
	 * relation.
	 */
	conRel = heap_open(ConstraintRelationId, AccessShareLock);
	if (OidIsValid(relationId))
	{
		ScanKeyInit(&key,
					Anum_pg_constraint_conrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(relationId));
		scan = systable_beginscan(conRel, ConstraintRelidIndexId,
								  true, NULL, 1, &key);
	}
	else
	{
		Assert(OidIsValid(domainId));
		ScanKeyInit(&key,
					Anum_pg_constraint_contypid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(domainId));
		scan = systable_beginscan(conRel, ConstraintTypidIndexId,
								  true, NULL, 1, &key);
	}

	/*
	 * For each constraint, add a node to the list of table elements.  In
	 * these nodes we include not only the printable information ("fmt"), but
	 * also separate attributes to indicate the type of constraint, for
	 * automatic processing.
	 */
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_constraint constrForm;
		char	   *contype;

		constrForm = (Form_pg_constraint) GETSTRUCT(tuple);

		switch (constrForm->contype)
		{
			case CONSTRAINT_CHECK:
				contype = "check";
				break;
			case CONSTRAINT_FOREIGN:
				continue;	/* not here */
			case CONSTRAINT_PRIMARY:
				contype = "primary key";
				break;
			case CONSTRAINT_UNIQUE:
				contype = "unique";
				break;
			case CONSTRAINT_TRIGGER:
				contype = "trigger";
				break;
			case CONSTRAINT_EXCLUSION:
				contype = "exclusion";
				break;
			default:
				elog(ERROR, "unrecognized constraint type");
		}

		/*
		 * "type" and "contype" are not part of the printable output, but are
		 * useful to programmatically distinguish these from columns and among
		 * different constraint types.
		 *
		 * XXX it might be useful to also list the column names in a PK, etc.
		 */
		tmp = new_objtree_VA("CONSTRAINT %{name}I %{definition}s",
							 4,
							 "type", ObjTypeString, "constraint",
							 "contype", ObjTypeString, contype,
						 "name", ObjTypeString, NameStr(constrForm->conname),
							 "definition", ObjTypeString,
						  pg_get_constraintdef_string(HeapTupleGetOid(tuple),
													  false));
		elements = lappend(elements, new_object_object(NULL, tmp));
	}

	systable_endscan(scan);
	heap_close(conRel, AccessShareLock);

	return elements;
}

/*
 * deparse_CreateStmt
 *		Deparse a CreateStmt (CREATE TABLE)
 *
 * Given a table OID and the parsetree that created it, return the JSON blob
 * representing the creation command.
 */
static char *
deparse_CreateStmt(Oid objectId, Node *parsetree)
{
	CreateStmt *node = (CreateStmt *) parsetree;
	Relation	relation = relation_open(objectId, AccessShareLock);
	List	   *dpcontext;
	ObjTree    *createStmt;
	ObjTree    *tmp;
	List	   *list;
	ListCell   *cell;
	char	   *command;
	char	   *fmtstr;

	/*
	 * Typed tables use a slightly different format string: we must not put
	 * table_elements with parents directly in the fmt string, because if
	 * there are no options the parens must not be emitted; and also, typed
	 * tables do not allow for inheritance.
	 */
	if (node->ofTypename)
		fmtstr = "CREATE %{persistence}s TABLE %{if_not_exists}s %{identity}D "
			"OF %{of_type}T %{table_elements}s "
			"%{on_commit}s WITH (%{with:, }s) %{tablespace}s";
	else
		fmtstr = "CREATE %{persistence}s TABLE %{if_not_exists}s %{identity}D "
			"(%{table_elements:, }s) %{inherits}s "
			"%{on_commit}s WITH (%{with:, }s) %{tablespace}s";

	createStmt =
		new_objtree_VA(fmtstr, 1,
					   "persistence", ObjTypeString,
					   get_persistence_str(relation->rd_rel->relpersistence));

	tmp = new_objtree_for_qualname(relation->rd_rel->relnamespace,
								   RelationGetRelationName(relation));
	append_object_object(createStmt, "identity", tmp);

	append_string_object(createStmt, "if_not_exists",
						 node->if_not_exists ? "IF NOT EXISTS" : "");

	dpcontext = deparse_context_for(RelationGetRelationName(relation),
									objectId);

	if (node->ofTypename)
	{
		List	   *tableelts = NIL;

		/*
		 * We can't put table elements directly in the fmt string as an array
		 * surrounded by parens here, because an empty clause would cause a
		 * syntax error.  Therefore, we use an indirection element and set
		 * present=false when there are no elements.
		 */
		append_string_object(createStmt, "table_kind", "typed");

		tmp = new_objtree_for_type(relation->rd_rel->reloftype, -1);
		append_object_object(createStmt, "of_type", tmp);

		tableelts = deparseTableElements(relation, node->tableElts, dpcontext,
										 true,		/* typed table */
										 false);	/* not composite */
		tableelts = obtainConstraints(tableelts, objectId, InvalidOid);
		if (tableelts == NIL)
			tmp = new_objtree_VA("", 1,
								 "present", ObjTypeBool, false);
		else
			tmp = new_objtree_VA("(%{elements:, }s)", 1,
								 "elements", ObjTypeArray, tableelts);
		append_object_object(createStmt, "table_elements", tmp);
	}
	else
	{
		List	   *tableelts = NIL;

		/*
		 * There is no need to process LIKE clauses separately; they have
		 * already been transformed into columns and constraints.
		 */
		append_string_object(createStmt, "table_kind", "plain");

		/*
		 * Process table elements: column definitions and constraints.	Only
		 * the column definitions are obtained from the parse node itself.	To
		 * get constraints we rely on pg_constraint, because the parse node
		 * might be missing some things such as the name of the constraints.
		 */
		tableelts = deparseTableElements(relation, node->tableElts, dpcontext,
										 false,		/* not typed table */
										 false);	/* not composite */
		tableelts = obtainConstraints(tableelts, objectId, InvalidOid);

		append_array_object(createStmt, "table_elements", tableelts);

		/*
		 * Add inheritance specification.  We cannot simply scan the list of
		 * parents from the parser node, because that may lack the actual
		 * qualified names of the parent relations.  Rather than trying to
		 * re-resolve them from the information in the parse node, it seems
		 * more accurate and convenient to grab it from pg_inherits.
		 */
		tmp = new_objtree_VA("INHERITS (%{parents:, }D)", 0);
		if (list_length(node->inhRelations) > 0)
		{
			List	   *parents = NIL;
			Relation	inhRel;
			SysScanDesc scan;
			ScanKeyData key;
			HeapTuple	tuple;

			inhRel = heap_open(InheritsRelationId, RowExclusiveLock);

			ScanKeyInit(&key,
						Anum_pg_inherits_inhrelid,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(objectId));

			scan = systable_beginscan(inhRel, InheritsRelidSeqnoIndexId,
									  true, NULL, 1, &key);

			while (HeapTupleIsValid(tuple = systable_getnext(scan)))
			{
				ObjTree    *parent;
				Form_pg_inherits formInh = (Form_pg_inherits) GETSTRUCT(tuple);

				parent = new_objtree_for_qualname_id(RelationRelationId,
													 formInh->inhparent);
				parents = lappend(parents, new_object_object(NULL, parent));
			}

			systable_endscan(scan);
			heap_close(inhRel, RowExclusiveLock);

			append_array_object(tmp, "parents", parents);
		}
		else
		{
			append_null_object(tmp, "parents");
			append_bool_object(tmp, "present", false);
		}
		append_object_object(createStmt, "inherits", tmp);
	}

	tmp = new_objtree_VA("TABLESPACE %{tablespace}I", 0);
	if (node->tablespacename)
		append_string_object(tmp, "tablespace", node->tablespacename);
	else
	{
		append_null_object(tmp, "tablespace");
		append_bool_object(tmp, "present", false);
	}
	append_object_object(createStmt, "tablespace", tmp);

	tmp = new_objtree_VA("ON COMMIT %{on_commit_value}s", 0);
	switch (node->oncommit)
	{
		case ONCOMMIT_DROP:
			append_string_object(tmp, "on_commit_value", "DROP");
			break;

		case ONCOMMIT_DELETE_ROWS:
			append_string_object(tmp, "on_commit_value", "DELETE ROWS");
			break;

		case ONCOMMIT_PRESERVE_ROWS:
			append_string_object(tmp, "on_commit_value", "PRESERVE ROWS");
			break;

		case ONCOMMIT_NOOP:
			append_null_object(tmp, "on_commit_value");
			append_bool_object(tmp, "present", false);
			break;
	}
	append_object_object(createStmt, "on_commit", tmp);

	/*
	 * WITH clause.  We always emit one, containing at least the OIDS option.
	 * That way we don't depend on the default value for default_with_oids.
	 * We can skip emitting other options if there don't appear in the parse
	 * node.
	 */
	tmp = new_objtree_VA("oids=%{value}s", 2,
						 "option", ObjTypeString, "oids",
						 "value", ObjTypeString,
						 relation->rd_rel->relhasoids ? "ON" : "OFF");
	list = list_make1(new_object_object(NULL, tmp));
	foreach(cell, node->options)
	{
		DefElem	*opt = (DefElem *) lfirst(cell);
		char   *fmt;
		char   *value;

		/* already handled above */
		if (strcmp(opt->defname, "oids") == 0)
			continue;

		if (opt->defnamespace)
			fmt = psprintf("%s.%s=%%{value}s", opt->defnamespace, opt->defname);
		else
			fmt = psprintf("%s=%%{value}s", opt->defname);
		value = opt->arg ? defGetString(opt) :
			defGetBoolean(opt) ? "TRUE" : "FALSE";
		tmp = new_objtree_VA(fmt, 2,
							 "option", ObjTypeString, opt->defname,
							 "value", ObjTypeString, value);
		list = lappend(list, new_object_object(NULL, tmp));
	}
	append_array_object(createStmt, "with", list);

	command = jsonize_objtree(createStmt);

	free_objtree(createStmt);
	relation_close(relation, AccessShareLock);

	return command;
}

static char *
deparse_CompositeTypeStmt(Oid objectId, Node *parsetree)
{
	CompositeTypeStmt *node = (CompositeTypeStmt *) parsetree;
	ObjTree	   *composite;
	Relation	typerel = relation_open(objectId, AccessShareLock);
	List	   *dpcontext;
	List	   *tableelts = NIL;
	char	   *command;

	dpcontext = deparse_context_for(RelationGetRelationName(typerel),
									objectId);

	composite = new_objtree_VA("CREATE TYPE %{identity}D AS (%{columns:, }s)",
							   0);
	append_object_object(composite, "identity",
						 new_objtree_for_qualname_id(RelationRelationId,
													 objectId));

	tableelts = deparseTableElements(typerel, node->coldeflist, dpcontext,
									 false,		/* not typed */
									 true);		/* composite type */

	append_array_object(composite, "columns", tableelts);

	command = jsonize_objtree(composite);
	free_objtree(composite);
	heap_close(typerel, AccessShareLock);

	return command;
}

static char *
deparse_CreateEnumStmt(Oid objectId, Node *parsetree)
{
	CreateEnumStmt *node = (CreateEnumStmt *) parsetree;
	ObjTree	   *enumtype;
	char	   *command;
	List	   *values;
	ListCell   *cell;

	enumtype = new_objtree_VA("CREATE TYPE %{identity}D AS ENUM (%{values:, }L)",
							  0);
	append_object_object(enumtype, "identity",
						 new_objtree_for_qualname_id(TypeRelationId,
													 objectId));
	values = NIL;
	foreach(cell, node->vals)
	{
		Value   *val = (Value *) lfirst(cell);

		values = lappend(values, new_string_object(NULL, strVal(val)));
	}
	append_array_object(enumtype, "values", values);

	command = jsonize_objtree(enumtype);
	free_objtree(enumtype);

	return command;
}

static char *
deparse_CreateRangeStmt(Oid objectId, Node *parsetree)
{
	ObjTree	   *range;
	ObjTree	   *tmp;
	List	   *definition = NIL;
	Relation	pg_range;
	HeapTuple	rangeTup;
	Form_pg_range rangeForm;
	ScanKeyData key[1];
	SysScanDesc scan;
	char	   *command;

	pg_range = heap_open(RangeRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_range_rngtypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objectId));

	scan = systable_beginscan(pg_range, RangeTypidIndexId, true,
							  NULL, 1, key);

	rangeTup = systable_getnext(scan);
	if (!HeapTupleIsValid(rangeTup))
		elog(ERROR, "cache lookup failed for range with type oid %u",
			 objectId);

	rangeForm = (Form_pg_range) GETSTRUCT(rangeTup);

	range = new_objtree_VA("CREATE TYPE %{identity}D AS RANGE (%{definition:, }s)", 0);
	tmp = new_objtree_for_qualname_id(TypeRelationId, objectId);
	append_object_object(range, "identity", tmp);

	/* SUBTYPE */
	tmp = new_objtree_for_qualname_id(TypeRelationId,
									  rangeForm->rngsubtype);
	tmp = new_objtree_VA("SUBTYPE = %{type}D",
						 2,
						 "clause", ObjTypeString, "subtype",
						 "type", ObjTypeObject, tmp);
	definition = lappend(definition, new_object_object(NULL, tmp));

	/* SUBTYPE_OPCLASS */
	if (OidIsValid(rangeForm->rngsubopc))
	{
		tmp = new_objtree_for_qualname_id(OperatorClassRelationId,
										  rangeForm->rngsubopc);
		tmp = new_objtree_VA("SUBTYPE_OPCLASS = %{opclass}D",
							 2,
							 "clause", ObjTypeString, "opclass",
							 "opclass", ObjTypeObject, tmp);
		definition = lappend(definition, new_object_object(NULL, tmp));
	}

	/* COLLATION */
	if (OidIsValid(rangeForm->rngcollation))
	{
		tmp = new_objtree_for_qualname_id(CollationRelationId,
										  rangeForm->rngcollation);
		tmp = new_objtree_VA("COLLATION = %{collation}D",
							 2,
							 "clause", ObjTypeString, "collation",
							 "collation", ObjTypeObject, tmp);
		definition = lappend(definition, new_object_object(NULL, tmp));
	}

	/* CANONICAL */
	if (OidIsValid(rangeForm->rngcanonical))
	{
		tmp = new_objtree_for_qualname_id(ProcedureRelationId,
										  rangeForm->rngcanonical);
		tmp = new_objtree_VA("CANONICAL = %{canonical}D",
							 2,
							 "clause", ObjTypeString, "canonical",
							 "canonical", ObjTypeObject, tmp);
		definition = lappend(definition, new_object_object(NULL, tmp));
	}

	/* SUBTYPE_DIFF */
	if (OidIsValid(rangeForm->rngsubdiff))
	{
		tmp = new_objtree_for_qualname_id(ProcedureRelationId,
										  rangeForm->rngsubdiff);
		tmp = new_objtree_VA("SUBTYPE_DIFF = %{subtype_diff}D",
							 2,
							 "clause", ObjTypeString, "subtype_diff",
							 "subtype_diff", ObjTypeObject, tmp);
		definition = lappend(definition, new_object_object(NULL, tmp));
	}

	append_array_object(range, "definition", definition);

	systable_endscan(scan);
	heap_close(pg_range, RowExclusiveLock);

	command = jsonize_objtree(range);
	free_objtree(range);

	return command;
}

static char *
deparse_CreateDomain(Oid objectId, Node *parsetree)
{
	ObjTree	   *createDomain;
	ObjTree	   *tmp;
	char	   *command;
	HeapTuple	typTup;
	Form_pg_type typForm;
	List	   *constraints;

	typTup = SearchSysCache1(TYPEOID,
							 objectId);
	if (!HeapTupleIsValid(typTup))
		elog(ERROR, "cache lookup failed for domain with OID %u", objectId);
	typForm = (Form_pg_type) GETSTRUCT(typTup);

	createDomain = new_objtree_VA("CREATE DOMAIN %{identity}D AS %{type}D %{not_null}s %{constraints}s %{collation}s",
								  0);

	append_object_object(createDomain,
						 "identity",
						 new_objtree_for_qualname_id(TypeRelationId,
													 objectId));
	append_object_object(createDomain,
						 "type",
						 new_objtree_for_qualname_id(TypeRelationId,
													 typForm->typbasetype));

	if (typForm->typnotnull)
		append_string_object(createDomain, "not_null", "NOT NULL");
	else
		append_string_object(createDomain, "not_null", "");

	constraints = obtainConstraints(NIL, InvalidOid, objectId);
	tmp = new_objtree_VA("%{elements: }s", 0);
	if (constraints == NIL)
		append_bool_object(tmp, "present", false);
	else
		append_array_object(tmp, "elements", constraints);
	append_object_object(createDomain, "constraints", tmp);

	tmp = new_objtree_VA("COLLATE %{collation}D", 0);
	if (OidIsValid(typForm->typcollation))
		append_object_object(tmp, "collation",
							 new_objtree_for_qualname_id(CollationRelationId,
														 typForm->typcollation));
	else
		append_bool_object(tmp, "present", false);
	append_object_object(createDomain, "collation", tmp);

	ReleaseSysCache(typTup);
	command = jsonize_objtree(createDomain);
	free_objtree(createDomain);

	return command;
}

/*
 * deparse_CreateFunctionStmt
 *		Deparse a CreateFunctionStmt (CREATE FUNCTION)
 *
 * Given a function OID and the parsetree that created it, return the JSON
 * blob representing the creation command.
 *
 * XXX this is missing the per-function custom-GUC thing.
 */
static char *
deparse_CreateFunction(Oid objectId, Node *parsetree)
{
	CreateFunctionStmt *node = (CreateFunctionStmt *) parsetree;
	ObjTree	   *createFunc;
	ObjTree	   *sign;
	ObjTree	   *tmp;
	Datum		tmpdatum;
	char	   *fmt;
	char	   *definition;
	char	   *command;
	char	   *source;
	char	   *probin;
	List	   *params;
	List	   *defaults;
	ListCell   *cell;
	ListCell   *curdef;
	ListCell   *table_params = NULL;
	HeapTuple	procTup;
	Form_pg_proc procForm;
	HeapTuple	langTup;
	Oid		   *typarray;
	Form_pg_language langForm;
	int			i;
	int			typnum;
	bool		isnull;

	/* get the pg_proc tuple */
	procTup = SearchSysCache1(PROCOID, objectId);
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "cache lookup failure for function with OID %u",
			 objectId);
	procForm = (Form_pg_proc) GETSTRUCT(procTup);

	/* get the corresponding pg_language tuple */
	langTup = SearchSysCache1(LANGOID, procForm->prolang);
	if (!HeapTupleIsValid(langTup))
		elog(ERROR, "cache lookup failure for language with OID %u",
			 procForm->prolang);
	langForm = (Form_pg_language) GETSTRUCT(langTup);

	/*
	 * Determine useful values for prosrc and probin.  We cope with probin
	 * being either NULL or "-", but prosrc must have a valid value.
	 */
	tmpdatum = SysCacheGetAttr(PROCOID, procTup,
							   Anum_pg_proc_prosrc, &isnull);
	if (isnull)
		elog(ERROR, "null prosrc in function with OID %u", objectId);
	source = TextDatumGetCString(tmpdatum);

	/* Determine a useful value for probin */
	tmpdatum = SysCacheGetAttr(PROCOID, procTup,
							   Anum_pg_proc_probin, &isnull);
	if (isnull)
		probin = NULL;
	else
	{
		probin = TextDatumGetCString(tmpdatum);
		if (probin[0] == '\0' || strcmp(probin, "-") == 0)
		{
			pfree(probin);
			probin = NULL;
		}
	}

	if (probin == NULL)
		definition = "%{definition}L";
	else
		definition = "%{objfile}L, %{symbol}L";

	fmt = psprintf("CREATE %%{or_replace}s FUNCTION %%{signature}s "
				   "RETURNS %%{return_type}s LANGUAGE %%{language}I "
				   "%%{window}s %%{volatility}s %%{leakproof}s "
				   "%%{strict}s %%{security_definer}s %%{cost}s %%{rows}s "
				   "%%{set_options: }s "
				   "AS %s", definition);

	createFunc = new_objtree_VA(fmt, 1,
								"or_replace", ObjTypeString,
								node->replace ? "OR REPLACE" : "");

	sign = new_objtree_VA("%{identity}D(%{arguments:, }s)", 0);

	/*
	 * To construct the arguments array, extract the type OIDs from the
	 * function's pg_proc entry.  If pronargs equals the parameter list length,
	 * there are no OUT parameters and thus we can extract the type OID from
	 * proargtypes; otherwise we need to decode proallargtypes, which is
	 * a bit more involved.
	 */
	typarray = palloc(list_length(node->parameters) * sizeof(Oid));
	if (list_length(node->parameters) > procForm->pronargs)
	{
		bool	isnull;
		Datum	alltypes;
		Datum  *values;
		bool   *nulls;
		int		nelems;

		alltypes = SysCacheGetAttr(PROCOID, procTup,
								   Anum_pg_proc_proallargtypes, &isnull);
		if (isnull)
			elog(ERROR, "NULL proallargtypes, but more parameters than args");
		deconstruct_array(DatumGetArrayTypeP(alltypes),
						  OIDOID, 4, 't', 'i',
						  &values, &nulls, &nelems);
		if (nelems != list_length(node->parameters))
			elog(ERROR, "mismatched proallargatypes");
		for (i = 0; i < list_length(node->parameters); i++)
			typarray[i] = values[i];
	}
	else
	{
		for (i = 0; i < list_length(node->parameters); i++)
			 typarray[i] = procForm->proargtypes.values[i];
	}

	/*
	 * If there are any default expressions, we read the deparsed expression as
	 * a list so that we can attach them to each argument.
	 */
	tmpdatum = SysCacheGetAttr(PROCOID, procTup,
							   Anum_pg_proc_proargdefaults, &isnull);
	if (!isnull)
	{
		defaults = FunctionGetDefaults(DatumGetTextP(tmpdatum));
		curdef = list_head(defaults);
	}
	else
	{
		defaults = NIL;
		curdef = NULL;
	}

	/*
	 * Now iterate over each parameter in the parsetree to create the
	 * parameters array.
	 */
	params = NIL;
	typnum = 0;
	foreach(cell, node->parameters)
	{
		FunctionParameter *param = (FunctionParameter *) lfirst(cell);
		ObjTree	   *tmp2;
		ObjTree	   *tmp3;

		/*
		 * A PARAM_TABLE parameter indicates end of input arguments; the
		 * following parameters are part of the return type.  We ignore them
		 * here, but keep track of the current position in the list so that
		 * we can easily produce the return type below.
		 */
		if (param->mode == FUNC_PARAM_TABLE)
		{
			table_params = cell;
			break;
		}

		/*
		 * Note that %{name}s is a string here, not an identifier; the reason
		 * for this is that an absent parameter name must produce an empty
		 * string, not "", which is what would happen if we were to use
		 * %{name}I here.  So we add another level of indirection to allow us
		 * to inject a "present" parameter.
		 */
		tmp2 = new_objtree_VA("%{mode}s %{name}s %{type}T %{default}s", 0);
		append_string_object(tmp2, "mode",
							 param->mode == FUNC_PARAM_IN ? "IN" :
							 param->mode == FUNC_PARAM_OUT ? "OUT" :
							 param->mode == FUNC_PARAM_INOUT ? "INOUT" :
							 param->mode == FUNC_PARAM_VARIADIC ? "VARIADIC" :
							 "INVALID MODE");

		/* optional wholesale suppression of "name" occurs here */
		append_object_object(tmp2, "name",
							 new_objtree_VA("%{name}I", 2,
											"name", ObjTypeString,
											param->name ? param->name : "NULL",
											"present", ObjTypeBool,
											param->name ? true : false));

		tmp3 = new_objtree_VA("DEFAULT %{value}s", 0);
		if (PointerIsValid(param->defexpr))
		{
			char *expr;

			if (curdef == NULL)
				elog(ERROR, "proargdefaults list too short");
			expr = lfirst(curdef);

			append_string_object(tmp3, "value", expr);
			curdef = lnext(curdef);
		}
		else
			append_bool_object(tmp3, "present", false);
		append_object_object(tmp2, "default", tmp3);

		append_object_object(tmp2, "type",
							 new_objtree_for_type(typarray[typnum++], -1));

		params = lappend(params,
						 new_object_object(NULL, tmp2));
	}
	append_array_object(sign, "arguments", params);
	append_object_object(sign, "identity",
						 new_objtree_for_qualname_id(ProcedureRelationId,
													 objectId));
	append_object_object(createFunc, "signature", sign);

	/*
	 * A return type can adopt one of two forms: either a [SETOF] some_type, or
	 * a TABLE(list-of-types).  We can tell the second form because we saw a
	 * table param above while scanning the argument list.
	 */
	if (table_params == NULL)
	{
		tmp = new_objtree_VA("%{setof}s %{rettype}T", 0);
		append_string_object(tmp, "setof",
							 procForm->proretset ? "SETOF" : "");
		append_object_object(tmp, "rettype",
							 new_objtree_for_type(procForm->prorettype, -1));
		append_string_object(tmp, "return_form", "plain");
	}
	else
	{
		List	   *rettypes = NIL;
		ObjTree	   *tmp2;

		tmp = new_objtree_VA("TABLE (%{rettypes:, }s)", 0);
		for (; table_params != NULL; table_params = lnext(table_params))
		{
			FunctionParameter *param = lfirst(table_params);

			tmp2 = new_objtree_VA("%{name}I %{type}T", 0);
			append_string_object(tmp2, "name", param->name);
			append_object_object(tmp2, "type",
								 new_objtree_for_type(typarray[typnum++], -1));
			rettypes = lappend(rettypes,
							   new_object_object(NULL, tmp2));
		}

		append_array_object(tmp, "rettypes", rettypes);
		append_string_object(tmp, "return_form", "table");
	}

	append_object_object(createFunc, "return_type", tmp);

	append_string_object(createFunc, "language",
						 NameStr(langForm->lanname));

	append_string_object(createFunc, "window",
						 procForm->proiswindow ? "WINDOW" : "");
	append_string_object(createFunc, "volatility",
						 procForm->provolatile == PROVOLATILE_VOLATILE ?
						 "VOLATILE" :
						 procForm->provolatile == PROVOLATILE_STABLE ?
						 "STABLE" :
						 procForm->provolatile == PROVOLATILE_IMMUTABLE ?
						 "IMMUTABLE" : "INVALID VOLATILITY");

	append_string_object(createFunc, "leakproof",
						 procForm->proleakproof ? "LEAKPROOF" : "");
	append_string_object(createFunc, "strict",
						 procForm->proisstrict ?
						 "RETURNS NULL ON NULL INPUT" :
						 "CALLED ON NULL INPUT");

	append_string_object(createFunc, "security_definer",
						 procForm->prosecdef ?
						 "SECURITY DEFINER" : "SECURITY INVOKER");

	append_object_object(createFunc, "cost",
						 new_objtree_VA("COST %{cost}s", 1,
										"cost", ObjTypeString,
										psprintf("%f", procForm->procost)));

	tmp = new_objtree_VA("ROWS %{rows}s", 0);
	if (procForm->prorows == 0)
		append_bool_object(tmp, "present", false);
	else
		append_string_object(tmp, "rows",
							 psprintf("%f", procForm->prorows));
	append_object_object(createFunc, "rows", tmp);

	append_array_object(createFunc, "set_options", NIL);

	if (probin == NULL)
	{
		append_string_object(createFunc, "definition",
							 source);
	}
	else
	{
		append_string_object(createFunc, "objfile", probin);
		append_string_object(createFunc, "symbol", source);
	}

	ReleaseSysCache(langTup);
	ReleaseSysCache(procTup);
	command = jsonize_objtree(createFunc);
	free_objtree(createFunc);

	return command;
}

/*
 * Return the given object type as a string.
 */
static const char *
stringify_objtype(ObjectType objtype)
{
	switch (objtype)
	{
		case OBJECT_AGGREGATE:
			return "AGGREGATE";
		case OBJECT_DOMAIN:
			return "DOMAIN";
		case OBJECT_COLLATION:
			return "COLLATION";
		case OBJECT_CONVERSION:
			return "CONVERSION";
		case OBJECT_FDW:
			return "FOREIGN DATA WRAPPER";
		case OBJECT_FOREIGN_SERVER:
			return "SERVER";
		case OBJECT_FOREIGN_TABLE:
			return "FOREIGN TABLE";
		case OBJECT_FUNCTION:
			return "FUNCTION";
		case OBJECT_INDEX:
			return "INDEX";
		case OBJECT_LANGUAGE:
			return "LANGUAGE";
		case OBJECT_LARGEOBJECT:
			return "LARGE OBJECT";
		case OBJECT_MATVIEW:
			return "MATERIALIZED VIEW";
		case OBJECT_OPERATOR:
			return "OPERATOR";
		case OBJECT_OPCLASS:
			return "OPERATOR CLASS";
		case OBJECT_OPFAMILY:
			return "OPERATOR FAMILY";
		case OBJECT_TABLE:
			return "TABLE";
		case OBJECT_TSCONFIGURATION:
			return "TEXT SEARCH CONFIGURATION";
		case OBJECT_TSDICTIONARY:
			return "TEXT SEARCH DICTIONARY";
		case OBJECT_TSPARSER:
			return "TEXT SEARCH PARSER";
		case OBJECT_TSTEMPLATE:
			return "TEXT SEARCH TEMPLATE";
		case OBJECT_TYPE:
			return "TYPE";
		case OBJECT_SCHEMA:
			return "SCHEMA";
		case OBJECT_SEQUENCE:
			return "SEQUENCE";
		case OBJECT_VIEW:
			return "VIEW";

		default:
			elog(ERROR, "unsupported objtype %d", objtype);
	}
}

static char *
deparse_RenameStmt(Oid objectId, Node *parsetree)
{
	RenameStmt *node = (RenameStmt *) parsetree;
	ObjTree	   *renameStmt;
	char	   *command;
	char	   *fmtstr;
	Relation	relation;
	Oid			schemaId;
	const char *subthing;

	/*
	 * FIXME --- this code is missing support for inheritance behavioral flags,
	 * i.e. the "*" and ONLY elements.
	 */

	/*
	 * In a ALTER .. RENAME command, we don't have the original name of the
	 * object in system catalogs: since we inspect them after the command has
	 * executed, the old name is already gone.  Therefore, we extract it from
	 * the parse node.  Note we still extract the schema name from the catalog
	 * (it might not be present in the parse node); it cannot possibly have
	 * changed anyway.
	 *
	 * XXX what if there's another event trigger running concurrently that
	 * renames the schema or moves the object to another schema?  Seems
	 * pretty far-fetched, but possible nonetheless.
	 */
	switch (node->renameType)
	{
		case OBJECT_TABLE:
		case OBJECT_SEQUENCE:
		case OBJECT_VIEW:
		case OBJECT_MATVIEW:
		case OBJECT_INDEX:
		case OBJECT_FOREIGN_TABLE:
			fmtstr = psprintf("ALTER %s %%{if_exists}s %%{identity}D RENAME TO %%{newname}I",
							  stringify_objtype(node->renameType));
			relation = relation_open(objectId, AccessShareLock);
			schemaId = RelationGetNamespace(relation);
			renameStmt = new_objtree_VA(fmtstr, 0);
			append_object_object(renameStmt, "identity",
								 new_objtree_for_qualname(schemaId,
														  node->relation->relname));
			append_string_object(renameStmt, "if_exists",
								 node->missing_ok ? "IF EXISTS" : "");
			relation_close(relation, AccessShareLock);
			break;

		case OBJECT_COLUMN:
		case OBJECT_ATTRIBUTE:
			relation = relation_open(objectId, AccessShareLock);
			schemaId = RelationGetNamespace(relation);

			if (node->renameType == OBJECT_COLUMN)
				subthing = "COLUMN";
			else
				subthing = "ATTRIBUTE";

			fmtstr = psprintf("ALTER %s %%{if_exists}s %%{identity}D RENAME %s %%{colname}I TO %%{newname}I",
							  stringify_objtype(node->relationType),
							  subthing);
			renameStmt = new_objtree_VA(fmtstr, 0);
			append_object_object(renameStmt, "identity",
								 new_objtree_for_qualname(schemaId,
														  node->relation->relname));
			append_string_object(renameStmt, "colname", node->subname);
			append_string_object(renameStmt, "if_exists",
								 node->missing_ok ? "IF EXISTS" : "");
			relation_close(relation, AccessShareLock);
			break;

		case OBJECT_SCHEMA:
		case OBJECT_FDW:
		case OBJECT_LANGUAGE:
		case OBJECT_FOREIGN_SERVER:
			fmtstr = psprintf("ALTER %s %%{identity}I RENAME TO %%{newname}I",
							  stringify_objtype(node->relationType));
			renameStmt = new_objtree_VA(fmtstr, 0);
			append_string_object(renameStmt, "identity",
								 node->subname);
			break;

		case OBJECT_COLLATION:
		case OBJECT_CONVERSION:
		case OBJECT_DOMAIN:
		case OBJECT_TSDICTIONARY:
		case OBJECT_TSPARSER:
		case OBJECT_TSTEMPLATE:
		case OBJECT_TSCONFIGURATION:
		case OBJECT_TYPE:
			{
				ObjTree    *ident;
				HeapTuple	objTup;
				Oid			catalogId;
				Relation	catalog;
				bool		isnull;
				AttrNumber	nspnum;

				catalogId = get_objtype_catalog_oid(node->renameType);
				catalog = heap_open(catalogId, AccessShareLock);
				objTup = get_catalog_object_by_oid(catalog, objectId);
				nspnum = get_object_attnum_namespace(catalogId);

				schemaId = DatumGetObjectId(heap_getattr(objTup,
														 nspnum,
														 RelationGetDescr(catalog),
														 &isnull));

				fmtstr = psprintf("ALTER %s %%{identity}D RENAME TO %%{newname}I",
								  stringify_objtype(node->renameType));
				renameStmt = new_objtree_VA(fmtstr, 0);
				ident = new_objtree_for_qualname(schemaId,
												 strVal(llast(node->object)));
				append_object_object(renameStmt, "identity", ident);
				relation_close(catalog, AccessShareLock);

			}
			break;

		case OBJECT_AGGREGATE:
		case OBJECT_FUNCTION:
			elog(ERROR, "renaming of functions and aggregates is not supported yet");

		case OBJECT_CONSTRAINT:
			{
				HeapTuple		conTup;
				Form_pg_constraint	constrForm;
				ObjTree		   *ident;

				conTup = SearchSysCache1(CONSTROID, objectId);
				constrForm = (Form_pg_constraint) GETSTRUCT(conTup);
				fmtstr = psprintf("ALTER %s %%{identity}D RENAME CONSTRAINT %%{conname}I TO %%{newname}I",
								  stringify_objtype(node->relationType));
				renameStmt = new_objtree_VA(fmtstr, 0);

				if (node->relationType == OBJECT_DOMAIN)
					ident = new_objtree_for_qualname_id(TypeRelationId,
														constrForm->contypid);
				else if (node->relationType == OBJECT_TABLE)
					ident = new_objtree_for_qualname_id(RelationRelationId,
														constrForm->conrelid);
				else
					elog(ERROR, "invalid relation type %d", node->relationType);

				append_string_object(renameStmt, "conname", node->subname);
				append_object_object(renameStmt, "identity", ident);
				ReleaseSysCache(conTup);
			}
			break;

		case OBJECT_OPCLASS:
		case OBJECT_OPFAMILY:
			ereport(ERROR,
					(errmsg("renaming of operator classes and families is not supported")));
			break;

		case OBJECT_RULE:
			{
				HeapTuple	rewrTup;
				Form_pg_rewrite rewrForm;
				Relation	pg_rewrite;

				pg_rewrite = relation_open(RewriteRelationId, AccessShareLock);
				rewrTup = get_catalog_object_by_oid(pg_rewrite, objectId);
				rewrForm = (Form_pg_rewrite) GETSTRUCT(rewrTup);

				renameStmt = new_objtree_VA("ALTER RULE %{rulename}I ON %{identity}D RENAME TO %{newname}I",
											0);
				append_string_object(renameStmt, "rulename", node->subname);
				append_object_object(renameStmt, "identity",
									 new_objtree_for_qualname_id(RelationRelationId,
																 rewrForm->ev_class));
				relation_close(pg_rewrite, AccessShareLock);
			}
			break;

		case OBJECT_TRIGGER:
			{
				HeapTuple	trigTup;
				Form_pg_trigger trigForm;
				Relation	pg_trigger;

				pg_trigger = relation_open(TriggerRelationId, AccessShareLock);
				trigTup = get_catalog_object_by_oid(pg_trigger, objectId);
				trigForm = (Form_pg_trigger) GETSTRUCT(trigTup);

				renameStmt = new_objtree_VA("ALTER TRIGGER %{triggername}I ON %{identity}D RENAME TO %{newname}I",
											0);
				append_string_object(renameStmt, "triggername", node->subname);
				append_object_object(renameStmt, "identity",
									 new_objtree_for_qualname_id(RelationRelationId,
																 trigForm->tgrelid));
				relation_close(pg_trigger, AccessShareLock);
			}
			break;
		default:
			elog(ERROR, "unsupported object type %d", node->renameType);
	}

	append_string_object(renameStmt, "newname", node->newname);

	command = jsonize_objtree(renameStmt);
	free_objtree(renameStmt);

	return command;
}

static inline ObjElem *
deparse_Seq_Cache(ObjTree *parent, Form_pg_sequence seqdata)
{
	ObjTree	   *tmp;
	char	   *tmpstr;

	tmpstr = psprintf("%lu", seqdata->cache_value);
	tmp = new_objtree_VA("CACHE %{value}s",
						 2,
						 "clause", ObjTypeString, "cache",
						 "value", ObjTypeString, tmpstr);
	return new_object_object(NULL, tmp);
}

static inline ObjElem *
deparse_Seq_Cycle(ObjTree *parent, Form_pg_sequence seqdata)
{
	ObjTree	   *tmp;

	tmp = new_objtree_VA("%{no}s CYCLE",
						 2,
						 "clause", ObjTypeString, "cycle",
						 "no", ObjTypeString,
						 seqdata->is_cycled ? "" : "NO");
	return new_object_object(NULL, tmp);
}

static inline ObjElem *
deparse_Seq_IncrementBy(ObjTree *parent, Form_pg_sequence seqdata)
{
	ObjTree	   *tmp;
	char	   *tmpstr;

	tmpstr = psprintf("%lu", seqdata->increment_by);
	tmp = new_objtree_VA("INCREMENT BY %{value}s",
						 2,
						 "clause", ObjTypeString, "increment_by",
						 "value", ObjTypeString, tmpstr);
	return new_object_object(NULL, tmp);
}

static inline ObjElem *
deparse_Seq_Minvalue(ObjTree *parent, Form_pg_sequence seqdata)
{
	ObjTree	   *tmp;
	char	   *tmpstr;

	tmpstr = psprintf("%lu", seqdata->min_value);
	tmp = new_objtree_VA("MINVALUE %{value}s",
						 2,
						 "clause", ObjTypeString, "minvalue",
						 "value", ObjTypeString, tmpstr);
	return new_object_object(NULL, tmp);
}

static inline ObjElem *
deparse_Seq_Maxvalue(ObjTree *parent, Form_pg_sequence seqdata)
{
	ObjTree	   *tmp;
	char	   *tmpstr;

	tmpstr = psprintf("%lu", seqdata->max_value);
	tmp = new_objtree_VA("MAXVALUE %{value}s",
						 2,
						 "clause", ObjTypeString, "maxvalue",
						 "value", ObjTypeString, tmpstr);
	return new_object_object(NULL, tmp);
}

static inline ObjElem *
deparse_Seq_Startwith(ObjTree *parent, Form_pg_sequence seqdata)
{
	ObjTree	   *tmp;
	char	   *tmpstr;

	tmpstr = psprintf("%lu", seqdata->start_value);
	tmp = new_objtree_VA("START WITH %{value}s",
						 2,
						 "clause", ObjTypeString, "start",
						 "value", ObjTypeString, tmpstr);
	return new_object_object(NULL, tmp);
}

static inline ObjElem *
deparse_Seq_Restart(ObjTree *parent, Form_pg_sequence seqdata)
{
	ObjTree	   *tmp;
	char	   *tmpstr;

	tmpstr = psprintf("%lu", seqdata->last_value);
	tmp = new_objtree_VA("RESTART %{value}s",
						 2,
						 "clause", ObjTypeString, "restart",
						 "value", ObjTypeString, tmpstr);
	return new_object_object(NULL, tmp);
}

static ObjElem *
deparse_Seq_OwnedBy(ObjTree *parent, Oid sequenceId)
{
	ObjTree    *ownedby = NULL;
	Relation	depRel;
	SysScanDesc scan;
	ScanKeyData keys[3];
	HeapTuple	tuple;

	depRel = heap_open(DependRelationId, AccessShareLock);
	ScanKeyInit(&keys[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&keys[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(sequenceId));
	ScanKeyInit(&keys[2],
				Anum_pg_depend_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(0));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  NULL, 3, keys);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Oid			ownerId;
		Form_pg_depend depform;
		ObjTree    *tmp;
		char	   *colname;

		depform = (Form_pg_depend) GETSTRUCT(tuple);

		/* only consider AUTO dependencies on pg_class */
		if (depform->deptype != DEPENDENCY_AUTO)
			continue;
		if (depform->refclassid != RelationRelationId)
			continue;
		if (depform->refobjsubid <= 0)
			continue;

		ownerId = depform->refobjid;
		colname = get_attname(ownerId, depform->refobjsubid);
		if (colname == NULL)
			continue;

		tmp = new_objtree_for_qualname_id(RelationRelationId, ownerId);
		append_string_object(tmp, "attrname", colname);
		ownedby = new_objtree_VA("OWNED BY %{owner}D",
								 2,
								 "clause", ObjTypeString, "owned",
								 "owner", ObjTypeObject, tmp);
	}

	systable_endscan(scan);
	relation_close(depRel, AccessShareLock);

	/*
	 * If there's no owner column, emit an empty OWNED BY element, set up so
	 * that it won't print anything.
	 */
	if (!ownedby)
		/* XXX this shouldn't happen ... */
		ownedby = new_objtree_VA("OWNED BY %{owner}D",
								 3,
								 "clause", ObjTypeString, "owned",
								 "owner", ObjTypeNull,
								 "present", ObjTypeBool, false);
	return new_object_object(NULL, ownedby);
}

/*
 * deparse_CreateSeqStmt
 *		deparse a CreateSeqStmt
 *
 * Given a sequence OID and the parsetree that created it, return the JSON blob
 * representing the creation command.
 */
static char *
deparse_CreateSeqStmt(Oid objectId, Node *parsetree)
{
	CreateSeqStmt *node = (CreateSeqStmt *) parsetree;
	ObjTree    *createSeq;
	ObjTree    *tmp;
	Relation	relation = relation_open(objectId, AccessShareLock);
	char	   *command;
	Form_pg_sequence seqdata;
	List	   *elems = NIL;

	seqdata = get_sequence_values(objectId);

	createSeq =
		new_objtree_VA("CREATE %{persistence}s SEQUENCE %{identity}D "
					   "%{definition: }s %{using}s",
					   1,
					   "persistence", ObjTypeString,
					   get_persistence_str(relation->rd_rel->relpersistence));

	tmp = new_objtree_for_qualname(relation->rd_rel->relnamespace,
								   RelationGetRelationName(relation));
	append_object_object(createSeq, "identity", tmp);

	/* definition elements */
	elems = lappend(elems, deparse_Seq_Cache(createSeq, seqdata));
	elems = lappend(elems, deparse_Seq_Cycle(createSeq, seqdata));
	elems = lappend(elems, deparse_Seq_IncrementBy(createSeq, seqdata));
	elems = lappend(elems, deparse_Seq_Minvalue(createSeq, seqdata));
	elems = lappend(elems, deparse_Seq_Maxvalue(createSeq, seqdata));
	elems = lappend(elems, deparse_Seq_Startwith(createSeq, seqdata));
	elems = lappend(elems, deparse_Seq_Restart(createSeq, seqdata));
	/* we purposefully do not emit OWNED BY here */

	append_array_object(createSeq, "definition", elems);

	tmp = new_objtree_VA("USING %{accessMethod}I", 0);
	if (node->accessMethod && node->accessMethod[0] != '\0')
		append_string_object(tmp, "accessMethod", node->accessMethod);
	else
		append_bool_object(tmp, "present", false);
	append_object_object(createSeq, "using", tmp);

	command = jsonize_objtree(createSeq);

	free_objtree(createSeq);
	relation_close(relation, AccessShareLock);

	return command;
}

static char *
deparse_AlterSeqStmt(Oid objectId, Node *parsetree)
{
	ObjTree	   *alterSeq;
	ObjTree	   *tmp;
	Relation	relation = relation_open(objectId, AccessShareLock);
	char	   *command;
	Form_pg_sequence seqdata;
	List	   *elems = NIL;
	ListCell   *cell;

	seqdata = get_sequence_values(objectId);

	alterSeq =
		new_objtree_VA("ALTER SEQUENCE %{identity}D %{definition: }s", 0);
	tmp = new_objtree_for_qualname(relation->rd_rel->relnamespace,
								   RelationGetRelationName(relation));
	append_object_object(alterSeq, "identity", tmp);

	foreach(cell, ((AlterSeqStmt *) parsetree)->options)
	{
		DefElem *elem = (DefElem *) lfirst(cell);
		ObjElem *newelm;

		if (strcmp(elem->defname, "cache") == 0)
			newelm = deparse_Seq_Cache(alterSeq, seqdata);
		else if (strcmp(elem->defname, "cycle") == 0)
			newelm = deparse_Seq_Cycle(alterSeq, seqdata);
		else if (strcmp(elem->defname, "increment_by") == 0)
			newelm = deparse_Seq_IncrementBy(alterSeq, seqdata);
		else if (strcmp(elem->defname, "minvalue") == 0)
			newelm = deparse_Seq_Minvalue(alterSeq, seqdata);
		else if (strcmp(elem->defname, "maxvalue") == 0)
			newelm = deparse_Seq_Maxvalue(alterSeq, seqdata);
		else if (strcmp(elem->defname, "start") == 0)
			newelm = deparse_Seq_Startwith(alterSeq, seqdata);
		else if (strcmp(elem->defname, "restart") == 0)
			newelm = deparse_Seq_Restart(alterSeq, seqdata);
		else if (strcmp(elem->defname, "owned_by") == 0)
			newelm = deparse_Seq_OwnedBy(alterSeq, objectId);
		else
			elog(ERROR, "invalid sequence option %s", elem->defname);

		elems = lappend(elems, newelm);
	}

	append_array_object(alterSeq, "definition", elems);

	command = jsonize_objtree(alterSeq);

	free_objtree(alterSeq);
	relation_close(relation, AccessShareLock);

	return command;
}

/*
 * deparse_IndexStmt
 *		deparse an IndexStmt
 *
 * Given an index OID and the parsetree that created it, return the JSON blob
 * representing the creation command.
 *
 * If the index corresponds to a constraint, NULL is returned.
 */
static char *
deparse_IndexStmt(Oid objectId, Node *parsetree)
{
	IndexStmt  *node = (IndexStmt *) parsetree;
	ObjTree    *indexStmt;
	ObjTree    *tmp;
	Relation	idxrel;
	Relation	heaprel;
	char	   *command;
	char	   *index_am;
	char	   *definition;
	char	   *reloptions;
	char	   *tablespace;
	char	   *whereClause;

	if (node->primary || node->isconstraint)
	{
		/*
		 * indexes for PRIMARY KEY and other constraints are output
		 * separately; return empty here.
		 */
		return NULL;
	}

	idxrel = relation_open(objectId, AccessShareLock);
	heaprel = relation_open(idxrel->rd_index->indrelid, AccessShareLock);

	pg_get_indexdef_detailed(objectId,
							 &index_am, &definition, &reloptions,
							 &tablespace, &whereClause);

	indexStmt =
		new_objtree_VA("CREATE %{unique}s INDEX %{concurrently}s %{name}I "
					   "ON %{table}D USING %{index_am}s (%{definition}s) "
					   "%{with}s %{tablespace}s %{where_clause}s",
					   5,
					   "unique", ObjTypeString, node->unique ? "UNIQUE" : "",
					   "concurrently", ObjTypeString,
					   node->concurrent ? "CONCURRENTLY" : "",
					   "name", ObjTypeString, RelationGetRelationName(idxrel),
					   "definition", ObjTypeString, definition,
					   "index_am", ObjTypeString, index_am);

	tmp = new_objtree_for_qualname(heaprel->rd_rel->relnamespace,
								   RelationGetRelationName(heaprel));
	append_object_object(indexStmt, "table", tmp);

	/* reloptions */
	tmp = new_objtree_VA("WITH (%{opts}s)", 0);
	if (reloptions)
		append_string_object(tmp, "opts", reloptions);
	else
		append_bool_object(tmp, "present", false);
	append_object_object(indexStmt, "with", tmp);

	/* tablespace */
	tmp = new_objtree_VA("TABLESPACE %{tablespace}s", 0);
	if (tablespace)
		append_string_object(tmp, "tablespace", tablespace);
	else
		append_bool_object(tmp, "present", false);
	append_object_object(indexStmt, "tablespace", tmp);

	/* WHERE clause */
	tmp = new_objtree_VA("WHERE %{where}s", 0);
	if (whereClause)
		append_string_object(tmp, "where", whereClause);
	else
		append_bool_object(tmp, "present", false);
	append_object_object(indexStmt, "where_clause", tmp);

	command = jsonize_objtree(indexStmt);
	free_objtree(indexStmt);

	heap_close(idxrel, AccessShareLock);
	heap_close(heaprel, AccessShareLock);

	return command;
}

static char *
deparse_RuleStmt(Oid objectId, Node *parsetree)
{
	RuleStmt *node = (RuleStmt *) parsetree;
	ObjTree	   *ruleStmt;
	ObjTree	   *tmp;
	char	   *command;
	Relation	pg_rewrite;
	Form_pg_rewrite rewrForm;
	HeapTuple	rewrTup;
	SysScanDesc	scan;
	ScanKeyData	key;
	Datum		ev_qual;
	Datum		ev_actions;
	bool		isnull;
	char	   *qual;
	List	   *actions;
	List	   *list;
	ListCell   *cell;

	pg_rewrite = heap_open(RewriteRelationId, AccessShareLock);
	ScanKeyInit(&key,
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(objectId));

	scan = systable_beginscan(pg_rewrite, RewriteOidIndexId, true,
							  NULL, 1, &key);
	rewrTup = systable_getnext(scan);
	if (!HeapTupleIsValid(rewrTup))
		elog(ERROR, "cache lookup failed for rewrite rule with oid %u",
			 objectId);

	rewrForm = (Form_pg_rewrite) GETSTRUCT(rewrTup);

	ruleStmt =
		new_objtree_VA("CREATE %{or_replace}s RULE %{identity}I "
					   "AS ON %{event}s TO %{table}D %{where_clause}s "
					   "DO %{instead}s (%{actions:; }s)", 2,
					   "identity", ObjTypeString, node->rulename,
					   "or_replace", ObjTypeString,
					   node->replace ? "OR REPLACE" : "");
	append_string_object(ruleStmt, "event",
						 node->event == CMD_SELECT ? "SELECT" :
						 node->event == CMD_UPDATE ? "UPDATE" :
						 node->event == CMD_DELETE ? "DELETE" :
						 node->event == CMD_INSERT ? "INSERT" : "XXX");
	append_object_object(ruleStmt, "table",
						 new_objtree_for_qualname_id(RelationRelationId,
													 rewrForm->ev_class));

	append_string_object(ruleStmt, "instead",
						 node->instead ? "INSTEAD" : "ALSO");

	ev_qual = heap_getattr(rewrTup, Anum_pg_rewrite_ev_qual,
						   RelationGetDescr(pg_rewrite), &isnull);
	ev_actions = heap_getattr(rewrTup, Anum_pg_rewrite_ev_action,
							  RelationGetDescr(pg_rewrite), &isnull);

	pg_get_ruledef_details(ev_qual, ev_actions, &qual, &actions);

	tmp = new_objtree_VA("WHERE %{clause}s", 0);

	if (qual)
		append_string_object(tmp, "clause", qual);
	else
	{
		append_null_object(tmp, "clause");
		append_bool_object(tmp, "present", false);
	}

	append_object_object(ruleStmt, "where_clause", tmp);

	list = NIL;
	foreach(cell, actions)
	{
		char *action = lfirst(cell);

		list = lappend(list, new_string_object(NULL, action));
	}
	append_array_object(ruleStmt, "actions", list);

	systable_endscan(scan);
	heap_close(pg_rewrite, AccessShareLock);

	command = jsonize_objtree(ruleStmt);
	free_objtree(ruleStmt);

	return command;
}



/*
 * deparse_CreateSchemaStmt
 *		deparse a CreateSchemaStmt
 *
 * Given a schema OID and the parsetree that created it, return the JSON blob
 * representing the creation command.
 *
 * Note we don't output the schema elements given in the creation command.
 * They must be output separately.	 (In the current implementation,
 * CreateSchemaCommand passes them back to ProcessUtility, which will lead to
 * this file if appropriate.)
 */
static char *
deparse_CreateSchemaStmt(Oid objectId, Node *parsetree)
{
	CreateSchemaStmt *node = (CreateSchemaStmt *) parsetree;
	ObjTree    *createSchema;
	ObjTree    *auth;
	char	   *command;

	createSchema =
		new_objtree_VA("CREATE SCHEMA %{if_not_exists}s %{name}I %{authorization}s",
					   2,
					   "name", ObjTypeString, node->schemaname,
					   "if_not_exists", ObjTypeString,
					   node->if_not_exists ? "IF NOT EXISTS" : "");

	auth = new_objtree_VA("AUTHORIZATION %{authorization_role}I", 0);
	if (node->authid)
		append_string_object(auth, "authorization_role", node->authid);
	else
	{
		append_null_object(auth, "authorization_role");
		append_bool_object(auth, "present", false);
	}
	append_object_object(createSchema, "authorization", auth);

	command = jsonize_objtree(createSchema);
	free_objtree(createSchema);

	return command;
}

static char *
deparse_AlterEnumStmt(Oid objectId, Node *parsetree)
{
	AlterEnumStmt *node = (AlterEnumStmt *) parsetree;
	ObjTree	   *alterEnum;
	ObjTree	   *tmp;
	char	   *command;

	alterEnum =
		new_objtree_VA("ALTER TYPE %{identity}D ADD VALUE %{if_not_exists}s %{value}L %{position}s",
					   0);

	append_string_object(alterEnum, "if_not_exists",
						 node->skipIfExists ? "IF NOT EXISTS" : "");
	append_object_object(alterEnum, "identity",
						 new_objtree_for_qualname_id(TypeRelationId,
													 objectId));
	append_string_object(alterEnum, "value", node->newVal);
	tmp = new_objtree_VA("%{after_or_before}s %{neighbour}L", 0);
	if (node->newValNeighbor)
	{
		append_string_object(tmp, "after_or_before",
							 node->newValIsAfter ? "AFTER" : "BEFORE");
		append_string_object(tmp, "neighbour", node->newValNeighbor);
	}
	else
	{
		append_bool_object(tmp, "present", false);
	}
	append_object_object(alterEnum, "position", tmp);

	command = jsonize_objtree(alterEnum);
	free_objtree(alterEnum);

	return command;
}

static char *
deparse_AlterOwnerStmt(Oid objectId, Node *parsetree)
{
	AlterOwnerStmt *node = (AlterOwnerStmt *) parsetree;
	ObjTree	   *ownerStmt;
	ObjectAddress addr;
	char	   *fmt;
	char	   *command;

	fmt = psprintf("ALTER %s %%{identity}s OWNER TO %%{newname}I",
				   stringify_objtype(node->objectType));
	ownerStmt = new_objtree_VA(fmt, 0);
	append_string_object(ownerStmt, "newname", node->newowner);

	addr.classId = get_objtype_catalog_oid(node->objectType);
	addr.objectId = objectId;
	addr.objectSubId = 0;

	append_string_object(ownerStmt, "identity",
						 getObjectIdentity(&addr));

	command = jsonize_objtree(ownerStmt);
	free_objtree(ownerStmt);

	return command;
}

static char *
deparse_CreateConversion(Oid objectId, Node *parsetree)
{
	HeapTuple   conTup;
	Form_pg_conversion conForm;
	ObjTree	   *ccStmt;
	char	   *command;

	conTup = SearchSysCache1(CONDEFAULT, ObjectIdGetDatum(objectId));
	if (!HeapTupleIsValid(conTup))
		elog(ERROR, "cache lookup failed for conversion with OID %u", objectId);
	conForm = (Form_pg_conversion) GETSTRUCT(conTup);

	ccStmt = new_objtree_VA("CREATE %{default}s CONVERSION %{identity}D FOR "
							"%{source}L TO %{dest}L FROM %{function}D", 0);

	append_string_object(ccStmt, "default",
						 conForm->condefault ? "DEFAULT" : "");
	append_object_object(ccStmt, "identity",
						 new_objtree_for_qualname(conForm->connamespace,
												  NameStr(conForm->conname)));
	append_string_object(ccStmt, "source", (char *)
						 pg_encoding_to_char(conForm->conforencoding));
	append_string_object(ccStmt, "dest", (char *)
						 pg_encoding_to_char(conForm->contoencoding));
	append_object_object(ccStmt, "function",
						 new_objtree_for_qualname_id(ProcedureRelationId,
													 conForm->conproc));

	command = jsonize_objtree(ccStmt);
	free_objtree(ccStmt);

	ReleaseSysCache(conTup);

	return command;
}

static char *
deparse_CreateOpFamily(Oid objectId, Node *parsetree)
{
	HeapTuple   opfTup;
	HeapTuple   amTup;
	Form_pg_opfamily opfForm;
	Form_pg_am  amForm;
	ObjTree	   *copfStmt;
	ObjTree	   *tmp;
	char	   *command;

	opfTup = SearchSysCache1(OPFAMILYOID, ObjectIdGetDatum(objectId));
	if (!HeapTupleIsValid(opfTup))
		elog(ERROR, "cache lookup failed for operator family with OID %u", objectId);
	opfForm = (Form_pg_opfamily) GETSTRUCT(opfTup);

	amTup = SearchSysCache1(AMOID, ObjectIdGetDatum(opfForm->opfmethod));
	if (!HeapTupleIsValid(amTup))
		elog(ERROR, "cache lookup failed for access method %u",
			 opfForm->opfmethod);
	amForm = (Form_pg_am) GETSTRUCT(amTup);

	copfStmt = new_objtree_VA("CREATE OPERATOR FAMILY %{identity}D USING %{amname}s",
							  0);

	tmp = new_objtree_for_qualname(opfForm->opfnamespace,
								   NameStr(opfForm->opfname));
	append_object_object(copfStmt, "identity", tmp);
	append_string_object(copfStmt, "amname", NameStr(amForm->amname));

	command = jsonize_objtree(copfStmt);
	free_objtree(copfStmt);

	ReleaseSysCache(amTup);
	ReleaseSysCache(opfTup);

	return command;
}

static char *
deparse_GrantStmt(StashedCommand *cmd)
{
	InternalGrant *istmt;
	ObjTree	   *grantStmt;
	char	   *command;
	char	   *fmt;
	char	   *objtype;
	List	   *list;
	ListCell   *cell;
	Oid			classId;
	ObjTree	   *tmp;

	istmt = cmd->d.grant.istmt;

	switch (istmt->objtype)
	{
		case ACL_OBJECT_COLUMN:
		case ACL_OBJECT_RELATION:
			objtype = "TABLE";
			classId = RelationRelationId;
			break;
		case ACL_OBJECT_SEQUENCE:
			objtype = "SEQUENCE";
			classId = RelationRelationId;
			break;
		case ACL_OBJECT_DOMAIN:
			objtype = "DOMAIN";
			classId = TypeRelationId;
			break;
		case ACL_OBJECT_FDW:
			objtype = "FOREIGN DATA WRAPPER";
			classId = ForeignDataWrapperRelationId;
			break;
		case ACL_OBJECT_FOREIGN_SERVER:
			objtype = "SERVER";
			classId = ForeignServerRelationId;
			break;
		case ACL_OBJECT_FUNCTION:
			objtype = "FUNCTION";
			classId = ProcedureRelationId;
			break;
		case ACL_OBJECT_LANGUAGE:
			objtype = "LANGUAGE";
			classId = LanguageRelationId;
			break;
		case ACL_OBJECT_LARGEOBJECT:
			objtype = "LARGE OBJECT";
			classId = LargeObjectRelationId;
			break;
		case ACL_OBJECT_NAMESPACE:
			objtype = "SCHEMA";
			classId = NamespaceRelationId;
			break;
		case ACL_OBJECT_TYPE:
			objtype = "TYPE";
			classId = TypeRelationId;
			break;
		case ACL_OBJECT_DATABASE:
		case ACL_OBJECT_TABLESPACE:
			objtype = "";
			classId = InvalidOid;
			elog(ERROR, "global objects not supported");
		default:
			elog(ERROR, "invalid ACL_OBJECT value %d", istmt->objtype);
	}

	/* GRANT TO or REVOKE FROM */
	if (istmt->is_grant)
		fmt = psprintf("GRANT %%{privileges:, }s ON %s %%{privtarget:, }s "
					   "TO %%{grantees:, }s %%{grant_option}s",
					   objtype);
	else
		fmt = psprintf("REVOKE %%{grant_option}s %%{privileges:, }s ON %s %%{privtarget:, }s "
					   "FROM %%{grantees:, }s %%{cascade}s",
					   objtype);

	grantStmt = new_objtree_VA(fmt, 0);

	/* build list of privileges to grant/revoke */
	if (istmt->all_privs)
	{
		tmp = new_objtree_VA("ALL PRIVILEGES", 0);
		list = list_make1(new_object_object(NULL, tmp));
	}
	else
	{
		list = NIL;

		if (istmt->privileges & ACL_INSERT)
			list = lappend(list, new_string_object(NULL, "INSERT"));
		if (istmt->privileges & ACL_SELECT)
			list = lappend(list, new_string_object(NULL, "SELECT"));
		if (istmt->privileges & ACL_UPDATE)
			list = lappend(list, new_string_object(NULL, "UPDATE"));
		if (istmt->privileges & ACL_DELETE)
			list = lappend(list, new_string_object(NULL, "DELETE"));
		if (istmt->privileges & ACL_TRUNCATE)
			list = lappend(list, new_string_object(NULL, "TRUNCATE"));
		if (istmt->privileges & ACL_REFERENCES)
			list = lappend(list, new_string_object(NULL, "REFERENCES"));
		if (istmt->privileges & ACL_TRIGGER)
			list = lappend(list, new_string_object(NULL, "TRIGGER"));
		if (istmt->privileges & ACL_EXECUTE)
			list = lappend(list, new_string_object(NULL, "EXECUTE"));
		if (istmt->privileges & ACL_USAGE)
			list = lappend(list, new_string_object(NULL, "USAGE"));
		if (istmt->privileges & ACL_CREATE)
			list = lappend(list, new_string_object(NULL, "CREATE"));
		if (istmt->privileges & ACL_CREATE_TEMP)
			list = lappend(list, new_string_object(NULL, "TEMPORARY"));
		if (istmt->privileges & ACL_CONNECT)
			list = lappend(list, new_string_object(NULL, "CONNECT"));

		if (istmt->col_privs != NIL)
		{
			ListCell   *ocell;

			foreach(ocell, istmt->col_privs)
			{
				AccessPriv *priv = lfirst(ocell);
				List   *cols = NIL;

				tmp = new_objtree_VA("%{priv}s (%{cols:, }I)", 0);
				foreach(cell, priv->cols)
				{
					Value *colname = lfirst(cell);

					cols = lappend(cols,
								   new_string_object(NULL,
													 strVal(colname)));
				}
				append_array_object(tmp, "cols", cols);
				if (priv->priv_name == NULL)
					append_string_object(tmp, "priv", "ALL PRIVILEGES");
				else
					append_string_object(tmp, "priv", priv->priv_name);

				list = lappend(list, new_object_object(NULL, tmp));
			}
		}
	}
	append_array_object(grantStmt, "privileges", list);

	/* target objects.  We use object identities here */
	list = NIL;
	foreach(cell, istmt->objects)
	{
		Oid		objid = lfirst_oid(cell);
		ObjectAddress addr;

		addr.classId = classId;
		addr.objectId = objid;
		addr.objectSubId = 0;

		tmp = new_objtree_VA("%{identity}s", 0);
		append_string_object(tmp, "identity",
							 getObjectIdentity(&addr));
		list = lappend(list, new_object_object(NULL, tmp));
	}
	append_array_object(grantStmt, "privtarget", list);

	/* list of grantees */
	list = NIL;
	foreach(cell, istmt->grantees)
	{
		Oid		grantee = lfirst_oid(cell);

		if (grantee == ACL_ID_PUBLIC)
			tmp = new_objtree_VA("PUBLIC", 0);
		else
		{
			HeapTuple	roltup;
			char	   *rolname;

			roltup = SearchSysCache1(AUTHOID, ObjectIdGetDatum(grantee));
			if (!HeapTupleIsValid(roltup))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("role with OID %u does not exist", grantee)));

			tmp = new_objtree_VA("%{name}I", 0);
			rolname = NameStr(((Form_pg_authid) GETSTRUCT(roltup))->rolname);
			append_string_object(tmp, "name", pstrdup(rolname));
			ReleaseSysCache(roltup);
		}
		list = lappend(list, new_object_object(NULL, tmp));
	}
	append_array_object(grantStmt, "grantees", list);

	/* the wording of the grant option is variable ... */
	if (istmt->is_grant)
		append_string_object(grantStmt, "grant_option",
							 istmt->grant_option ?  "WITH GRANT OPTION" : "");
	else
		append_string_object(grantStmt, "grant_option",
							 istmt->grant_option ?  "GRANT OPTION FOR" : "");

	if (!istmt->is_grant)
	{
		if (istmt->behavior == DROP_CASCADE)
			append_string_object(grantStmt, "cascade", "CASCADE");
		else
			append_string_object(grantStmt, "cascade", "");
	}

	command = jsonize_objtree(grantStmt);
	free_objtree(grantStmt);

	return command;
}

static char *
deparse_AlterTableStmt(StashedCommand *cmd)
{
	ObjTree	   *alterTableStmt;
	ObjTree	   *tmp;
	ObjTree	   *tmp2;
	List	   *dpcontext;
	Relation	rel;
	List	   *subcmds = NIL;
	ListCell   *cell;
	char	   *command;

	rel = heap_open(cmd->d.alterTable.objectId, AccessShareLock);
	dpcontext = deparse_context_for(RelationGetRelationName(rel),
									cmd->d.alterTable.objectId);

	alterTableStmt =
		new_objtree_VA("ALTER TABLE %{identity}D %{subcmds:, }s", 0);
	tmp = new_objtree_for_qualname(rel->rd_rel->relnamespace,
								   RelationGetRelationName(rel));
	append_object_object(alterTableStmt, "identity", tmp);

	foreach(cell, cmd->d.alterTable.subcmds)
	{
		StashedATSubcmd	*substashed = (StashedATSubcmd *) lfirst(cell);
		AlterTableCmd	*subcmd = (AlterTableCmd *) substashed->parsetree;
		ObjTree	   *tree;

		Assert(IsA(subcmd, AlterTableCmd));

		switch (subcmd->subtype)
		{
			case AT_AddColumn:
			case AT_AddColumnRecurse:
				/* XXX need to set the "recurse" bit somewhere? */
				Assert(IsA(subcmd->def, ColumnDef));
				tree = deparse_ColumnDef(rel, dpcontext, false,
										 (ColumnDef *) subcmd->def);
				tmp = new_objtree_VA("ADD COLUMN %{definition}s",
									 2, "type", ObjTypeString, "add column",
									 "definition", ObjTypeObject, tree);
				subcmds = lappend(subcmds,
								  new_object_object(NULL, tmp));
				break;

			case AT_DropColumnRecurse:
			case AT_ValidateConstraintRecurse:
			case AT_DropConstraintRecurse:
			case AT_AddOidsRecurse:
			case AT_AddIndexConstraint:
			case AT_ReAddIndex:
			case AT_ReAddConstraint:
			case AT_ProcessedConstraint:
			case AT_ReplaceRelOptions:
				/* Subtypes used for internal operations; nothing to do here */
				break;

			case AT_AddColumnToView:
				/* CREATE OR REPLACE VIEW -- nothing to do here */
				break;

			case AT_ColumnDefault:
				if (subcmd->def == NULL)
				{
					tmp = new_objtree_VA("ALTER COLUMN %{column}I DROP DEFAULT",
										 1, "type", ObjTypeString, "drop default");
				}
				else
				{
					List	   *dpcontext;
					HeapTuple	attrtup;
					AttrNumber	attno;

					tmp = new_objtree_VA("ALTER COLUMN %{column}I SET DEFAULT %{definition}s",
										 1, "type", ObjTypeString, "set default");

					dpcontext = deparse_context_for(RelationGetRelationName(rel),
													RelationGetRelid(rel));
					attrtup = SearchSysCacheAttName(RelationGetRelid(rel), subcmd->name);
					attno = ((Form_pg_attribute) GETSTRUCT(attrtup))->attnum;
					append_string_object(tmp, "definition",
										 RelationGetColumnDefault(rel, attno, dpcontext));
					ReleaseSysCache(attrtup);
				}
				append_string_object(tmp, "column", subcmd->name);

				subcmds = lappend(subcmds, new_object_object(NULL, tmp));
				break;

			case AT_DropNotNull:
				tmp = new_objtree_VA("ALTER COLUMN %{column}I DROP NOT NULL",
									 1, "type", ObjTypeString, "drop not null");
				append_string_object(tmp, "column", subcmd->name);
				subcmds = lappend(subcmds, new_object_object(NULL, tmp));
				break;

			case AT_SetNotNull:
				tmp = new_objtree_VA("ALTER COLUMN %{column}I SET NOT NULL",
									 1, "type", ObjTypeString, "set not null");
				append_string_object(tmp, "column", subcmd->name);
				subcmds = lappend(subcmds, new_object_object(NULL, tmp));
				break;

			case AT_SetStatistics:
				/* not yet */
				break;

			case AT_SetOptions:
				/* not yet */
				break;

			case AT_ResetOptions:
				/* not yet */
				break;

			case AT_SetStorage:
				Assert(IsA(subcmd->def, String));
				tmp = new_objtree_VA("ALTER COLUMN %{column}I SET STORAGE %{storage}s",
									 3, "type", ObjTypeString, "set storage",
									 "column", ObjTypeString, subcmd->name,
									 "storage", ObjTypeString,
									 strVal((Value *) subcmd->def));
				subcmds = lappend(subcmds, new_object_object(NULL, tmp));
				break;

			case AT_DropColumn:
				tmp = new_objtree_VA("DROP COLUMN %{column}I",
									 2, "type", ObjTypeString, "drop column",
								 "column", ObjTypeString, subcmd->name);
				subcmds = lappend(subcmds, new_object_object(NULL, tmp));
				break;

			case AT_AddIndex:
				{
					Oid			idxOid = substashed->oid;
					IndexStmt  *istmt;
					Relation	idx;
					const char *idxname;
					Oid			constrOid;

					Assert(IsA(subcmd->def, IndexStmt));
					istmt = (IndexStmt *) subcmd->def;

					if (!istmt->isconstraint)
						break;

					idx = relation_open(idxOid, AccessShareLock);
					idxname = RelationGetRelationName(idx);

					constrOid = get_relation_constraint_oid(
						cmd->d.alterTable.objectId, idxname, false);

					tmp = new_objtree_VA("ADD CONSTRAINT %{name}I %{definition}s",
										 3, "type", ObjTypeString, "add constraint",
										 "name", ObjTypeString, idxname,
										 "definition", ObjTypeString,
										 pg_get_constraintdef_string(constrOid, false));
					subcmds = lappend(subcmds, new_object_object(NULL, tmp));

					relation_close(idx, AccessShareLock);
				}
				break;

			case AT_AddConstraint:
			case AT_AddConstraintRecurse:
				{
					/* XXX need to set the "recurse" bit somewhere? */
					Oid			constrOid = substashed->oid;

					tmp = new_objtree_VA("ADD CONSTRAINT %{name}I %{definition}s",
										 3, "type", ObjTypeString, "add constraint",
										 "name", ObjTypeString, get_constraint_name(constrOid),
										 "definition", ObjTypeString,
										 pg_get_constraintdef_string(constrOid, false));
					subcmds = lappend(subcmds, new_object_object(NULL, tmp));
				}
				break;

			case AT_AlterConstraint:
				break;

			case AT_ValidateConstraint:
				tmp = new_objtree_VA("VALIDATE CONSTRAINT %{constraint}I", 2,
									 "type", ObjTypeString, "validate constraint",
									 "constraint", ObjTypeString, subcmd->name);
				subcmds = lappend(subcmds, new_object_object(NULL, tmp));
				break;

			case AT_DropConstraint:
				tmp = new_objtree_VA("DROP CONSTRAINT %{constraint}I", 2,
									 "type", ObjTypeString, "drop constraint",
									 "constraint", ObjTypeString, subcmd->name);
				subcmds = lappend(subcmds, new_object_object(NULL, tmp));
				break;

			case AT_AlterColumnType:
				tmp = new_objtree_VA("ALTER COLUMN %{column}I SET DATA TYPE %{datatype}T collate_clause using_clause",
									 2, "type", ObjTypeString, "alter column type",
									 "column", ObjTypeString, subcmd->name);
				/* FIXME figure out correct typid/typmod , collate clause, using_clause */
				append_object_object(tmp, "datatype",
									 new_objtree_for_type(INT4OID, -1));
				subcmds = lappend(subcmds, new_object_object(NULL, tmp));
				break;

			case AT_AlterColumnGenericOptions:
				break;

			case AT_ChangeOwner:
				tmp = new_objtree_VA("OWNER TO %{owner}I",
									 2, "type", ObjTypeString, "change owner",
									 "owner",  ObjTypeString, subcmd->name);
				subcmds = lappend(subcmds, new_object_object(NULL, tmp));
				break;

			case AT_ClusterOn:
				tmp = new_objtree_VA("CLUSTER ON %{index}I", 2,
									 "type", ObjTypeString, "cluster on",
									 "index", ObjTypeString, subcmd->name);
				subcmds = lappend(subcmds, new_object_object(NULL, tmp));
				break;

			case AT_DropCluster:
				tmp = new_objtree_VA("SET WITHOUT CLUSTER", 1,
									 "type", ObjTypeString, "set without cluster");
				subcmds = lappend(subcmds, new_object_object(NULL, tmp));
				break;

			case AT_AddOids:
				tmp = new_objtree_VA("SET WITH OIDS", 1,
									 "type", ObjTypeString, "set with oids");
				subcmds = lappend(subcmds, new_object_object(NULL, tmp));
				break;

			case AT_DropOids:
				tmp = new_objtree_VA("SET WITHOUT OIDS", 1,
									 "type", ObjTypeString, "set without oids");
				subcmds = lappend(subcmds, new_object_object(NULL, tmp));
				break;

			case AT_SetTableSpace:
				tmp = new_objtree_VA("SET TABLESPACE %{tablespace}I", 2,
									 "type", ObjTypeString, "set tablespace",
									 "tablespace", ObjTypeString, subcmd->name);
				subcmds = lappend(subcmds, new_object_object(NULL, tmp));
				break;

			case AT_SetRelOptions:
				break;

			case AT_ResetRelOptions:
				break;

				/*
				 * FIXME --- should we unify representation of all these
				 * ENABLE/DISABLE TRIGGER commands??
				 */
			case AT_EnableTrig:
				tmp = new_objtree_VA("ENABLE TRIGGER %{trigger}I", 2,
									 "type", ObjTypeString, "enable trigger",
									 "trigger", ObjTypeString, subcmd->name);
				subcmds = lappend(subcmds, new_object_object(NULL, tmp));
				break;

			case AT_EnableAlwaysTrig:
				tmp = new_objtree_VA("ENABLE ALWAYS TRIGGER %{trigger}I", 2,
									 "type", ObjTypeString, "enable always trigger",
									 "trigger", ObjTypeString, subcmd->name);
				subcmds = lappend(subcmds, new_object_object(NULL, tmp));
				break;

			case AT_EnableReplicaTrig:
				tmp = new_objtree_VA("ENABLE REPLICA TRIGGER %{trigger}I", 2,
									 "type", ObjTypeString, "enable replica trigger",
									 "trigger", ObjTypeString, subcmd->name);
				subcmds = lappend(subcmds, new_object_object(NULL, tmp));
				break;

			case AT_DisableTrig:
				tmp = new_objtree_VA("DISABLE TRIGGER %{trigger}I", 2,
									 "type", ObjTypeString, "disable trigger",
									 "trigger", ObjTypeString, subcmd->name);
				subcmds = lappend(subcmds, new_object_object(NULL, tmp));
				break;

			case AT_EnableTrigAll:
				tmp = new_objtree_VA("ENABLE TRIGGER ALL", 1,
									 "type", ObjTypeString, "enable trigger all");
				subcmds = lappend(subcmds, new_object_object(NULL, tmp));
				break;

			case AT_DisableTrigAll:
				tmp = new_objtree_VA("DISABLE TRIGGER ALL", 1,
									 "type", ObjTypeString, "disable trigger all");
				subcmds = lappend(subcmds, new_object_object(NULL, tmp));
				break;

			case AT_EnableTrigUser:
				tmp = new_objtree_VA("ENABLE TRIGGER USER", 1,
									 "type", ObjTypeString, "enable trigger user");
				subcmds = lappend(subcmds, new_object_object(NULL, tmp));
				break;

			case AT_DisableTrigUser:
				tmp = new_objtree_VA("DISABLE TRIGGER USER", 1,
									 "type", ObjTypeString, "disable trigger user");
				subcmds = lappend(subcmds, new_object_object(NULL, tmp));
				break;

			case AT_EnableRule:
				break;

			case AT_EnableAlwaysRule:
				break;

			case AT_EnableReplicaRule:
				break;

			case AT_DisableRule:
				break;

			case AT_AddInherit:
				/*
				 * XXX this case is interesting: we cannot rely on parse node
				 * because parent name might be unqualified; but there's no way
				 * to extract it from catalog either, since we don't know which
				 * of the parents is the new one.
				 */
				break;

			case AT_DropInherit:
				/* XXX ditto ... */
				break;

			case AT_AddOf:
				break;

			case AT_DropOf:
				break;

			case AT_ReplicaIdentity:
				tmp = new_objtree_VA("REPLICA IDENTITY %{ident}s", 1,
									 "type", ObjTypeString, "replica identity");
				switch (((ReplicaIdentityStmt *) subcmd->def)->identity_type)
				{
					case REPLICA_IDENTITY_DEFAULT:
						append_string_object(tmp, "ident", "DEFAULT");
						break;
					case REPLICA_IDENTITY_FULL:
						append_string_object(tmp, "ident", "FULL");
						break;
					case REPLICA_IDENTITY_NOTHING:
						append_string_object(tmp, "ident", "NOTHING");
						break;
					case REPLICA_IDENTITY_INDEX:
						tmp2 = new_objtree_VA("USING INDEX %{index}I", 1,
											  "index", ObjTypeString,
											  ((ReplicaIdentityStmt *) subcmd->def)->name);
						append_object_object(tmp, "ident", tmp2);
						break;
				}
				subcmds = lappend(subcmds, new_object_object(NULL, tmp));
				break;

			case AT_GenericOptions:
				break;

			default:
				elog(WARNING, "unsupported alter table subtype %d",
					 subcmd->subtype);
				break;
		}
	}

	if (list_length(subcmds) == 0)
	{
		command = NULL;
	}
	else
	{
		append_array_object(alterTableStmt, "subcmds", subcmds);
		command = jsonize_objtree(alterTableStmt);
	}

	free_objtree(alterTableStmt);
	heap_close(rel, AccessShareLock);

	return command;
}

static char *
deparse_parsenode_cmd(StashedCommand *cmd)
{
	Oid			objectId;
	Node	   *parsetree;
	char	   *command;

	parsetree = cmd->parsetree;

	switch (cmd->type)
	{
		case SCT_Basic:
			objectId = cmd->d.basic.objectId;
			break;
		case SCT_AlterTable:
			/* XXX needed? */
			objectId = cmd->d.alterTable.objectId;
			break;
		default:
			elog(ERROR, "unexpected deparse node type %d", cmd->type);
	}

	switch (nodeTag(parsetree))
	{
		case T_CreateSchemaStmt:
			command = deparse_CreateSchemaStmt(objectId, parsetree);
			break;

		case T_CreateStmt:
			command = deparse_CreateStmt(objectId, parsetree);
			break;

		case T_IndexStmt:
			command = deparse_IndexStmt(objectId, parsetree);
			break;

		case T_ViewStmt:
			command = deparse_ViewStmt(objectId, parsetree);
			break;

		case T_CreateSeqStmt:
			command = deparse_CreateSeqStmt(objectId, parsetree);
			break;

		case T_AlterSeqStmt:
			command = deparse_AlterSeqStmt(objectId, parsetree);
			break;

			/* creation of objects hanging off tables */
		case T_CreateTrigStmt:
			command = deparse_CreateTrigStmt(objectId, parsetree);
			break;

		case T_RuleStmt:
			command = deparse_RuleStmt(objectId, parsetree);
			break;

			/* FDW-related objects */
		case T_CreateForeignTableStmt:
		case T_CreateFdwStmt:
		case T_CreateForeignServerStmt:
		case T_CreateUserMappingStmt:
			command = NULL;
			break;

			/* other local objects */
		case T_DefineStmt:
			command = deparse_DefineStmt(objectId, parsetree);
			break;

		case T_CreateExtensionStmt:
			command = deparse_CreateExtensionStmt(objectId, parsetree);
			break;

		case T_AlterExtensionStmt:
			command = deparse_AlterExtensionStmt(objectId, parsetree);
			break;

		case T_CompositeTypeStmt:		/* CREATE TYPE (composite) */
			command = deparse_CompositeTypeStmt(objectId, parsetree);
			break;

		case T_CreateEnumStmt:	/* CREATE TYPE AS ENUM */
			command = deparse_CreateEnumStmt(objectId, parsetree);
			break;

		case T_CreateRangeStmt:	/* CREATE TYPE AS RANGE */
			command = deparse_CreateRangeStmt(objectId, parsetree);
			break;

		case T_RenameStmt:		/* ALTER .. RENAME */
			command = deparse_RenameStmt(objectId, parsetree);
			break;

		case T_CreateDomainStmt:
			command = deparse_CreateDomain(objectId, parsetree);
			break;

		case T_CreateFunctionStmt:
			command = deparse_CreateFunction(objectId, parsetree);
			break;

		case T_CreateTableAsStmt:
		case T_CreatePLangStmt:
		case T_CreateCastStmt:
		case T_CreateOpClassStmt:
			command = NULL;
			break;

		case T_CreateConversionStmt:
			command = deparse_CreateConversion(objectId, parsetree);
			break;

		case T_CreateOpFamilyStmt:
			command = deparse_CreateOpFamily(objectId, parsetree);
			break;

			/* matviews */
		case T_RefreshMatViewStmt:
			command = NULL;
			break;

		case T_AlterTableStmt:
			command = deparse_AlterTableStmt(cmd);
			break;

		case T_AlterEnumStmt:
			command = deparse_AlterEnumStmt(objectId, parsetree);
			break;

		case T_AlterOwnerStmt:
			command = deparse_AlterOwnerStmt(objectId, parsetree);
			break;

		case T_GrantStmt:
			elog(ERROR, "unexpected node type T_GrantStmt");
			break;

		default:
			command = NULL;
			elog(LOG, "unrecognized node type: %d",
				 (int) nodeTag(parsetree));
	}

	return command;
}

/*
 * Given a utility command parsetree and the OID of the corresponding object,
 * return a JSON representation of the command.
 *
 * The command is expanded fully, so that there are no ambiguities even in the
 * face of search_path changes.
 */
char *
deparse_utility_command(StashedCommand *cmd)
{
	MemoryContext	oldcxt;
	MemoryContext	tmpcxt;
	OverrideSearchPath *overridePath;
	char	   *command;

	/*
	 * Allocate everything done by the deparsing routines into a temp context,
	 * to avoid having to sprinkle them with memory handling code
	 */
	tmpcxt = AllocSetContextCreate(CurrentMemoryContext,
								   "deparse ctx",
								   ALLOCSET_DEFAULT_MINSIZE,
								   ALLOCSET_DEFAULT_INITSIZE,
								   ALLOCSET_DEFAULT_MAXSIZE);
	oldcxt = MemoryContextSwitchTo(tmpcxt);

	/*
	 * Many routines underlying this one will invoke ruleutils.c functionality
	 * in order to obtain deparsed versions of expressions.  In such results,
	 * we want all object names to be qualified, so that results are "portable"
	 * to environments with different search_path settings.  Rather than inject
	 * what would be repetitive calls to override search path all over the
	 * place, we do it centrally here.
	 */
	overridePath = GetOverrideSearchPath(CurrentMemoryContext);
	overridePath->schemas = NIL;
	overridePath->addCatalog = false;
	overridePath->addTemp = false;
	PushOverrideSearchPath(overridePath);

	switch (cmd->type)
	{
		case SCT_Basic:
		case SCT_AlterTable:
			command = deparse_parsenode_cmd(cmd);
			break;
		case SCT_Grant:
			command = deparse_GrantStmt(cmd);
			break;
		default:
			elog(ERROR, "unexpected deparse node type %d", cmd->type);
	}

	PopOverrideSearchPath();

	/*
	 * XXX to avoid the pstrdup we could have the routines return the
	 * ObjTree and do the jsonize_objtree() here after changing cxt ...
	 */
	MemoryContextSwitchTo(oldcxt);
	if (command != NULL)
		command = pstrdup(command);
	MemoryContextDelete(tmpcxt);

	return command;
}
