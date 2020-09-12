#line 1667 "scanner.mdc"
#include <unistd.h>
#include <stdlib.h>

#line 1367 "scanner.mdc"
#include <gmp.h>
#include "mdcode.h"
#line 1453 "scanner.mdc"
#include <ctype.h>
#line 1379 "scanner.mdc"
static int parse_digits(mpz_t num, struct text tok, int base,
                        int *placesp)
{
	/* Accept digits up to 'base', ignore '_' and
	 * (for base 10) ' ' if they appear between two
	 * legal digits, and if `placesp` is not NULL,
	 * allow a single '.' or ',' and report the number
	 * of digits beyond there.
	 * Return number of characters processed (p),
	 * or 0 if something illegal was found.
	 */
	int p;
	int decimal = -1; // digits after marker
	enum {Digit, Space, Other} prev = Other;
	int digits = 0;

	for (p = 0; p < tok.len; p++) {
		int dig;
		char c = tok.txt[p];

		if (c == '_' || (c == ' ' && base == 10)) {
			if (prev != Digit)
				goto bad;
			prev = Space;
			continue;
		}
		if (c == '.' || c == ',') {
			if (prev != Digit)
				goto bad;
			if (!placesp || decimal >= 0)
				return p-1;
			decimal = 0;
			prev = Other;
			continue;
		}
		if (isdigit(c))
			dig = c - '0';
		else if (isupper(c))
			dig = 10 + c - 'A';
		else if (islower(c))
			dig = 10 + c - 'a';
		else
			dig = base;
		if (dig >= base) {
			if (prev == Space)
				p--;
			break;
		}
		prev = Digit;
		if (digits)
			mpz_mul_ui(num, num, base);
		else
			mpz_init(num);
		digits += 1;
		mpz_add_ui(num, num, dig);
		if (decimal >= 0)
			decimal++;
	}
	if (digits == 0)
		return 0;
	if (placesp) {
		if (decimal >= 0)
			*placesp = decimal;
		else
			*placesp = 0;
	}
	return p;
bad:
	if (digits)
		mpz_clear(num);
	return 0;
}
#line 1633 "scanner.mdc"
int number_parse(mpq_t num, char tail[3], struct text tok)
{
#line 1464 "scanner.mdc"
	int base = 10;
	char expc = 'e';
#line 1519 "scanner.mdc"
	int places = 0;
	mpz_t mant;
	int d;
#line 1539 "scanner.mdc"
	long lexp = 0;
	mpz_t exp;
	int esign = 1;
#line 1636 "scanner.mdc"
	int i;

#line 1469 "scanner.mdc"
	if (tok.txt[0] == '0' && tok.len > 1) {
		int skip = 0;
		switch(tok.txt[1]) {
		case 'x':
		case 'X':
			base = 16;
			skip = 2;
			expc = 'p';
			break;
		case 'o':
		case 'O':
			base = 8;
			skip = 2;
			expc = 'p';
			break;
		case 'b':
		case 'B':
			base = 2;
			skip = 2;
			expc = 'p';
			break;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
		case '_':
		case ' ':
			// another digit is not permitted
			// after a zero.
			return 0;
		default:
			// must be decimal marker or trailing
			// letter, which are OK;
			break;
		}
		tok.txt += skip;
		tok.len -= skip;
	}
#line 1525 "scanner.mdc"
	d = parse_digits(mant, tok, base, &places);
	if (d == 0)
		return 0;
	tok.txt += d;
	tok.len -= d;
	mpq_init(num);
	mpq_set_z(num, mant);
	mpz_clear(mant);
#line 1640 "scanner.mdc"
	if (tok.len > 1 && (tok.txt[0] == expc ||
	                    tok.txt[0] == toupper(expc))) {
		tok.txt++;
		tok.len--;
#line 1544 "scanner.mdc"
	        if (tok.len > 1) {
	        	if (tok.txt[0] == '+') {
	        		tok.txt++;
	        		tok.len--;
	        	} else if (tok.txt[0] == '-') {
	        		esign = -1;
	        		tok.txt++;
	        		tok.len--;
	        	}
	        }
	        d = parse_digits(exp, tok, 10, NULL);
	        if (d == 0) {
	        	mpq_clear(num);
	        	return 0;
	        }
	        if (!mpz_fits_slong_p(exp)) {
	        	mpq_clear(num);
	        	mpz_clear(exp);
	        	return 0;
	        }
	        lexp = mpz_get_si(exp) * esign;
	        mpz_clear(exp);
	        tok.txt += d;
	        tok.len -= d;
#line 1645 "scanner.mdc"
	}
#line 1580 "scanner.mdc"
	switch (base) {
	case 10:
	case 2:
		lexp -= places;
		break;
	case 16:
		lexp -= 4*places;
		break;
	case 8:
		lexp -= 3*places;
		break;
	}
	if (lexp < 0) {
		lexp = -lexp;
		esign = -1;
	} else
		esign = 1;
#line 1604 "scanner.mdc"
	if (expc == 'e') {
		mpq_t tens;
		mpq_init(tens);
		mpq_set_ui(tens, 10, 1);
		while (1) {
			if (lexp & 1) {
				if (esign > 0)
					mpq_mul(num, num, tens);
				else
					mpq_div(num, num, tens);
			}
			lexp >>= 1;
			if (lexp == 0)
				break;
			mpq_mul(tens, tens, tens);
		}
		mpq_clear(tens);
	} else {
		if (esign > 0)
			mpq_mul_2exp(num, num, lexp);
		else
			mpq_div_2exp(num, num, lexp);
	}
#line 1647 "scanner.mdc"

	for (i = 0; i < 2; i++) {
		if (tok.len <= i)
			break;
		if (!isalpha(tok.txt[i]))
			goto err;
		tail[i] = tok.txt[i];
	}
	tail[i] = 0;
	if (i == tok.len)
		return 1;
err:
	mpq_clear(num);
	return 0;
}
#line 1672 "scanner.mdc"

