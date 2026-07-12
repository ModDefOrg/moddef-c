// SPDX-License-Identifier: Apache-2.0

/* Document conformance: the golden binary fixtures from moddef/fixtures
 * must parse and navigate; the full growatt registry profile (converted
 * yaml→binary at build time) must expose all 396 points with sane
 * envelopes. */
#include <stdio.h>
#include <stdlib.h>

#include "moddef/moddef.h"
#include "test.h"

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("SKIP-FATAL: cannot open %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n)
        exit(1);
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static void test_golden(const char *path, const char *doc_id)
{
    size_t len;
    uint8_t *buf = read_file(path, &len);
    md_doc_t doc;
    OK(md_doc_init(&doc, buf, len) == MD_OK);
    OK(md_str_eq(md_doc_id(&doc), md_str_cstr(doc_id)));

    md_devprof_t prof;
    OK(md_doc_device(&doc, (md_str_t){0, 0}, &prof) == MD_OK);

    /* Every point must parse into a descriptor. */
    md_point_iter_t it = md_point_iter(&prof);
    md_point_t pt;
    int n = 0;
    while (md_point_next(&it, &pt)) {
        md_point_desc_t d;
        OK(md_point_parse(&pt, &d) == MD_OK);
        OK(d.id.len > 0);
        OK(md_point_words(&d) >= 1);
        n++;
    }
    OK(n > 0);
    free(buf);
}

static void test_growatt(void)
{
    size_t len;
    uint8_t *buf = read_file("build/growatt-sph.moddef", &len);
    md_doc_t doc;
    OK(md_doc_init(&doc, buf, len) == MD_OK);

    md_devprof_t prof;
    OK(md_doc_device(&doc, (md_str_t){0, 0}, &prof) == MD_OK);

    int n = 0;
    md_point_iter_t it = md_point_iter(&prof);
    md_point_t pt;
    while (md_point_next(&it, &pt)) {
        md_point_desc_t d;
        OK(md_point_parse(&pt, &d) == MD_OK);
        n++;
    }
    OK(n == 396); /* full documented register coverage */

    /* Spot-check a known point: pv1_voltage @ input 3, scale 1/10. */
    md_point_t pv;
    OK(md_devprof_find_point(&prof, MD_STR("pv1_voltage"), &pv) == MD_OK);
    md_point_desc_t d;
    OK(md_point_parse(&pv, &d) == MD_OK);
    OK(d.space == MD_SPACE_INPUT);
    OK(d.offset == 3);
    OK(d.scale.num == 1 && d.scale.den == 10);
    md_value_t v;
    uint16_t regs[] = {2305};
    OK(md_decode(&d, regs, 1, &MD_CTX_EMPTY, &v) == MD_OK);
    OK(APPROX(v.v.f64, 230.5));

    free(buf);
}

int main(void)
{
    test_golden("../moddef/fixtures/golden/energy-meter/energy-meter.moddef",
                "example.energy-meter");
    test_golden("../moddef/fixtures/golden/sunspec-inverter/sunspec-inverter.moddef",
                "example.sunspec-inverter");
    test_golden("../moddef/fixtures/golden/battery-control/battery-control.moddef",
                "example.battery");
    test_growatt();
    return t_report("test_doc");
}
