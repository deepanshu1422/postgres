/*-------------------------------------------------------------------------
 *
 * llvmjit_expr.c
 *	  JIT compile expressions.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/jit/llvm/llvmjit_expr.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/DebugInfo.h>

#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/tupconvert.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_type.h"
#include "executor/execdebug.h"
#include "executor/nodeAgg.h"
#include "executor/nodeSubplan.h"
#include "executor/execExpr.h"
#include "funcapi.h"
#include "jit/llvmjit.h"
#include "jit/llvmjit_emit.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_coerce.h"
#include "parser/parsetree.h"
#include "pgstat.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/fmgrtab.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/typcache.h"
#include "utils/xml.h"


typedef struct CompiledExprState
{
	LLVMJitContext *context;
	const char *funcname;
} CompiledExprState;

typedef struct ExprCompileState
{
	ExprState  *state;
	PlanState  *parent;

	LLVMJitContext *context;

	LLVMBuilderRef b;
	LLVMDIBuilderRef b_d;

	LLVMModuleRef mod;
	LLVMValueRef fn;

	LLVMBasicBlockRef b_entry;

	LLVMValueRef v_state;
	LLVMValueRef v_econtext;
	LLVMValueRef v_parent;
	LLVMValueRef v_steps;

	LLVMValueRef v_tmpvaluep;

	LLVMValueRef v_scanslot;
	LLVMValueRef v_scanvalues;
	LLVMValueRef v_outerslot;
	LLVMValueRef v_outervalues;
	LLVMValueRef v_innerslot;
	LLVMValueRef v_innervalues;
	LLVMValueRef v_resultslot;
	LLVMValueRef v_resultvalues;

	union
	{
		/* for nodeAgg related steps */
		struct
		{
			LLVMValueRef state;
			LLVMValueRef tmpcontext_pertup;
			LLVMValueRef aggvalues;
		}			agg;
	}			d;

	LLVMMetadataRef dbg_file;
	LLVMMetadataRef dbg_mod;
	LLVMMetadataRef dbg_cu;
	LLVMMetadataRef dbg_fn;

	LLVMMetadataRef dbg_scope;
} ExprCompileState;


static Datum ExecRunCompiledExpr(ExprState *state, ExprContext *econtext, bool *isNull);

static void expr_set_aggstatep(ExprCompileState *ecs);
static void expr_set_aggtmpcontextp(ExprCompileState *ecs);
static void expr_set_aggvalues(ExprCompileState *ecs);
static void expr_set_scanslot(ExprCompileState *ecs);
static void expr_set_outerslot(ExprCompileState *ecs);
static void expr_set_innerslot(ExprCompileState *ecs);
static void expr_set_resultslot(ExprCompileState *ecs);
static void expr_set_tmpvaluep(ExprCompileState *ecs);

static LLVMValueRef BuildV1Call(ExprCompileState *ecs,
								FunctionCallInfo fcinfo,
								LLVMValueRef *v_fcinfo_isnull);
static LLVMValueRef BuildV1CallFC(ExprCompileState *ecs,
								  FunctionCallInfo fcinfo,
								  LLVMValueRef v_fcinfo,
								  LLVMValueRef *v_fcinfo_isnull);
static void build_EvalXFunc(ExprCompileState *ecs, const char *funcname,
							int opno);
static LLVMValueRef create_LifetimeEnd(LLVMModuleRef mod);


/*
 * JIT compile expression.
 */
bool
llvm_compile_expr(ExprState *state, ExprStateBuilder *esb)
{
	ExprCompileState ecs = {
		.state = state,
		.parent = state->parent
	};

	char	   *funcname;

	LLVMBuilderRef b;
	LLVMTypeRef eval_sig;
	LLVMBasicBlockRef *opblocks;

	instr_time	starttime;
	instr_time	endtime;


	llvm_enter_fatal_on_oom();

	/*
	 * Right now we don't support compiling expressions without a parent, as
	 * we need access to the EState.
	 */
	Assert(ecs.parent);

	/* get or create JIT context */
	if (ecs.parent->state->es_jit)
		ecs.context = (LLVMJitContext *) ecs.parent->state->es_jit;
	else
	{
		ecs.context = llvm_create_context(ecs.parent->state->es_jit_flags);
		ecs.parent->state->es_jit = &ecs.context->base;
	}

	INSTR_TIME_SET_CURRENT(starttime);

	ecs.mod = llvm_mutable_module(ecs.context);

	ecs.b = LLVMCreateBuilder();
	b = ecs.b;

	funcname = llvm_expand_funcname(ecs.context, "evalexpr");

	{
		ecs.b_d = LLVMCreateDIBuilder(ecs.mod);
		ecs.dbg_file =
			LLVMDIBuilderCreateFile(ecs.b_d, funcname,
									strlen(funcname), ".", 1);
		ecs.dbg_cu =
			LLVMDIBuilderCreateCompileUnit(ecs.b_d,
										   LLVMDWARFSourceLanguageC, ecs.dbg_file, "pg", 2, true, NULL, 0, 0,
										   NULL, 0, LLVMDWARFEmissionFull, 0, 0, true);
		ecs.dbg_mod =
			LLVMDIBuilderCreateModule(ecs.b_d, ecs.dbg_mod,
									  "pg", 2,
									  "", 0,
									  NULL, 0,
									  "", 0);
	}

	/* Create the signature and function */
	{
		LLVMTypeRef param_types[3];

		param_types[0] = l_ptr(StructExprState);	/* state */
		param_types[1] = l_ptr(StructExprContext);	/* econtext */
		param_types[2] = l_ptr(TypeParamBool);	/* isnull */

		eval_sig = LLVMFunctionType(TypeSizeT,
									param_types, lengthof(param_types),
									false);
	}

	ecs.fn = LLVMAddFunction(ecs.mod, funcname, eval_sig);

	LLVMSetLinkage(ecs.fn, LLVMExternalLinkage);
	LLVMSetVisibility(ecs.fn, LLVMDefaultVisibility);
	llvm_copy_attributes(AttributeTemplate, ecs.fn);

	{
		int			id = LLVMGetEnumAttributeKindForName("noalias", sizeof("noalias") - 1);
		LLVMAttributeRef attr;

		attr = LLVMCreateEnumAttribute(LLVMGetGlobalContext(), id, 0);

		/*
		 * Cannot currently legally be set on first argument (the expression
		 * itself), there likely are "outgoing" pointers that alias from
		 * within expression steps.
		 */
		LLVMAddAttributeAtIndex(ecs.fn, 2, attr);
		LLVMAddAttributeAtIndex(ecs.fn, 3, attr);
	}

	{
		LLVMMetadataRef Int64Ty =
			LLVMDIBuilderCreateBasicType(ecs.b_d, "Int64", strlen("Int64"), 64, 0, LLVMDIFlagZero);
		LLVMMetadataRef Int8Ty =
			LLVMDIBuilderCreateBasicType(ecs.b_d, "Int8", strlen("Int8"), 64, 0, LLVMDIFlagZero);

		LLVMMetadataRef ParamTypes[] = {Int64Ty, Int64Ty, Int64Ty, Int8Ty};

		LLVMMetadataRef FunctionTy = LLVMDIBuilderCreateSubroutineType(ecs.b_d, ecs.dbg_file, ParamTypes, 4, 0);

		ecs.dbg_fn = LLVMDIBuilderCreateFunction(ecs.b_d, ecs.dbg_file,
												 funcname, strlen(funcname),
												 funcname, strlen(funcname),
												 ecs.dbg_file, 0, FunctionTy,
												 false, true, 0, 0, true);

		ecs.b_entry = LLVMAppendBasicBlock(ecs.fn, "entry");
		LLVMPositionBuilderAtEnd(b, ecs.b_entry);

		{
			LLVMMetadataRef FooParamLocation =
				LLVMDIBuilderCreateDebugLocation(LLVMGetGlobalContext(), 0, 0,
												 ecs.dbg_fn, NULL);
			LLVMMetadataRef FooParamExpression =
				LLVMDIBuilderCreateExpression(ecs.b_d, NULL, 0);
			LLVMMetadataRef FooParamVar1 =
				LLVMDIBuilderCreateParameterVariable(ecs.b_d, ecs.dbg_fn, "state", strlen("state"), 1,
													 ecs.dbg_file, 0, Int64Ty, true, 0);
			LLVMMetadataRef FooParamVar2 =
				LLVMDIBuilderCreateParameterVariable(ecs.b_d, ecs.dbg_fn, "econtext", strlen("econtext"), 2,
													 ecs.dbg_file, 0, Int64Ty, true, 0);
			LLVMMetadataRef FooParamVar3 =
				LLVMDIBuilderCreateParameterVariable(ecs.b_d, ecs.dbg_fn, "isNull", strlen("isNull"), 3,
													 ecs.dbg_file, 0, Int8Ty, true, 0);
			LLVMDIBuilderInsertDeclareAtEnd(ecs.b_d, LLVMConstInt(LLVMInt64Type(), 0, false),
											FooParamVar1, FooParamExpression,
											FooParamLocation, ecs.b_entry);
			LLVMDIBuilderInsertDeclareAtEnd(ecs.b_d, LLVMConstInt(LLVMInt64Type(), 0, false),
											FooParamVar2, FooParamExpression,
											FooParamLocation, ecs.b_entry);
			LLVMDIBuilderInsertDeclareAtEnd(ecs.b_d, LLVMConstInt(LLVMInt8Type(), 0, false),
											FooParamVar3, FooParamExpression,
											FooParamLocation, ecs.b_entry);
		}

		LLVMSetSubprogram(ecs.fn, ecs.dbg_fn);
	}

	ecs.v_state = LLVMGetParam(ecs.fn, 0);
	ecs.v_econtext = LLVMGetParam(ecs.fn, 1);

	{
		LLVMMetadataRef l2;
#if 0
		{
			LLVMMetadataRef l = LLVMDIBuilderCreateDebugLocation(LLVMGetGlobalContext(), 0,
																 0, ecs.dbg_fn, NULL);
			LLVMSetCurrentDebugLocation2(b, l);
		}
#endif
		ecs.dbg_scope =
			LLVMDIBuilderCreateLexicalBlock(ecs.b_d, ecs.dbg_fn, ecs.dbg_file, 0, 0);

		l2 = LLVMDIBuilderCreateDebugLocation(LLVMGetGlobalContext(), 0,
															  0, ecs.dbg_file, NULL);
		LLVMSetCurrentDebugLocation2(b, l2);
	}

	{
		LLVMMetadataRef f = LLVMDIBuilderCreateFile(ecs.b_d, "setup", strlen("setup"),
													"", sizeof("") - 1);
		LLVMMetadataRef SetupBlock =
			LLVMDIBuilderCreateLexicalBlock(ecs.b_d, ecs.dbg_scope, f, 1, 1);

		LLVMMetadataRef l2 = LLVMDIBuilderCreateDebugLocation(LLVMGetGlobalContext(), 1,
															  1, SetupBlock, NULL);
		LLVMSetCurrentDebugLocation2(b, l2);
	}

	ecs.v_steps = l_load_struct_gep(b, ecs.v_state,
									FIELDNO_EXPRSTATE_STEPS,
									"v.state.steps");

	/* allocate blocks for each op upfront, so we can do jumps easily */
	opblocks = palloc(sizeof(LLVMBasicBlockRef) * state->steps_final_len);
	for (int opno = 0; opno < state->steps_final_len; opno++)
	{
		ExprEvalOp	opcode = ExecEvalStepOp(state, &state->steps[opno]);

		opblocks[opno] = l_bb_append_v(ecs.fn, "b.op.%d.%s.start",
									   opno, ExprOpToString(opcode));
	}

	/*
	 * We only build a jump from the entry block after building the rest of
	 * the function, making it easier to append to the entry block.
	 */

	for (int opno = 0; opno < state->steps_final_len; opno++)
	{
		const ExprEvalStep *op = &state->steps[opno];
		ExprEvalOp	opcode = ExecEvalStepOp(state, op);
		const char *opcode_name = ExprOpToString(opcode);
		LLVMValueRef v_opno = l_int32_const(opno);
		LLVMValueRef v_resultp;
		LLVMValueRef v_resvaluep;
		LLVMValueRef v_resnullp;
		LLVMMetadataRef l;

		{
			char *opname = psprintf("step %u: opcode:%d/%s", opno, opcode, opcode_name);

			LLVMMetadataRef f = LLVMDIBuilderCreateFile(ecs.b_d, opname, strlen(opname),
														"", sizeof("") - 1);
			LLVMMetadataRef OpLexicalBlock =
				LLVMDIBuilderCreateLexicalBlock(ecs.b_d, ecs.dbg_scope, f, opno + 5, 2);

			l = LLVMDIBuilderCreateDebugLocation(LLVMGetGlobalContext(), opno + 7,
																 opno + 7, OpLexicalBlock, NULL);
			LLVMPositionBuilderAtEnd(b, opblocks[opno]);
			LLVMSetCurrentDebugLocation2(b, l);
		}

		if (op->result == NULL)
		{
			v_resultp = NULL;
			v_resvaluep = NULL;
			v_resnullp = NULL;
		}
		else
		{
			v_resultp = l_ptr_const(op->result, l_ptr(StructNullableDatum));
			v_resvaluep = LLVMBuildStructGEP(b, v_resultp,
											 FIELDNO_NULLABLE_DATUM_DATUM,
											 "");
			v_resnullp = LLVMBuildStructGEP(b, v_resultp,
											FIELDNO_NULLABLE_DATUM_ISNULL,
											"");
		}

		switch (opcode)
		{
			case EEOP_DONE_RETURN:
				{
					LLVMValueRef v_tmpisnull;
					LLVMValueRef v_tmpvalue;

					expr_set_tmpvaluep(&ecs);
					v_tmpvalue = l_load_struct_gep(b, ecs.v_tmpvaluep,
												   FIELDNO_NULLABLE_DATUM_DATUM,
												   "");
					v_tmpisnull = l_load_struct_gep(b, ecs.v_tmpvaluep,
													FIELDNO_NULLABLE_DATUM_ISNULL,
													"");
					v_tmpisnull =
						LLVMBuildTrunc(b, v_tmpisnull, TypeParamBool, "");

					LLVMBuildStore(b, v_tmpisnull, LLVMGetParam(ecs.fn, 2));

					LLVMBuildRet(b, v_tmpvalue);
					break;
				}

			case EEOP_DONE_NO_RETURN:
				LLVMBuildRet(b, l_sizet_const(0));
				break;

			case EEOP_INNER_FETCHSOME:
			case EEOP_OUTER_FETCHSOME:
			case EEOP_SCAN_FETCHSOME:
				{
					TupleDesc	desc = NULL;
					LLVMValueRef v_slot;
					LLVMBasicBlockRef b_fetch;
					LLVMValueRef v_nvalid;
					LLVMValueRef l_jit_deform = NULL;
					const TupleTableSlotOps *tts_ops = NULL;

					b_fetch =
						l_bb_before_v(opblocks[opno + 1], "op.%d.%s.fetch",
									  opno, opcode_name);

					if (op->d.fetch.known_desc)
						desc = op->d.fetch.known_desc;

					if (op->d.fetch.fixed)
						tts_ops = op->d.fetch.kind;

					/* step should not have been generated */
					Assert(tts_ops != &TTSOpsVirtual);

					if (opcode == EEOP_INNER_FETCHSOME)
					{
						expr_set_innerslot(&ecs);
						v_slot = ecs.v_innerslot;
					}
					else if (opcode == EEOP_OUTER_FETCHSOME)
					{
						expr_set_outerslot(&ecs);
						v_slot = ecs.v_outerslot;
					}
					else
					{
						expr_set_scanslot(&ecs);
						v_slot = ecs.v_scanslot;
					}

					/*
					 * Check if all required attributes are available, or
					 * whether deforming is required.
					 */
					v_nvalid =
						l_load_struct_gep(b, v_slot,
										  FIELDNO_TUPLETABLESLOT_NVALID,
										  "");
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntUGE, v_nvalid,
												  l_int16_const(op->d.fetch.last_var),
												  ""),
									opblocks[opno + 1], b_fetch);

					LLVMPositionBuilderAtEnd(b, b_fetch);

					/*
					 * If the tupledesc of the to-be-deformed tuple is known,
					 * and JITing of deforming is enabled, build deform
					 * function specific to tupledesc and the exact number of
					 * to-be-extracted attributes.
					 */
					if (tts_ops && desc && (ecs.context->base.flags & PGJIT_DEFORM))
					{
						l_jit_deform =
							slot_compile_deform(ecs.context, desc,
												tts_ops,
												op->d.fetch.last_var);
					}

					if (l_jit_deform)
					{
						LLVMValueRef params[1];

						params[0] = v_slot;

						LLVMBuildCall(b, l_jit_deform,
									  params, lengthof(params), "");
					}
					else
					{
						LLVMValueRef params[2];

						params[0] = v_slot;
						params[1] = l_int32_const(op->d.fetch.last_var);

						LLVMBuildCall(b,
									  llvm_get_decl(ecs.mod, FuncSlotGetsomeattrsInt),
									  params, lengthof(params), "");
					}

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_INNER_VAR:
			case EEOP_OUTER_VAR:
			case EEOP_SCAN_VAR:
				{
					LLVMValueRef v_value;
					LLVMValueRef v_attnum;
					LLVMValueRef v_values;

					if (opcode == EEOP_INNER_VAR)
					{
						expr_set_innerslot(&ecs);
						v_values = ecs.v_innervalues;
					}
					else if (opcode == EEOP_OUTER_VAR)
					{
						expr_set_outerslot(&ecs);
						v_values = ecs.v_outervalues;
					}
					else
					{
						expr_set_scanslot(&ecs);
						v_values = ecs.v_scanvalues;
					}

					v_attnum = l_int32_const(op->d.var.attnum);
					v_value = l_load_gep1(b, v_values, v_attnum, "");
					LLVMBuildStore(b, v_value, v_resultp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_INNER_SYSVAR:
			case EEOP_OUTER_SYSVAR:
			case EEOP_SCAN_SYSVAR:
				{
					LLVMValueRef v_slot;
					LLVMValueRef v_params[4];
					LLVMValueRef v_opp = LLVMBuildGEP(b, ecs.v_steps, &v_opno, 1, "");

					if (opcode == EEOP_INNER_SYSVAR)
					{
						expr_set_innerslot(&ecs);
						v_slot = ecs.v_innerslot;
					}
					else if (opcode == EEOP_OUTER_SYSVAR)
					{
						expr_set_outerslot(&ecs);
						v_slot = ecs.v_outerslot;
					}
					else
					{
						expr_set_scanslot(&ecs);
						v_slot = ecs.v_scanslot;
					}

					v_params[0] = ecs.v_state;
					v_params[1] = v_opp;
					v_params[2] = ecs.v_econtext;
					v_params[3] = v_slot;

					LLVMBuildCall(b,
								  llvm_get_decl(ecs.mod, FuncExecEvalSysVar),
								  v_params, lengthof(v_params), "");

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_WHOLEROW:
				build_EvalXFunc(&ecs, "ExecEvalWholeRowVar", opno);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_ASSIGN_INNER_VAR:
			case EEOP_ASSIGN_OUTER_VAR:
			case EEOP_ASSIGN_SCAN_VAR:
				{
					LLVMValueRef v_value;
					LLVMValueRef v_rvaluep;
					LLVMValueRef v_attnum;
					LLVMValueRef v_resultnum;
					LLVMValueRef v_values;

					if (opcode == EEOP_ASSIGN_INNER_VAR)
					{
						expr_set_innerslot(&ecs);
						v_values = ecs.v_innervalues;
					}
					else if (opcode == EEOP_ASSIGN_OUTER_VAR)
					{
						expr_set_outerslot(&ecs);
						v_values = ecs.v_outervalues;
					}
					else
					{
						expr_set_scanslot(&ecs);
						v_values = ecs.v_scanvalues;
					}

					expr_set_resultslot(&ecs);

					/* load data */
					v_attnum = l_int32_const(op->d.assign_var.attnum);
					v_value = l_load_gep1(b, v_values, v_attnum, "");

					/* compute addresses of targets */
					v_resultnum = l_int32_const(op->d.assign_var.resultnum);
					v_rvaluep = LLVMBuildGEP(b, ecs.v_resultvalues,
											 &v_resultnum, 1, "");

					/* and store */
					LLVMBuildStore(b, v_value, v_rvaluep);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_ASSIGN_TMP:
				{
					LLVMValueRef v_value;
					LLVMValueRef v_rvaluep;
					LLVMValueRef v_resultnum;
					size_t		resultnum = op->d.assign_tmp.resultnum;

					expr_set_resultslot(&ecs);

					/* load data */
					expr_set_tmpvaluep(&ecs);
					v_value = LLVMBuildLoad(b, ecs.v_tmpvaluep, "");

					/* compute address of target */
					v_resultnum = l_int32_const(resultnum);
					v_rvaluep =
						LLVMBuildGEP(b, ecs.v_resultvalues, &v_resultnum, 1, "");

					/* and store */
					LLVMBuildStore(b, v_value, v_rvaluep);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_ASSIGN_TMP_MAKE_RO:
				{
					LLVMBasicBlockRef b_notnull;
					LLVMValueRef v_params[1];
					LLVMValueRef v_ret;
					LLVMValueRef v_isnull;
					LLVMValueRef v_rvaluep;
					LLVMValueRef v_resultnum;
					size_t		resultnum = op->d.assign_tmp.resultnum;

					expr_set_resultslot(&ecs);

					b_notnull = l_bb_before_v(opblocks[opno + 1],
											  "op.%d.%s.assign_tmp.notnull",
											  opno, opcode_name);

					/* load data */
					expr_set_tmpvaluep(&ecs);
					v_isnull = l_load_struct_gep(b, ecs.v_tmpvaluep,
												 FIELDNO_NULLABLE_DATUM_ISNULL,
												 "");

					/* compute address of target */
					v_resultnum = l_int32_const(resultnum);
					v_rvaluep = LLVMBuildGEP(b, ecs.v_resultvalues, &v_resultnum, 1, "");


					/* store nullness */
					LLVMBuildStore(b, v_isnull,
								   LLVMBuildStructGEP(b, v_rvaluep,
													  FIELDNO_NULLABLE_DATUM_ISNULL,
													  ""));

					/* check if value is NULL */
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_isnull,
												  l_sbool_const(0), ""),
									b_notnull, opblocks[opno + 1]);

					/* if value is not null, convert to RO datum */
					LLVMPositionBuilderAtEnd(b, b_notnull);
					v_params[0] =
						l_load_struct_gep(b, ecs.v_tmpvaluep,
										  FIELDNO_NULLABLE_DATUM_DATUM,
										  "");
					v_ret =
						LLVMBuildCall(b,
									  llvm_get_decl(ecs.mod, FuncMakeExpandedObjectReadOnlyInternal),
									  v_params, lengthof(v_params), "");

					/* store value */
					LLVMBuildStore(b, v_ret,
								   LLVMBuildStructGEP(b, v_rvaluep,
													  FIELDNO_NULLABLE_DATUM_DATUM,
													  ""));

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_CONST:
				{
					LLVMValueRef v_constvalue;

					v_constvalue = l_nullable_datum_const(&op->d.constval.value);

					LLVMBuildStore(b, v_constvalue, v_resultp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_FUNCEXPR_STRICT:
			case EEOP_FUNCEXPR_STRICT_1:
			case EEOP_FUNCEXPR_STRICT_2:
				{
					FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
					LLVMBasicBlockRef b_nonull;
					LLVMValueRef v_fcinfo;
					LLVMBasicBlockRef *b_checkargnulls;
					LLVMValueRef v_fcinfo_isnull;
					LLVMValueRef v_retval;

					/*
					 * Block for the actual function call, if args are
					 * non-NULL.
					 */
					b_nonull = l_bb_before_v(opblocks[opno + 1],
											 "b.%d.%s.no-null-args",
											 opno, opcode_name);

					/* should make sure they're optimized beforehand */
					if (op->d.func.nargs == 0)
						elog(ERROR, "argumentless strict functions are pointless");

					v_fcinfo =
						l_ptr_const(fcinfo, l_ptr(StructFunctionCallInfoData));

					/*
					 * set result->isnull to true, if the function is actually
					 * called, it'll be reset
					 */
					LLVMBuildStore(b, l_sbool_const(1), v_resnullp);

					/* create blocks for checking args, one for each */
					b_checkargnulls =
						palloc(sizeof(LLVMBasicBlockRef *) * op->d.func.nargs);
					for (int argno = 0; argno < op->d.func.nargs; argno++)
						b_checkargnulls[argno] =
							l_bb_before_v(b_nonull, "b.%d.%s.isnull.%d",
										  opno, opcode_name, argno);

					/* jump to check of first argument */
					LLVMBuildBr(b, b_checkargnulls[0]);

					/* check each arg for NULLness */
					for (int argno = 0; argno < op->d.func.nargs; argno++)
					{
						LLVMValueRef v_argisnull;
						LLVMBasicBlockRef b_argnotnull;

						LLVMPositionBuilderAtEnd(b, b_checkargnulls[argno]);

						/* compute block to jump to if argument is not null */
						if (argno + 1 == op->d.func.nargs)
							b_argnotnull = b_nonull;
						else
							b_argnotnull = b_checkargnulls[argno + 1];

						/* and finally load & check NULLness of arg */
						v_argisnull = l_funcnull(b, v_fcinfo, argno);
						LLVMBuildCondBr(b,
										LLVMBuildICmp(b, LLVMIntEQ,
													  v_argisnull,
													  l_sbool_const(1),
													  ""),
										opblocks[opno + 1],
										b_argnotnull);
					}

					LLVMPositionBuilderAtEnd(b, b_nonull);


					v_retval = BuildV1CallFC(&ecs, fcinfo, v_fcinfo,
											 &v_fcinfo_isnull);
					LLVMBuildStore(b, v_retval, v_resvaluep);
					LLVMBuildStore(b, v_fcinfo_isnull, v_resnullp);

					LLVMBuildBr(b, opblocks[opno + 1]);
				}
				break;

			case EEOP_FUNCEXPR:
				{
					FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
					LLVMValueRef v_fcinfo_isnull;
					LLVMValueRef v_retval;

					v_retval = BuildV1Call(&ecs, fcinfo,
										   &v_fcinfo_isnull);
					LLVMBuildStore(b, v_retval, v_resvaluep);
					LLVMBuildStore(b, v_fcinfo_isnull, v_resnullp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_FUNCEXPR_FUSAGE:
				build_EvalXFunc(&ecs, "ExecEvalFuncExprFusage", opno);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;


			case EEOP_FUNCEXPR_STRICT_FUSAGE:
				build_EvalXFunc(&ecs, "ExecEvalFuncExprStrictFusage", opno);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_BOOL_AND_STEP_FIRST:
				{
					LLVMValueRef v_boolanynullp;

					v_boolanynullp = l_ptr_const(op->d.boolexpr.anynull,
												 l_ptr(TypeStorageBool));
					LLVMBuildStore(b, l_sbool_const(0), v_boolanynullp);

				}
				/* FALLTHROUGH */

				/*
				 * Treat them the same for now, optimizer can remove
				 * redundancy. Could be worthwhile to optimize during emission
				 * though.
				 */
			case EEOP_BOOL_AND_STEP_LAST:
			case EEOP_BOOL_AND_STEP:
				{
					LLVMValueRef v_boolvalue;
					LLVMValueRef v_boolnull;
					LLVMValueRef v_boolanynullp,
								v_boolanynull;
					LLVMBasicBlockRef b_boolisnull;
					LLVMBasicBlockRef b_boolcheckfalse;
					LLVMBasicBlockRef b_boolisfalse;
					LLVMBasicBlockRef b_boolcont;
					LLVMBasicBlockRef b_boolisanynull;

					b_boolisnull =
						l_bb_before_v(opblocks[opno + 1],
									  "b.%d.%s.boolisnull",
									  opno, opcode_name);
					b_boolcheckfalse =
						l_bb_before_v(opblocks[opno + 1],
									  "b.%d.%s.boolcheckfalse",
									  opno, opcode_name);
					b_boolisfalse =
						l_bb_before_v(opblocks[opno + 1],
									  "b.%d.%s.boolisfalse",
									  opno, opcode_name);
					b_boolisanynull =
						l_bb_before_v(opblocks[opno + 1],
									  "b.%d.%s.boolisanynull",
									  opno, opcode_name);
					b_boolcont =
						l_bb_before_v(opblocks[opno + 1],
									  "b.%d.%s.boolcont",
									  opno, opcode_name);

					v_boolanynullp = l_ptr_const(op->d.boolexpr.anynull,
												 l_ptr(TypeStorageBool));

					v_boolnull = LLVMBuildLoad(b, v_resnullp, "");
					v_boolvalue = LLVMBuildLoad(b, v_resvaluep, "");

					/* set result->isnull to boolnull */
					LLVMBuildStore(b, v_boolnull, v_resnullp);
					/* set result->value to boolvalue */
					LLVMBuildStore(b, v_boolvalue, v_resvaluep);

					/* check if current input is NULL */
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_boolnull,
												  l_sbool_const(1), ""),
									b_boolisnull,
									b_boolcheckfalse);

					/* build block that sets anynull */
					LLVMPositionBuilderAtEnd(b, b_boolisnull);
					/* set boolanynull to true */
					LLVMBuildStore(b, l_sbool_const(1), v_boolanynullp);
					/* and jump to next block */
					LLVMBuildBr(b, b_boolcont);

					/* build block checking for false */
					LLVMPositionBuilderAtEnd(b, b_boolcheckfalse);
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_boolvalue,
												  l_sizet_const(0), ""),
									b_boolisfalse,
									b_boolcont);

					/*
					 * Build block handling FALSE. Value is false, so short
					 * circuit.
					 */
					LLVMPositionBuilderAtEnd(b, b_boolisfalse);
					/* result is already set to FALSE, need not change it */
					/* and jump to the end of the AND expression */
					LLVMBuildBr(b, opblocks[op->d.boolexpr.jumpdone]);

					/* Build block that continues if bool is TRUE. */
					LLVMPositionBuilderAtEnd(b, b_boolcont);

					v_boolanynull = LLVMBuildLoad(b, v_boolanynullp, "");

					/* set value to NULL if any previous values were NULL */
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_boolanynull,
												  l_sbool_const(0), ""),
									opblocks[opno + 1], b_boolisanynull);

					LLVMPositionBuilderAtEnd(b, b_boolisanynull);
					/* set result->isnull to true */
					LLVMBuildStore(b, l_sbool_const(1), v_resnullp);
					/* reset result->value */
					LLVMBuildStore(b, l_sizet_const(0), v_resvaluep);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}
			case EEOP_BOOL_OR_STEP_FIRST:
				{
					LLVMValueRef v_boolanynullp;

					v_boolanynullp = l_ptr_const(op->d.boolexpr.anynull,
												 l_ptr(TypeStorageBool));
					LLVMBuildStore(b, l_sbool_const(0), v_boolanynullp);
				}
				/* FALLTHROUGH */

				/*
				 * Treat them the same for now, optimizer can remove
				 * redundancy. Could be worthwhile to optimize during emission
				 * though.
				 */
			case EEOP_BOOL_OR_STEP_LAST:
			case EEOP_BOOL_OR_STEP:
				{
					LLVMValueRef v_boolvalue;
					LLVMValueRef v_boolnull;
					LLVMValueRef v_boolanynullp,
								v_boolanynull;

					LLVMBasicBlockRef b_boolisnull;
					LLVMBasicBlockRef b_boolchecktrue;
					LLVMBasicBlockRef b_boolistrue;
					LLVMBasicBlockRef b_boolcont;
					LLVMBasicBlockRef b_boolisanynull;

					b_boolisnull =
						l_bb_before_v(opblocks[opno + 1],
									  "b.%d.%s.boolisnull",
									  opno, opcode_name);
					b_boolchecktrue =
						l_bb_before_v(opblocks[opno + 1],
									  "b.%d.%s.boolchecktrue",
									  opno, opcode_name);
					b_boolistrue =
						l_bb_before_v(opblocks[opno + 1],
									  "b.%d.%s.boolistrue",
									  opno, opcode_name);
					b_boolisanynull =
						l_bb_before_v(opblocks[opno + 1],
									  "b.%d.%s.boolisanynull",
									  opno, opcode_name);
					b_boolcont =
						l_bb_before_v(opblocks[opno + 1],
									  "b.%d.%s.boolcont",
									  opno, opcode_name);

					v_boolanynullp = l_ptr_const(op->d.boolexpr.anynull,
												 l_ptr(TypeStorageBool));

					v_boolnull = LLVMBuildLoad(b, v_resnullp, "");
					v_boolvalue = LLVMBuildLoad(b, v_resvaluep, "");

					/* set result->isnull to boolnull */
					LLVMBuildStore(b, v_boolnull, v_resnullp);
					/* set result->value to boolvalue */
					LLVMBuildStore(b, v_boolvalue, v_resvaluep);

					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_boolnull,
												  l_sbool_const(1), ""),
									b_boolisnull,
									b_boolchecktrue);

					/* build block that sets anynull */
					LLVMPositionBuilderAtEnd(b, b_boolisnull);
					/* set boolanynull to true */
					LLVMBuildStore(b, l_sbool_const(1), v_boolanynullp);
					/* and jump to next block */
					LLVMBuildBr(b, b_boolcont);

					/* build block checking for true */
					LLVMPositionBuilderAtEnd(b, b_boolchecktrue);
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_boolvalue,
												  l_sizet_const(1), ""),
									b_boolistrue,
									b_boolcont);

					/*
					 * Build block handling True. Value is true, so short
					 * circuit.
					 */
					LLVMPositionBuilderAtEnd(b, b_boolistrue);
					/* result is already set to TRUE, need not change it */
					/* and jump to the end of the OR expression */
					LLVMBuildBr(b, opblocks[op->d.boolexpr.jumpdone]);

					/* build block that continues if bool is FALSE */
					LLVMPositionBuilderAtEnd(b, b_boolcont);

					v_boolanynull = LLVMBuildLoad(b, v_boolanynullp, "");

					/* set value to NULL if any previous values were NULL */
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_boolanynull,
												  l_sbool_const(0), ""),
									opblocks[opno + 1], b_boolisanynull);

					LLVMPositionBuilderAtEnd(b, b_boolisanynull);
					/* set result->isnull to true */
					LLVMBuildStore(b, l_sbool_const(1), v_resnullp);
					/* reset result->isnull */
					LLVMBuildStore(b, l_sizet_const(0), v_resvaluep);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_BOOL_NOT_STEP:
				{
					LLVMValueRef v_boolvalue;
					LLVMValueRef v_boolnull;
					LLVMValueRef v_negbool;

					v_boolnull = LLVMBuildLoad(b, v_resnullp, "");
					v_boolvalue = LLVMBuildLoad(b, v_resvaluep, "");

					v_negbool = LLVMBuildZExt(b,
											  LLVMBuildICmp(b, LLVMIntEQ,
															v_boolvalue,
															l_sizet_const(0),
															""),
											  TypeSizeT, "");
					/* set result->isnull to boolnull */
					LLVMBuildStore(b, v_boolnull, v_resnullp);
					/* set result->value to !boolvalue */
					LLVMBuildStore(b, v_negbool, v_resvaluep);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_QUAL:
				{
					LLVMValueRef v_resnull;
					LLVMValueRef v_resvalue;
					LLVMValueRef v_nullorfalse;
					LLVMBasicBlockRef b_qualfail;

					b_qualfail = l_bb_before_v(opblocks[opno + 1],
											   "op.%d.%s.qualfail",
											   opno, opcode_name);

					v_resvalue = LLVMBuildLoad(b, v_resvaluep, "");
					v_resnull = LLVMBuildLoad(b, v_resnullp, "");

					v_nullorfalse =
						LLVMBuildOr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_resnull,
												  l_sbool_const(1), ""),
									LLVMBuildICmp(b, LLVMIntEQ, v_resvalue,
												  l_sizet_const(0), ""),
									"");

					LLVMBuildCondBr(b,
									v_nullorfalse,
									b_qualfail,
									opblocks[opno + 1]);

					/* build block handling NULL or false */
					LLVMPositionBuilderAtEnd(b, b_qualfail);
					/* set result->isnull to false */
					LLVMBuildStore(b, l_sbool_const(0), v_resnullp);
					/* set result->value to false */
					LLVMBuildStore(b, l_sizet_const(0), v_resvaluep);
					/* and jump out */
					LLVMBuildBr(b, opblocks[op->d.qualexpr.jumpdone]);
					break;
				}

			case EEOP_JUMP:
				{
					LLVMBuildBr(b, opblocks[op->d.jump.jumpdone]);
					break;
				}

			case EEOP_JUMP_IF_NULL:
				{
					LLVMValueRef v_resnull;

					/* Transfer control if current result is null */

					v_resnull = LLVMBuildLoad(b, v_resnullp, "");

					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_resnull,
												  l_sbool_const(1), ""),
									opblocks[op->d.jump.jumpdone],
									opblocks[opno + 1]);
					break;
				}

			case EEOP_JUMP_IF_NOT_NULL:
				{
					LLVMValueRef v_resnull;

					/* Transfer control if current result is non-null */

					v_resnull = LLVMBuildLoad(b, v_resnullp, "");

					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_resnull,
												  l_sbool_const(0), ""),
									opblocks[op->d.jump.jumpdone],
									opblocks[opno + 1]);
					break;
				}


			case EEOP_JUMP_IF_NOT_TRUE:
				{
					LLVMValueRef v_resnull;
					LLVMValueRef v_resvalue;
					LLVMValueRef v_nullorfalse;

					/* Transfer control if current result is null or false */

					v_resvalue = LLVMBuildLoad(b, v_resvaluep, "");
					v_resnull = LLVMBuildLoad(b, v_resnullp, "");

					v_nullorfalse =
						LLVMBuildOr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_resnull,
												  l_sbool_const(1), ""),
									LLVMBuildICmp(b, LLVMIntEQ, v_resvalue,
												  l_sizet_const(0), ""),
									"");

					LLVMBuildCondBr(b,
									v_nullorfalse,
									opblocks[op->d.jump.jumpdone],
									opblocks[opno + 1]);
					break;
				}

			case EEOP_NULLTEST_ISNULL:
				{
					LLVMValueRef v_resnull = LLVMBuildLoad(b, v_resnullp, "");
					LLVMValueRef v_resvalue;

					v_resvalue =
						LLVMBuildSelect(b,
										LLVMBuildICmp(b, LLVMIntEQ, v_resnull,
													  l_sbool_const(1), ""),
										l_sizet_const(1),
										l_sizet_const(0),
										"");
					LLVMBuildStore(b, v_resvalue, v_resvaluep);
					LLVMBuildStore(b, l_sbool_const(0), v_resnullp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_NULLTEST_ISNOTNULL:
				{
					LLVMValueRef v_resnull = LLVMBuildLoad(b, v_resnullp, "");
					LLVMValueRef v_resvalue;

					v_resvalue =
						LLVMBuildSelect(b,
										LLVMBuildICmp(b, LLVMIntEQ, v_resnull,
													  l_sbool_const(1), ""),
										l_sizet_const(0),
										l_sizet_const(1),
										"");
					LLVMBuildStore(b, v_resvalue, v_resvaluep);
					LLVMBuildStore(b, l_sbool_const(0), v_resnullp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_NULLTEST_ROWISNULL:
				build_EvalXFunc(&ecs, "ExecEvalRowNull", opno);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_NULLTEST_ROWISNOTNULL:
				build_EvalXFunc(&ecs, "ExecEvalRowNotNull", opno);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_BOOLTEST_IS_TRUE:
			case EEOP_BOOLTEST_IS_NOT_FALSE:
			case EEOP_BOOLTEST_IS_FALSE:
			case EEOP_BOOLTEST_IS_NOT_TRUE:
				{
					LLVMBasicBlockRef b_isnull;
					LLVMBasicBlockRef b_notnull;
					LLVMValueRef v_resnull = LLVMBuildLoad(b, v_resnullp, "");

					b_isnull = l_bb_before_v(opblocks[opno + 1],
											 "op.%d.%s.isnull",
											 opno, opcode_name);
					b_notnull = l_bb_before_v(opblocks[opno + 1],
											  "op.%d.%s.isnotnull",
											  opno, opcode_name);

					/* check if value is NULL */
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_resnull,
												  l_sbool_const(1), ""),
									b_isnull, b_notnull);

					/* if value is NULL, return false */
					LLVMPositionBuilderAtEnd(b, b_isnull);

					/* result is not null */
					LLVMBuildStore(b, l_sbool_const(0), v_resnullp);

					if (opcode == EEOP_BOOLTEST_IS_TRUE ||
						opcode == EEOP_BOOLTEST_IS_FALSE)
					{
						LLVMBuildStore(b, l_sizet_const(0), v_resvaluep);
					}
					else
					{
						LLVMBuildStore(b, l_sizet_const(1), v_resvaluep);
					}

					LLVMBuildBr(b, opblocks[opno + 1]);

					LLVMPositionBuilderAtEnd(b, b_notnull);

					if (opcode == EEOP_BOOLTEST_IS_TRUE ||
						opcode == EEOP_BOOLTEST_IS_NOT_FALSE)
					{
						/*
						 * if value is not null NULL, return value (already
						 * set)
						 */
					}
					else
					{
						LLVMValueRef v_value =
						LLVMBuildLoad(b, v_resvaluep, "");

						v_value = LLVMBuildZExt(b,
												LLVMBuildICmp(b, LLVMIntEQ,
															  v_value,
															  l_sizet_const(0),
															  ""),
												TypeSizeT, "");
						LLVMBuildStore(b, v_value, v_resvaluep);
					}
					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_PARAM_EXEC:
				build_EvalXFunc(&ecs, "ExecEvalParamExec", opno);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_PARAM_EXTERN:
				build_EvalXFunc(&ecs, "ExecEvalParamExtern", opno);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_PARAM_CALLBACK:
				{
					LLVMValueRef v_opp = LLVMBuildGEP(b, ecs.v_steps, &v_opno, 1, "");
					LLVMValueRef v_cparam_data;
					LLVMValueRef v_func;
					LLVMValueRef v_funcarg;
					LLVMValueRef v_opparamp;
					LLVMValueRef v_params[4];

					v_cparam_data =
						LLVMBuildBitCast(b, LLVMBuildStructGEP(b, v_opp,
															   FIELDNO_EXPREVALSTEP_D,
															   ""),
										 l_ptr(StructExprEvalStepParamCallback), "");
					v_func =
						l_load_struct_gep(b, v_cparam_data,
										  FIELDNO_EXPREVALSTEPPARAMCALLBACK_PARAMFUNC,
										  "");
					v_funcarg =
						l_load_struct_gep(b, v_cparam_data,
										  FIELDNO_EXPREVALSTEPPARAMCALLBACK_PARAMARG,
										  "");
					v_opparamp =
						LLVMBuildStructGEP(b, v_cparam_data,
										   FIELDNO_EXPREVALSTEPPARAMCALLBACK_PARAM,
										   "");

					v_params[0] = v_funcarg;
					v_params[1] = ecs.v_econtext;
					v_params[2] = v_opparamp;
					v_params[3] = v_resultp;
					LLVMBuildCall(b,
								  v_func,
								  v_params, lengthof(v_params), "");

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_SBSREF_OLD:
				build_EvalXFunc(&ecs, "ExecEvalSubscriptingRefOld", opno);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_SBSREF_ASSIGN:
				build_EvalXFunc(&ecs, "ExecEvalSubscriptingRefAssign", opno);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_SBSREF_FETCH:
				build_EvalXFunc(&ecs, "ExecEvalSubscriptingRefFetch", opno);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_CASE_TESTVAL:
				{
					LLVMBasicBlockRef b_avail;
					LLVMBasicBlockRef b_notavail;
					LLVMValueRef v_casevaluep,
								v_casevalue;
					LLVMValueRef v_casevaluenull;

					b_avail = l_bb_before_v(opblocks[opno + 1],
											"op.%d.%s.avail",
											opno, opcode_name);
					b_notavail = l_bb_before_v(opblocks[opno + 1],
											   "op.%d.%s.notavail",
											   opno, opcode_name);

					v_casevaluep = l_ptr_const(op->d.casetest.value,
											   l_ptr(StructNullableDatum));

					v_casevaluenull =
						LLVMBuildICmp(b, LLVMIntEQ,
									  LLVMBuildPtrToInt(b, v_casevaluep,
														TypeSizeT, ""),
									  l_sizet_const(0), "");
					LLVMBuildCondBr(b, v_casevaluenull, b_notavail, b_avail);

					/* if casetest != NULL */
					LLVMPositionBuilderAtEnd(b, b_avail);
					v_casevalue = LLVMBuildLoad(b, v_casevaluep, "");
					LLVMBuildStore(b, v_casevalue, v_resultp);
					LLVMBuildBr(b, opblocks[opno + 1]);

					/* if casetest == NULL */
					LLVMPositionBuilderAtEnd(b, b_notavail);
					v_casevalue =
						l_load_struct_gep(b, ecs.v_econtext,
										  FIELDNO_EXPRCONTEXT_CASEVALUE,
										  "");
					LLVMBuildStore(b, v_casevalue, v_resultp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_MAKE_READONLY:
				{
					LLVMBasicBlockRef b_notnull;
					LLVMValueRef v_params[1];
					LLVMValueRef v_ret;
					LLVMValueRef v_valuep;
					LLVMValueRef v_null;

					b_notnull = l_bb_before_v(opblocks[opno + 1],
											  "op.%d.%s.readonly.notnull",
											  opno, opcode_name);

					v_valuep = l_ptr_const(op->d.make_readonly.value,
										  l_ptr(StructNullableDatum));
					v_null = l_load_struct_gep(b, v_valuep,
											   FIELDNO_NULLABLE_DATUM_ISNULL, "");

					/* store null isnull value in result */
					LLVMBuildStore(b, v_null, v_resnullp);

					/* check if value is NULL */
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_null,
												  l_sbool_const(1), ""),
									opblocks[opno + 1], b_notnull);

					/* if value is not null, convert to RO datum */
					LLVMPositionBuilderAtEnd(b, b_notnull);

					v_params[0] =
						l_load_struct_gep(b, v_valuep,
										  FIELDNO_NULLABLE_DATUM_DATUM, "");
					v_ret =
						LLVMBuildCall(b,
									  llvm_get_decl(ecs.mod, FuncMakeExpandedObjectReadOnlyInternal),
									  v_params, lengthof(v_params), "");
					LLVMBuildStore(b, v_ret, v_resvaluep);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_IOCOERCE:
				{
					FunctionCallInfo fcinfo_out,
								fcinfo_in;
					LLVMValueRef v_fn_out,
								v_fn_in;
					LLVMValueRef v_fcinfo_out,
								v_fcinfo_in;
					LLVMValueRef v_fcinfo_in_isnullp;
					LLVMValueRef v_retval;
					LLVMValueRef v_resvalue;
					LLVMValueRef v_resnull;

					LLVMValueRef v_output_skip;
					LLVMValueRef v_output;

					LLVMBasicBlockRef b_skipoutput;
					LLVMBasicBlockRef b_calloutput;
					LLVMBasicBlockRef b_input;
					LLVMBasicBlockRef b_inputcall;

					fcinfo_out = op->d.iocoerce.fcinfo_data_out;
					fcinfo_in = op->d.iocoerce.fcinfo_data_in;

					b_skipoutput = l_bb_before_v(opblocks[opno + 1],
												 "op.%d.%s.skipoutputnull",
												 opno, opcode_name);
					b_calloutput = l_bb_before_v(opblocks[opno + 1],
												 "op.%d.%s.calloutput",
												 opno, opcode_name);
					b_input = l_bb_before_v(opblocks[opno + 1],
											"op.%d.%s.input",
											opno, opcode_name);
					b_inputcall = l_bb_before_v(opblocks[opno + 1],
												"op.%d.%s.inputcall",
												opno, opcode_name);

					v_fcinfo_out = l_ptr_const(fcinfo_out, l_ptr(StructFunctionCallInfoData));
					v_fcinfo_in = l_ptr_const(fcinfo_in, l_ptr(StructFunctionCallInfoData));
					v_fn_out = llvm_function_reference(ecs.context, b, ecs.mod, fcinfo_out);
					v_fn_in = llvm_function_reference(ecs.context, b, ecs.mod, fcinfo_in);

					v_fcinfo_in_isnullp =
						LLVMBuildStructGEP(b, v_fcinfo_in,
										   FIELDNO_FUNCTIONCALLINFODATA_ISNULL,
										   "v_fcinfo_in_isnull");

					/* output functions are not called on nulls */
					v_resnull = LLVMBuildLoad(b, v_resnullp, "");
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_resnull,
												  l_sbool_const(1), ""),
									b_skipoutput,
									b_calloutput);

					LLVMPositionBuilderAtEnd(b, b_skipoutput);
					v_output_skip = l_sizet_const(0);
					LLVMBuildBr(b, b_input);

					LLVMPositionBuilderAtEnd(b, b_calloutput);
					v_resvalue = LLVMBuildLoad(b, v_resvaluep, "");

					/* set arg[0] */
					LLVMBuildStore(b,
								   v_resvalue,
								   l_funcvaluep(b, v_fcinfo_out, 0));
					LLVMBuildStore(b,
								   l_sbool_const(0),
								   l_funcnullp(b, v_fcinfo_out, 0));
					/* and call output function (can never return NULL) */
					v_output = LLVMBuildCall(b, v_fn_out, &v_fcinfo_out,
											 1, "funccall_coerce_out");
					LLVMBuildBr(b, b_input);

					/* build block handling input function call */
					LLVMPositionBuilderAtEnd(b, b_input);

					/* phi between resnull and output function call branches */
					{
						LLVMValueRef incoming_values[2];
						LLVMBasicBlockRef incoming_blocks[2];

						incoming_values[0] = v_output_skip;
						incoming_blocks[0] = b_skipoutput;

						incoming_values[1] = v_output;
						incoming_blocks[1] = b_calloutput;

						v_output = LLVMBuildPhi(b, TypeSizeT, "output");
						LLVMAddIncoming(v_output,
										incoming_values, incoming_blocks,
										lengthof(incoming_blocks));
					}

					/*
					 * If input function is strict, skip if input string is
					 * NULL.
					 */
					if (op->d.iocoerce.fn_strict_in)
					{
						LLVMBuildCondBr(b,
										LLVMBuildICmp(b, LLVMIntEQ, v_output,
													  l_sizet_const(0), ""),
										opblocks[opno + 1],
										b_inputcall);
					}
					else
					{
						LLVMBuildBr(b, b_inputcall);
					}

					LLVMPositionBuilderAtEnd(b, b_inputcall);
					/* set arguments */
					/* arg0: output */
					LLVMBuildStore(b, v_output,
								   l_funcvaluep(b, v_fcinfo_in, 0));
					LLVMBuildStore(b, v_resnull,
								   l_funcnullp(b, v_fcinfo_in, 0));

					/* arg1: ioparam: preset in execExpr.c */
					/* arg2: typmod: preset in execExpr.c  */

					/* reset fcinfo_in->isnull */
					LLVMBuildStore(b, l_sbool_const(0), v_fcinfo_in_isnullp);
					/* and call function */
					v_retval = LLVMBuildCall(b, v_fn_in, &v_fcinfo_in, 1,
											 "funccall_iocoerce_in");

					LLVMBuildStore(b, v_retval, v_resvaluep);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_DISTINCT:
			case EEOP_NOT_DISTINCT:
				{
					FunctionCallInfo fcinfo = op->d.func.fcinfo_data;

					LLVMValueRef v_fcinfo;
					LLVMValueRef v_fcinfo_isnull;

					LLVMValueRef v_argnull0,
								v_argisnull0;
					LLVMValueRef v_argnull1,
								v_argisnull1;

					LLVMValueRef v_anyargisnull;
					LLVMValueRef v_bothargisnull;

					LLVMValueRef v_result;

					LLVMBasicBlockRef b_noargnull;
					LLVMBasicBlockRef b_checkbothargnull;
					LLVMBasicBlockRef b_bothargnull;
					LLVMBasicBlockRef b_anyargnull;

					b_noargnull =
						l_bb_before_v(opblocks[opno + 1],
									  "op.%d.%s.noargnull",
									  opno, opcode_name);
					b_checkbothargnull =
						l_bb_before_v(opblocks[opno + 1],
									  "op.%d.%s.checkbothargnull",
									  opno, opcode_name);
					b_bothargnull =
						l_bb_before_v(opblocks[opno + 1],
									  "op.%d.%s.bothargnull",
									  opno, opcode_name);
					b_anyargnull =
						l_bb_before_v(opblocks[opno + 1],
									  "op.%d.%s.anyargnull",
									  opno, opcode_name);

					v_fcinfo = l_ptr_const(fcinfo, l_ptr(StructFunctionCallInfoData));

					/* load args[0|1].isnull for both arguments */
					v_argnull0 = l_funcnull(b, v_fcinfo, 0);
					v_argisnull0 = LLVMBuildICmp(b, LLVMIntEQ, v_argnull0,
												 l_sbool_const(1), "");
					v_argnull1 = l_funcnull(b, v_fcinfo, 1);
					v_argisnull1 = LLVMBuildICmp(b, LLVMIntEQ, v_argnull1,
												 l_sbool_const(1), "");

					v_anyargisnull = LLVMBuildOr(b, v_argisnull0, v_argisnull1, "");
					v_bothargisnull = LLVMBuildAnd(b, v_argisnull0, v_argisnull1, "");

					/*
					 * Check function arguments for NULLness: If either is
					 * NULL, we check if both args are NULL. Otherwise call
					 * comparator.
					 */
					LLVMBuildCondBr(b, v_anyargisnull, b_checkbothargnull,
									b_noargnull);

					/*
					 * build block checking if any arg is null
					 */
					LLVMPositionBuilderAtEnd(b, b_checkbothargnull);
					LLVMBuildCondBr(b, v_bothargisnull, b_bothargnull,
									b_anyargnull);


					/* Both NULL? Then is not distinct... */
					LLVMPositionBuilderAtEnd(b, b_bothargnull);
					LLVMBuildStore(b, l_sbool_const(0), v_resnullp);
					if (opcode == EEOP_NOT_DISTINCT)
						LLVMBuildStore(b, l_sizet_const(1), v_resvaluep);
					else
						LLVMBuildStore(b, l_sizet_const(0), v_resvaluep);

					LLVMBuildBr(b, opblocks[opno + 1]);

					/* Only one is NULL? Then is distinct... */
					LLVMPositionBuilderAtEnd(b, b_anyargnull);
					LLVMBuildStore(b, l_sbool_const(0), v_resnullp);
					if (opcode == EEOP_NOT_DISTINCT)
						LLVMBuildStore(b, l_sizet_const(0), v_resvaluep);
					else
						LLVMBuildStore(b, l_sizet_const(1), v_resvaluep);
					LLVMBuildBr(b, opblocks[opno + 1]);

					/* neither argument is null: compare */
					LLVMPositionBuilderAtEnd(b, b_noargnull);

					v_result = BuildV1CallFC(&ecs, fcinfo, v_fcinfo,
											 &v_fcinfo_isnull);

					if (opcode == EEOP_DISTINCT)
					{
						/* Must invert result of "=" */
						v_result =
							LLVMBuildZExt(b,
										  LLVMBuildICmp(b, LLVMIntEQ,
														v_result,
														l_sizet_const(0), ""),
										  TypeSizeT, "");
					}

					LLVMBuildStore(b, v_fcinfo_isnull, v_resnullp);
					LLVMBuildStore(b, v_result, v_resvaluep);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_NULLIF:
				{
					FunctionCallInfo fcinfo = op->d.func.fcinfo_data;

					LLVMValueRef v_fcinfo;
					LLVMValueRef v_fcinfo_isnull;
					LLVMValueRef v_argnull0;
					LLVMValueRef v_argnull1;
					LLVMValueRef v_anyargisnull;
					LLVMValueRef v_arg0;
					LLVMBasicBlockRef b_hasnull;
					LLVMBasicBlockRef b_nonull;
					LLVMBasicBlockRef b_argsequal;
					LLVMValueRef v_retval;
					LLVMValueRef v_argsequal;

					b_hasnull =
						l_bb_before_v(opblocks[opno + 1],
									  "b.%d.%s.null-args",
									  opno, opcode_name);
					b_nonull =
						l_bb_before_v(opblocks[opno + 1],
									  "b.%d.%s.no-null-args",
									  opno, opcode_name);
					b_argsequal =
						l_bb_before_v(opblocks[opno + 1],
									  "b.%d.%s.argsequal",
									  opno, opcode_name);

					v_fcinfo = l_ptr_const(fcinfo, l_ptr(StructFunctionCallInfoData));

					/* if either argument is NULL they can't be equal */
					v_argnull0 = l_funcnull(b, v_fcinfo, 0);
					v_argnull1 = l_funcnull(b, v_fcinfo, 1);

					v_anyargisnull =
						LLVMBuildOr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_argnull0,
												  l_sbool_const(1), ""),
									LLVMBuildICmp(b, LLVMIntEQ, v_argnull1,
												  l_sbool_const(1), ""),
									"");

					LLVMBuildCondBr(b, v_anyargisnull, b_hasnull, b_nonull);

					/* one (or both) of the arguments are null, return arg[0] */
					LLVMPositionBuilderAtEnd(b, b_hasnull);
					v_arg0 = l_funcvalue(b, v_fcinfo, 0);
					LLVMBuildStore(b, v_argnull0, v_resnullp);
					LLVMBuildStore(b, v_arg0, v_resvaluep);
					LLVMBuildBr(b, opblocks[opno + 1]);

					/* build block to invoke function and check result */
					LLVMPositionBuilderAtEnd(b, b_nonull);

					v_retval = BuildV1CallFC(&ecs, fcinfo, v_fcinfo,
											 &v_fcinfo_isnull);

					/*
					 * If result not null, and arguments are equal return null
					 * (same result as if there'd been NULLs, hence reuse
					 * b_hasnull).
					 */
					v_argsequal = LLVMBuildAnd(b,
											   LLVMBuildICmp(b, LLVMIntEQ,
															 v_fcinfo_isnull,
															 l_sbool_const(0),
															 ""),
											   LLVMBuildICmp(b, LLVMIntEQ,
															 v_retval,
															 l_sizet_const(1),
															 ""),
											   "");
					LLVMBuildCondBr(b, v_argsequal, b_argsequal, b_hasnull);

					/* build block setting result to NULL, if args are equal */
					LLVMPositionBuilderAtEnd(b, b_argsequal);
					LLVMBuildStore(b, l_sbool_const(1), v_resnullp);
					LLVMBuildStore(b, l_sizet_const(0), v_resvaluep);
					LLVMBuildStore(b, v_retval, v_resvaluep);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_SQLVALUEFUNCTION:
				build_EvalXFunc(&ecs, "ExecEvalSQLValueFunction", opno);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_CURRENTOFEXPR:
				build_EvalXFunc(&ecs, "ExecEvalCurrentOfExpr", opno);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_NEXTVALUEEXPR:
				build_EvalXFunc(&ecs, "ExecEvalNextValueExpr", opno);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_ARRAYEXPR:
				build_EvalXFunc(&ecs, "ExecEvalArrayExpr", opno);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_ARRAYCOERCE_RELABEL:
				{
					LLVMValueRef v_params[2];

					v_params[0] = ecs.v_state;
					v_params[1] = LLVMBuildGEP(ecs.b, ecs.v_steps, &v_opno, 1, "");

					LLVMBuildCall(b,
								  llvm_get_decl(ecs.mod, FuncExecEvalArrayCoerceRelabel),
								  v_params, lengthof(v_params), "");

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}
			case EEOP_ARRAYCOERCE_UNPACK:
				{
					LLVMValueRef v_params[2];
					LLVMValueRef v_ret;

					v_params[0] = ecs.v_state;
					v_params[1] = LLVMBuildGEP(ecs.b, ecs.v_steps, &v_opno, 1, "");

					v_ret =
						LLVMBuildCall(b,
									  llvm_get_decl(ecs.mod, FuncExecEvalArrayCoerceUnpack),
									  v_params, lengthof(v_params), "");
					v_ret = LLVMBuildZExt(b, v_ret, TypeStorageBool, "");

					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_ret,
												  l_sbool_const(1), ""),
									opblocks[opno + 1],
									opblocks[op->d.arraycoerce.jumpnext]);
					break;
				}

			case EEOP_ARRAYCOERCE_PACK:
				{
					LLVMValueRef v_params[2];
					LLVMValueRef v_ret;

					v_params[0] = ecs.v_state;
					v_params[1] = LLVMBuildGEP(ecs.b, ecs.v_steps, &v_opno, 1, "");

					v_ret =
						LLVMBuildCall(b,
									  llvm_get_decl(ecs.mod, FuncExecEvalArrayCoercePack),
									  v_params, lengthof(v_params), "");
					v_ret = LLVMBuildZExt(b, v_ret, TypeStorageBool, "");

					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_ret,
												  l_sbool_const(1), ""),
									opblocks[op->d.arraycoerce.jumpnext],
									opblocks[opno + 1]);
					break;
				}

			case EEOP_ROW:
				build_EvalXFunc(&ecs, "ExecEvalRow", opno);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_ROWCOMPARE_STEP:
				{
					FunctionCallInfo fcinfo = op->d.rowcompare_step.fcinfo_data;
					LLVMValueRef v_fcinfo;
					LLVMValueRef v_fcinfo_isnull;
					LLVMBasicBlockRef b_null;
					LLVMBasicBlockRef b_compare;
					LLVMBasicBlockRef b_compare_result;

					LLVMValueRef v_retval;

					b_null =
						l_bb_before_v(opblocks[opno + 1],
									  "op.%d.%s.row-null",
									  opno, opcode_name);
					b_compare =
						l_bb_before_v(opblocks[opno + 1],
									  "op.%d.%s.row-compare",
									  opno, opcode_name);
					b_compare_result =
						l_bb_before_v(opblocks[opno + 1],
									  "op.%d.%s.row-compare-result",
									  opno, opcode_name);

					v_fcinfo = l_ptr_const(fcinfo,
										   l_ptr(StructFunctionCallInfoData));

					/*
					 * If function is strict, and either arg is null, we're
					 * done.
					 */
					if (op->d.rowcompare_step.fn_strict)
					{
						LLVMValueRef v_argnull0;
						LLVMValueRef v_argnull1;
						LLVMValueRef v_anyargisnull;

						v_argnull0 = l_funcnull(b, v_fcinfo, 0);
						v_argnull1 = l_funcnull(b, v_fcinfo, 1);

						v_anyargisnull =
							LLVMBuildOr(b,
										LLVMBuildICmp(b,
													  LLVMIntEQ,
													  v_argnull0,
													  l_sbool_const(1),
													  ""),
										LLVMBuildICmp(b, LLVMIntEQ,
													  v_argnull1,
													  l_sbool_const(1), ""),
										"");

						LLVMBuildCondBr(b, v_anyargisnull, b_null, b_compare);
					}
					else
					{
						LLVMBuildBr(b, b_compare);
					}

					/* build block invoking comparison function */
					LLVMPositionBuilderAtEnd(b, b_compare);

					/* call function */
					v_retval = BuildV1CallFC(&ecs, fcinfo, v_fcinfo,
											 &v_fcinfo_isnull);
					LLVMBuildStore(b, v_retval, v_resvaluep);

					/* if result of function is NULL, force NULL result */
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b,
												  LLVMIntEQ,
												  v_fcinfo_isnull,
												  l_sbool_const(0),
												  ""),
									b_compare_result,
									b_null);

					/* build block analyzing the !NULL comparator result */
					LLVMPositionBuilderAtEnd(b, b_compare_result);

					/* if results equal, compare next, otherwise done */
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b,
												  LLVMIntEQ,
												  v_retval,
												  l_sizet_const(0), ""),
									opblocks[opno + 1],
									opblocks[op->d.rowcompare_step.jumpdone]);

					/*
					 * Build block handling NULL input or NULL comparator
					 * result.
					 */
					LLVMPositionBuilderAtEnd(b, b_null);
					LLVMBuildStore(b, l_sbool_const(1), v_resnullp);
					LLVMBuildBr(b, opblocks[op->d.rowcompare_step.jumpnull]);

					break;
				}

			case EEOP_ROWCOMPARE_FINAL:
				{
					RowCompareType rctype = op->d.rowcompare_final.rctype;

					LLVMValueRef v_cmpresult;
					LLVMValueRef v_result;
					LLVMIntPredicate predicate;

					/*
					 * Btree comparators return 32 bit results, need to be
					 * careful about sign (used as a 64 bit value it's
					 * otherwise wrong).
					 */
					v_cmpresult =
						LLVMBuildTrunc(b,
									   LLVMBuildLoad(b, v_resvaluep, ""),
									   LLVMInt32Type(), "");

					switch (rctype)
					{
						case ROWCOMPARE_LT:
							predicate = LLVMIntSLT;
							break;
						case ROWCOMPARE_LE:
							predicate = LLVMIntSLE;
							break;
						case ROWCOMPARE_GT:
							predicate = LLVMIntSGT;
							break;
						case ROWCOMPARE_GE:
							predicate = LLVMIntSGE;
							break;
						default:
							/* EQ and NE cases aren't allowed here */
							Assert(false);
							predicate = 0;	/* prevent compiler warning */
							break;
					}

					v_result = LLVMBuildICmp(b,
											 predicate,
											 v_cmpresult,
											 l_int32_const(0),
											 "");
					v_result = LLVMBuildZExt(b, v_result, TypeSizeT, "");

					LLVMBuildStore(b, l_sbool_const(0), v_resnullp);
					LLVMBuildStore(b, v_result, v_resvaluep);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_MINMAX:
				build_EvalXFunc(&ecs, "ExecEvalMinMax", opno);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_FIELDSELECT:
				build_EvalXFunc(&ecs, "ExecEvalFieldSelect", opno);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_FIELDSTORE_DEFORM:
				build_EvalXFunc(&ecs, "ExecEvalFieldStoreDeForm", opno);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_FIELDSTORE_FORM:
				build_EvalXFunc(&ecs, "ExecEvalFieldStoreForm", opno);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_SBSREF_SUBSCRIPT:
				{
					int			jumpdone = op->d.sbsref_subscript.jumpdone;
					LLVMValueRef v_params[2];
					LLVMValueRef v_ret;

					v_params[0] = ecs.v_state;
					v_params[1] = LLVMBuildGEP(ecs.b, ecs.v_steps, &v_opno, 1, "");
					v_ret =
						LLVMBuildCall(b,
									  llvm_get_decl(ecs.mod, FuncExecEvalSubscriptingRef),
									  v_params, lengthof(v_params), "");
					v_ret = LLVMBuildZExt(b, v_ret, TypeStorageBool, "");

					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_ret,
												  l_sbool_const(1), ""),
									opblocks[opno + 1],
									opblocks[jumpdone]);
					break;
				}

			case EEOP_DOMAIN_TESTVAL:
				{
					LLVMBasicBlockRef b_avail;
					LLVMBasicBlockRef b_notavail;
					LLVMValueRef v_casevaluep,
								v_casevalue;
					LLVMValueRef v_casevaluenull;

					b_avail =
						l_bb_before_v(opblocks[opno + 1],
									  "op.%d.%s.avail",
									  opno, opcode_name);
					b_notavail =
						l_bb_before_v(opblocks[opno + 1],
									  "op.%d.%s.notavail",
									  opno, opcode_name);

					v_casevaluep = l_ptr_const(op->d.casetest.value,
											   l_ptr(StructNullableDatum));

					v_casevaluenull =
						LLVMBuildICmp(b, LLVMIntEQ,
									  LLVMBuildPtrToInt(b, v_casevaluep,
														TypeSizeT, ""),
									  l_sizet_const(0), "");
					LLVMBuildCondBr(b, v_casevaluenull, b_notavail, b_avail);

					/* if casetest != NULL */
					LLVMPositionBuilderAtEnd(b, b_avail);
					v_casevalue = LLVMBuildLoad(b, v_casevaluep, "");
					LLVMBuildStore(b, v_casevalue, v_resultp);
					LLVMBuildBr(b, opblocks[opno + 1]);

					/* if casetest == NULL */
					LLVMPositionBuilderAtEnd(b, b_notavail);
					v_casevalue =
						l_load_struct_gep(b, ecs.v_econtext,
										  FIELDNO_EXPRCONTEXT_DOMAINVALUE,
										  "");
					LLVMBuildStore(b, v_casevalue, v_resultp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_DOMAIN_NOTNULL:
				build_EvalXFunc(&ecs, "ExecEvalConstraintNotNull", opno);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_DOMAIN_CHECK:
				build_EvalXFunc(&ecs, "ExecEvalConstraintCheck", opno);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_CONVERT_ROWTYPE:
				build_EvalXFunc(&ecs, "ExecEvalConvertRowtype", opno);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_SCALARARRAYOP:
				build_EvalXFunc(&ecs, "ExecEvalScalarArrayOp", opno);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_XMLEXPR:
				build_EvalXFunc(&ecs, "ExecEvalXmlExpr", opno);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_AGGREF:
				{
					AggrefExprState *aggref = op->d.aggref.astate;
					LLVMValueRef v_aggnop;
					LLVMValueRef v_aggno;
					LLVMValueRef v_value;

					expr_set_aggvalues(&ecs);

					/*
					 * At this point aggref->aggno is not yet set (it's set up
					 * in ExecInitAgg() after initializing the expression). So
					 * load it from memory each time round.
					 */
					v_aggnop = l_ptr_const(&aggref->aggno,
										   l_ptr(LLVMInt32Type()));
					v_aggno = LLVMBuildLoad(b, v_aggnop, "v_aggno");

					/* and store result */
					v_value = l_load_gep1(b, ecs.d.agg.aggvalues, v_aggno,
										  "aggvalue");
					LLVMBuildStore(b, v_value, v_resultp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_GROUPING_FUNC:
				build_EvalXFunc(&ecs, "ExecEvalGroupingFunc", opno);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_WINDOW_FUNC:
				{
					WindowFuncExprState *wfunc = op->d.window_func.wfstate;
					LLVMValueRef v_wfuncnop;
					LLVMValueRef v_wfuncno;
					LLVMValueRef v_value;

					expr_set_aggvalues(&ecs);

					/*
					 * At this point aggref->wfuncno is not yet set (it's set
					 * up in ExecInitWindowAgg() after initializing the
					 * expression). So load it from memory each time round.
					 */
					v_wfuncnop = l_ptr_const(&wfunc->wfuncno,
											 l_ptr(LLVMInt32Type()));
					v_wfuncno = LLVMBuildLoad(b, v_wfuncnop, "v_wfuncno");

					/* and store result */
					v_value = l_load_gep1(b, ecs.d.agg.aggvalues, v_wfuncno,
										  "windowvalue");
					LLVMBuildStore(b, v_value, v_resultp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_SUBPLAN:
				build_EvalXFunc(&ecs, "ExecEvalSubPlan", opno);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_ALTERNATIVE_SUBPLAN:
				build_EvalXFunc(&ecs, "ExecEvalAlternativeSubPlan", opno);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_AGG_STRICT_DESERIALIZE:
				{
					FunctionCallInfo fcinfo = op->d.agg_deserialize.fcinfo_data;
					LLVMValueRef v_fcinfo;
					LLVMValueRef v_argnull0;
					LLVMBasicBlockRef b_deserialize;

					b_deserialize =
						l_bb_before_v(opblocks[opno + 1],
									  "op.%d.%s.deserialize",
									  opno, opcode_name);

					v_fcinfo = l_ptr_const(fcinfo,
										   l_ptr(StructFunctionCallInfoData));
					v_argnull0 = l_funcnull(b, v_fcinfo, 0);

					LLVMBuildCondBr(b,
									LLVMBuildICmp(b,
												  LLVMIntEQ,
												  v_argnull0,
												  l_sbool_const(1),
												  ""),
									opblocks[op->d.agg_deserialize.jumpnull],
									b_deserialize);
					LLVMPositionBuilderAtEnd(b, b_deserialize);
				}
				/* FALLTHROUGH */

			case EEOP_AGG_DESERIALIZE:
				{
					FunctionCallInfo fcinfo;

					LLVMValueRef v_retval;
					LLVMValueRef v_fcinfo_isnull;
					LLVMValueRef v_oldcontext;

					fcinfo = op->d.agg_deserialize.fcinfo_data;

					expr_set_aggtmpcontextp(&ecs);

					v_oldcontext = l_mcxt_switch(ecs.mod, b, ecs.d.agg.tmpcontext_pertup);
					v_retval = BuildV1Call(&ecs, fcinfo, &v_fcinfo_isnull);
					l_mcxt_switch(ecs.mod, b, v_oldcontext);

					LLVMBuildStore(b, v_retval, v_resvaluep);
					LLVMBuildStore(b, v_fcinfo_isnull, v_resnullp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_AGG_STRICT_INPUT_CHECK_ARGS:
			case EEOP_AGG_STRICT_INPUT_CHECK_ARGS_1:
				{
					int			nargs = op->d.agg_strict_input_check.nargs;
					NullableDatum *args = op->d.agg_strict_input_check.args;
					int			jumpnull;

					LLVMValueRef v_argsp;
					LLVMBasicBlockRef *b_checknulls;

					Assert(nargs > 0);

					jumpnull = op->d.agg_strict_input_check.jumpnull;
					v_argsp = l_ptr_const(args, l_ptr(StructNullableDatum));

					/* create blocks for checking args */
					b_checknulls = palloc(sizeof(LLVMBasicBlockRef *) * nargs);
					for (int argno = 0; argno < nargs; argno++)
					{
						b_checknulls[argno] =
							l_bb_before_v(opblocks[opno + 1],
										  "op.%d.%s.check-null.%d",
										  opno, opcode_name, argno);
					}

					LLVMBuildBr(b, b_checknulls[0]);

					/* strict function, check for NULL args */
					for (int argno = 0; argno < nargs; argno++)
					{
						LLVMValueRef v_argno = l_int32_const(argno);
						LLVMValueRef v_argisnull;
						LLVMBasicBlockRef b_argnotnull;
						LLVMValueRef v_argn;

						LLVMPositionBuilderAtEnd(b, b_checknulls[argno]);

						if (argno + 1 == nargs)
							b_argnotnull = opblocks[opno + 1];
						else
							b_argnotnull = b_checknulls[argno + 1];

						v_argn = LLVMBuildGEP(b, v_argsp, &v_argno, 1, "");
						v_argisnull =
							l_load_struct_gep(b, v_argn,
											  FIELDNO_NULLABLE_DATUM_ISNULL,
											  "");

						LLVMBuildCondBr(b,
										LLVMBuildICmp(b,
													  LLVMIntEQ,
													  v_argisnull,
													  l_sbool_const(1), ""),
										opblocks[jumpnull],
										b_argnotnull);
					}

					break;
				}

			case EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL:
			case EEOP_AGG_PLAIN_TRANS_STRICT_BYVAL:
			case EEOP_AGG_PLAIN_TRANS_BYVAL:
			case EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF:
			case EEOP_AGG_PLAIN_TRANS_STRICT_BYREF:
			case EEOP_AGG_PLAIN_TRANS_BYREF:
				{
					FunctionCallInfo fcinfo;

					LLVMValueRef v_fcinfo;
					LLVMValueRef v_fcinfo_isnull;
					LLVMValueRef v_fcinfo_context;

					LLVMValueRef v_transvaluep;
					LLVMValueRef v_transvalue;
					LLVMValueRef v_transvalue_isnullp;
					LLVMValueRef v_transvalue_valuep;

					LLVMValueRef v_setoff;
					LLVMValueRef v_transno;

					LLVMValueRef v_percall;

					LLVMValueRef v_allpergroupsp;

					LLVMValueRef v_pergroupp;

					LLVMValueRef v_retval;

					LLVMValueRef v_oldcontext;

					fcinfo = op->d.agg_trans.fcinfo_data;

					v_setoff = l_int32_const(op->d.agg_trans.setoff);
					v_transno = l_int32_const(op->d.agg_trans.transno);
					v_percall = l_ptr_const((void *) op->d.agg_trans.percall,
											l_ptr(StructAggStatePerCallContext));
					v_fcinfo = l_ptr_const(fcinfo, l_ptr(StructFunctionCallInfoData));

					/*
					 * pergroup = &aggstate->all_pergroups
					 * [op->d.agg_strict_trans_check.setoff]
					 * [op->d.agg_init_trans_check.transno];
					 */
					expr_set_aggstatep(&ecs);
					v_allpergroupsp =
						l_load_struct_gep(b, ecs.d.agg.state,
										  FIELDNO_AGGSTATE_ALL_PERGROUPS,
										  "aggstate.all_pergroups");
					v_pergroupp =
						LLVMBuildGEP(b,
									 l_load_gep1(b, v_allpergroupsp, v_setoff, ""),
									 &v_transno, 1, "");

					v_transvaluep =
						LLVMBuildStructGEP(b, v_pergroupp,
										   FIELDNO_AGGSTATEPERGROUPDATA_TRANSVALUE,
										   "transvaluep");
					v_transvalue = LLVMBuildLoad(b, v_transvaluep, "");
					v_transvalue_valuep =
						LLVMBuildStructGEP(b, v_transvaluep,
										   FIELDNO_NULLABLE_DATUM_DATUM,
										   "transvalue_valuep");
					v_transvalue_isnullp =
						LLVMBuildStructGEP(b, v_transvaluep,
										   FIELDNO_NULLABLE_DATUM_ISNULL,
										   "transvalue_valuep");

					if (opcode == EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL ||
						opcode == EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF)
					{
						LLVMValueRef v_notransvalue;
						LLVMBasicBlockRef b_init;
						LLVMBasicBlockRef b_no_init;

						v_notransvalue =
							l_load_struct_gep(b, v_pergroupp,
											  FIELDNO_AGGSTATEPERGROUPDATA_NOTRANSVALUE,
											  "notransvalue");

						b_init = l_bb_before_v(opblocks[opno + 1],
											   "op.%d.%s.inittrans",
											   opno, opcode_name);
						b_no_init = l_bb_before_v(opblocks[opno + 1],
												  "op.%d.%s.no_inittrans",
												  opno, opcode_name);

						LLVMBuildCondBr(b,
										LLVMBuildICmp(b, LLVMIntEQ, v_notransvalue,
													  l_sbool_const(1), ""),
										b_init,
										b_no_init);

						/* block to init the transition value if necessary */
						{
							LLVMValueRef params[3];

							LLVMPositionBuilderAtEnd(b, b_init);

							params[0] = v_percall;
							params[1] = v_pergroupp;
							params[2] = v_fcinfo;

							LLVMBuildCall(b,
										  llvm_get_decl(ecs.mod, FuncExecAggInitGroup),
										  params, lengthof(params),
										  "");

							LLVMBuildBr(b, opblocks[opno + 1]);

						}

						LLVMPositionBuilderAtEnd(b, b_no_init);
					}

					if (opcode == EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL ||
						opcode == EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF ||
						opcode == EEOP_AGG_PLAIN_TRANS_STRICT_BYVAL ||
						opcode == EEOP_AGG_PLAIN_TRANS_STRICT_BYREF)
					{
						LLVMValueRef v_transnull;
						LLVMBasicBlockRef b_strictpass;

						b_strictpass = l_bb_before_v(opblocks[opno + 1],
													 "op.%d.%s.strictpass",
													 opno, opcode_name);
						v_transnull = LLVMBuildLoad(b, v_transvalue_isnullp, "transnull");

						LLVMBuildCondBr(b,
										LLVMBuildICmp(b, LLVMIntEQ, v_transnull,
													  l_sbool_const(1), ""),
										opblocks[opno + 1],
										b_strictpass);

						LLVMPositionBuilderAtEnd(b, b_strictpass);
					}

					/* invoke transition function in per-tuple context */
					expr_set_aggtmpcontextp(&ecs);
					v_oldcontext = l_mcxt_switch(ecs.mod, b, ecs.d.agg.tmpcontext_pertup);

					/* set the per-call context */
					v_fcinfo_context =
						LLVMBuildStructGEP(b, v_fcinfo,
										   FIELDNO_FUNCTIONCALLINFODATA_CONTEXT,
										   "fcinfo.context");
					LLVMBuildStore(b,
								   LLVMBuildBitCast(b, v_percall, l_ptr(StructNode), ""),
								   v_fcinfo_context);

					/* store transvalue in fcinfo->args[0] */
					LLVMBuildStore(b, v_transvalue, l_funcargp(b, v_fcinfo, 0));

					/* and invoke transition function */
					v_retval = BuildV1CallFC(&ecs, fcinfo,
											 v_fcinfo, &v_fcinfo_isnull);

					/*
					 * For pass-by-ref datatype, must copy the new value into
					 * aggcontext and free the prior transValue.  But if
					 * transfn returned a pointer to its first input, we don't
					 * need to do anything.  Also, if transfn returned a
					 * pointer to a R/W expanded object that is already a
					 * child of the aggcontext, assume we can adopt that value
					 * without copying it.
					 */
					if (opcode == EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF ||
						opcode == EEOP_AGG_PLAIN_TRANS_STRICT_BYREF ||
						opcode == EEOP_AGG_PLAIN_TRANS_BYREF)
					{
						LLVMBasicBlockRef b_call;
						LLVMBasicBlockRef b_nocall;
						LLVMValueRef v_fn;
						LLVMValueRef v_newval;
						LLVMValueRef params[4];

						b_call = l_bb_before_v(opblocks[opno + 1],
											   "op.%d.%s.transcall",
											   opno, opcode_name);
						b_nocall = l_bb_before_v(opblocks[opno + 1],
												 "op.%d.%s.transnocall",
												 opno, opcode_name);

						/*
						 * DatumGetPointer(newVal) !=
						 * DatumGetPointer(pergroup->transValue))
						 */
						LLVMBuildCondBr(b,
										LLVMBuildICmp(b, LLVMIntEQ,
													  LLVMBuildLoad(b, v_transvalue_valuep, ""),
													  v_retval, ""),
										b_nocall, b_call);

						/* returned datum not passed datum, reparent */
						LLVMPositionBuilderAtEnd(b, b_call);

						params[0] = v_percall;
						params[1] = v_retval;
						params[2] = LLVMBuildTrunc(b, v_fcinfo_isnull,
												   TypeParamBool, "");
						params[3] = v_transvaluep;

						v_fn = llvm_get_decl(ecs.mod, FuncExecAggTransReparent);
						v_newval =
							LLVMBuildCall(b, v_fn,
										  params, lengthof(params),
										  "");

						/* store trans value */
						LLVMBuildStore(b, v_newval, v_transvalue_valuep);
						LLVMBuildStore(b, v_fcinfo_isnull, v_transvalue_isnullp);

						l_mcxt_switch(ecs.mod, b, v_oldcontext);
						LLVMBuildBr(b, opblocks[opno + 1]);

						/* returned datum passed datum, no need to reparent */
						LLVMPositionBuilderAtEnd(b, b_nocall);
					}

					/* store trans value */
					LLVMBuildStore(b, v_retval, v_transvalue_valuep);
					LLVMBuildStore(b, v_fcinfo_isnull, v_transvalue_isnullp);

					l_mcxt_switch(ecs.mod, b, v_oldcontext);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_AGG_ORDERED_TRANS_DATUM:
				build_EvalXFunc(&ecs, "ExecEvalAggOrderedTransDatum", opno);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_AGG_ORDERED_TRANS_TUPLE:
				build_EvalXFunc(&ecs, "ExecEvalAggOrderedTransTuple", opno);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_LAST:
				Assert(false);
				break;
		}
	}

	/* jump from entry to first block */
	LLVMPositionBuilderAtEnd(b, ecs.b_entry);
	LLVMBuildBr(b, opblocks[0]);

	LLVMDIBuilderFinalize(ecs.b_d);
	LLVMDisposeDIBuilder(ecs.b_d);
	LLVMDisposeBuilder(b);

	/*
	 * Don't immediately emit function, instead do so the first time the
	 * expression is actually evaluated. That allows to emit a lot of
	 * functions together, avoiding a lot of repeated llvm and memory
	 * remapping overhead.
	 */
	{

		CompiledExprState *cstate = palloc0(sizeof(CompiledExprState));

		cstate->context = ecs.context;
		cstate->funcname = funcname;

		state->evalfunc = ExecRunCompiledExpr;
		state->evalfunc_private = cstate;
	}

	llvm_leave_fatal_on_oom();

	INSTR_TIME_SET_CURRENT(endtime);
	INSTR_TIME_ACCUM_DIFF(ecs.context->base.instr.generation_counter,
						  endtime, starttime);

	return true;
}

/*
 * Run compiled expression.
 *
 * This will only be called the first time a JITed expression is called. We
 * first make sure the expression is still up2date, and then get a pointer to
 * the emitted function. The latter can be the first thing that triggers
 * optimizing and emitting all the generated functions.
 */
static Datum
ExecRunCompiledExpr(ExprState *state, ExprContext *econtext, bool *isNull)
{
	CompiledExprState *cstate = state->evalfunc_private;
	ExprStateEvalFunc func;

	CheckExprStillValid(state, econtext);

	llvm_enter_fatal_on_oom();
	func = (ExprStateEvalFunc) llvm_get_function(cstate->context,
												 cstate->funcname);
	llvm_leave_fatal_on_oom();
	Assert(func);

	/* remove indirection via this function for future calls */
	state->evalfunc = func;

	return func(state, econtext, isNull);
}

static void
expr_set_aggstatep(ExprCompileState *ecs)
{
	if (ecs->d.agg.state == NULL)
	{
		LLVMBasicBlockRef b_cur = LLVMGetInsertBlock(ecs->b);

		LLVMPositionBuilderAtEnd(ecs->b, ecs->b_entry);
		ecs->v_parent = l_load_struct_gep(ecs->b, ecs->v_state,
										  FIELDNO_EXPRSTATE_PARENT,
										  "v.state.parent");
		ecs->d.agg.state =
			LLVMBuildBitCast(ecs->b, ecs->v_parent, l_ptr(StructAggState),
							 "v.state.parent.asaggstate");
		LLVMPositionBuilderAtEnd(ecs->b, b_cur);
	}
}

static void
expr_set_aggtmpcontextp(ExprCompileState *ecs)
{
	if (ecs->d.agg.tmpcontext_pertup == NULL)
	{
		LLVMBasicBlockRef b_cur = LLVMGetInsertBlock(ecs->b);
		LLVMValueRef v_tmp_exprcontext;

		expr_set_aggstatep(ecs);

		LLVMPositionBuilderAtEnd(ecs->b, ecs->b_entry);

		v_tmp_exprcontext = l_load_struct_gep(ecs->b, ecs->d.agg.state,
											  FIELDNO_AGGSTATE_TMPCONTEXT,
											  "aggstate.tmpcontext");
		ecs->d.agg.tmpcontext_pertup =
			l_load_struct_gep(ecs->b,
							  v_tmp_exprcontext,
							  FIELDNO_EXPRCONTEXT_PERTUPLEMEMORY,
							  "aggstate.tmpcontext.ecxt_per_tuple_memory");
		LLVMPositionBuilderAtEnd(ecs->b, b_cur);
	}
}

static void
expr_set_aggvalues(ExprCompileState *ecs)
{
	if (ecs->d.agg.aggvalues == NULL)
	{
		LLVMBasicBlockRef b_cur = LLVMGetInsertBlock(ecs->b);

		LLVMPositionBuilderAtEnd(ecs->b, ecs->b_entry);

		ecs->d.agg.aggvalues =
			l_load_struct_gep(ecs->b, ecs->v_econtext,
							  FIELDNO_EXPRCONTEXT_AGGVALUES,
							  "v.econtext.aggvalues");
		LLVMPositionBuilderAtEnd(ecs->b, b_cur);
	}
}

static void
expr_set_scanslot(ExprCompileState *ecs)
{
	if (ecs->v_scanslot == NULL)
	{
		LLVMBasicBlockRef b_cur = LLVMGetInsertBlock(ecs->b);

		LLVMPositionBuilderAtEnd(ecs->b, ecs->b_entry);

		ecs->v_scanslot = l_load_struct_gep(ecs->b, ecs->v_econtext,
											FIELDNO_EXPRCONTEXT_SCANTUPLE,
											"v_scanslot");
		ecs->v_scanvalues = l_load_struct_gep(ecs->b, ecs->v_scanslot,
											  FIELDNO_TUPLETABLESLOT_VALUES,
											  "v_scanvalues");
		LLVMPositionBuilderAtEnd(ecs->b, b_cur);
	}
}

static void
expr_set_outerslot(ExprCompileState *ecs)
{
	if (ecs->v_outerslot == NULL)
	{
		LLVMBasicBlockRef b_cur = LLVMGetInsertBlock(ecs->b);

		LLVMPositionBuilderAtEnd(ecs->b, ecs->b_entry);


		ecs->v_outerslot = l_load_struct_gep(ecs->b, ecs->v_econtext,
											 FIELDNO_EXPRCONTEXT_OUTERTUPLE,
											 "v_outerslot");
		ecs->v_outervalues = l_load_struct_gep(ecs->b, ecs->v_outerslot,
											   FIELDNO_TUPLETABLESLOT_VALUES,
											   "v_outervalues");
		LLVMPositionBuilderAtEnd(ecs->b, b_cur);
	}
}

static void
expr_set_innerslot(ExprCompileState *ecs)
{
	if (ecs->v_innerslot == NULL)
	{
		LLVMBasicBlockRef b_cur = LLVMGetInsertBlock(ecs->b);

		LLVMPositionBuilderAtEnd(ecs->b, ecs->b_entry);

		ecs->v_innerslot = l_load_struct_gep(ecs->b, ecs->v_econtext,
											 FIELDNO_EXPRCONTEXT_INNERTUPLE,
											 "v_innerslot");
		ecs->v_innervalues = l_load_struct_gep(ecs->b, ecs->v_innerslot,
											   FIELDNO_TUPLETABLESLOT_VALUES,
											   "v_innervalues");
		LLVMPositionBuilderAtEnd(ecs->b, b_cur);
	}
}

static void
expr_set_resultslot(ExprCompileState *ecs)
{
	if (ecs->v_resultslot == NULL)
	{
		LLVMBasicBlockRef b_cur = LLVMGetInsertBlock(ecs->b);

		LLVMPositionBuilderAtEnd(ecs->b, ecs->b_entry);

		ecs->v_resultslot = l_load_struct_gep(ecs->b, ecs->v_state,
											  FIELDNO_EXPRSTATE_RESULTSLOT,
											  "v_resultslot");
		ecs->v_resultvalues = l_load_struct_gep(ecs->b, ecs->v_resultslot,
												FIELDNO_TUPLETABLESLOT_VALUES,
												"v_resultvalues");
		LLVMPositionBuilderAtEnd(ecs->b, b_cur);
	}
}

static void
expr_set_tmpvaluep(ExprCompileState *ecs)
{
	if (ecs->v_tmpvaluep == NULL)
	{
		LLVMBasicBlockRef b_cur = LLVMGetInsertBlock(ecs->b);

		LLVMPositionBuilderAtEnd(ecs->b, ecs->b_entry);

		ecs->v_tmpvaluep =
			LLVMBuildStructGEP(ecs->b, ecs->v_state,
							   FIELDNO_EXPRSTATE_RESULT,
							   "v.state.result");
		LLVMPositionBuilderAtEnd(ecs->b, b_cur);
	}
}

static LLVMValueRef
BuildV1CallFC(ExprCompileState *ecs,
			  FunctionCallInfo fcinfo,
			  LLVMValueRef v_fcinfo,
			  LLVMValueRef *v_fcinfo_isnull)
{
	LLVMValueRef v_fn;
	LLVMValueRef v_fcinfo_isnullp;
	LLVMValueRef v_retval;

	v_fn = llvm_function_reference(ecs->context, ecs->b, ecs->mod, fcinfo);

	v_fcinfo_isnullp = LLVMBuildStructGEP(ecs->b, v_fcinfo,
										  FIELDNO_FUNCTIONCALLINFODATA_ISNULL,
										  "v_fcinfo_isnull");
	LLVMBuildStore(ecs->b, l_sbool_const(0), v_fcinfo_isnullp);

	v_retval = LLVMBuildCall(ecs->b, v_fn, &v_fcinfo, 1, "funccall");

	if (v_fcinfo_isnull)
		*v_fcinfo_isnull = LLVMBuildLoad(ecs->b, v_fcinfo_isnullp, "");

	/*
	 * Add lifetime-end annotation, signalling that writes to memory don't
	 * have to be retained (important for inlining potential).
	 */
	{
		LLVMValueRef v_lifetime = create_LifetimeEnd(ecs->mod);
		LLVMValueRef params[2];

		params[0] = l_int64_const(sizeof(NullableDatum) * fcinfo->nargs);
		params[1] = l_ptr_const(fcinfo->args, l_ptr(LLVMInt8Type()));
		LLVMBuildCall(ecs->b, v_lifetime, params, lengthof(params), "");

		params[0] = l_int64_const(sizeof(fcinfo->isnull));
		params[1] = l_ptr_const(&fcinfo->isnull, l_ptr(LLVMInt8Type()));
		LLVMBuildCall(ecs->b, v_lifetime, params, lengthof(params), "");
	}

	return v_retval;
}

static LLVMValueRef
BuildV1Call(ExprCompileState *ecs, FunctionCallInfo fcinfo,
			LLVMValueRef *v_fcinfo_isnull)
{
	LLVMValueRef v_fcinfo;

	v_fcinfo = l_ptr_const(fcinfo, l_ptr(StructFunctionCallInfoData));

	return BuildV1CallFC(ecs, fcinfo, v_fcinfo, v_fcinfo_isnull);
}

/*
 * Implement an expression step by calling the function funcname.
 */
static void
build_EvalXFunc(ExprCompileState *ecs, const char *funcname, int opno)
{
	LLVMValueRef v_opno = l_int32_const(opno);
	LLVMValueRef v_opp = LLVMBuildGEP(ecs->b, ecs->v_steps, &v_opno, 1, "");
	LLVMTypeRef sig;
	LLVMValueRef v_fn;
	LLVMTypeRef param_types[3];
	LLVMValueRef params[3];

	v_fn = LLVMGetNamedFunction(ecs->mod, funcname);
	if (!v_fn)
	{
		param_types[0] = l_ptr(StructExprState);
		param_types[1] = l_ptr(StructExprEvalStep);
		param_types[2] = l_ptr(StructExprContext);

		sig = LLVMFunctionType(LLVMVoidType(),
							   param_types, lengthof(param_types),
							   false);
		v_fn = LLVMAddFunction(ecs->mod, funcname, sig);
	}

	params[0] = ecs->v_state;
	params[1] = v_opp;
	params[2] = ecs->v_econtext;

	LLVMBuildCall(ecs->b,
				  v_fn,
				  params, lengthof(params), "");
}

static LLVMValueRef
create_LifetimeEnd(LLVMModuleRef mod)
{
	LLVMTypeRef sig;
	LLVMValueRef fn;
	LLVMTypeRef param_types[2];

	/* LLVM 5+ has a variadic pointer argument */
#if LLVM_VERSION_MAJOR < 5
	const char *nm = "llvm.lifetime.end";
#else
	const char *nm = "llvm.lifetime.end.p0i8";
#endif

	fn = LLVMGetNamedFunction(mod, nm);
	if (fn)
		return fn;

	param_types[0] = LLVMInt64Type();
	param_types[1] = l_ptr(LLVMInt8Type());

	sig = LLVMFunctionType(LLVMVoidType(),
						   param_types, lengthof(param_types),
						   false);
	fn = LLVMAddFunction(mod, nm, sig);

	LLVMSetFunctionCallConv(fn, LLVMCCallConv);

	Assert(LLVMGetIntrinsicID(fn));

	return fn;
}
