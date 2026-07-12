// SPDX-License-Identifier: Apache-2.0

/* Codec vectors mirroring the shared Go/TS/Rust/Py suite: integer widths,
 * endianness, scaling, refs, strings, BCD, flags, fields, datetime,
 * sentinels, composed values, encode round-trips, write constraints. */
#include "moddef/moddef.h"
#include "pbw.h"
#include "test.h"

/* Wire enum values (types/mapping.proto). */
#define SP_HOLD 4
#define ST_U16 2
#define ST_S16 3
#define ST_U32 4
#define ST_U64 6
#define ST_S48 18
#define ST_F32 8
#define ST_STRING_ASCII 10
#define ST_BCD 13
#define ST_COMPOSED 15
#define PR_BOOL 1
#define PR_UINT32 3
#define PR_INT32 2
#define PR_INT64 4
#define PR_UINT64 5
#define PR_DECIMAL 8
#define PR_STRING 9
#define PR_DATETIME 11
#define BO_BIG 1
#define WO_BIG 1
#define WO_LITTLE 2
#define MODE_POW10 1
#define ACC_RW 3

static md_point_desc_t parse(const pb_t *b)
{
    md_point_desc_t d;
    md_point_t pt = tp_point(b, SP_HOLD);
    OK(md_point_parse(&pt, &d) == MD_OK);
    return d;
}

static void test_integers(void)
{
    { /* U16 with 0.1 scale */
        PB(b, 128);
        tp_head(&b, "v", 0, ST_U16);
        tp_prim(&b, PR_DECIMAL);
        tp_mapping(&b, SP_HOLD, 0, 1, 0, 0);
        tp_scale(&b, 1, 10, 0, 0);
        md_point_desc_t d = parse(&b);
        md_value_t v;
        uint16_t regs[] = {2305};
        OK(md_decode(&d, regs, 1, &MD_CTX_EMPTY, &v) == MD_OK);
        OK(v.kind == MD_VAL_F64 && APPROX(v.v.f64, 230.5));
    }
    { /* S16 negative two's complement */
        PB(b, 128);
        tp_head(&b, "t", 0, ST_S16);
        tp_prim(&b, PR_DECIMAL);
        tp_mapping(&b, 0, 0, 1, 0, 0);
        tp_scale(&b, 1, 10, 0, 0);
        md_point_desc_t d = parse(&b);
        md_value_t v;
        uint16_t regs[] = {0xfff6};
        OK(md_decode(&d, regs, 1, &MD_CTX_EMPTY, &v) == MD_OK);
        OK(APPROX(v.v.f64, -1.0));
    }
    { /* U32 word orders */
        PB(b, 128);
        tp_head(&b, "e", 0, ST_U32);
        tp_prim(&b, PR_UINT32);
        tp_mapping(&b, 0, 0, 2, BO_BIG, WO_BIG);
        md_point_desc_t d = parse(&b);
        md_value_t v;
        uint16_t big[] = {0x0001, 0x86a0};
        OK(md_decode(&d, big, 2, &MD_CTX_EMPTY, &v) == MD_OK);
        OK(v.kind == MD_VAL_U64 && v.v.u64 == 100000);

        PB(b2, 128);
        tp_head(&b2, "e2", 0, ST_U32);
        tp_prim(&b2, PR_UINT32);
        tp_mapping(&b2, 0, 0, 2, BO_BIG, WO_LITTLE);
        md_point_desc_t d2 = parse(&b2);
        uint16_t little[] = {0x86a0, 0x0001};
        OK(md_decode(&d2, little, 2, &MD_CTX_EMPTY, &v) == MD_OK);
        OK(v.v.u64 == 100000);
    }
    { /* U64 */
        PB(b, 128);
        tp_head(&b, "acc", 0, ST_U64);
        tp_prim(&b, PR_UINT64);
        tp_mapping(&b, 0, 0, 4, 0, 0);
        md_point_desc_t d = parse(&b);
        md_value_t v;
        uint16_t regs[] = {0, 1, 0, 0};
        OK(md_decode(&d, regs, 4, &MD_CTX_EMPTY, &v) == MD_OK);
        OK(v.kind == MD_VAL_U64 && v.v.u64 == 4294967296ull);
    }
    { /* S48 sign extension */
        PB(b, 128);
        tp_head(&b, "n", 0, ST_S48);
        tp_prim(&b, PR_INT64);
        tp_mapping(&b, 0, 0, 3, 0, 0);
        md_point_desc_t d = parse(&b);
        md_value_t v;
        uint16_t regs[] = {0xffff, 0xffff, 0xfffe};
        OK(md_decode(&d, regs, 3, &MD_CTX_EMPTY, &v) == MD_OK);
        OK(v.kind == MD_VAL_I64 && v.v.i64 == -2);
    }
    { /* enum-backed returns raw */
        PB(b, 128);
        tp_head(&b, "mode", 0, ST_U16);
        tp_mapping(&b, 0, 0, 1, 0, 0);
        md_point_desc_t d = parse(&b);
        OK(d.vkind == MD_VK_ENUM);
        md_value_t v;
        uint16_t regs[] = {5};
        OK(md_decode(&d, regs, 1, &MD_CTX_EMPTY, &v) == MD_OK);
        OK(v.kind == MD_VAL_U64 && v.v.u64 == 5);
    }
}

static void test_float_sentinels_strings(void)
{
    { /* IEEE754 F32: 230.5f = 0x43668000 */
        PB(b, 128);
        tp_head(&b, "v", 0, ST_F32);
        tp_mapping(&b, 0, 0, 2, 0, 0);
        md_point_desc_t d = parse(&b);
        md_value_t v;
        uint16_t regs[] = {0x4366, 0x8000};
        OK(md_decode(&d, regs, 2, &MD_CTX_EMPTY, &v) == MD_OK);
        OK(fabs(v.v.f64 - 230.5) < 1e-4);
    }
    { /* u16 0xFFFF sentinel */
        PB(b, 128);
        tp_head(&b, "a", 0, ST_U16);
        tp_prim(&b, PR_DECIMAL);
        tp_mapping(&b, 0, 0, 1, 0, 0);
        tp_na(&b, 65535, "not_implemented");
        md_point_desc_t d = parse(&b);
        md_value_t v;
        uint16_t na[] = {0xffff};
        OK(md_decode(&d, na, 1, &MD_CTX_EMPTY, &v) == MD_OK);
        OK(v.kind == MD_VAL_UNAVAILABLE);
        OK(md_str_eq(v.na_meaning, MD_STR("not_implemented")));
        uint16_t ok_regs[] = {0xfffe};
        OK(md_decode(&d, ok_regs, 1, &MD_CTX_EMPTY, &v) == MD_OK);
        OK(v.kind != MD_VAL_UNAVAILABLE);
    }
    { /* s16 sentinel matches on masked raw */
        PB(b, 128);
        tp_head(&b, "a", 0, ST_S16);
        tp_prim(&b, PR_DECIMAL);
        tp_mapping(&b, 0, 0, 1, 0, 0);
        tp_na(&b, 32768, "");
        md_point_desc_t d = parse(&b);
        md_value_t v;
        uint16_t s[] = {0x8000};
        OK(md_decode(&d, s, 1, &MD_CTX_EMPTY, &v) == MD_OK);
        OK(v.kind == MD_VAL_UNAVAILABLE);
        uint16_t s2[] = {0x7fff};
        OK(md_decode(&d, s2, 1, &MD_CTX_EMPTY, &v) == MD_OK);
        OK(v.kind != MD_VAL_UNAVAILABLE);
    }
    { /* fixed-length space-padded ASCII: "AB12  " */
        PB(senc, 16);
        pb_v(&senc, 2, 2); /* padding: PADDING_SPACE */
        pb_v(&senc, 3, 1); /* termination: FIXED_LENGTH */
        PB(m, 32);
        pb_v(&m, 3, 3); /* length_words */
        pb_msg(&m, 12, &senc);
        PB(b, 128);
        tp_head(&b, "sn", 0, ST_STRING_ASCII);
        tp_prim(&b, PR_STRING);
        pb_msg(&b, 20, &m);
        md_point_desc_t d = parse(&b);
        OK(d.vkind == MD_VK_STRING);
        char buf[8];
        size_t len;
        uint16_t regs[] = {0x4142, 0x3132, 0x2020};
        OK(md_decode_string(&d, regs, 3, buf, sizeof buf, &len) == MD_OK);
        OK(len == 4 && strcmp(buf, "AB12") == 0);
    }
    { /* BCD digits */
        PB(b, 128);
        tp_head(&b, "b", 0, ST_BCD);
        tp_prim(&b, PR_DECIMAL);
        tp_mapping(&b, 0, 0, 1, 0, 0);
        md_point_desc_t d = parse(&b);
        md_value_t v;
        uint16_t regs[] = {0x1234};
        OK(md_decode(&d, regs, 1, &MD_CTX_EMPTY, &v) == MD_OK);
        OK(v.kind == MD_VAL_I64 && v.v.i64 == 1234);
    }
}

static void build_flags_point(pb_t *b)
{
    /* value_type(12){flags(5){bits(1): {0:"over_voltage"},{2:"over_temp"},{7:"door_open"}}} */
    PB(fl, 96);
    const struct {
        uint32_t bit;
        const char *name;
    } entries[] = {{0, "over_voltage"}, {2, "over_temp"}, {7, "door_open"}};
    for (int i = 0; i < 3; i++) {
        PB(e, 32);
        pb_v(&e, 1, entries[i].bit);
        pb_str(&e, 2, entries[i].name);
        pb_msg(&fl, 1, &e);
    }
    PB(vt, 112);
    pb_msg(&vt, 5, &fl);
    tp_head(b, "alarms", 0, ST_U16);
    pb_msg(b, 12, &vt);
    tp_mapping(b, 0, 0, 1, 0, 0);
}

static void test_flags_fields_datetime(void)
{
    { /* flag set */
        PB(b, 256);
        build_flags_point(&b);
        md_point_desc_t d = parse(&b);
        OK(d.vkind == MD_VK_FLAGS);
        md_value_t v;
        uint16_t regs[] = {0x85}; /* 0b10000101 */
        OK(md_decode(&d, regs, 1, &MD_CTX_EMPTY, &v) == MD_OK);
        OK(v.kind == MD_VAL_FLAGS);
        OK(md_flags_has(&d, v.v.bits, MD_STR("over_voltage")));
        OK(md_flags_has(&d, v.v.bits, MD_STR("over_temp")));
        OK(md_flags_has(&d, v.v.bits, MD_STR("door_open")));
        uint16_t zero[] = {0};
        OK(md_decode(&d, zero, 1, &MD_CTX_EMPTY, &v) == MD_OK);
        OK(!md_flags_has(&d, v.v.bits, MD_STR("over_voltage")));
    }
    { /* packed hour/minute fields (61) */
        PB(f1, 32);
        pb_str(&f1, 1, "hour");
        pb_v(&f1, 3, 8);
        pb_v(&f1, 4, 8);
        PB(f2, 32);
        pb_str(&f2, 1, "minute");
        pb_v(&f2, 3, 0);
        pb_v(&f2, 4, 8);
        PB(b, 192);
        tp_head(&b, "slot", 0, ST_U16);
        tp_prim(&b, PR_UINT32);
        pb_msg(&b, 61, &f1);
        pb_msg(&b, 61, &f2);
        tp_mapping(&b, 0, 0, 1, 0, 0);
        md_point_desc_t d = parse(&b);
        OK(d.vkind == MD_VK_FIELDS);
        md_value_t v;
        uint16_t regs[] = {(21 << 8) | 45};
        OK(md_decode(&d, regs, 1, &MD_CTX_EMPTY, &v) == MD_OK);
        uint64_t hour, minute;
        OK(md_field_value(&d, v.v.bits, MD_STR("hour"), &hour) == MD_OK);
        OK(md_field_value(&d, v.v.bits, MD_STR("minute"), &minute) == MD_OK);
        OK(hour == 21 && minute == 45);
    }
    { /* datetime epoch seconds */
        PB(dt, 8);
        pb_v(&dt, 1, 1); /* EPOCH_S */
        PB(b, 128);
        tp_head(&b, "rtc", 0, ST_U32);
        tp_prim(&b, PR_DATETIME);
        pb_msg(&b, 63, &dt);
        tp_mapping(&b, 0, 0, 2, 0, 0);
        md_point_desc_t d = parse(&b);
        md_value_t v;
        uint16_t regs[] = {0x6543, 0x2100};
        OK(md_decode(&d, regs, 2, &MD_CTX_EMPTY, &v) == MD_OK);
        OK(v.kind == MD_VAL_DATETIME && v.v.epoch == 0x65432100);
    }
}

static void test_refs_composed_selector(void)
{
    { /* scale_ref POW10 */
        PB(b, 128);
        tp_head(&b, "w", 0, ST_S16);
        tp_prim(&b, PR_DECIMAL);
        tp_mapping(&b, 0, 0, 1, 0, 0);
        tp_scale_ref(&b, "w_sf", MODE_POW10);
        md_point_desc_t d = parse(&b);
        OK(d.has_scale_ref);

        md_ctx_entry_t refs[] = {{MD_STR("w_sf"), -1}};
        md_ctx_t ctx = {refs, 1};
        md_value_t v;
        uint16_t regs[] = {2301};
        OK(md_decode(&d, regs, 1, &ctx, &v) == MD_OK);
        OK(APPROX(v.v.f64, 230.1));

        refs[0].val = 2;
        uint16_t regs2[] = {15};
        OK(md_decode(&d, regs2, 1, &ctx, &v) == MD_OK);
        OK(APPROX(v.v.f64, 1500.0));

        /* missing ref errors */
        OK(md_decode(&d, regs, 1, &MD_CTX_EMPTY, &v) == MD_ERR_UNRESOLVED_REF);

        /* encode divides by 10^sf */
        refs[0].val = -1;
        uint16_t out[1];
        md_value_t in = md_value_f64(230.1);
        OK(md_encode(&d, &in, &ctx, out, 1) == MD_OK);
        OK(out[0] == 2301);
    }
    { /* composed mantissa*base^exponent: 1500 * 10^-1 */
        PB(mant, 16);
        pb_v(&mant, 2, 0); /* offset 0 */
        pb_v(&mant, 3, 1);
        PB(expo, 16);
        pb_v(&expo, 2, 1);
        pb_v(&expo, 3, 1);
        PB(comp, 64);
        pb_msg(&comp, 1, &mant);
        pb_msg(&comp, 2, &expo);
        pb_i(&comp, 3, 10); /* base */
        PB(m, 96);
        pb_v(&m, 3, 2); /* length_words */
        pb_msg(&m, 10, &comp);
        PB(b, 192);
        tp_head(&b, "pwr", 0, ST_COMPOSED);
        tp_prim(&b, PR_DECIMAL);
        pb_msg(&b, 20, &m);
        md_point_desc_t d = parse(&b);
        OK(d.vkind == MD_VK_COMPOSED);
        md_value_t v;
        uint16_t regs[] = {1500, 0xffff};
        OK(md_decode(&d, regs, 2, &MD_CTX_EMPTY, &v) == MD_OK);
        OK(APPROX(v.v.f64, 150.0));
    }
    { /* selector cases (§10.5) */
        PB(case0, 32);
        pb_rational(&case0, 1, 1, 1000);
        PB(entry0, 48);
        pb_v(&entry0, 1, 0);
        pb_msg(&entry0, 2, &case0);
        PB(case1, 32);
        pb_rational(&case1, 1, 1, 1);
        PB(entry1, 48);
        pb_v(&entry1, 1, 1);
        pb_msg(&entry1, 2, &case1);
        PB(sel, 128);
        pb_str(&sel, 1, "fmt");
        pb_msg(&sel, 2, &entry0);
        pb_msg(&sel, 2, &entry1);
        PB(b, 256);
        tp_head(&b, "energy", 0, ST_U32);
        tp_prim(&b, PR_DECIMAL);
        tp_mapping(&b, 0, 0, 2, 0, 0);
        pb_msg(&b, 66, &sel);
        md_point_desc_t d = parse(&b);
        OK(d.has_selector);

        md_ctx_entry_t refs[] = {{MD_STR("fmt"), 0}};
        md_ctx_t ctx = {refs, 1};
        md_value_t v;
        uint16_t regs[] = {0, 5000};
        OK(md_decode(&d, regs, 2, &ctx, &v) == MD_OK);
        OK(APPROX(v.v.f64, 5.0));
        refs[0].val = 1;
        OK(md_decode(&d, regs, 2, &ctx, &v) == MD_OK);
        OK(APPROX(v.v.f64, 5000.0));
    }
}

static void test_encode_and_constraints(void)
{
    { /* scaled U16 round-trip + negative offset transform */
        PB(b, 128);
        tp_head(&b, "pf", ACC_RW, ST_U16);
        tp_prim(&b, PR_DECIMAL);
        tp_mapping(&b, 0, 0, 1, 0, 0);
        tp_scale(&b, 1, 10, -1, 1); /* value = raw*0.1 - 1 */
        md_point_desc_t d = parse(&b);
        md_value_t v;
        uint16_t regs[] = {15};
        OK(md_decode(&d, regs, 1, &MD_CTX_EMPTY, &v) == MD_OK);
        OK(APPROX(v.v.f64, 0.5));
        uint16_t out[1];
        md_value_t in = md_value_f64(0.5);
        OK(md_encode(&d, &in, &MD_CTX_EMPTY, out, 1) == MD_OK);
        OK(out[0] == 15);
    }
    { /* S16 negative encode */
        PB(b, 128);
        tp_head(&b, "t", ACC_RW, ST_S16);
        tp_prim(&b, PR_DECIMAL);
        tp_mapping(&b, 0, 0, 1, 0, 0);
        tp_scale(&b, 1, 10, 0, 0);
        md_point_desc_t d = parse(&b);
        uint16_t out[1];
        md_value_t in = md_value_f64(-1.0);
        OK(md_encode(&d, &in, &MD_CTX_EMPTY, out, 1) == MD_OK);
        OK(out[0] == 0xfff6);
    }
    { /* bool + f32 round-trips */
        PB(b, 128);
        tp_head(&b, "b", ACC_RW, ST_U16);
        tp_prim(&b, PR_BOOL);
        tp_mapping(&b, 0, 0, 1, 0, 0);
        md_point_desc_t d = parse(&b);
        uint16_t out[1];
        md_value_t in = md_value_bool(true);
        OK(md_encode(&d, &in, &MD_CTX_EMPTY, out, 1) == MD_OK);
        md_value_t v;
        OK(md_decode(&d, out, 1, &MD_CTX_EMPTY, &v) == MD_OK);
        OK(v.kind == MD_VAL_BOOL && v.v.b);

        PB(b2, 128);
        tp_head(&b2, "f32", ACC_RW, ST_F32);
        tp_mapping(&b2, 0, 0, 2, 0, 0);
        md_point_desc_t d2 = parse(&b2);
        uint16_t out2[2];
        in = md_value_f64(230.5);
        OK(md_encode(&d2, &in, &MD_CTX_EMPTY, out2, 2) == MD_OK);
        OK(md_decode(&d2, out2, 2, &MD_CTX_EMPTY, &v) == MD_OK);
        OK(fabs(v.v.f64 - 230.5) < 1e-4);
    }
    { /* string round-trip */
        PB(senc, 16);
        pb_v(&senc, 2, 1); /* PADDING_NULL */
        PB(m, 32);
        pb_v(&m, 3, 2);
        pb_msg(&m, 12, &senc);
        PB(b, 128);
        tp_head(&b, "s", ACC_RW, ST_STRING_ASCII);
        tp_prim(&b, PR_STRING);
        pb_msg(&b, 20, &m);
        md_point_desc_t d = parse(&b);
        uint16_t out[2];
        OK(md_encode_string(&d, MD_STR("Hi!"), out, 2) == MD_OK);
        char buf[8];
        size_t len;
        OK(md_decode_string(&d, out, 2, buf, sizeof buf, &len) == MD_OK);
        OK(strcmp(buf, "Hi!") == 0);
    }
    { /* §11.4 constraints: min/max/step then allowed_values */
        PB(wc, 96);
        pb_rational(&wc, 1, 0, 1);   /* min 0 */
        pb_rational(&wc, 2, 100, 1); /* max 100 */
        pb_rational(&wc, 3, 1, 1);   /* step 1 */
        PB(wr, 112);
        pb_msg(&wr, 4, &wc);
        PB(b, 224);
        tp_head(&b, "soc", ACC_RW, ST_U16);
        tp_prim(&b, PR_DECIMAL);
        tp_mapping(&b, 0, 0, 1, 0, 0);
        pb_msg(&b, 80, &wr);
        md_point_desc_t d = parse(&b);
        OK(d.has_constraints);
        md_value_t v = md_value_f64(80);
        OK(md_validate_write(&d, &v) == MD_OK);
        v = md_value_f64(101);
        OK(md_validate_write(&d, &v) == MD_ERR_CONSTRAINT_MAX);
        v = md_value_f64(-1);
        OK(md_validate_write(&d, &v) == MD_ERR_CONSTRAINT_MIN);
        v = md_value_f64(50.5);
        OK(md_validate_write(&d, &v) == MD_ERR_CONSTRAINT_STEP);

        /* allowed_values: packed int64 [0,1,2] */
        PB(wc2, 32);
        {
            uint8_t packed[] = {0, 1, 2};
            pb_bytes(&wc2, 4, packed, 3);
        }
        PB(wr2, 48);
        pb_msg(&wr2, 4, &wc2);
        PB(b2, 128);
        tp_head(&b2, "mode", ACC_RW, ST_U16);
        tp_prim(&b2, PR_UINT32);
        tp_mapping(&b2, 0, 0, 1, 0, 0);
        pb_msg(&b2, 80, &wr2);
        md_point_desc_t d2 = parse(&b2);
        v = md_value_i64(2);
        OK(md_validate_write(&d2, &v) == MD_OK);
        v = md_value_i64(3);
        OK(md_validate_write(&d2, &v) == MD_ERR_CONSTRAINT_ALLOWED);
    }
}

int main(void)
{
    test_integers();
    test_float_sentinels_strings();
    test_flags_fields_datetime();
    test_refs_composed_selector();
    test_encode_and_constraints();
    return t_report("test_codec");
}
