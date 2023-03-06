/*
 * gpu_groupby.c
 *
 * Aggregation and Group-By with GPU acceleration
 * ----
 * Copyright 2011-2023 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014-2023 (C) PG-Strom Developers Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the PostgreSQL License.
 */
#include "pg_strom.h"

/* static variables */
static create_upper_paths_hook_type	create_upper_paths_next;
static CustomPathMethods	gpugroupby_path_methods;
static CustomScanMethods	gpugroupby_plan_methods;
static CustomExecMethods	gpugroupby_exec_methods;
static bool		pgstrom_enable_gpugroupby;
static bool		pgstrom_enable_partitionwise_gpugroupby;
static bool		pgstrom_enable_numeric_aggfuncs;
int				pgstrom_hll_register_bits;

/*
 * List of supported aggregate functions
 */
typedef struct
{
	/* aggregate function can be preprocessed */
	const char *aggfn_signature;
	/*
	 * A pair of final/partial function will generate same result.
	 * Its prefix indicates the schema that stores these functions.
	 * c: pg_catalog ... the system default
	 * s: pgstrom    ... PG-Strom's special ones
	 */
	const char *finalfn_signature;
	const char *partfn_signature;
	int			partfn_action;	/* any of KAGG_ACTION__* */
	bool		numeric_aware;	/* ignored, if !enable_numeric_aggfuncs */
} aggfunc_catalog_t;

#define ALTFUNC_EXPR_NROWS			-101	/* NROWS(X) */
#define ALTFUNC_EXPR_PMIN			-102	/* PMIN(X) */
#define ALTFUNC_EXPR_PMAX			-103	/* PMAX(X) */
#define ALTFUNC_EXPR_PSUM			-104	/* PSUM(X) */
#define ALTFUNC_EXPR_PSUM_X2		-105	/* PSUM_X2(X) = PSUM(X^2) */
#define ALTFUNC_EXPR_PCOV_X			-106	/* PCOV_X(X,Y) */
#define ALTFUNC_EXPR_PCOV_Y			-107	/* PCOV_Y(X,Y) */
#define ALTFUNC_EXPR_PCOV_X2		-108	/* PCOV_X2(X,Y) */
#define ALTFUNC_EXPR_PCOV_Y2		-109	/* PCOV_Y2(X,Y) */
#define ALTFUNC_EXPR_PCOV_XY		-110	/* PCOV_XY(X,Y) */
#define ALTFUNC_EXPR_HLL_HASH		-111	/* HLL_HASH(X) */

static aggfunc_catalog_t	aggfunc_catalog_array[] = {
	/* COUNT(*) = SUM(NROWS()) */
	{"count()",
	 "s:sum(int8)",
	 "s:nrows()",
	 KAGG_ACTION__NROWS_ANY, false
	},
	/* COUNT(X) = SUM(NROWS(X)) */
	{"count(any)",
	 "s:sum(int8)",
	 "s:nrows(any)",
	 KAGG_ACTION__NROWS_COND, false
	},
	/*
	 * MIN(X) = MIN(PMIN(X))
	 */
	{"min(int1)",
	 "s:min_i1(bytea)",
	 "s:pmin(int1)",
	 KAGG_ACTION__PMIN_INT32, false
	},
	{"min(int2)",
	 "s:min_i2(bytea)",
	 "s:pmin(int2)",
	 KAGG_ACTION__PMIN_INT32, false
	},
	{"min(int4)",
	 "s:min_i4(bytea)",
	 "s:pmin(int4)",
	 KAGG_ACTION__PMIN_INT32, false
	},
	{"min(int8)",
	 "s:min_i8(bytea)",
	 "s:pmin(int8)",
	 KAGG_ACTION__PMIN_INT64, false
	},
	{"min(float2)",
     "s:min_f2(bytea)",
	 "s:pmin(float4)",
	 KAGG_ACTION__PMIN_FP32, false
	},
	{"min(float4)",
     "s:min_f4(bytea)",
	 "s:pmin(float4)",
	 KAGG_ACTION__PMIN_FP32, false
	},
	{"min(float8)",
     "s:min_f8(bytea)",
	 "s:pmin(float8)",
	 KAGG_ACTION__PMIN_FP64, false
	},
	{"min(numeric)",
	 "s:min_num(bytea)",
	 "s:pmin(float8)",
	 KAGG_ACTION__PMIN_FP64, true
	},
	{"min(money)",
	 "s:min_cash(bytea)",
	 "s:pmin(money)",
	 KAGG_ACTION__PMIN_INT64, false
	},
	{"min(date)",
	 "s:min_date(bytea)",
	 "s:pmin(date)",
	 KAGG_ACTION__PMIN_INT32, false
	},
	{"min(time)",
	 "s:min_time(bytea)",
	 "s:pmin(time)",
	 KAGG_ACTION__PMIN_INT64, false
	},
	{"min(timestamp)",
	 "s:min_ts(bytea)",
	 "s:pmin(timestamp)",
	 KAGG_ACTION__PMIN_INT64, false
	},
	{"min(timestamptz)",
	 "s:min_tstz(bytea)",
	 "s:pmin(timestamptz)",
	 KAGG_ACTION__PMIN_INT64, false
	},
	/*
	 * MAX(X) = MAX(PMAX(X))
	 */
	{"max(int1)",
	 "s:max_i1(bytea)",
	 "s:pmax(int1)",
	 KAGG_ACTION__PMAX_INT32, false
	},
	{"max(int2)",
	 "s:max_i2(bytea)",
	 "s:pmax(int2)",
	 KAGG_ACTION__PMAX_INT32, false
	},
	{"max(int4)",
	 "s:max_i4(bytea)",
	 "s:pmax(int4)",
	 KAGG_ACTION__PMAX_INT32, false
	},
	{"max(int8)",
	 "s:max_i8(bytea)",
	 "s:pmax(int8)",
	 KAGG_ACTION__PMAX_INT64, false
	},
	{"max(float2)",
     "s:max_f2(bytea)",
	 "s:pmax(float4)",
	 KAGG_ACTION__PMAX_FP32, false
	},
	{"max(float4)",
     "s:max_f4(bytea)",
	 "s:pmax(float4)",
	 KAGG_ACTION__PMAX_FP32, false
	},
	{"max(float8)",
     "s:max_f8(bytea)",
	 "s:pmax(float8)",
	 KAGG_ACTION__PMAX_FP64, false
	},
	{"max(numeric)",
	 "s:max_num(bytea)",
	 "s:pmax(float8)",
	 KAGG_ACTION__PMAX_FP64, true
	},
	{"max(money)",
	 "s:max_cash(bytea)",
	 "s:pmax(money)",
	 KAGG_ACTION__PMAX_INT64, false
	},
	{"max(date)",
	 "s:max_date(bytea)",
	 "s:pmax(date)",
	 KAGG_ACTION__PMAX_INT32, false
	},
	{"max(time)",
	 "s:max_time(bytea)",
	 "s:pmax(time)",
	 KAGG_ACTION__PMAX_INT64, false
	},
	{"max(timestamp)",
	 "s:max_ts(bytea)",
	 "s:pmax(timestamp)",
	 KAGG_ACTION__PMAX_INT64, false
	},
	{"max(timestamptz)",
	 "s:max_tstz(bytea)",
	 "s:pmax(timestamptz)",
	 KAGG_ACTION__PMAX_INT64, false
	},
	/*
	 * SUM(X) = SUM(PSUM(X))
	 */
	{"sum(int1)",
	 "s:sum(int8)",
     "s:psum(int8)",
	 KAGG_ACTION__PSUM_INT,  false
	},
	{"sum(int2)",
	 "s:sum(int8)",
     "s:psum(int8)",
	 KAGG_ACTION__PSUM_INT,  false
	},
	{"sum(int4)",
	 "s:sum(int8)",
     "s:psum(int8)",
	 KAGG_ACTION__PSUM_INT,  false
	},
	{"sum(int8)",
	 "c:sum(int8)",
     "s:psum(int8)",
	 KAGG_ACTION__PSUM_INT,  false
	},
	{"sum(float2)",
	 "c:sum(float4)",
	 "s:psum(float4)",
	 KAGG_ACTION__PSUM_FP32, false
	},
	{"sum(float4)",
	 "c:sum(float4)",
	 "s:psum(float4)",
	 KAGG_ACTION__PSUM_FP32, false
	},
	{"sum(float8)",
	 "c:sum(float8)",
	 "s:psum(float8)",
	 KAGG_ACTION__PSUM_FP64, false
	},
	{"sum(numeric)",
	 "s:sum(float8)",
	 "s:psum(float8)",
	 KAGG_ACTION__PSUM_FP64, true
	},
	{"sum(money)",
	 "s:sum_cash(int8)",
	 "s:psum(money)",
	 KAGG_ACTION__PSUM_INT,  false
	},
	/*
	 * AVG(X) = EX_AVG(NROWS(X), PSUM(X))
	 */
	{"avg(int1)",
	 "s:avg_int(bytea)",
	 "s:pavg(int8)",
	 KAGG_ACTION__PAVG_INT, false
	},
	{"avg(int2)",
	 "s:avg_int(bytea)",
	 "s:pavg(int8)",
	 KAGG_ACTION__PAVG_INT, false
	},
	{"avg(int4)",
	 "s:avg_int(bytea)",
	 "s:pavg(int8)",
	 KAGG_ACTION__PAVG_INT, false
	},
	{"avg(int8)",
	 "s:avg_int(bytea)",
	 "s:pavg(int8)",
	 KAGG_ACTION__PAVG_INT, false
	},
	{"avg(float2)",
	 "s:avg_fp(bytea)",
	 "s:pavg(float8)",
	 KAGG_ACTION__PAVG_FP, false
	},
	{"avg(float4)",
	 "s:avg_fp(bytea)",
	 "s:pavg(float8)",
	 KAGG_ACTION__PAVG_FP, false
	},
	{"avg(float8)",
	 "s:avg_fp(bytea)",
	 "s:pavg(float8)",
	 KAGG_ACTION__PAVG_FP, false
	},
	{"avg(numeric)",
	 "s:avg_num(bytea)",
	 "s:pavg(float8)",
	 KAGG_ACTION__PAVG_FP, true
	},
	/*
	 * STDDEV(X) = EX_STDDEV_SAMP(NROWS(),PSUM(X),PSUM(X*X))
	 */
	{"stddev(int1)",
	 "s:stddev_samp(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"stddev(int2)",
	 "s:stddev_samp(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"stddev(int4)",
	 "s:stddev_samp(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"stddev(int8)",
	 "s:stddev_samp(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"stddev(float2)",
	 "s:stddev_sampf(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"stddev(float4)",
	 "s:stddev_sampf(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"stddev(float8)",
	 "s:stddev_sampf(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"stddev(numeric)",
	 "s:stddev_samp(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, true
	},
	/*
	 * STDDEV_SAMP(X) = EX_STDDEV_SAMP(NROWS(),PSUM(X),PSUM(X*X))
	 */
	{"stddev_samp(int1)",
	 "s:stddev_samp(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"stddev_samp(int2)",
	 "s:stddev_samp(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"stddev_samp(int4)",
	 "s:stddev_samp(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"stddev_samp(int8)",
	 "s:stddev_samp(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"stddev_samp(float2)",
	 "s:stddev_sampf(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"stddev_samp(float4)",
	 "s:stddev_sampf(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"stddev_samp(float8)",
	 "s:stddev_sampf(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"stddev_samp(numeric)",
	 "s:stddev_samp(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, true
	},
	/*
	 * STDDEV_POP(X) = EX_STDDEV(NROWS(),PSUM(X),PSUM(X*X))
	 */
	{"stddev_pop(int1)",
	 "s:stddev_pop(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"stddev_pop(int2)",
	 "s:stddev_pop(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"stddev_pop(int4)",
	 "s:stddev_pop(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"stddev_pop(int8)",
	 "s:stddev_pop(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"stddev_pop(float2)",
	 "s:stddev_popf(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"stddev_pop(float4)",
	 "s:stddev_popf(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"stddev_pop(float8)",
	 "s:stddev_popf(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"stddev_pop(numeric)",
	 "s:stddev_pop(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, true
	},
	/*
	 * VARIANCE(X) = VAR_SAMP(NROWS(), PSUM(X),PSUM(X^2))
	 */
	{"variance(int1)",
	 "s:var_samp(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"variance(int2)",
	 "s:var_samp(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"variance(int4)",
	 "s:var_samp(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"variance(int8)",
	 "s:var_samp(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"variance(float2)",
	 "s:var_sampf(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"variance(float4)",
	 "s:var_sampf(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"variance(float8)",
	 "s:var_sampf(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"variance(numeric)",
	 "s:var_samp(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, true
	},
	/*
	 * VAR_SAMP(X) = VAR_SAMP(NROWS(), PSUM(X),PSUM(X^2))
	 */
	{"var_samp(int1)",
	 "s:var_samp(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"var_samp(int2)",
	 "s:var_samp(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"var_samp(int4)",
	 "s:var_samp(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"var_samp(int8)",
	 "s:var_samp(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"var_samp(float2)",
	 "s:var_sampf(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"var_samp(float4)",
	 "s:var_sampf(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"var_samp(float8)",
	 "s:var_sampf(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"var_samp(numeric)",
	 "s:var_samp(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, true
	},
	/*
	 * VAR_POP(X)  = VAR_POP(NROWS(), PSUM(X),PSUM(X^2))
	 */
	{"var_pop(int1)",
	 "s:var_pop(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"var_pop(int2)",
	 "s:var_pop(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"var_pop(int4)",
	 "s:var_pop(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false},
	{"var_pop(int8)",
	 "s:var_pop(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"var_pop(float2)",
	 "s:var_popf(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"var_pop(float4)",
	 "s:var_popf(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"var_pop(float8)",
	 "s:var_popf(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, false
	},
	{"var_pop(numeric)",
	 "s:var_pop(bytea)",
	 "s:pvariance(float8)",
	 KAGG_ACTION__STDDEV, true
	},
	/*
	 * CORR(X,Y) = PGSTROM.CORR(NROWS(X,Y),
	 *                          PCOV_X(X,Y),  PCOV_Y(X,Y)
	 *                          PCOV_X2(X,Y), PCOV_Y2(X,Y),
	 *                          PCOV_XY(X,Y))
	 */
	{"corr(float8,float8)",
	 "s:covar_samp(bytea)",
	 "s:pcovar(float8,float8)",
	 KAGG_ACTION__COVAR, false
	},
	{"covar_samp(float8,float8)",
	 "s:covar_samp(bytea)",
	 "s:pcovar(float8,float8)",
	 KAGG_ACTION__COVAR, false
	},
	{"covar_pop(float8,float8)",
	 "s:covar_pop(bytea)",
	 "s:pcovar(float8,float8)",
	 KAGG_ACTION__COVAR, false
	},
	/*
	 * Aggregation to support least squares method
	 *
	 * That takes PSUM_X, PSUM_Y, PSUM_X2, PSUM_Y2, PSUM_XY according
	 * to the function
	 */
	{"regr_avgx(float8,float8)",
	 "s:regr_avgx(bytea)",
	 "s:pcovar(float8,float8)",
	 KAGG_ACTION__COVAR, false
	},
	{"regr_avgy(float8,float8)",
	 "s:regr_avgy(bytea)",
	 "s:pcovar(float8,float8)",
	 KAGG_ACTION__COVAR, false
	},
	{"regr_count(float8,float8)",
	 "s:regr_count(bytea)",
     "s:pcovar(float8,float8)",
	 KAGG_ACTION__COVAR, false
	},
	{"regr_intercept(float8,float8)",
	 "s:regr_intercept(bytea)",
	 "s:pcovar(float8,float8)",
	 KAGG_ACTION__COVAR, false
	},
	{"regr_r2(float8,float8)",
	 "s:regr_r2(bytea)",
	 "s:pcovar(float8,float8)",
	 KAGG_ACTION__COVAR, false
	},
	{"regr_slope(float8,float8)",
	 "s:regr_slope(bytea)",
     "s:pcovar(float8,float8)",
	 KAGG_ACTION__COVAR, false
	},
	{"regr_sxx(float8,float8)",
	 "s:regr_sxx(bytea)",
	 "s:pcovar(float8,float8)",
	 KAGG_ACTION__COVAR, false
	},
	{"regr_sxy(float8,float8)",
	 "s:regr_sxy(bytea)",
	 "s:pcovar(float8,float8)",
	 KAGG_ACTION__COVAR, false
	},
	{"regr_syy(float8,float8)",
	 "s:regr_syy(bytea)",
	 "s:pcovar(float8,float8)",
	 KAGG_ACTION__COVAR, false
	},
	{ NULL, NULL, NULL, -1, false },
};

/*
 * aggfunc_catalog_entry; hashed catalog entry
 */
typedef struct
{
	Oid		aggfn_oid;
	Oid		final_func_oid;
	Oid		partial_func_oid;
	Oid		partial_func_rettype;
	int		partial_func_nargs;
	int		partial_func_action;
	bool	numeric_aware;
	bool	is_valid_entry;
} aggfunc_catalog_entry;

static HTAB	   *aggfunc_catalog_htable = NULL;

static void
aggfunc_catalog_htable_invalidator(Datum arg, int cacheid, uint32 hashvalue)
{
	hash_destroy(aggfunc_catalog_htable);
	aggfunc_catalog_htable = NULL;
}

static Oid
__aggfunc_resolve_func_signature(const char *signature)
{
	char	   *fn_name = alloca(strlen(signature));
	Oid			fn_namespace;
	oidvector  *fn_argtypes;
	int			fn_nargs = 0;
	Oid			fn_oid;
	Oid			type_oid;
	char	   *base, *tok, *pos;

	if (strncmp(signature, "c:", 2) == 0)
		fn_namespace = PG_CATALOG_NAMESPACE;
	else if (strncmp(signature, "s:", 2) == 0)
		fn_namespace = get_namespace_oid("pgstrom", false);
	else
		elog(ERROR, "wrong function signature: %s", signature);

	strcpy(fn_name, signature + 2);
	base = strchr(fn_name, '(');
	if (!base)
		elog(ERROR, "wrong function signature: %s", signature);
	*base++ = '\0';
	pos = strchr(base, ')');
	if (!pos)
		elog(ERROR, "wrong function signature: %s", signature);
	*pos = '\0';

	fn_argtypes = alloca(offsetof(oidvector, values[80]));
	fn_argtypes->ndim = 1;
	fn_argtypes->dataoffset = 0;
	fn_argtypes->elemtype = OIDOID;
	fn_argtypes->dim1 = 0;
	fn_argtypes->lbound1 = 0;
	for (tok = strtok_r(base, ",", &pos);
		 tok != NULL;
		 tok = strtok_r(NULL, ",", &pos))
	{
		type_oid = GetSysCacheOid2(TYPENAMENSP,
								   Anum_pg_type_oid,
								   CStringGetDatum(tok),
								   ObjectIdGetDatum(PG_CATALOG_NAMESPACE));
		if (!OidIsValid(type_oid))
			elog(ERROR, "cache lookup failed for type '%s'", tok);
		fn_argtypes->values[fn_nargs++] = type_oid;
	}
	fn_argtypes->dim1 = fn_nargs;
	SET_VARSIZE(fn_argtypes, offsetof(oidvector, values[fn_nargs]));

	fn_oid = GetSysCacheOid3(PROCNAMEARGSNSP,
							 Anum_pg_proc_oid,
							 CStringGetDatum(fn_name),
							 PointerGetDatum(fn_argtypes),
							 ObjectIdGetDatum(fn_namespace));
	if (!OidIsValid(fn_oid))
		elog(ERROR, "Catalog corruption? '%s' was not found",
			 funcname_signature_string(fn_name,
									   fn_argtypes->dim1,
									   NIL,
									   fn_argtypes->values));
	return fn_oid;
}

static void
__aggfunc_resolve_partial_func(aggfunc_catalog_entry *entry,
							   const char *partfn_signature,
							   int partfn_action)
{
	Oid		func_oid = __aggfunc_resolve_func_signature(partfn_signature);
	Oid		type_oid;
	int		func_nargs = 1;

	switch (partfn_action)
	{
		case KAGG_ACTION__NROWS_ANY:
		case KAGG_ACTION__NROWS_COND:
		case KAGG_ACTION__PSUM_INT:
			type_oid = INT8OID;
			break;
		case KAGG_ACTION__PSUM_FP32:
			type_oid = FLOAT4OID;
			break;
		case KAGG_ACTION__PSUM_FP64:
			type_oid = FLOAT8OID;
			break;
		case KAGG_ACTION__PMIN_INT32:
		case KAGG_ACTION__PMIN_INT64:
		case KAGG_ACTION__PMIN_FP32:
		case KAGG_ACTION__PMIN_FP64:
		case KAGG_ACTION__PMAX_INT32:
		case KAGG_ACTION__PMAX_INT64:
		case KAGG_ACTION__PMAX_FP32:
		case KAGG_ACTION__PMAX_FP64:
		case KAGG_ACTION__PAVG_INT:
		case KAGG_ACTION__PAVG_FP:
		case KAGG_ACTION__STDDEV:
			type_oid = BYTEAOID;
			break;
		case KAGG_ACTION__COVAR:
			func_nargs = 2;
			type_oid = BYTEAOID;
			break;
		default:
			elog(ERROR, "Catalog corruption? unknown action: %d", partfn_action);
			break;
	}
	entry->partial_func_oid = func_oid;
	entry->partial_func_rettype = get_func_rettype(func_oid);
	entry->partial_func_nargs = get_func_nargs(func_oid);
	entry->partial_func_action = partfn_action;

	if (entry->partial_func_rettype != type_oid ||
		entry->partial_func_nargs != func_nargs)
		elog(ERROR, "Catalog curruption? partial function mismatch: %s",
			 partfn_signature);
}

static void
__aggfunc_resolve_final_func(aggfunc_catalog_entry *entry,
							 const char *finalfn_signature,
							 Oid agg_rettype)
{
	Oid			func_oid = __aggfunc_resolve_func_signature(finalfn_signature);
	HeapTuple	htup;
	Form_pg_proc proc;

	if (!SearchSysCacheExists1(AGGFNOID, ObjectIdGetDatum(func_oid)) ||
		get_func_rettype(func_oid) != agg_rettype)
		elog(ERROR, "Catalog corruption? final function mismatch: %s",
			 format_procedure(func_oid));
	htup = SearchSysCache1(PROCOID, ObjectIdGetDatum(func_oid));
	if (!HeapTupleIsValid(htup))
		elog(ERROR, "cache lookup failed for function %u", func_oid);
	proc = (Form_pg_proc) GETSTRUCT(htup);
	if (proc->pronargs != 1 ||
		proc->proargtypes.dim1 != 1 ||
		proc->proargtypes.values[0] != entry->partial_func_rettype)
		elog(ERROR, "Catalog corruption? final function mismatch: %s",
			 format_procedure(func_oid));
	ReleaseSysCache(htup);

	entry->final_func_oid = func_oid;
}

static const aggfunc_catalog_entry *
aggfunc_catalog_lookup_by_oid(Oid aggfn_oid)
{
	aggfunc_catalog_entry *entry;
	bool		found;

	/* fast path by the hashtable */
	if (!aggfunc_catalog_htable)
	{
		HASHCTL		hctl;

		memset(&hctl, 0, sizeof(HASHCTL));
        hctl.keysize = sizeof(Oid);
		hctl.entrysize = sizeof(aggfunc_catalog_entry);
		hctl.hcxt = CacheMemoryContext;
		aggfunc_catalog_htable = hash_create("XPU GroupBy Catalog Hash",
											 256,
											 &hctl,
											 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	entry = hash_search(aggfunc_catalog_htable,
						&aggfn_oid,
						HASH_ENTER,
						&found);
	if (!found)
	{
		Form_pg_proc proc;
		HeapTuple htup;

		entry->is_valid_entry = false;
		PG_TRY();
		{
			htup = SearchSysCache1(PROCOID, ObjectIdGetDatum(aggfn_oid));
			if (!HeapTupleIsValid(htup))
				elog(ERROR, "cache lookup failed for function %u", aggfn_oid);
			proc = (Form_pg_proc) GETSTRUCT(htup);
			if (proc->pronamespace == PG_CATALOG_NAMESPACE &&
				proc->pronargs <= 2)
			{
				char	buf[3*NAMEDATALEN+100];
				int		off;

				off = sprintf(buf, "%s(", NameStr(proc->proname));
				for (int j=0; j < proc->pronargs; j++)
				{
					Oid		type_oid = proc->proargtypes.values[j];
					char   *type_name = get_type_name(type_oid, false);

					off += sprintf(buf + off, "%s%s",
								   (j>0 ? "," : ""),
								   type_name);
				}
				off += sprintf(buf + off, ")");

				for (int i=0; aggfunc_catalog_array[i].aggfn_signature != NULL; i++)
				{
					const aggfunc_catalog_t *cat = &aggfunc_catalog_array[i];

					if (strcmp(buf, cat->aggfn_signature) == 0)
					{
						__aggfunc_resolve_partial_func(entry,
													   cat->partfn_signature,
													   cat->partfn_action);
						__aggfunc_resolve_final_func(entry,
													 cat->finalfn_signature,
													 proc->prorettype);
						entry->numeric_aware = cat->numeric_aware;
						entry->is_valid_entry = true;
						break;
					}
				}
			}
			ReleaseSysCache(htup);
		}
		PG_CATCH();
		{
			hash_search(aggfunc_catalog_htable, &aggfn_oid, HASH_REMOVE, NULL);
			PG_RE_THROW();
		}
		PG_END_TRY();
	}
	if (!entry->is_valid_entry)
		return NULL;
	if (entry->numeric_aware && !pgstrom_enable_numeric_aggfuncs)
		return NULL;
	return entry;
}

/*
 * xpugroupby_build_path_context
 */
typedef struct
{
	bool		device_executable;
	PlannerInfo	   *root;
	RelOptInfo	   *group_rel;
	double			num_groups;
	Path		   *input_path;
	PathTarget	   *target_upper;
	PathTarget	   *target_partial;
	PathTarget	   *target_final;
	AggClauseCosts	final_clause_costs;
	pgstromPlanInfo *pp_prev;
	List		   *input_rels_tlist;
	List		   *inner_paths_list;
	Node		   *havingQual;
	uint32_t		task_kind;
	const CustomPathMethods *custom_path_methods;
} xpugroupby_build_path_context;

/*
 * make_expr_typecast - constructor of type cast
 */
static Expr *
make_expr_typecast(Expr *expr, Oid target_type)
{
	Oid		source_type = exprType((Node *) expr);
	HeapTuple htup;
	Form_pg_cast cast;

	if (source_type == target_type)
		return expr;

	htup = SearchSysCache2(CASTSOURCETARGET,
						   ObjectIdGetDatum(source_type),
						   ObjectIdGetDatum(target_type));
	if (!HeapTupleIsValid(htup))
		elog(ERROR, "cache lookup failed for cast (%s -> %s)",
			 format_type_be(source_type),
			 format_type_be(target_type));
	cast = (Form_pg_cast) GETSTRUCT(htup);
	if (cast->castmethod == COERCION_METHOD_BINARY)
	{
		RelabelType    *relabel = makeNode(RelabelType);

		relabel->arg = expr;
		relabel->resulttype = target_type;
		relabel->resulttypmod = exprTypmod((Node *) expr);
		relabel->resultcollid = exprCollation((Node *) expr);
		relabel->relabelformat = COERCE_IMPLICIT_CAST;
		relabel->location = -1;

		expr = (Expr *) relabel;
	}
	else if (cast->castmethod == COERCION_METHOD_FUNCTION)
	{
		Assert(OidIsValid(cast->castfunc));
        expr = (Expr *)makeFuncExpr(cast->castfunc,
									target_type,
									list_make1(expr),
									InvalidOid,		/* always right? */
									exprCollation((Node *) expr),
									COERCE_IMPLICIT_CAST);
	}
	else
	{
		elog(ERROR, "cast-method '%c' is not supported in the kernel mode",
			 cast->castmethod);
	}
	ReleaseSysCache(htup);

	return expr;
}

/*
 * make_alternative_aggref
 *
 * It makes an alternative final aggregate function towards the supplied
 * Aggref, and append its arguments on the target_partial/target_device.
 */
static Node *
make_alternative_aggref(xpugroupby_build_path_context *con, Aggref *aggref)
{
	const aggfunc_catalog_entry *aggfn_cat;
	List	   *partfn_args = NIL;
	Expr	   *partfn;
	Aggref	   *aggref_alt;
	Oid			func_oid;
	HeapTuple	htup;
	Form_pg_proc proc;
	Form_pg_aggregate agg;
	ListCell   *lc;
	int			j;

	if (aggref->aggorder != NIL || aggref->aggdistinct != NIL)
	{
		elog(DEBUG2, "Aggregate with ORDER BY/DISTINCT is not supported: %s",
			 nodeToString(aggref));
		return NULL;
	}
	if (AGGKIND_IS_ORDERED_SET(aggref->aggkind))
	{
		elog(DEBUG2, "ORDERED SET Aggregation is not supported: %s",
			 nodeToString(aggref));
		return NULL;
	}

	/*
	 * Lookup properties of aggregate function
	 */
	aggfn_cat = aggfunc_catalog_lookup_by_oid(aggref->aggfnoid);
	if (!aggfn_cat)
	{
		elog(DEBUG2, "Aggregate function '%s' is not device executable",
			 format_procedure(aggref->aggfnoid));
		return NULL;
	}
	/* sanity checks */
	Assert(aggref->aggkind == AGGKIND_NORMAL &&
		   !aggref->aggvariadic);

	/*
	 * Build partial-aggregate function
	 */
	htup = SearchSysCache1(PROCOID, ObjectIdGetDatum(aggfn_cat->partial_func_oid));
	if (!HeapTupleIsValid(htup))
		elog(ERROR, "cache lookup failed for function %u",
			 aggfn_cat->partial_func_oid);
	proc = (Form_pg_proc) GETSTRUCT(htup);
	Assert(list_length(aggref->args) == proc->pronargs);
	j = 0;
	foreach (lc, aggref->args)
	{
		TargetEntry *tle = lfirst(lc);
		Expr   *expr = tle->expr;
		Oid		type_oid = exprType((Node *)expr);
		Oid		dest_oid = proc->proargtypes.values[j++];

		if (type_oid != dest_oid)
			expr = make_expr_typecast(expr, dest_oid);
		if (!pgstrom_xpu_expression(expr,
									con->task_kind,
									con->input_rels_tlist,
									NULL))
		{
			elog(DEBUG2, "Partial aggregate argument is not executable: %s",
				 nodeToString(expr));
			return NULL;
		}
		partfn_args = lappend(partfn_args, expr);

		//try add prep_tlist as input
	}
	ReleaseSysCache(htup);

	partfn = (Expr *)makeFuncExpr(aggfn_cat->partial_func_oid,
								  aggfn_cat->partial_func_rettype,
								  partfn_args,
								  aggref->aggcollid,
								  aggref->inputcollid,
								  COERCE_EXPLICIT_CALL);
	add_new_column_to_pathtarget(con->target_partial, partfn);

	/*
	 * Build final-aggregate function
	 */
	func_oid = aggfn_cat->final_func_oid;
	htup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(func_oid));
	if (!HeapTupleIsValid(htup))
		elog(ERROR, "cache lookup failed for pg_aggregate %u", func_oid);
	agg = (Form_pg_aggregate) GETSTRUCT(htup);

	aggref_alt = makeNode(Aggref);
	aggref_alt->aggfnoid      = func_oid;
	aggref_alt->aggtype       = aggref->aggtype;
	aggref_alt->aggcollid     = aggref->aggcollid;
	aggref_alt->inputcollid   = aggref->inputcollid;
	aggref_alt->aggtranstype  = agg->aggtranstype;
	aggref_alt->aggargtypes   = list_make1_oid(exprType((Node *)partfn));
	aggref_alt->aggdirectargs = NIL;	/* see sanity checks */
	aggref_alt->args          = list_make1(makeTargetEntry(partfn, 1, NULL, false));
	aggref_alt->aggorder      = NIL;  /* see sanity check */
    aggref_alt->aggdistinct   = NIL;  /* see sanity check */
    aggref_alt->aggfilter     = NULL; /* processed in partial-function */
	aggref_alt->aggstar       = false;
	aggref_alt->aggvariadic   = false;
	aggref_alt->aggkind       = AGGKIND_NORMAL;   /* see sanity check */
	aggref_alt->agglevelsup   = 0;
	aggref_alt->aggsplit      = AGGSPLIT_SIMPLE;
	aggref_alt->aggno         = aggref->aggno;
	aggref_alt->aggtransno    = aggref->aggtransno;
	aggref_alt->location      = aggref->location;

	/*
	 * Update the cost factor
	 */
	if (OidIsValid(agg->aggtransfn))
		add_function_cost(con->root,
						  agg->aggtransfn,
						  NULL,
						  &con->final_clause_costs.transCost);
	if (OidIsValid(agg->aggfinalfn))
		add_function_cost(con->root,
						  agg->aggfinalfn,
						  NULL,
						  &con->final_clause_costs.finalCost);
	ReleaseSysCache(htup);

	return (Node *)aggref_alt;
}

static Node *
replace_expression_by_altfunc(Node *node, xpugroupby_build_path_context *con)
{
	PathTarget *target_input = con->input_path->pathtarget;
	Node	   *aggfn;
	ListCell   *lc;

	if (!node)
		return NULL;
	if (IsA(node, Aggref))
	{
		aggfn = make_alternative_aggref(con, (Aggref *)node);
		if (!aggfn)
			con->device_executable = false;
		return aggfn;
	}

	foreach (lc, target_input->exprs)
	{
		Expr   *expr = lfirst(lc);

		if (equal(node, expr))
		{
			add_new_column_to_pathtarget(con->target_partial, copyObject(expr));
			return copyObject(node);
		}
	}
	if (IsA(node, Var) || IsA(node, PlaceHolderVar))
		elog(ERROR, "Bug? referenced variable is grouping-key nor its dependent key: %s",
			 nodeToString(node));
	return expression_tree_mutator(node, replace_expression_by_altfunc, con);
}

static bool
xpugroupby_build_path_target(xpugroupby_build_path_context *con)
{
	PlannerInfo *root = con->root;
	Query	   *parse = root->parse;
	PathTarget *target_upper = con->target_upper;
	Node	   *havingQual = NULL;
	ListCell   *lc;
	int			i;

	/*
	 * Pick up Grouping-Keys and Aggregate-Functions
	 */
	i = 0;
	foreach (lc, target_upper->exprs)
	{
		Expr   *expr = lfirst(lc);
		Index	sortgroupref = get_pathtarget_sortgroupref(target_upper, i);

		if (sortgroupref && parse->groupClause &&
			get_sortgroupref_clause_noerr(sortgroupref,
										  parse->groupClause) != NULL)
		{
			/* Grouping Key */
			devtype_info *dtype;
			Oid		type_oid = exprType((Node *)expr);
			Oid		coll_oid;

			dtype = pgstrom_devtype_lookup(type_oid);
			if (!dtype || !dtype->type_hashfunc)
			{
				elog(DEBUG2, "GROUP BY contains unsupported type (%s): %s",
					 format_type_be(type_oid),
					 nodeToString(expr));
				return false;
			}
			coll_oid = exprCollation((Node *)expr);
			if (devtype_lookup_equal_func(dtype, coll_oid) == NULL)
			{
				elog(DEBUG2, "GROUP BY contains unsupported device type (%s): %s",
					 format_type_be(type_oid),
					 nodeToString(expr));
				return false;
			}
			/*
			 * grouping-key must be device executable.
			 */
			if (!pgstrom_xpu_expression(expr,
										con->task_kind,
										con->input_rels_tlist,
										NULL))
			{
				elog(DEBUG2, "Grouping-key must be device executable: %s",
					 nodeToString(expr));
				return false;
			}
			/* add grouping-key */
			add_column_to_pathtarget(con->target_partial, expr, sortgroupref);
			add_column_to_pathtarget(con->target_final, expr, sortgroupref);
		}
		else
		{
			/* Aggregation */
			Expr   *altfn;

			altfn = (Expr *)replace_expression_by_altfunc((Node *)expr, con);
			if (!altfn)
			{
				elog(DEBUG2, "No alternative aggregation: %s",
					 nodeToString(expr));
				return false;
			}
			if (exprType((Node *)expr) != exprType((Node *)altfn))
			{
				elog(ERROR, "Bug? XpuGroupBy catalog is not consistent: %s --> %s",
					 nodeToString(expr),
					 nodeToString(altfn));
			}
			add_column_to_pathtarget(con->target_final, altfn, 0);
		}
		i++;
	}

	/*
	 * HAVING clause
	 */
	if (parse->havingQual)
	{
		havingQual = replace_expression_by_altfunc(parse->havingQual, con);
		if (!havingQual)
		{
			elog(DEBUG2, "unable to replace HAVING to alternative aggregation: %s",
				 nodeToString(parse->havingQual));
			return false;
		}
	}
	con->havingQual = havingQual;

	set_pathtarget_cost_width(root, con->target_final);
	set_pathtarget_cost_width(root, con->target_partial);

	return true;
}

/*
 * prepend_partial_groupby_custompath
 */
static Path *
prepend_partial_groupby_custompath(xpugroupby_build_path_context *con)
{
	Query	   *parse = con->root->parse;
	CustomPath *cpath = makeNode(CustomPath);
	Path	   *input_path = con->input_path;
	PathTarget *target_partial = con->target_partial;
	pgstromPlanInfo *pp_prev = con->pp_prev;
	pgstromPlanInfo *pp_info;
	double		num_group_keys;
	double		xpu_ratio;
	Cost		xpu_operator_cost;
	Cost		xpu_tuple_cost;
	Cost		startup_cost = 0.0;
	Cost		run_cost = 0.0;
	Cost		final_cost = 0.0;

	/*
	 * Parameters related to devices
	 */
	if ((con->task_kind & DEVKIND__ANY) == DEVKIND__NVIDIA_GPU)
	{
		xpu_operator_cost = pgstrom_gpu_operator_cost;
		xpu_tuple_cost    = pgstrom_gpu_tuple_cost;
		xpu_ratio         = pgstrom_gpu_operator_ratio();
	}
	else if ((con->task_kind & DEVKIND__ANY) == DEVKIND__NVIDIA_DPU)
	{
		xpu_operator_cost = pgstrom_dpu_operator_cost;
        xpu_tuple_cost    = pgstrom_dpu_tuple_cost;
		xpu_ratio         = pgstrom_dpu_operator_ratio();
	}
	else
	{
		elog(ERROR, "Bug? unexpected task_kind: %08x", con->task_kind);
	}
	startup_cost = input_path->startup_cost;
	run_cost = (input_path->total_cost -
				input_path->startup_cost - pp_prev->final_cost);
	/* Cost estimation for grouping */
	num_group_keys = list_length(parse->groupClause);
	startup_cost += (xpu_operator_cost *
					 num_group_keys *
					 input_path->rows);
	/* Cost estimation for aggregate function */
	startup_cost += (target_partial->cost.per_tuple * input_path->rows +
					 target_partial->cost.startup) * xpu_ratio;
	/* Cost estimation to fetch results */
	final_cost = xpu_tuple_cost * con->num_groups;
	if (input_path->parallel_workers > 0)
		final_cost *= (0.5 + (double)input_path->parallel_workers);

	pp_info = pmemdup(pp_prev, offsetof(pgstromPlanInfo,
										inners[pp_prev->num_rels]));
	pp_info->final_cost = final_cost;

	cpath->path.pathtype         = T_CustomScan;
	cpath->path.parent           = input_path->parent;
	cpath->path.pathtarget       = con->target_partial;
	cpath->path.param_info       = input_path->param_info;
	cpath->path.parallel_safe    = input_path->parallel_safe;
	cpath->path.parallel_aware   = input_path->parallel_aware;
	cpath->path.parallel_workers = input_path->parallel_workers;
	cpath->path.rows             = con->num_groups;
	cpath->path.startup_cost     = startup_cost;
	cpath->path.total_cost       = startup_cost + run_cost + final_cost;
	cpath->path.pathkeys         = NIL;
	cpath->custom_paths          = con->inner_paths_list;
	cpath->custom_private        = list_make1(pp_info);
	cpath->methods               = con->custom_path_methods;

	return &cpath->path;
}

/*
 * try_add_final_groupby_paths
 */
static void
try_add_final_groupby_paths(xpugroupby_build_path_context *con,
							Path *part_path)
{
	Query	   *parse = con->root->parse;
	Path	   *agg_path;
	double		hashTableSz;

	if (!parse->groupClause)
	{
		agg_path = (Path *)create_agg_path(con->root,
										   con->group_rel,
										   part_path,
										   con->target_final,
										   AGG_PLAIN,
										   AGGSPLIT_SIMPLE,
										   parse->groupClause,
										   (List *)con->havingQual,
										   &con->final_clause_costs,
										   con->num_groups);
		add_path(con->group_rel, agg_path);
	}
	else
	{
		Assert(grouping_is_hashable(parse->groupClause));
		hashTableSz = estimate_hashagg_tablesize(con->root,
												 part_path,
												 &con->final_clause_costs,
												 con->num_groups);
		if (hashTableSz <= (double)work_mem * 1024.0)
		{
			agg_path = (Path *)create_agg_path(con->root,
											   con->group_rel,
											   part_path,
											   con->target_final,
											   AGG_HASHED,
											   AGGSPLIT_SIMPLE,
											   parse->groupClause,
											   (List *)con->havingQual,
											   &con->final_clause_costs,
											   con->num_groups);
			add_path(con->group_rel, agg_path);
		}
	}
}

static void
__xpugroupby_add_custompath(PlannerInfo *root,
							Path *input_path,
							RelOptInfo *group_rel,
							void *extra,
							bool try_parallel,
							double num_groups,
							uint32_t task_kind,
							const CustomPathMethods *custom_path_methods)
{
	xpugroupby_build_path_context con;
	Path	   *part_path;

	/* setup context */
	memset(&con, 0, sizeof(con));
	con.device_executable = true;
	con.root           = root;
	con.group_rel      = group_rel;
	con.num_groups     = num_groups;
	con.input_path     = input_path;
	con.target_upper   = root->upper_targets[UPPERREL_GROUP_AGG];
	con.target_partial = create_empty_pathtarget();
    con.target_final   = create_empty_pathtarget();
	con.task_kind      = task_kind;
	con.custom_path_methods = custom_path_methods;
	extract_input_path_params(input_path,
							  NULL,
							  &con.pp_prev,
							  &con.input_rels_tlist,
							  &con.inner_paths_list);
	/* construction of the target-list for each level */
	if (!xpugroupby_build_path_target(&con))
		return;

	/* build partial groupby custom-path */
	part_path = prepend_partial_groupby_custompath(&con);

	/* prepend Gather if parallel-aware path */
	if (try_parallel)
	{
		if (part_path->parallel_aware &&
			part_path->parallel_workers > 0)
		{
			double	total_groups = (part_path->rows *
									part_path->parallel_workers);
			part_path = (Path *)create_gather_path(root,
												   group_rel,
												   part_path,
												   con.target_partial,
												   NULL,
												   &total_groups);
		}
		else
		{
			/* unable to inject parallel paths */
			return;
		}
	}
	/* try add final groupby path */
	try_add_final_groupby_paths(&con, part_path);
}

void
xpugroupby_add_custompath(PlannerInfo *root,
						  RelOptInfo *input_rel,
						  RelOptInfo *group_rel,
						  void *extra,
						  uint32_t task_kind,
						  const CustomPathMethods *custom_path_methods)
{
	Query	   *parse = root->parse;
	Path	   *input_path;
	double		num_groups = 1.0;
	ListCell   *lc;

	/* fetch num groups from the standard paths */
	if (parse->groupClause)
	{
		if (parse->groupingSets != NIL ||
			!grouping_is_hashable(parse->groupClause))
		{
			elog(DEBUG2, "GROUP BY clause is not supported form");
			return;
		}

		foreach (lc, group_rel->pathlist)
		{
			Path   *i_path = lfirst(lc);

			if (IsA(i_path, Agg))
			{
				num_groups = ((AggPath *)i_path)->numGroups;
				break;
			}
		}
		if (!lc)
			return;		/* unable to determine the num_groups */
	}
	
	for (int try_parallel=0; try_parallel < 2; try_parallel++)
	{
		if (IS_SIMPLE_REL(input_rel))
		{
			input_path = (Path *)buildXpuScanPath(root,
												  input_rel,
												  (try_parallel > 0),
												  false,
												  true,
												  task_kind);
		}
		else
		{
			input_path = (Path *)custom_path_find_cheapest(root,
														   input_rel,
														   (try_parallel > 0),
														   task_kind);
		}

		if (input_path)
			__xpugroupby_add_custompath(root,
										input_path,
										group_rel,
										extra,
										num_groups,
										(try_parallel > 0),
										task_kind,
										custom_path_methods);
	}
}

/*
 * gpugroupby_add_custompath
 */
static void
gpugroupby_add_custompath(PlannerInfo *root,
						  UpperRelationKind stage,
						  RelOptInfo *input_rel,
						  RelOptInfo *group_rel,
						  void *extra)
{
	if (create_upper_paths_next)
		create_upper_paths_next(root,
								stage,
								input_rel,
								group_rel,
								extra);
	if (stage != UPPERREL_GROUP_AGG)
		return;
	if (!pgstrom_enabled || !pgstrom_enable_gpugroupby)
		return;
	/* add custom-paths */
	xpugroupby_add_custompath(root,
							  input_rel,
							  group_rel,
							  extra,
							  TASK_KIND__GPUGROUPBY,
							  &gpugroupby_path_methods);
}

/*
 * PlanGpuGroupByPath
 */
static Plan *
PlanGpuGroupByPath(PlannerInfo *root,
				   RelOptInfo *joinrel,
				   CustomPath *cpath,
				   List *tlist,
				   List *clauses,
				   List *custom_plans)
{
	pgstromPlanInfo *pp_info = linitial(cpath->custom_private);
	CustomScan	   *cscan;

	cscan = PlanXpuJoinPathCommon(root,
								  joinrel,
								  cpath,
								  tlist,
								  custom_plans,
								  pp_info,
								  &gpugroupby_plan_methods);
	form_pgstrom_plan_info(cscan, pp_info);
	return &cscan->scan.plan;
}

/*
 * CreateGpuGroupByScanState
 */
static Node *
CreateGpuGroupByScanState(CustomScan *cscan)
{
	pgstromTaskState *pts;
	int		num_rels = list_length(cscan->custom_plans);

	Assert(cscan->methods == &gpugroupby_plan_methods);
	pts = palloc0(offsetof(pgstromTaskState, inners[num_rels]));
	NodeSetTag(pts, T_CustomScanState);
	pts->css.flags = cscan->flags;
	pts->css.methods = &gpugroupby_exec_methods;
	pts->task_kind = TASK_KIND__GPUGROUPBY;
	pts->pp_info = deform_pgstrom_plan_info(cscan);
	Assert(pts->pp_info->task_kind == pts->task_kind &&
		   pts->pp_info->num_rels == num_rels);
	pts->num_rels = num_rels;

	return (Node *)pts;
}

/*
 * ExecGpuGroupBy
 */
static TupleTableSlot *
ExecGpuGroupBy(CustomScanState *node)
{
	pgstromTaskState *pts = (pgstromTaskState *)node;

	return pgstromExecTaskState(pts);
}

/*
 * ExecFallbackCpuGroupBy
 */
void
ExecFallbackCpuGroupBy(pgstromTaskState *pts, HeapTuple tuple)
{
	elog(ERROR, "ExecFallbackCpuGroupBy implemented");
}

/*
 * Entrypoint of GpuPreAgg
 */
void
pgstrom_init_gpu_groupby(void)
{
	/* turn on/off gpu_groupby */
	DefineCustomBoolVariable("pg_strom.enable_gpugroupby",
							 "Enables the use of GPU Group-By",
							 NULL,
							 &pgstrom_enable_gpugroupby,
							 true,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	/* pg_strom.enable_numeric_aggfuncs */
	DefineCustomBoolVariable("pg_strom.enable_numeric_aggfuncs",
							 "Enable aggregate functions on numeric type",
							 NULL,
							 &pgstrom_enable_numeric_aggfuncs,
							 true,
							 PGC_USERSET,
							 GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	/* pg_strom.enable_partitionwise_gpugroupby */
	DefineCustomBoolVariable("pg_strom.enable_partitionwise_gpugroupby",
							 "Enabled Enables partition wise GpuGroupBy",
							 NULL,
							 &pgstrom_enable_partitionwise_gpugroupby,
							 true,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	/* pg_strom.hll_registers_bits */
	DefineCustomIntVariable("pg_strom.hll_registers_bits",
							"Accuracy of HyperLogLog COUNT(distinct ...) estimation",
							NULL,
							&pgstrom_hll_register_bits,
							9,
							4,
							15,
							PGC_USERSET,
							GUC_NOT_IN_SAMPLE,
							NULL, NULL, NULL);

	/* initialization of path method table */
	memset(&gpugroupby_path_methods, 0, sizeof(CustomPathMethods));
	gpugroupby_path_methods.CustomName          = "GpuGroupBy";
	gpugroupby_path_methods.PlanCustomPath      = PlanGpuGroupByPath;

	/* initialization of plan method table */
	memset(&gpugroupby_plan_methods, 0, sizeof(CustomScanMethods));
	gpugroupby_plan_methods.CustomName          = "GpuGroupBy";
	gpugroupby_plan_methods.CreateCustomScanState = CreateGpuGroupByScanState;
	RegisterCustomScanMethods(&gpugroupby_plan_methods);

	/* initialization of exec method table */
	memset(&gpugroupby_exec_methods, 0, sizeof(CustomExecMethods));
	gpugroupby_exec_methods.CustomName          = "GpuGroupBy";
	gpugroupby_exec_methods.BeginCustomScan     = pgstromExecInitTaskState;
	gpugroupby_exec_methods.ExecCustomScan      = ExecGpuGroupBy;
	gpugroupby_exec_methods.EndCustomScan       = pgstromExecEndTaskState;
	gpugroupby_exec_methods.ReScanCustomScan    = pgstromExecResetTaskState;
	gpugroupby_exec_methods.EstimateDSMCustomScan = pgstromSharedStateEstimateDSM;
	gpugroupby_exec_methods.InitializeDSMCustomScan = pgstromSharedStateInitDSM;
	gpugroupby_exec_methods.InitializeWorkerCustomScan = pgstromSharedStateAttachDSM;
	gpugroupby_exec_methods.ShutdownCustomScan  = pgstromSharedStateShutdownDSM;
	gpugroupby_exec_methods.ExplainCustomScan   = pgstromExplainTaskState;
	/* hook registration */
	create_upper_paths_next = create_upper_paths_hook;
	create_upper_paths_hook = gpugroupby_add_custompath;

	CacheRegisterSyscacheCallback(PROCOID, aggfunc_catalog_htable_invalidator, 0);
}
