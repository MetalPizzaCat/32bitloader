#include <stdint.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <errno.h>
#include <file.h>
#include <scan_helpers.h>
#include <ctype.h>

#define KMAX 2048
#if LDBL_MANT_DIG == 53 && LDBL_MAX_EXP == 1024

#define LD_B1B_DIG 2
#define LD_B1B_MAX 9007199, 254740991
#define KMAX 128

#elif LDBL_MANT_DIG == 64 && LDBL_MAX_EXP == 16384

#define LD_B1B_DIG 3
#define LD_B1B_MAX 18, 446744073, 709551615
#define KMAX 2048

#elif LDBL_MANT_DIG == 113 && LDBL_MAX_EXP == 16384

#define LD_B1B_DIG 4
#define LD_B1B_MAX 10384593, 717069655, 257060992, 658440191
#define KMAX 2048
#else
#error Unsupported long double representation
#endif

#define MASK (KMAX - 1)

static long long gob_scanexp(FILE *file, int pok)
{
    int c;
    int x;
    long long y;
    int neg = 0;

    c = scan_help_getc(file);
    if (c == '+' || c == '-')
    {
        neg = (c == '-');
        c = scan_help_getc(file);
        if (c - '0' >= 10U && pok)
            scan_help_unget(file);
    }
    if (c - '0' >= 10U)
    {
        scan_help_unget(file);
        return LLONG_MIN;
    }
    for (x = 0; c - '0' < 10U && x < INT_MAX / 10; c = scan_help_getc(file))
        x = 10 * x + c - '0';
    for (y = x; c - '0' < 10U && y < LLONG_MAX / 100; c = scan_help_getc(file))
        y = 10 * y + c - '0';
    for (; c - '0' < 10U; c = scan_help_getc(file))
        ;
    scan_help_unget(file);
    return neg ? -y : y;
}

static long double gob_decfloat(FILE *file, int c, int bits, int emin, int sign, int pok)
{
    uint32_t x[KMAX];
    static const uint32_t th[] = {LD_B1B_MAX};
    int i, j, k, a, z;
    long long lrp = 0, dc = 0;
    long long e10 = 0;
    int lnz = 0;
    int gotdig = 0, gotrad = 0;
    int rp;
    int e2;
    int emax = -emin - bits + 3;
    int denormal = 0;
    long double y;
    long double frac = 0;
    long double bias = 0;
    static const int p10s[] = {10, 100, 1000, 10000,
                               100000, 1000000, 10000000, 100000000};

    j = 0;
    k = 0;

    /* Don't let leading zeros consume buffer space */
    for (; c == '0'; c = scan_help_getc(file))
        gotdig = 1;
    if (c == '.')
    {
        gotrad = 1;
        for (c = scan_help_getc(file); c == '0'; c = scan_help_getc(file))
            gotdig = 1, lrp--;
    }

    x[0] = 0;
    for (; c - '0' < 10U || c == '.'; c = scan_help_getc(file))
    {
        if (c == '.')
        {
            if (gotrad)
                break;
            gotrad = 1;
            lrp = dc;
        }
        else if (k < KMAX - 3)
        {
            dc++;
            if (c != '0')
                lnz = dc;
            if (j)
                x[k] = x[k] * 10 + c - '0';
            else
                x[k] = c - '0';
            if (++j == 9)
            {
                k++;
                j = 0;
            }
            gotdig = 1;
        }
        else
        {
            dc++;
            if (c != '0')
            {
                lnz = (KMAX - 4) * 9;
                x[KMAX - 4] |= 1;
            }
        }
    }
    if (!gotrad)
        lrp = dc;

    if (gotdig && (c | 32) == 'e')
    {
        e10 = gob_scanexp(file, pok);
        if (e10 == LLONG_MIN)
        {
            if (pok)
            {
                scan_help_unget(file);
            }
            else
            {
                scan_help_lim(file, 0);
                return 0;
            }
            e10 = 0;
        }
        lrp += e10;
    }
    else if (c >= 0)
    {
        scan_help_unget(file);
    }
    if (!gotdig)
    {
        errno = EINVAL;
        scan_help_lim(file, 0);
        // shlim(f, 0);
        return 0;
    }

    /* Handle zero specially to avoid nasty special cases later */
    if (!x[0])
        return sign * 0.0;

    /* Optimize small integers (w/no exponent) and over/under-flow */
    if (lrp == dc && dc < 10 && (bits > 30 || x[0] >> bits == 0))
        return sign * (long double)x[0];
    if (lrp > -emin / 2)
    {
        errno = ERANGE;
        return sign * LDBL_MAX * LDBL_MAX;
    }
    if (lrp < emin - 2 * LDBL_MANT_DIG)
    {
        errno = ERANGE;
        return sign * LDBL_MIN * LDBL_MIN;
    }

    /* Align incomplete final B1B digit */
    if (j)
    {
        for (; j < 9; j++)
            x[k] *= 10;
        k++;
        j = 0;
    }

    a = 0;
    z = k;
    e2 = 0;
    rp = lrp;

    /* Optimize small to mid-size integers (even in exp. notation) */
    if (lnz < 9 && lnz <= rp && rp < 18)
    {
        if (rp == 9)
            return sign * (long double)x[0];
        if (rp < 9)
            return sign * (long double)x[0] / p10s[8 - rp];
        int bitlim = bits - 3 * (int)(rp - 9);
        if (bitlim > 30 || x[0] >> bitlim == 0)
            return sign * (long double)x[0] * p10s[rp - 10];
    }

    /* Drop trailing zeros */
    for (; !x[z - 1]; z--)
        ;

    /* Align radix point to B1B digit boundary */
    if (rp % 9)
    {
        int rpm9 = rp >= 0 ? rp % 9 : rp % 9 + 9;
        int p10 = p10s[8 - rpm9];
        uint32_t carry = 0;
        for (k = a; k != z; k++)
        {
            uint32_t tmp = x[k] % p10;
            x[k] = x[k] / p10 + carry;
            carry = 1000000000 / p10 * tmp;
            if (k == a && !x[k])
            {
                a = (a + 1 & MASK);
                rp -= 9;
            }
        }
        if (carry)
            x[z++] = carry;
        rp += 9 - rpm9;
    }

    /* Upscale until desired number of bits are left of radix point */
    while (rp < 9 * LD_B1B_DIG || (rp == 9 * LD_B1B_DIG && x[a] < th[0]))
    {
        uint32_t carry = 0;
        e2 -= 29;
        for (k = (z - 1 & MASK);; k = (k - 1 & MASK))
        {
            uint64_t tmp = ((uint64_t)x[k] << 29) + carry;
            if (tmp > 1000000000)
            {
                carry = tmp / 1000000000;
                x[k] = tmp % 1000000000;
            }
            else
            {
                carry = 0;
                x[k] = tmp;
            }
            if (k == (z - 1 & MASK) && k != a && !x[k])
                z = k;
            if (k == a)
                break;
        }
        if (carry)
        {
            rp += 9;
            a = (a - 1 & MASK);
            if (a == z)
            {
                z = (z - 1 & MASK);
                x[z - 1 & MASK] |= x[z];
            }
            x[a] = carry;
        }
    }

    /* Downscale until exactly number of bits are left of radix point */
    for (;;)
    {
        uint32_t carry = 0;
        int sh = 1;
        for (i = 0; i < LD_B1B_DIG; i++)
        {
            k = (a + i & MASK);
            if (k == z || x[k] < th[i])
            {
                i = LD_B1B_DIG;
                break;
            }
            if (x[a + i & MASK] > th[i])
                break;
        }
        if (i == LD_B1B_DIG && rp == 9 * LD_B1B_DIG)
            break;
        /* FIXME: find a way to compute optimal sh */
        if (rp > 9 + 9 * LD_B1B_DIG)
            sh = 9;
        e2 += sh;
        for (k = a; k != z; k = (k + 1 & MASK))
        {
            uint32_t tmp = x[k] & (1 << sh) - 1;
            x[k] = (x[k] >> sh) + carry;
            carry = (1000000000 >> sh) * tmp;
            if (k == a && !x[k])
            {
                a = (a + 1 & MASK);
                i--;
                rp -= 9;
            }
        }
        if (carry)
        {
            if ((z + 1 & MASK) != a)
            {
                x[z] = carry;
                z = (z + 1 & MASK);
            }
            else
                x[z - 1 & MASK] |= 1;
        }
    }

    /* Assemble desired bits into floating point variable */
    for (y = i = 0; i < LD_B1B_DIG; i++)
    {
        if ((a + i & MASK) == z)
            x[(z = (z + 1 & MASK)) - 1] = 0;
        y = 1000000000.0L * y + x[a + i & MASK];
    }

    y *= sign;

    /* Limit precision for denormal results */
    if (bits > LDBL_MANT_DIG + e2 - emin)
    {
        bits = LDBL_MANT_DIG + e2 - emin;
        if (bits < 0)
            bits = 0;
        denormal = 1;
    }

    /* Calculate bias term to force rounding, move out lower bits */
    if (bits < LDBL_MANT_DIG)
    {
        bias = copysignl(scalbn(1, 2 * LDBL_MANT_DIG - bits - 1), y);
        frac = fmodl(y, scalbn(1, LDBL_MANT_DIG - bits));
        y -= frac;
        y += bias;
    }

    /* Process tail of decimal input so it can affect rounding */
    if ((a + i & MASK) != z)
    {
        uint32_t t = x[a + i & MASK];
        if (t < 500000000 && (t || (a + i + 1 & MASK) != z))
            frac += 0.25 * sign;
        else if (t > 500000000)
            frac += 0.75 * sign;
        else if (t == 500000000)
        {
            if ((a + i + 1 & MASK) == z)
                frac += 0.5 * sign;
            else
                frac += 0.75 * sign;
        }
        if (LDBL_MANT_DIG - bits >= 2 && !fmodl(frac, 1))
            frac++;
    }

    y += frac;
    y -= bias;

    if ((e2 + LDBL_MANT_DIG & INT_MAX) > emax - 5)
    {
        if (fabsl(y) >= 2 / LDBL_EPSILON)
        {
            if (denormal && bits == LDBL_MANT_DIG + e2 - emin)
                denormal = 0;
            y *= 0.5;
            e2++;
        }
        if (e2 + LDBL_MANT_DIG > emax || (denormal && frac))
            errno = ERANGE;
    }

    return scalbnl(y, e2);
}

/// @brief Temporary reimplementation of __floatscan
/// @param f
/// @param prec
/// @param pok
/// @return
long double gob_floatscan(FILE *file, int prec, int pok)
{
    int sign = 1;
    uint32_t i;
    int bits;
    int emin;
    int ch;

    switch (prec)
    {
    case 0:
        bits = FLT_MANT_DIG;
        emin = FLT_MIN_EXP - bits;
        break;
    case 1:
        bits = DBL_MANT_DIG;
        emin = DBL_MIN_EXP - bits;
        break;
    case 2:
        bits = LDBL_MANT_DIG;
        emin = LDBL_MIN_EXP - bits;
        break;
    default:
        return 0;
    }
    while (isspace((ch = scan_help_getc(file))))
        ;

    if (ch == '+' || ch == '-')
    {
        sign -= 2 * (ch == '-');
        ch = scan_help_getc(file);
    }

    for (i = 0; i < 8 && (ch | 32) == "infinity"[i]; i++)
        if (i < 7)
            ch = scan_help_getc(file);
    if (i == 3 || i == 8 || (i > 3 && pok))
    {
        if (i != 8)
        {
            scan_help_unget(file);
            if (pok)
                for (; i > 3; i--)
                    scan_help_unget(file);
        }
        return sign * INFINITY;
    }
    if (!i)
        for (i = 0; i < 3 && (ch | 32) == "nan"[i]; i++)
            if (i < 2)
                ch = scan_help_getc(file);
    if (i == 3)
    {
        if (scan_help_getc(file) != '(')
        {
            scan_help_unget(file);
            return NAN;
        }
        for (i = 1;; i++)
        {
            ch = scan_help_getc(file);
            if (ch - '0' < 10U || ch - 'A' < 26U || ch - 'a' < 26U || ch == '_')
                continue;
            if (ch == ')')
                return NAN;
            scan_help_unget(file);
            if (!pok)
            {
                errno = EINVAL;
                scan_help_lim(file, 0);
                return 0;
            }
            while (i--)
                scan_help_unget(file);
            return NAN;
        }
        return NAN;
    }

    if (i)
    {
        scan_help_unget(file);
        errno = EINVAL;
        scan_help_lim(file, 0);
        return 0;
    }

    // TODO: Add hex float
    // if (ch == '0')
    // {
    //     ch = scan_help_getc(file);
    //     if ((ch | 32) == 'x')
    //         return hexfloat(f, bits, emin, sign, pok);
    //     scan_help_unget(file);
    //     ch = '0';
    // }

    return gob_decfloat(file, ch, bits, emin, sign, pok);
}