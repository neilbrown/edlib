/*
 * Copyright Neil Brown ©2020 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Perform numeric calculations.
 *
 * 'str' should hold an expression which is computed.
 * It may can contain variables in which can comm2 should produce
 * the contents as a string
 * The result is passed to comm2 in decimal and hex
 */

#include <gmp.h>
#include "core.h"

int do_calc(const char *expr, mpq_t result,
	    int (*getvar)(const char *name, int len, mpq_t val, void *data),
	    void *data);

static int getvar(const char *name, int len, mpq_t val, void *data)
{
	struct cmd_info *ci = data;
	char *vnm;
	char *vval;

	if (!ci)
		return 0;
	vnm = strnsave(ci->focus, name, len);
	vval = comm_call_ret(strsave, ci->comm2, "get", ci->focus, 0, NULL, vnm);
	if (!vval)
		return 0;
	if (do_calc(vval, val, NULL, NULL) != 0)
		return 0;
	return 1;
}

DEF_CMD(calc)
{
	int ret;
	mpq_t result;

	if (!ci->str || !ci->comm2)
		return Enoarg;
	mpq_init(result);
	ret = do_calc(ci->str, result, getvar, (void*)ci);
	if (ret == 0) {
		/* success */
		char buf[100];
		if (mpz_cmp_si(mpq_denref(result), 1) == 0) {
			gmp_snprintf(buf, sizeof(buf), "%#Zd",
				     mpq_numref(result));
			comm_call(ci->comm2, "result", ci->focus, 0, NULL, buf);
			gmp_snprintf(buf, sizeof(buf), "%#Zx",
				     mpq_numref(result));
			comm_call(ci->comm2, "hex-result", ci->focus, 0, NULL, buf);
		} else {
			mpf_t fl;
			mpf_init2(fl, 20);
			mpf_set_q(fl, result);
			gmp_snprintf(buf, sizeof(buf), "%.10Fg", fl);
			mpf_clear(fl);
			comm_call(ci->comm2, "result", ci->focus, 0, NULL, buf);
			gmp_snprintf(buf, sizeof(buf), "%Qd", result);
			comm_call(ci->comm2, "frac-result", ci->focus, 0, NULL, buf);
		}
	} else {
		comm_call(ci->comm2, "err", ci->focus, ret-1);
	}
	mpq_clear(result);
	return ret == 0 ? 1 : Efail;
}

DEF_CMD(calc_replace)
{
	int ret;
	mpq_t result;
	const char *expr = ci->str;
	struct mark *m2 = ci->mark2;
	bool hex = False;

	if (!expr) {
		if (!ci->mark)
			return Enoarg;
		/* Try to find a WORD */
		call("Move-WORD", ci->focus, -1, ci->mark);
		m2 = mark_dup(ci->mark);
		call("Move-WORD", ci->focus, 1, m2);
		expr = call_ret(strsave, "doc:get-str", ci->focus, 0, ci->mark, NULL,
				0, m2);
		if (!expr || !*expr)
			return Enoarg;
	}
	mpq_init(result);
	if (expr[0] == '#') {
		hex = True;
		expr += 1;
	}
	ret = do_calc(expr, result, NULL, NULL);
	if (ret == 0) {
		/* success */
		char buf[100];
		mpf_t fl;

		if (mpz_cmp_si(mpq_denref(result), 1) == 0) {
			gmp_snprintf(buf, sizeof(buf),
				     hex ? "%#Zx" : "%Zd",
				     mpq_numref(result));
		} else {
			mpf_init(fl);
			mpf_set_q(fl, result);
			gmp_snprintf(buf, sizeof(buf), "%.10Fg", fl);
			mpf_clear(fl);
		}
		if (!ci->mark || !m2 ||
		    call("doc:replace", ci->focus, 0, m2, buf, 0, ci->mark) <= 0)
			call("Message", ci->focus, 0, NULL,
			     strconcat(ci->focus, expr, " -> ", buf));
	} else {
		call("Message", ci->focus, 0, NULL,
		     strconcat(ci->focus, expr, " -> error at ",
			       expr + ret - 1));
	}
	mpq_clear(result);
	return ret == 0 ? 1 : Efail;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &calc,
		  0, NULL, "CalcExpr");
	call_comm("global-set-command", ed, &calc_replace,
		  0, NULL, "interactive-cmd-calc-replace");
}
