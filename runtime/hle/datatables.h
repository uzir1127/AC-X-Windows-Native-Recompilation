#ifndef PSPRECO_DATATABLES_H
#define PSPRECO_DATATABLES_H

#include "pspreco.h"

/* The game's fixed-stride parameter tables, described once and edited live.
 *
 * PL_PARAM, WPN_PRM and GUN_PRM share a shape -- a u32 count then N rows of a
 * fixed size -- so one reader and one editor cover all three.  They differ
 * only in the field list and in how an edit reaches the running game, both of
 * which are data.
 */

enum { PF_F32, PF_U8, PF_S8, PF_U16, PF_S16, PF_S32, PF_U32, PF_STR };

/* How an edit reaches the running game. */
enum {
    LIVE_INPLACE = 0,  /* the engine indexes the table itself   */
    LIVE_COPY    = 1,  /* it copies a row out; patch both       */
    LIVE_REOPEN  = 2   /* it copies at build; next open shows it */
};

typedef struct {
    unsigned    off;
    int         type;
    const char *name;                 /* "" when nothing has named it */
    const char *note;                 /* the evidence, or what it reads as */
    int         traced;               /* a reader was found, vs inferred */
    double      scale;                /* 0 = show the raw value only */
    const char *unit;                 /* what one unit is, once scaled */
    unsigned    len;                  /* PF_STR only: the fixed cell width */
} TableField;

/* Both pinned from the engine in docs/formats/PRM.md: sub_69AA8 writes
 * 18.518518 / 3.4722223 / 10.416667 / 13.888889 into speed fields, which are
 * 800 / 150 / 450 / 600 km/h at 43.2, and the speed integrator's gravity term
 * is 9.8 m/s^2 in 0.2 m units per frame^2 at 60 fps. */
#define TBL_KMH_PER_UNIT 43.2
#define TBL_METRES_PER_UNIT 0.2

typedef struct {
    const char       *record;         /* the registry name */
    const char       *title;
    unsigned          stride;
    unsigned          header;         /* bytes before row 0 -- the count word */
    unsigned          float_block;    /* enumerate every float below this */
    const TableField *fields;
    int               field_count;
    int               live_kind;      /* LIVE_* above */
    const char       *live_note;
    const char       *aliases;        /* comma separated; MMES ships once per
                                       * language, one loaded at a time      */
    int               list_view;      /* render every row down the page rather
                                       * than one row at a time */
} AcxTable;

extern const AcxTable g_acx_tables[];
extern const int      g_acx_table_count;

void table_init(void);

/* Guest address of row 0, or 0 before the record is registered. */
u32  table_base(u8 *ram, const AcxTable *t, int *out_rows);

const TableField *table_field_at(const AcxTable *t, unsigned off);

double table_get(u8 *ram, const AcxTable *t, int row, const TableField *f);
void   table_set(u8 *ram, const AcxTable *t, int row, const TableField *f,
                 double v);

/* What the disc shipped.  `ok` is cleared if the stock copy has not been
 * captured yet. */
double table_get_stock(const AcxTable *t, int row, const TableField *f, int *ok);

/* PF_STR fields.  `out` is filled and NUL-terminated. */
void table_get_str(u8 *ram, const AcxTable *t, int row, const TableField *f,
                   char *out, int out_len);
void table_set_str(u8 *ram, const AcxTable *t, int row, const TableField *f,
                   const char *s);
void table_get_stock_str(const AcxTable *t, int row, const TableField *f,
                         char *out, int out_len);

void table_revert_row(u8 *ram, const AcxTable *t, int row);
int  table_save_mod(u8 *ram, const AcxTable *t, const char *mod,
                    char *out_path, int out_len);

/* PL_PARAM only.  Aircraft_Load copies a row to object+600 and folds the
 * fitted parts in, so the table alone is not what the engine flies. */
u32  plane_live(void);
int  plane_live_row(void);

#endif
