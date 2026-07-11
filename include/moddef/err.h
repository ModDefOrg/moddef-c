/* Error codes (spec §26.3/§26.4, §32). Every fallible call returns md_err_t;
 * MD_OK is 0 so `if (err)` reads naturally. */
#ifndef MODDEF_ERR_H
#define MODDEF_ERR_H

typedef enum md_err {
    MD_OK = 0,
    MD_ERR_TRANSPORT,        /* transport callback reported failure */
    MD_ERR_PARSE,            /* malformed wire data */
    MD_ERR_NOT_FOUND,        /* device profile / point id not found */
    MD_ERR_SHORT_READ,       /* register window shorter than the point */
    MD_ERR_BUFFER,           /* caller buffer too small */
    MD_ERR_UNRESOLVED_REF,   /* scale_ref/selector_ref target not in context */
    MD_ERR_ZERO_SCALE_DEN,   /* scale denominator is zero (MDE405) */
    MD_ERR_COMPOSED_BASE,    /* composed base is zero (MDE406) */
    MD_ERR_NOT_WRITABLE,     /* access mode forbids writing */
    MD_ERR_WRONG_TYPE,       /* value kind does not match the point */
    MD_ERR_UNSUPPORTED,      /* composed/packed-field encode, bad space, ... */
    MD_ERR_CONSTRAINT_MIN,   /* §11.4 below min_value */
    MD_ERR_CONSTRAINT_MAX,   /* §11.4 above max_value */
    MD_ERR_CONSTRAINT_STEP,  /* §11.4 not a multiple of step */
    MD_ERR_CONSTRAINT_ALLOWED, /* §11.4 not in allowed_values */
    MD_ERR_DISCOVERY,        /* SunS marker / model not found (§7.3) */
    MD_ERR_UNAVAILABLE       /* §8.4 sentinel hit (typed accessors) */
} md_err_t;

/* Static human-readable name, e.g. "MD_ERR_SHORT_READ". */
const char *md_err_str(md_err_t err);

#endif /* MODDEF_ERR_H */
