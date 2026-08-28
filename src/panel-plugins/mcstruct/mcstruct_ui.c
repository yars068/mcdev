/*
   mcstruct - the screen: structure tree, hex strip, def-file

   Copyright (C) 2026
   Free Software Foundation, Inc.

   Written by:
   Ilia Maslakov <il.smind@gmail.com>, 2026.

   This file is part of the Midnight Commander.

   The Midnight Commander is free software: you can redistribute it
   and/or modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation, either version 3 of the License,
   or (at your option) any later version.

   The Midnight Commander is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <stdlib.h>
#include <string.h>

#include "lib/global.h"
#include "lib/tty/tty.h"
#include "lib/tty/key.h"
#include "lib/skin.h"
#include "lib/strutil.h"
#include "lib/util.h"
#include "lib/widget.h"
#include "lib/event.h"
#include "lib/lock.h"
#include "lib/vfs/vfs.h"

#include "src/editor/edit.h"

#include "slv.h"
#include "slv_load.h"
#include "slv_reader.h"
#include "slv_edit.h"
#include "mcstruct_ui.h"

/*** global variables ****************************************************************************/

/*** file scope macro definitions ****************************************************************/

#define TREE_COLS 5
#define DEF_COLS  2

/*** file scope type declarations ****************************************************************/

/* a one-line status bar in the skin's statusbar color, as mcedit has */
typedef struct
{
    Widget widget;
    char *text;
    int color; /* -1 = the statusbar color of the skin */
} WStatus;

/* skin sections [mcstruct-tree], [mcstruct-hex], [mcstruct-def] */
typedef struct
{
    int tree_normal, tree_frame, tree_frame_active, tree_head, tree_selected;
    int tree_offset, tree_name, tree_type, tree_value;
    int tree_struct, tree_jump, tree_remark, tree_error;
    int hex_head;
    hexstrip_colors_t hex;
    int def_normal, def_frame, def_frame_active, def_head, def_selected;
    int def_lineno, def_directive, def_comment, def_label;
} ui_colors_t;

enum
{
    CMD_HELP = 1,
    CMD_SAVE,
    CMD_STRUCT,
    CMD_EDIT,
    CMD_GOTO,
    CMD_DEF,
    CMD_SEARCH,
    CMD_HEX,
    CMD_ERRORS,
    CMD_QUIT,
    CMD_BACK, /* Shift-F4: back to the viewer */
    CMD_EDIT_DEF,
    CMD_SEARCH_NEXT,
    CMD_CALC
};

enum
{
    COL_OFFSET,
    COL_KEY,
    COL_HINT,
    COL_VALUE,
    COL_COMMENT
};

typedef struct
{
    slv_node_t *node;
    int depth;
    int index; /* position in the parent's children */
} row_t;

typedef struct
{
    slv_node_t *root;
    int current;
} jump_t;

typedef struct
{
    WDialog *dlg;
    WStatus *title;
    WStatus *tree_head;
    WTable *tree;
    WStatus *hex_head;
    WHexStrip *hex;
    WStatus *def_head;
    WTable *def;
    WButtonBar *bb;
    ui_colors_t colors;

    slv_settings_t settings;
    slv_file_reader_t *fr;
    slv_file_t *file;
    char *def_path;
    slv_eval_t ev;
    slv_node_t *root;
    GArray *rows;     /* row_t */
    GPtrArray *jumps; /* jump_t * */
    GString *cell;    /* scratch for the datasource */
    gboolean syncing;
    gboolean hex_hidden;
    char *search;
    char *display_name;
    gboolean quit;
    gboolean back; /* left with Shift-F4 */
    vfs_path_t *vpath;
    gboolean locked;

    /* table screen: replaces the tree zone while grid != NULL */
    WTable *grid;
    slv_node_t *grid_node;
    char *grid_path;      /* index path of grid_node, to find it again after a refresh */
    GPtrArray *grid_cols; /* slv_node_t *: the leaf fields of the first row, one per column */
    int grid_cache_row;   /* leaves of this row are in grid_cache */
    GPtrArray *grid_cache;
    int grid_first_col; /* horizontal scroll */
    int grid_ncols;     /* columns shown */
    int tree_current;   /* tree row to return to */
    WRect box[3];       /* frames around tree, def-file and hex zones; lines 0 = none */
    int zoom;           /* 0 none, 1 tree, 2 hex, 3 def-file */
    int zoom_key;
} ui_t;

/*** forward declarations (file scope functions) *************************************************/

static void ui_rebuild_rows (ui_t *ui);
static void ui_sync_from_tree (ui_t *ui);
static void ui_layout (ui_t *ui);
static void ui_update_heads (ui_t *ui);
static void ui_edit_node (ui_t *ui, slv_node_t *n);
static void ui_close_grid (ui_t *ui);
static void ui_follow_jump_node (ui_t *ui, slv_node_t *n);
static void ui_open_grid (ui_t *ui, slv_node_t *node);
static gboolean node_is_gridable (const slv_node_t *n);
static void ui_grid_build (ui_t *ui);
static slv_node_t *ui_grid_cell_node (ui_t *ui, int row, int col);
static void ui_grid_heads (ui_t *ui);

/*** file scope variables ************************************************************************/

static const global_keymap_t bb_keymap[] = {
    { KEY_F (1), CMD_HELP, "F1" },        { KEY_F (2), CMD_SAVE, "F2" },
    { KEY_F (3), CMD_STRUCT, "F3" },      { KEY_F (4), CMD_EDIT, "F4" },
    { KEY_F (5), CMD_GOTO, "F5" },        { KEY_F (6), CMD_DEF, "F6" },
    { KEY_F (7), CMD_SEARCH, "F7" },      { KEY_F (8), CMD_HEX, "F8" },
    { KEY_F (9), CMD_ERRORS, "F9" },      { KEY_F (10), CMD_QUIT, "F10" },
    { KEY_F (16), CMD_EDIT_DEF, "S-F6" }, { KEY_F (17), CMD_SEARCH_NEXT, "S-F7" },
    { KEY_F (14), CMD_BACK, "S-F4" },     { 0, 0, "" }
};

/* --------------------------------------------------------------------------------------------- */
/*** file scope functions ************************************************************************/
/* --------------------------------------------------------------------------------------------- */

static cb_ret_t
status_callback (Widget *w, Widget *sender, widget_msg_t msg, int parm, void *data)
{
    WStatus *st = (WStatus *) w;

    switch (msg)
    {
    case MSG_DRAW:
        tty_setcolor (st->color >= 0 ? st->color : STATUSBAR_COLOR);
        widget_gotoyx (w, 0, 0);
        tty_print_string (str_fit_to_term (st->text != NULL ? st->text : "", w->rect.cols, J_LEFT));
        return MSG_HANDLED;
    case MSG_DESTROY:
        g_free (st->text);
        return MSG_HANDLED;
    default:
        return widget_default_callback (w, sender, msg, parm, data);
    }
}

/* --------------------------------------------------------------------------------------------- */

static WStatus *
status_new (int y, int x, int cols)
{
    WStatus *st = g_new0 (WStatus, 1);
    WRect r = { y, x, 1, cols };

    widget_init (WIDGET (st), &r, status_callback, NULL);
    st->color = -1;
    return st;
}

/* --------------------------------------------------------------------------------------------- */

static void
status_set_text (WStatus *st, const char *text)
{
    g_free (st->text);
    st->text = g_strdup (text);
    widget_draw (WIDGET (st));
}

/* --------------------------------------------------------------------------------------------- */

static row_t *
ui_current_row (ui_t *ui)
{
    int cur = table_get_current (ui->tree);

    if (ui->rows->len == 0 || cur < 0 || (guint) cur >= ui->rows->len)
        return NULL;
    return &g_array_index (ui->rows, row_t, cur);
}

/* --------------------------------------------------------------------------------------------- */

static gboolean
node_has_children (const slv_node_t *n)
{
    return n->lazy || (n->children != NULL && n->children->len > 0);
}

/* --------------------------------------------------------------------------------------------- */

static gboolean
node_visible (const ui_t *ui, const slv_node_t *n)
{
    if (n->kind == SLV_NODE_FIELD && n->item != NULL && n->item->hidden)
        return ui->settings.show_hidden;
    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */

static void
add_rows (ui_t *ui, slv_node_t *node, int depth)
{
    guint i;

    if (node->children == NULL)
        return;
    for (i = 0; i < node->children->len; i++)
    {
        slv_node_t *c = g_ptr_array_index (node->children, i);
        row_t r;

        if (!node_visible (ui, c))
            continue;
        r.node = c;
        r.depth = depth;
        r.index = (int) i;
        g_array_append_val (ui->rows, r);
        if (c->expanded && !c->lazy)
            add_rows (ui, c, depth + 1);
    }
}

/* --------------------------------------------------------------------------------------------- */

static void
ui_rebuild_rows (ui_t *ui)
{
    row_t r;

    g_array_set_size (ui->rows, 0);
    if (ui->root == NULL)
        return;
    r.node = ui->root;
    r.depth = 0;
    r.index = 0;
    g_array_append_val (ui->rows, r);
    if (ui->root->expanded)
        add_rows (ui, ui->root, 1);
}

/* --------------------------------------------------------------------------------------------- */

static void
expand_default (slv_node_t *node, int depth)
{
    guint i;

    node->expanded = depth < 2 && !node->lazy;
    if (node->children == NULL)
        return;
    for (i = 0; i < node->children->len; i++)
        expand_default (g_ptr_array_index (node->children, i), depth + 1);
}

/* --------------------------------------------------------------------------------------------- */

static const char *
tree_key_text (ui_t *ui, const row_t *r)
{
    const slv_node_t *n = r->node;
    const char *marker = node_has_children (n) ? (n->expanded ? "- " : "+ ") : "  ";
    int i;

    g_string_truncate (ui->cell, 0);
    for (i = 0; i < r->depth; i++)
        g_string_append (ui->cell, "  ");
    g_string_append (ui->cell, marker);
    switch (n->kind)
    {
    case SLV_NODE_STRUCT:
        if (r->depth == 0)
            g_string_append_printf (ui->cell, "/%s", n->key != NULL ? n->key : "");
        else
            g_string_append_printf (ui->cell, "[%d] %s", r->index, n->key != NULL ? n->key : "");
        break;
    case SLV_NODE_REPEAT:
        g_string_append (ui->cell, "#repeat");
        break;
    case SLV_NODE_REMARK:
        g_string_append_printf (ui->cell, ": %s", n->key != NULL ? n->key : "");
        break;
    case SLV_NODE_ERROR:
        g_string_append (ui->cell, "ERROR");
        break;
    default:
        g_string_append (ui->cell, n->key != NULL ? n->key : "");
        break;
    }
    return ui->cell->str;
}

/* --------------------------------------------------------------------------------------------- */

static int
tree_get_nrows (const void *data)
{
    const ui_t *ui = data;

    return (int) ui->rows->len;
}

/* --------------------------------------------------------------------------------------------- */

static const char *
tree_get_text (const void *data, int row, int col)
{
    ui_t *ui = (ui_t *) data;
    const row_t *r;
    const slv_node_t *n;

    if (row < 0 || (guint) row >= ui->rows->len)
        return "";
    r = &g_array_index (ui->rows, row_t, row);
    n = r->node;

    switch (col)
    {
    case COL_OFFSET:
        if (n->kind == SLV_NODE_ERROR)
            return "";
        g_string_printf (ui->cell, "%08llX",
                         (unsigned long long) (ui->settings.offset_column == 1 && ui->root != NULL
                                                   ? n->offset - ui->root->offset
                                                   : n->offset));
        return ui->cell->str;
    case COL_KEY:
        return tree_key_text (ui, r);
    case COL_HINT:
        if (n->kind == SLV_NODE_STRUCT)
        {
            g_string_printf (ui->cell, "%lld", (long long) n->size);
            return ui->cell->str;
        }
        return n->hint != NULL ? n->hint : "";
    case COL_VALUE:
        if (n->kind == SLV_NODE_ERROR)
            return n->text != NULL ? n->text : "";
        if (n->kind == SLV_NODE_STRUCT || n->kind == SLV_NODE_REMARK)
            return "";
        g_string_truncate (ui->cell, 0);
        if (n->text != NULL)
            g_string_append (ui->cell, n->text);
        if (n->legend != NULL)
            g_string_append_printf (ui->cell, " (%s)", n->legend);
        if (n->kind == SLV_NODE_JUMP)
            g_string_append_printf (ui->cell, " -> %08llX", (unsigned long long) n->jump_target);
        if (n->lazy)
            g_string_append (ui->cell, " ...");
        return ui->cell->str;
    case COL_COMMENT:
        return n->comment != NULL ? n->comment : "";
    default:
        return "";
    }
}

/* --------------------------------------------------------------------------------------------- */

static int
def_get_nrows (const void *data)
{
    const ui_t *ui = data;

    return ui->file != NULL ? (int) ui->file->lines->len : 0;
}

/* --------------------------------------------------------------------------------------------- */

static const char *
def_get_text (const void *data, int row, int col)
{
    ui_t *ui = (ui_t *) data;

    if (ui->file == NULL || row < 0 || (guint) row >= ui->file->lines->len)
        return "";
    if (col == 0)
    {
        g_string_printf (ui->cell, "%5d", row + 1);
        return ui->cell->str;
    }
    return g_ptr_array_index (ui->file->lines, row);
}

/* --------------------------------------------------------------------------------------------- */

static int
tree_get_color (const void *data, int row, int col)
{
    const ui_t *ui = data;
    const slv_node_t *n;

    if (row < 0 || (guint) row >= ui->rows->len)
        return -1;
    n = g_array_index (ui->rows, row_t, row).node;
    switch (n->kind)
    {
    case SLV_NODE_ERROR:
        return ui->colors.tree_error;
    case SLV_NODE_REMARK:
        return ui->colors.tree_remark;
    case SLV_NODE_STRUCT:
    case SLV_NODE_NESTED:
    case SLV_NODE_TABLE:
    case SLV_NODE_REPEAT:
        return col == COL_OFFSET ? ui->colors.tree_offset : ui->colors.tree_struct;
    case SLV_NODE_JUMP:
        return col == COL_OFFSET ? ui->colors.tree_offset : ui->colors.tree_jump;
    default:
        break;
    }
    if (n->legend != NULL && col == COL_VALUE && g_str_has_prefix (n->legend, "MISMATCH"))
        return ui->colors.tree_error;
    switch (col)
    {
    case COL_OFFSET:
        return ui->colors.tree_offset;
    case COL_KEY:
        return ui->colors.tree_name;
    case COL_HINT:
        return ui->colors.tree_type;
    default:
        return ui->colors.tree_value;
    }
}

/* --------------------------------------------------------------------------------------------- */

static int
def_get_color (const void *data, int row, int col)
{
    const ui_t *ui = data;
    const char *line;
    if (ui->file == NULL || row < 0 || (guint) row >= ui->file->lines->len)
        return -1;
    if (col == 0)
        return ui->colors.def_lineno;
    line = g_ptr_array_index (ui->file->lines, row);
    while (*line == ' ' || *line == '\t')
        line++;
    if (*line == '#' || *line == '/')
        return ui->colors.def_directive;
    if (*line == ';')
        return ui->colors.def_comment;
    {
        const char *p = line;

        while (g_ascii_isalnum (*p) || *p == '_')
            p++;
        if (p > line && *p == ':' && !g_ascii_isdigit (*line))
            return ui->colors.def_label;
    }
    return -1;
}

/* --------------------------------------------------------------------------------------------- */

static void
ui_update_heads (ui_t *ui)
{
    const row_t *r = ui_current_row (ui);
    const slv_node_t *n = r != NULL ? r->node : NULL;
    off_t cur = hexstrip_get_cursor (ui->hex);
    int cols = WIDGET (ui->dlg)->rect.cols;
    char *s, *padded;

    if (ui->grid != NULL)
        s = g_strdup_printf (" mcstruct  %s  %s  %s  row %d/%u  col %d/%u  %s", ui->display_name,
                             ui->def_path != NULL ? x_basename (ui->def_path) : "-",
                             ui->grid_node->def != NULL       ? ui->grid_node->def->name
                                 : ui->grid_node->key != NULL ? ui->grid_node->key
                                                              : "",
                             table_get_current (ui->grid) + 1, ui->grid_node->children->len,
                             ui->grid_first_col + ui->grid->current_col + 1, ui->grid_cols->len,
                             slv_file_reader_change_count (ui->fr) > 0 ? "[modified]" : "");
    else
        s = g_strdup_printf (" mcstruct  %s  %s  %s  %s", ui->display_name,
                             ui->def_path != NULL ? x_basename (ui->def_path) : "-",
                             ui->root != NULL && ui->root->def != NULL ? ui->root->def->name : "",
                             slv_file_reader_change_count (ui->fr) > 0 ? "[modified]" : "");
    padded = g_strdup_printf ("%-*s", cols, s);
    status_set_text (ui->title, s);
    g_free (padded);
    g_free (s);

    if (ui->grid != NULL)
    {
        ui_grid_heads (ui);
        n = ui_grid_cell_node (ui, table_get_current (ui->grid), ui->grid->current_col);
    }
    else
    {
        s = g_strdup_printf (" %s: %lld bytes at %08llX%*s%d/%u   %s",
                             ui->root != NULL && ui->root->def != NULL ? ui->root->def->name : "-",
                             ui->root != NULL ? (long long) ui->root->size : 0LL,
                             ui->root != NULL ? (unsigned long long) ui->root->offset : 0ULL, 4, "",
                             table_get_current (ui->tree) + 1, ui->rows->len,
                             n != NULL && n->key != NULL ? n->key : "");
        padded = g_strdup_printf ("%-*s", WIDGET (ui->tree)->rect.cols, s);
        status_set_text (ui->tree_head, padded);
        g_free (padded);
        g_free (s);
    }

    s = g_strdup_printf (" offset %08llX (%lld)  local +%llX  field %lld bytes  file %lld bytes",
                         (unsigned long long) cur, (long long) cur,
                         (unsigned long long) (ui->root != NULL ? cur - ui->root->offset : cur),
                         n != NULL ? (long long) n->size : 0LL, (long long) ui->fr->size);
    padded = g_strdup_printf ("%-*s", cols, s);
    status_set_text (ui->hex_head, s);
    g_free (padded);
    g_free (s);

    s = g_strdup_printf (
        " %s  line %d/%u%s%u errors", ui->def_path != NULL ? x_basename (ui->def_path) : "-",
        table_get_current (ui->def) + 1, ui->file != NULL ? ui->file->lines->len : 0, "  ",
        ui->file != NULL ? ui->file->errors->len : 0);
    padded = g_strdup_printf ("%-*s", WIDGET (ui->def)->rect.cols, s);
    status_set_text (ui->def_head, padded);
    g_free (padded);
    g_free (s);
}

/* --------------------------------------------------------------------------------------------- */

static void
ui_sync_from_tree (ui_t *ui)
{
    const row_t *r = ui_current_row (ui);

    if (r == NULL || ui->syncing)
        return;
    ui->syncing = TRUE;
    hexstrip_set_mark (ui->hex, r->node->offset, r->node->size);
    if (r->node->line > 0 && ui->file != NULL && (guint) r->node->line <= ui->file->lines->len)
    {
        table_set_current (ui->def, r->node->line - 1);
        widget_draw (WIDGET (ui->def));
    }
    ui_update_heads (ui);
    ui->syncing = FALSE;
}

/* --------------------------------------------------------------------------------------------- */

/* the deepest visible row covering the hex cursor */
static void
ui_sync_from_hex (ui_t *ui)
{
    off_t cur = hexstrip_get_cursor (ui->hex);
    int best = -1;
    off_t best_size = -1;
    guint i;

    if (ui->syncing)
        return;
    for (i = 0; i < ui->rows->len; i++)
    {
        const row_t *r = &g_array_index (ui->rows, row_t, i);
        const slv_node_t *n = r->node;

        if (n->size <= 0 || cur < n->offset || cur >= n->offset + n->size)
            continue;
        if (best < 0 || n->size <= best_size)
        {
            best = (int) i;
            best_size = n->size;
        }
    }
    ui->syncing = TRUE;
    if (best >= 0 && best != table_get_current (ui->tree))
    {
        const row_t *r = &g_array_index (ui->rows, row_t, best);

        table_set_current (ui->tree, best);
        widget_draw (WIDGET (ui->tree));
        if (r->node->line > 0 && ui->file != NULL)
        {
            table_set_current (ui->def, r->node->line - 1);
            widget_draw (WIDGET (ui->def));
        }
        widget_draw (WIDGET (ui->def));
    }
    ui_update_heads (ui);
    ui->syncing = FALSE;
}

/* --------------------------------------------------------------------------------------------- */

static void
hex_on_cursor (WHexStrip *h, void *data)
{
    (void) h;
    ui_sync_from_hex (data);
}

/* --------------------------------------------------------------------------------------------- */

static void
ui_set_root (ui_t *ui, slv_node_t *root, int current)
{
    ui->root = root;
    ui_rebuild_rows (ui);
    if (ui->rows->len == 0)
        current = 0;
    else if (current >= (int) ui->rows->len)
        current = (int) ui->rows->len - 1;
    table_set_current (ui->tree, current);
    if (ui->rows->len == 0)
    {
        /* table_set_current does nothing on an empty table */
        ui->tree->current = 0;
        ui->tree->top = 0;
    }
    widget_draw (WIDGET (ui->tree));
    ui_sync_from_tree (ui);
}

/* --------------------------------------------------------------------------------------------- */

static void
ui_clear_jumps (ui_t *ui)
{
    guint i;

    for (i = 0; i < ui->jumps->len; i++)
    {
        jump_t *j = g_ptr_array_index (ui->jumps, i);

        slv_node_free (j->root);
        g_free (j);
    }
    g_ptr_array_set_size (ui->jumps, 0);
}

/* --------------------------------------------------------------------------------------------- */

static void
ui_build (ui_t *ui, const slv_def_t *def, off_t offset)
{
    slv_node_t *root;

    ui_close_grid (ui);
    ui_clear_jumps (ui);
    if (ui->root != NULL)
        slv_node_free (ui->root);
    ui->root = NULL;
    if (def == NULL)
    {
        ui_set_root (ui, NULL, 0);
        return;
    }
    if (def->kind == SLV_DEF_TABLE)
        root = slv_eval_table (&ui->ev, def, offset, 1);
    else
        root = slv_eval_struct (&ui->ev, def, offset);
    expand_default (root, 0);
    ui_set_root (ui, root, 0);
}

/* --------------------------------------------------------------------------------------------- */

static char *
node_path (const slv_node_t *node)
{
    GString *p = g_string_new ("");

    while (node->parent != NULL)
    {
        guint i;
        char *s;

        for (i = 0; i < node->parent->children->len; i++)
            if (g_ptr_array_index (node->parent->children, i) == node)
                break;
        s = g_strdup_printf ("%u/%s", i, p->str);
        g_string_assign (p, s);
        g_free (s);
        node = node->parent;
    }
    return g_string_free (p, FALSE);
}

/* --------------------------------------------------------------------------------------------- */

static slv_node_t *
find_by_path (slv_node_t *root, const char *path)
{
    slv_node_t *n = root;
    const char *p = path;

    while (n != NULL && *p != '\0')
    {
        guint i = (guint) strtoul (p, (char **) &p, 10);

        if (*p == '/')
            p++;
        if (n->children == NULL || i >= n->children->len)
            return NULL;
        n = g_ptr_array_index (n->children, i);
    }
    return n;
}

/* --------------------------------------------------------------------------------------------- */

static void
collect_expanded (const slv_node_t *node, GString *path, GHashTable *set)
{
    guint i;
    gsize len = path->len;

    if (node->expanded && !node->lazy)
        g_hash_table_add (set, g_strdup (path->str));
    if (node->children == NULL)
        return;
    for (i = 0; i < node->children->len; i++)
    {
        g_string_append_printf (path, "%u/", i);
        collect_expanded (g_ptr_array_index (node->children, i), path, set);
        g_string_truncate (path, len);
    }
}

/* --------------------------------------------------------------------------------------------- */

static void
apply_expanded (const slv_eval_t *ev, slv_node_t *node, GString *path, GHashTable *set)
{
    guint i;
    gsize len = path->len;

    node->expanded = g_hash_table_contains (set, path->str);
    if (node->expanded && node->lazy)
        slv_node_expand (ev, node);
    if (node->children == NULL)
        return;
    for (i = 0; i < node->children->len; i++)
    {
        g_string_append_printf (path, "%u/", i);
        apply_expanded (ev, g_ptr_array_index (node->children, i), path, set);
        g_string_truncate (path, len);
    }
}

/* --------------------------------------------------------------------------------------------- */

/* re-evaluate the current root in place, keep the cursor row and the expansion by index path */
static void
ui_refresh (ui_t *ui)
{
    slv_node_t *root;
    int current = table_get_current (ui->tree);
    const slv_def_t *def;
    off_t offset;
    GHashTable *expanded;
    GString *path;

    if (ui->root == NULL || ui->root->def == NULL)
        return;
    def = ui->root->def;
    offset = ui->root->offset;
    if (ui->root->kind == SLV_NODE_TABLE)
        root = slv_eval_table (&ui->ev, def, offset, ui->root->rows);
    else
        root = slv_eval_struct (&ui->ev, def, offset);

    expanded = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    path = g_string_new ("/");
    collect_expanded (ui->root, path, expanded);
    g_string_assign (path, "/");
    apply_expanded (&ui->ev, root, path, expanded);
    g_string_free (path, TRUE);
    g_hash_table_destroy (expanded);

    if (ui->grid != NULL)
    {
        /* the grid points into the old tree: close it, reopen on the new one */
        char *saved_path = g_strdup (ui->grid_path);
        int grid_row = table_get_current (ui->grid);
        int grid_col = ui->grid->current_col;
        int first = ui->grid_first_col;
        slv_node_t *again;

        ui_close_grid (ui);
        slv_node_free (ui->root);
        ui_set_root (ui, root, current);
        again = find_by_path (root, saved_path);
        g_free (saved_path);
        if (node_is_gridable (again))
        {
            ui_open_grid (ui, again);
            if (ui->grid != NULL)
            {
                ui->grid_first_col = first;
                ui_grid_build (ui);
                table_set_current (ui->grid, grid_row);
                ui->grid->current_col = grid_col;
                ui_update_heads (ui);
                widget_draw (WIDGET (ui->dlg));
            }
        }
        return;
    }

    slv_node_free (ui->root);
    ui_set_root (ui, root, current);
}

/* --------------------------------------------------------------------------------------------- */

/* before the first edit: take the file lock as mcedit does */
static void
ui_note_change (ui_t *ui)
{
    if (!ui->locked && slv_file_reader_change_count (ui->fr) == 0)
        ui->locked = lock_file (ui->vpath) != 0;
}

/* --------------------------------------------------------------------------------------------- */

static void
ui_cmd_edit_field (ui_t *ui)
{
    if (ui->grid != NULL)
        ui_edit_node (ui,
                      ui_grid_cell_node (ui, table_get_current (ui->grid), ui->grid->current_col));
    else
    {
        row_t *r = ui_current_row (ui);

        ui_edit_node (ui, r != NULL ? r->node : NULL);
    }
}

/* --------------------------------------------------------------------------------------------- */

static void
ui_edit_node (ui_t *ui, slv_node_t *n)
{
    char *text, *title, *err = NULL;
    unsigned char *out, *orig;
    off_t i;

    if (n == NULL || !slv_node_editable (n))
    {
        message (D_NORMAL, _ ("Edit"), _ ("This field cannot be edited"));
        return;
    }
    orig = g_malloc (n->size);
    if (ui->fr->reader.read (ui->fr->reader.ctx, n->offset, orig, n->size) != (gssize) n->size)
    {
        g_free (orig);
        return;
    }
    text = slv_node_edit_text (n, orig);
    g_free (orig);
    if (text == NULL)
    {
        message (D_NORMAL, _ ("Edit"),
                 _ ("The bytes of this field are not text, edit them in the hex zone"));
        return;
    }
    title = g_strdup_printf ("%s  (%s, %lld bytes at %08llX)", n->key != NULL ? n->key : "",
                             n->hint != NULL ? n->hint : "", (long long) n->size,
                             (unsigned long long) n->offset);
    for (;;)
    {
        char *edited;

        edited = input_dialog (title, _ ("New value:"), "mcstruct-edit", text, INPUT_COMPLETE_NONE);
        g_free (text);
        text = edited;
        if (text == NULL)
            break;
        out = g_malloc (n->size);
        if (slv_node_encode (n, text, out, &err))
        {
            orig = g_malloc (n->size);
            if (ui->fr->reader.read (ui->fr->reader.ctx, n->offset, orig, n->size)
                == (gssize) n->size)
            {
                ui_note_change (ui);
                for (i = 0; i < n->size; i++)
                    if (out[i] != orig[i])
                        slv_file_reader_set_byte (ui->fr, n->offset + i, out[i]);
            }
            g_free (orig);
            g_free (out);
            ui_refresh (ui);
            widget_draw (WIDGET (ui->hex));
            break;
        }
        g_free (out);
        message (D_ERROR, _ ("Edit"), "%s", err);
        g_free (err);
        err = NULL;
    }
    g_free (text);
    g_free (title);
}

/* --------------------------------------------------------------------------------------------- */

static gboolean
ui_load_def (ui_t *ui, const char *path)
{
    GError *error = NULL;
    slv_file_t *file;

    file = slv_file_load (path, &error);
    if (file == NULL)
    {
        message (D_ERROR, _ ("Struct look"), "%s", error->message);
        g_error_free (error);
        return FALSE;
    }
    if (ui->file != NULL)
        slv_file_free (ui->file);
    ui->file = file;
    ui->ev.file = file;
    g_free (ui->def_path);
    ui->def_path = g_strdup (path);
    widget_draw (WIDGET (ui->def));
    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */

/* --- table screen -------------------------------------------------------------------------- */

static int
grid_col_width (const slv_node_t *leaf)
{
    const slv_item_t *it = leaf->item;
    int w;

    if (it->view_width > 0)
        return it->view_width + 1;
    switch (it->type)
    {
    case SLV_TYPE_CHAR:
    case SLV_TYPE_STR8:
    case SLV_TYPE_STR16:
    case SLV_TYPE_PSTRING:
    case SLV_TYPE_CSTRING:
        /* the width the field has in this file */
        w = leaf->size > 0 ? CLAMP ((int) leaf->size, 4, 40) : 16;
        break;
    case SLV_TYPE_HEX:
    case SLV_TYPE_BE_HEX:
    case SLV_TYPE_JUMP:
        w = it->size * 2;
        break;
    case SLV_TYPE_INT:
    case SLV_TYPE_UINT:
        w = it->size >= 8 ? 20 : it->size == 4 ? 11 : it->size == 2 ? 6 : 4;
        break;
    case SLV_TYPE_BITS:
        w = it->size * 8 + 2;
        break;
    case SLV_TYPE_FLOAT:
    case SLV_TYPE_MSBIN:
    case SLV_TYPE_PTR:
        w = 14;
        break;
    case SLV_TYPE_TIME_DOS:
    case SLV_TYPE_TIME_UNIX:
        w = 19;
        break;
    default:
        w = 10;
        break;
    }
    if (it->legend != NULL)
        w += 14;
    if (it->name != NULL && (int) str_term_width1 (it->name) > w)
        w = MIN ((int) str_term_width1 (it->name), 30);
    return w + 1; /* the column cursor marker */
}

/* --------------------------------------------------------------------------------------------- */

/* the fields of a row in order, through nested structures and #repeat passes */
static void
collect_leaves (slv_node_t *node, GPtrArray *out)
{
    guint i;

    if (node->children == NULL)
        return;
    for (i = 0; i < node->children->len; i++)
    {
        slv_node_t *c = g_ptr_array_index (node->children, i);

        switch (c->kind)
        {
        case SLV_NODE_FIELD:
            if (c->item != NULL && c->item->hidden)
                break;
            g_ptr_array_add (out, c);
            break;
        case SLV_NODE_JUMP:
        case SLV_NODE_ERROR:
            g_ptr_array_add (out, c);
            break;
        case SLV_NODE_REMARK:
            break;
        default:
            collect_leaves (c, out);
            break;
        }
    }
}

/* --------------------------------------------------------------------------------------------- */

/* the field node of row @row in column @col, or NULL; rows share the shape of the first */
static slv_node_t *
ui_grid_cell_node (ui_t *ui, int row, int col)
{
    slv_node_t *rec;

    if (ui->grid_node == NULL || ui->grid_node->children == NULL || row < 0
        || (guint) row >= ui->grid_node->children->len)
        return NULL;
    col += ui->grid_first_col;
    if (col < 0 || (guint) col >= ui->grid_cols->len)
        return NULL;
    rec = g_ptr_array_index (ui->grid_node->children, row);
    if (ui->grid_cache == NULL)
        ui->grid_cache = g_ptr_array_new ();
    if (ui->grid_cache_row != row)
    {
        g_ptr_array_set_size (ui->grid_cache, 0);
        collect_leaves (rec, ui->grid_cache);
        ui->grid_cache_row = row;
    }
    if ((guint) col >= ui->grid_cache->len)
        return NULL;
    return g_ptr_array_index (ui->grid_cache, col);
}

/* --------------------------------------------------------------------------------------------- */

static int
grid_get_nrows (const void *data)
{
    const ui_t *ui = data;

    return ui->grid_node != NULL && ui->grid_node->children != NULL
        ? (int) ui->grid_node->children->len
        : 0;
}

/* --------------------------------------------------------------------------------------------- */

static const char *
grid_get_text (const void *data, int row, int col)
{
    ui_t *ui = (ui_t *) data;
    const slv_node_t *n = ui_grid_cell_node (ui, row, col);

    if (n == NULL)
        return "";
    if (n->kind == SLV_NODE_ERROR)
        return n->text != NULL ? n->text : "ERROR";
    g_string_truncate (ui->cell, 0);
    if (n->text != NULL)
        g_string_append (ui->cell, n->text);
    if (n->legend != NULL)
        g_string_append_printf (ui->cell, " (%s)", n->legend);
    return ui->cell->str;
}

/* --------------------------------------------------------------------------------------------- */

static void
ui_grid_heads (ui_t *ui)
{
    GString *h = g_string_new (" ");
    int c;
    char *padded;

    for (c = 0; c < ui->grid_ncols; c++)
    {
        const slv_node_t *leaf = g_ptr_array_index (ui->grid_cols, ui->grid_first_col + c);
        int w = ui->grid->col_defs[c].width;
        const char *name = leaf->key != NULL ? leaf->key : "";

        /* the table widget draws a marker column and a separator */
        g_string_append (h, str_fit_to_term (name, w - 1, J_LEFT));
        g_string_append (h, "  ");
    }
    padded = g_strdup_printf ("%-*s", WIDGET (ui->tree)->rect.cols, h->str);
    status_set_text (ui->tree_head, padded);
    g_free (padded);
    g_string_free (h, TRUE);
}

/* --------------------------------------------------------------------------------------------- */

/* (re)create the grid widget for the columns from grid_first_col that fit the zone */
static void
ui_grid_build (ui_t *ui)
{
    const WRect *tr = &WIDGET (ui->tree)->rect;
    table_column_def_t *defs;
    table_datasource_t ds = { grid_get_nrows, grid_get_text, NULL, NULL, ui, NULL, NULL };
    int n = 0, used = 0, c;
    int current = ui->grid != NULL ? table_get_current (ui->grid) : 0;
    /* a rebuild (resize, refresh) must not take the focus away from the hex or def-file zone */
    gboolean take_focus = !widget_get_state (WIDGET (ui->hex), WST_FOCUSED)
        && !widget_get_state (WIDGET (ui->def), WST_FOCUSED);
    int avail = tr->cols - 2; /* the margin and the scrollbar */

    if (ui->grid != NULL)
    {
        group_remove_widget (ui->grid);
        widget_destroy (WIDGET (ui->grid));
        ui->grid = NULL;
    }

    defs = g_new0 (table_column_def_t, ui->grid_cols->len + 1);
    for (c = ui->grid_first_col; c < (int) ui->grid_cols->len; c++)
    {
        const slv_node_t *leaf = g_ptr_array_index (ui->grid_cols, c);
        int w = leaf->item != NULL ? grid_col_width (leaf) : 12;
        int sep = n > 0 ? 1 : 0; /* separator before this column */

        if (n > 0 && used + sep + w > avail)
            break;
        if (used + sep + w > avail)
            w = MAX (avail - used - sep, 4);
        defs[n].width = w;
        defs[n].align = J_LEFT_FIT;
        defs[n].type = TABLE_COL_CHOICE;
        used += sep + w;
        n++;
    }
    ui->grid_ncols = n;

    ui->grid = table_new (tr->y, tr->x, tr->lines, tr->cols, n, defs);
    g_free (defs);
    table_set_datasource (ui->grid, ds);
    ui->grid->scrollbar = TRUE;
    ui->grid->scrollbar_on_frame = TRUE;
    ui->grid->normal_color = ui->colors.tree_normal;
    ui->grid->selected_color = ui->colors.tree_selected;
    ui->grid->scrollbar_color = ui->colors.tree_frame;
    group_add_widget (GROUP (ui->dlg), ui->grid);
    table_set_current (ui->grid, current);
    if (take_focus)
        widget_select (WIDGET (ui->grid));
    ui_grid_heads (ui);
}

/* --------------------------------------------------------------------------------------------- */

/* tables, arrays of structures and #repeat blocks open as a grid */
static gboolean
node_is_gridable (const slv_node_t *n)
{
    return n != NULL
        && (n->kind == SLV_NODE_TABLE || n->kind == SLV_NODE_NESTED || n->kind == SLV_NODE_REPEAT);
}

/* --------------------------------------------------------------------------------------------- */

static void
ui_open_grid (ui_t *ui, slv_node_t *node)
{
    if (!node_is_gridable (node))
        return;
    if (node->lazy)
        slv_node_expand (&ui->ev, node);
    if (node->children == NULL || node->children->len == 0)
    {
        message (D_NORMAL, _ ("Table"), _ ("The table has no rows"));
        return;
    }

    if (ui->grid == NULL)
        ui->tree_current = table_get_current (ui->tree);
    g_free (ui->grid_path);
    ui->grid_path = node_path (node);
    ui->grid_node = node;
    ui->grid_first_col = 0;

    if (ui->grid_cols == NULL)
        ui->grid_cols = g_ptr_array_new ();
    g_ptr_array_set_size (ui->grid_cols, 0);
    ui->grid_cache_row = -1;
    collect_leaves (g_ptr_array_index (node->children, 0), ui->grid_cols);
    if (ui->grid_cols->len == 0)
    {
        message (D_NORMAL, _ ("Table"), _ ("The table has no columns"));
        ui->grid_node = NULL;
        return;
    }

    widget_set_visibility (WIDGET (ui->tree), FALSE);
    ui_grid_build (ui);
    ui_update_heads (ui);
    widget_draw (WIDGET (ui->dlg));
}

/* --------------------------------------------------------------------------------------------- */

static void
ui_close_grid (ui_t *ui)
{
    if (ui->grid == NULL)
        return;
    group_remove_widget (ui->grid);
    widget_destroy (WIDGET (ui->grid));
    ui->grid = NULL;
    ui->grid_node = NULL;
    ui->grid_cache_row = -1;
    widget_set_visibility (WIDGET (ui->tree), TRUE);
    widget_select (WIDGET (ui->tree));
    ui_update_heads (ui);
    widget_draw (WIDGET (ui->dlg));
}

/* --------------------------------------------------------------------------------------------- */

static void
ui_grid_scroll (ui_t *ui, int dir)
{
    int first = ui->grid_first_col + dir;

    if (ui->grid == NULL || first < 0 || first >= (int) ui->grid_cols->len)
        return;
    ui->grid_first_col = first;
    ui_grid_build (ui);
    widget_draw (WIDGET (ui->dlg));
}

/* --------------------------------------------------------------------------------------------- */

static void
ui_sync_from_grid (ui_t *ui)
{
    slv_node_t *n;
    int row;

    if (ui->grid == NULL || ui->syncing)
        return;
    row = table_get_current (ui->grid);
    n = ui_grid_cell_node (ui, row, ui->grid->current_col);
    if (n == NULL && ui->grid_node->children != NULL && row >= 0
        && (guint) row < ui->grid_node->children->len)
        n = g_ptr_array_index (ui->grid_node->children, row);
    if (n == NULL)
        return;
    ui->syncing = TRUE;
    hexstrip_set_mark (ui->hex, n->offset, n->size);
    if (n->line > 0 && ui->file != NULL && (guint) n->line <= ui->file->lines->len)
    {
        table_set_current (ui->def, n->line - 1);
        widget_draw (WIDGET (ui->def));
    }
    ui_update_heads (ui);
    ui->syncing = FALSE;
}

/* --------------------------------------------------------------------------------------------- */

static void
ui_layout (ui_t *ui)
{
    const WRect *d = &WIDGET (ui->dlg)->rect;
    int cols = d->cols;
    int y = d->y;
    int hex_lines = ui->hex_hidden || ui->settings.hex_lines == 0 ? 0 : ui->settings.hex_lines + 1;
    int def_lines = ui->settings.def_lines > 0 ? ui->settings.def_lines + 1 : 0;
    int tree_lines = ui->settings.tree_lines > 0 ? ui->settings.tree_lines + 1 : 0;
    int avail = d->lines - 1 - 1 - 2; /* title, buttonbar, tree frame; zones count their head */
    gboolean side = ui->settings.def_layout == 1 || (ui->settings.def_layout == 0 && cols >= 120);
    int tree_cols = cols, def_cols = 0, def_x = d->x;

    if (ui->zoom != 0)
    {
        /* one zone takes everything between the title and the button bar */
        int rows = d->lines - 2;
        Widget *head = ui->zoom == 1 ? WIDGET (ui->tree_head)
            : ui->zoom == 2          ? WIDGET (ui->hex_head)
                                     : WIDGET (ui->def_head);
        Widget *body = ui->zoom == 1 ? WIDGET (ui->tree)
            : ui->zoom == 2          ? WIDGET (ui->hex)
                                     : WIDGET (ui->def);

        memset (ui->box, 0, sizeof (ui->box));
        widget_set_size (WIDGET (ui->title), y, d->x, 1, cols);
        widget_set_visibility (WIDGET (ui->tree_head), ui->zoom == 1);
        widget_set_visibility (WIDGET (ui->tree), ui->zoom == 1 && ui->grid == NULL);
        if (ui->grid != NULL)
            widget_set_visibility (WIDGET (ui->grid), ui->zoom == 1);
        widget_set_visibility (WIDGET (ui->hex_head), ui->zoom == 2);
        widget_set_visibility (WIDGET (ui->hex), ui->zoom == 2);
        widget_set_visibility (WIDGET (ui->def_head), ui->zoom == 3);
        widget_set_visibility (WIDGET (ui->def), ui->zoom == 3);
        if (ui->zoom == 2)
        {
            widget_set_size (head, y + 1, d->x, 1, cols);
            widget_set_size (body, y + 2, d->x, MAX (rows - 2, 1), cols);
        }
        else
        {
            ui->box[ui->zoom - 1] = (WRect) { y + 1, d->x, rows, cols };
            widget_set_size (head, y + 2, d->x + 1, 1, cols - 2);
            widget_set_size (body, y + 3, d->x + 1, MAX (rows - 3, 1), cols - 1);
        }
        if (ui->zoom == 1)
        {
            widget_set_size (WIDGET (ui->tree), y + 3, d->x + 1, MAX (rows - 3, 1), cols - 1);
            if (ui->grid != NULL)
                ui_grid_build (ui);
        }
        goto columns;
    }

    if (hex_lines > 0)
        avail -= hex_lines;
    if (avail < 3)
    {
        hex_lines = 0;
        avail = d->lines - 4;
    }

    if (side)
    {
        /* tree and def-file share the rows above the hex zone */
        def_cols = cols * ui->settings.def_width / 100;
        if (def_cols < 20)
            def_cols = 0;
        tree_cols = cols - def_cols;
        def_x = d->x + tree_cols;
        tree_lines = MAX (avail, 1);
        def_lines = def_cols > 0 ? tree_lines : 0;
    }
    else if (tree_lines > 0 && def_lines == 0)
    {
        /* fixed tree, the def-file zone takes the rest */
        if (tree_lines > avail - 4)
            tree_lines = MAX (avail - 4, 2);
        def_lines = avail - tree_lines - 2;
        if (def_lines < 2)
            def_lines = 0;
    }
    else
    {
        if (def_lines > 0)
            avail -= def_lines + 2;
        if (avail < 3)
        {
            def_lines = 0;
            avail = d->lines - 4 - (hex_lines > 0 ? hex_lines : 0);
        }
        tree_lines = tree_lines > 0 ? MIN (tree_lines, avail) : avail;
        if (tree_lines < avail)
            def_lines = avail - tree_lines - 2 > 1 ? avail - tree_lines - 2 : 0;
        tree_lines = MAX (tree_lines, 1);
    }

    /* every zone sits inside a frame, its head line is the first row inside */
    memset (ui->box, 0, sizeof (ui->box));
    widget_set_visibility (WIDGET (ui->tree_head), TRUE);
    widget_set_visibility (WIDGET (ui->tree), ui->grid == NULL);
    widget_set_size (WIDGET (ui->title), y, d->x, 1, cols);
    y++;
    ui->box[0] = (WRect) { y, d->x, tree_lines + 2, tree_cols };
    widget_set_size (WIDGET (ui->tree_head), y + 1, d->x + 1, 1, tree_cols - 2);
    /* the table is one column wider than the frame's inside: its scrollbar sits on the frame */
    widget_set_size (WIDGET (ui->tree), y + 2, d->x + 1, MAX (tree_lines - 1, 1), tree_cols - 1);
    if (side && def_lines > 0)
    {
        ui->box[1] = (WRect) { y, def_x, def_lines + 2, def_cols };
        widget_set_size (WIDGET (ui->def_head), y + 1, def_x + 1, 1, def_cols - 2);
        widget_set_size (WIDGET (ui->def), y + 2, def_x + 1, MAX (def_lines - 1, 1), def_cols - 1);
    }
    y += tree_lines + 2;
    if (ui->grid != NULL)
        ui_grid_build (ui);

    widget_set_visibility (WIDGET (ui->hex_head), hex_lines > 0);
    widget_set_visibility (WIDGET (ui->hex), hex_lines > 0);
    if (hex_lines > 0)
    {
        /* no frame: the status line, then the dump rows */
        widget_set_size (WIDGET (ui->hex_head), y, d->x, 1, cols);
        widget_set_size (WIDGET (ui->hex), y + 1, d->x, MAX (hex_lines - 1, 1), cols);
        y += hex_lines;
    }

    widget_set_visibility (WIDGET (ui->def_head), def_lines > 0);
    widget_set_visibility (WIDGET (ui->def), def_lines > 0);
    if (!side && def_lines > 0)
    {
        ui->box[1] = (WRect) { y, d->x, def_lines + 2, cols };
        widget_set_size (WIDGET (ui->def_head), y + 1, d->x + 1, 1, cols - 2);
        widget_set_size (WIDGET (ui->def), y + 2, d->x + 1, MAX (def_lines - 1, 1), cols - 1);
        y += def_lines + 2;
    }

columns:
    /* tree columns */
    {
        table_column_def_t *c = ui->tree->col_defs;
        int tw = WIDGET (ui->tree)->rect.cols;
        int dw = WIDGET (ui->def)->rect.cols;
        int off_w = ui->settings.offset_column != 0 ? 9 : 0;
        int key_w = ui->settings.name_width + 4;
        int hint_w = 8;
        int comment_w = tw > 110 ? 24 : 0;
        /* WTable: one column of margin, a separator after every column but the last,
           the scrollbar in the last column: TREE_COLS + 1 cells are not for text */
        int value_w = tw - (TREE_COLS + 1) - off_w - key_w - hint_w - comment_w;

        if (value_w < 10)
        {
            comment_w = 0;
            value_w = tw - (TREE_COLS + 1) - off_w - key_w - hint_w;
        }
        c[COL_OFFSET].width = off_w;
        c[COL_KEY].width = key_w;
        c[COL_HINT].width = hint_w;
        c[COL_VALUE].width = MAX (value_w, 1);
        c[COL_COMMENT].width = comment_w;

        ui->def->col_defs[0].width = 6;
        ui->def->col_defs[1].width = MAX (dw - 6 - (DEF_COLS + 1), 1);
    }
}

/* --------------------------------------------------------------------------------------------- */

static void
ui_toggle_node (ui_t *ui, gboolean expand)
{
    row_t *r = ui_current_row (ui);
    slv_node_t *n;
    int current = table_get_current (ui->tree);

    if (r == NULL)
        return;
    n = r->node;
    if (!node_has_children (n))
        return;
    if (expand && n->lazy)
        slv_node_expand (&ui->ev, n);
    n->expanded = expand;
    ui_rebuild_rows (ui);
    table_set_current (ui->tree, current);
    widget_draw (WIDGET (ui->tree));
    ui_update_heads (ui);
}

/* --------------------------------------------------------------------------------------------- */

static void
ui_go_parent (ui_t *ui)
{
    row_t *r = ui_current_row (ui);
    int i;

    if (r == NULL || r->node->parent == NULL)
        return;
    for (i = table_get_current (ui->tree) - 1; i >= 0; i--)
        if (g_array_index (ui->rows, row_t, i).node == r->node->parent)
        {
            table_set_current (ui->tree, i);
            ui_sync_from_tree (ui);
            return;
        }
}

/* --------------------------------------------------------------------------------------------- */

static void
ui_expand_to_depth (ui_t *ui, int depth)
{
    guint i;
    int current = table_get_current (ui->tree);

    if (ui->root == NULL)
        return;
    /* only over the rows already built; lazy nodes stay collapsed */
    for (i = 0; i < ui->rows->len; i++)
    {
        row_t *r = &g_array_index (ui->rows, row_t, i);

        if (node_has_children (r->node) && !r->node->lazy)
            r->node->expanded = r->depth < depth;
    }
    ui_rebuild_rows (ui);
    table_set_current (ui->tree, current);
    widget_draw (WIDGET (ui->tree));
    ui_update_heads (ui);
}

/* --------------------------------------------------------------------------------------------- */

static void
ui_follow_jump (ui_t *ui)
{
    row_t *r = ui_current_row (ui);

    if (r != NULL)
        ui_follow_jump_node (ui, r->node);
}

/* --------------------------------------------------------------------------------------------- */

static void
ui_follow_jump_node (ui_t *ui, slv_node_t *n)
{
    slv_node_t *root;
    jump_t *j;

    if (n == NULL || n->kind != SLV_NODE_JUMP || n->def == NULL)
        return;
    if (n->jump_target < 0 || n->jump_target >= ui->fr->size)
    {
        message (D_ERROR, _ ("Struct look"), _ ("Jump target 0x%llX is outside the file"),
                 (unsigned long long) n->jump_target);
        return;
    }
    if (ui->jumps->len >= 256)
        return;

    if (n->def->kind == SLV_DEF_TABLE)
        root = slv_eval_table (&ui->ev, n->def, n->jump_target, n->rows > 0 ? n->rows : 1);
    else
        root = slv_eval_struct (&ui->ev, n->def, n->jump_target);
    expand_default (root, 0);

    j = g_new0 (jump_t, 1);
    j->root = ui->root;
    j->current = table_get_current (ui->tree);
    g_ptr_array_add (ui->jumps, j);
    ui_set_root (ui, root, 0);
}

/* --------------------------------------------------------------------------------------------- */

static void
ui_jump_back (ui_t *ui)
{
    jump_t *j;

    if (ui->jumps->len == 0)
        return;
    j = g_ptr_array_index (ui->jumps, ui->jumps->len - 1);
    g_ptr_array_remove_index (ui->jumps, ui->jumps->len - 1);
    slv_node_free (ui->root);
    ui_set_root (ui, j->root, j->current);
    g_free (j);
}

/* --------------------------------------------------------------------------------------------- */

static gboolean
node_is_text (const slv_node_t *n)
{
    if (n->kind != SLV_NODE_FIELD || n->item == NULL)
        return FALSE;
    switch (n->item->type)
    {
    case SLV_TYPE_CHAR:
    case SLV_TYPE_CSTRING:
    case SLV_TYPE_PSTRING:
    case SLV_TYPE_STR8:
        return TRUE;
    default:
        return FALSE;
    }
}

/* --------------------------------------------------------------------------------------------- */

/* the bytes of a string field in a window, one row per line of the data */
static void
ui_view_text (ui_t *ui, const slv_node_t *n)
{
    Listbox *lb;
    unsigned char *buf;
    gsize len = (gsize) n->size, i, start;
    int lines = LINES - 8, cols = COLS - 8, width;
    GString *row;
    GPtrArray *rows;
    char *title;

    if (len == 0)
        return;
    buf = g_malloc (len);
    if (ui->fr->reader.read (ui->fr->reader.ctx, n->offset, buf, len) != (gssize) len)
    {
        g_free (buf);
        return;
    }
    if (n->item->type == SLV_TYPE_PSTRING)
        start = 1;
    else
        start = 0;
    if (n->item->type == SLV_TYPE_CSTRING || n->item->type == SLV_TYPE_STR8)
        while (len > start && buf[len - 1] == '\0')
            len--;

    width = cols - 4;
    rows = g_ptr_array_new_with_free_func (g_free);
    row = g_string_new ("");
    for (i = start; i <= len; i++)
    {
        unsigned char c = i < len ? buf[i] : '\n';

        if (c == '\n' || (int) row->len >= width)
        {
            g_ptr_array_add (rows, g_strdup (row->str));
            g_string_truncate (row, 0);
            if (c == '\n')
                continue;
        }
        if (c == '\r')
            continue;
        if (c == '\t')
            g_string_append_c (row, ' ');
        else if (c >= 0x20 && c != 0x7F)
            g_string_append_c (row, (char) c);
        else
            g_string_append_c (row, '.');
    }
    g_string_free (row, TRUE);
    g_free (buf);

    if ((int) rows->len < lines)
        lines = MAX ((int) rows->len, 1);
    title = g_strdup_printf ("%s: %d bytes at %08llX", n->item->name != NULL ? n->item->name : "",
                             (int) n->size, (unsigned long long) n->offset);
    lb = listbox_window_new (lines, cols, title, NULL);
    g_free (title);
    for (i = 0; i < rows->len; i++)
        listbox_add_item (lb->list, LISTBOX_APPEND_AT_END, 0, g_ptr_array_index (rows, i), NULL,
                          FALSE);
    g_ptr_array_free (rows, TRUE);
    listbox_run (lb);
}

/* --------------------------------------------------------------------------------------------- */

static void
ui_activate_row (ui_t *ui)
{
    row_t *r = ui_current_row (ui);

    if (r == NULL)
        return;
    if (r->node->kind == SLV_NODE_JUMP)
        ui_follow_jump (ui);
    else if (node_is_text (r->node))
        ui_view_text (ui, r->node);
    else if (r->node->kind == SLV_NODE_TABLE || (node_is_gridable (r->node) && r->node->rows > 1))
        ui_open_grid (ui, r->node);
    else if (node_has_children (r->node))
        ui_toggle_node (ui, !r->node->expanded);
}

/* --------------------------------------------------------------------------------------------- */

static gboolean
ui_input_offset (ui_t *ui, const char *title, off_t *offset)
{
    char *def, *text, *err = NULL;
    gint64 v = 0;
    const row_t *r = ui_current_row (ui);

    def = g_strdup_printf ("0x%llX", (unsigned long long) hexstrip_get_cursor (ui->hex));
    text = input_dialog (title, _ ("Offset (expression, labels of the current structure):"),
                         "mcstruct-offset", def, INPUT_COMPLETE_NONE);
    g_free (def);
    if (text == NULL)
        return FALSE;
    if (!slv_eval_calc (&ui->ev, r != NULL ? r->node : NULL, text, &v, &err))
    {
        message (D_ERROR, title, "%s", err);
        g_free (err);
        g_free (text);
        return FALSE;
    }
    g_free (text);
    if (v < 0 || v >= ui->fr->size)
    {
        message (D_ERROR, title, _ ("Offset 0x%llX is outside the file"), (unsigned long long) v);
        return FALSE;
    }
    *offset = (off_t) v;
    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */

static void
ui_cmd_select_struct (ui_t *ui)
{
    Listbox *lb;
    guint i;
    const slv_def_t *chosen;
    off_t offset;

    if (ui->file == NULL)
        return;
    lb = listbox_window_new (16, 56, _ ("Select structure"), NULL);
    for (i = 0; i < ui->file->defs->len; i++)
    {
        const slv_def_t *def = g_ptr_array_index (ui->file->defs, i);

        if (def->kind != SLV_DEF_STRUCT && def->kind != SLV_DEF_TABLE)
            continue;
        if (def->hidden && !ui->settings.show_hidden)
            continue;
        {
            gssize size = slv_def_fixed_size (def);
            char *text;

            if (size >= 0)
                text = g_strdup_printf ("%s%-28s %6ld bytes", def->kind == SLV_DEF_TABLE ? ":" : "",
                                        def->name, (long) size);
            else
                text = g_strdup_printf ("%s%-28s variable", def->kind == SLV_DEF_TABLE ? ":" : "",
                                        def->name);
            listbox_add_item_take (lb->list, LISTBOX_APPEND_AT_END, 0, text, (void *) def, FALSE);
        }
    }
    chosen = listbox_run_with_data (lb, ui->root != NULL ? ui->root->def : NULL);
    if (chosen == NULL)
        return;
    {
        gssize size = slv_def_fixed_size (chosen);
        char *title;

        if (size >= 0)
            title = g_strdup_printf (_ ("%s: %ld bytes, file %lld bytes"), chosen->name,
                                     (long) size, (long long) ui->fr->size);
        else
            title = g_strdup_printf (_ ("%s: variable size, file %lld bytes"), chosen->name,
                                     (long long) ui->fr->size);
        if (!ui_input_offset (ui, title, &offset))
        {
            g_free (title);
            return;
        }
        g_free (title);
        if (size >= 0 && offset + size > ui->fr->size)
            message (D_NORMAL, _ ("Select structure"),
                     _ ("%s needs %ld bytes, only %lld left after 0x%llX"), chosen->name,
                     (long) size, (long long) (ui->fr->size - offset), (unsigned long long) offset);
    }
    ui_build (ui, chosen, offset);
}

/* --------------------------------------------------------------------------------------------- */

static void
ui_cmd_select_def (ui_t *ui)
{
    GPtrArray *defs = slv_load_list_defs ();
    Listbox *lb;
    guint i;
    const char *chosen;
    const slv_def_t *def;

    if (defs->len == 0)
    {
        message (D_ERROR, _ ("Struct look"), _ ("No def-files found on the search path"));
        g_ptr_array_free (defs, TRUE);
        return;
    }
    lb = listbox_window_new (16, 50, _ ("Select def-file"), NULL);
    for (i = 0; i < defs->len; i++)
    {
        const char *path = g_ptr_array_index (defs, i);

        listbox_add_item (lb->list, LISTBOX_APPEND_AT_END, 0, x_basename (path), (void *) path,
                          FALSE);
    }
    chosen = listbox_run_with_data (lb, NULL);
    if (chosen != NULL && ui_load_def (ui, chosen))
    {
        def = slv_file_first_struct (ui->file);
        ui_build (ui, def, 0);
    }
    g_ptr_array_free (defs, TRUE);
}

/* --------------------------------------------------------------------------------------------- */

static void
ui_cmd_edit_def (ui_t *ui, int line)
{
    edit_arg_t *arg;
    const slv_def_t *def = ui->root != NULL ? ui->root->def : NULL;
    char *def_name = def != NULL ? g_strdup (def->name) : NULL;
    off_t offset = ui->root != NULL ? ui->root->offset : 0;

    if (ui->def_path == NULL)
        return;
    arg = edit_arg_new (ui->def_path, line > 0 ? line : 1);
    edit_file (arg);
    edit_arg_free (arg);

    if (ui_load_def (ui, ui->def_path))
    {
        def = def_name != NULL ? slv_file_lookup (ui->file, def_name) : NULL;
        if (def == NULL)
            def = slv_file_first_struct (ui->file);
        ui_build (ui, def, offset);
    }
    g_free (def_name);
    widget_draw (WIDGET (ui->dlg));
}

/* --------------------------------------------------------------------------------------------- */

static void
ui_cmd_errors (ui_t *ui)
{
    Listbox *lb;
    guint i;
    const slv_error_t *chosen;

    if (ui->file == NULL || ui->file->errors->len == 0)
    {
        message (D_NORMAL, _ ("Struct look"), _ ("No errors in %s"),
                 ui->def_path != NULL ? x_basename (ui->def_path) : "-");
        return;
    }
    lb = listbox_window_new (16, 70, _ ("Def-file errors"), NULL);
    for (i = 0; i < ui->file->errors->len; i++)
    {
        const slv_error_t *err = g_ptr_array_index (ui->file->errors, i);

        listbox_add_item_take (lb->list, LISTBOX_APPEND_AT_END, 0,
                               g_strdup_printf ("%5d: %s", err->line, err->message), (void *) err,
                               FALSE);
    }
    chosen = listbox_run_with_data (lb, NULL);
    if (chosen != NULL)
        ui_cmd_edit_def (ui, chosen->line);
}

/* --------------------------------------------------------------------------------------------- */

static gboolean
row_matches (ui_t *ui, const row_t *r, const char *needle)
{
    const slv_node_t *n = r->node;
    const char *fields[4];
    int i;

    fields[0] = n->key;
    fields[1] = n->text;
    fields[2] = n->legend;
    fields[3] = n->comment;
    for (i = 0; i < 4; i++)
    {
        char *lower;
        gboolean hit;

        if (fields[i] == NULL)
            continue;
        lower = g_utf8_strdown (fields[i], -1);
        hit = strstr (lower, needle) != NULL;
        g_free (lower);
        if (hit)
            return TRUE;
    }
    (void) ui;
    return FALSE;
}

/* --------------------------------------------------------------------------------------------- */

static void
ui_cmd_search (ui_t *ui, gboolean next)
{
    guint start, i, n = ui->rows->len;

    if (!next || ui->search == NULL)
    {
        char *text;

        text = input_dialog (_ ("Search"), _ ("Field name or value:"), "mcstruct-search",
                             ui->search != NULL ? ui->search : "", INPUT_COMPLETE_NONE);
        if (text == NULL || text[0] == '\0')
        {
            g_free (text);
            return;
        }
        g_free (ui->search);
        ui->search = g_utf8_strdown (text, -1);
        g_free (text);
    }
    if (n == 0)
        return;
    start = (guint) table_get_current (ui->tree);
    for (i = 1; i <= n; i++)
    {
        guint idx = (start + i) % n;

        if (row_matches (ui, &g_array_index (ui->rows, row_t, idx), ui->search))
        {
            table_set_current (ui->tree, (int) idx);
            ui_sync_from_tree (ui);
            widget_draw (WIDGET (ui->tree));
            return;
        }
    }
    message (D_NORMAL, _ ("Search"), _ ("Not found"));
}

/* --------------------------------------------------------------------------------------------- */

static void
ui_cmd_calc (ui_t *ui)
{
    char *text, *err = NULL;
    gint64 v = 0;
    const row_t *r = ui_current_row (ui);

    text = input_dialog (_ ("Calculator"), _ ("Expression:"), "mcstruct-calc", "",
                         INPUT_COMPLETE_NONE);
    if (text == NULL || text[0] == '\0')
    {
        g_free (text);
        return;
    }
    if (!slv_eval_calc (&ui->ev, r != NULL ? r->node : NULL, text, &v, &err))
    {
        message (D_ERROR, _ ("Calculator"), "%s", err);
        g_free (err);
    }
    else
        message (D_NORMAL, _ ("Calculator"), "%s = %lld (0x%llX)", text, (long long) v,
                 (unsigned long long) v);
    g_free (text);
}

/* --------------------------------------------------------------------------------------------- */

static gboolean
ui_cmd_save (ui_t *ui)
{
    GError *error = NULL;

    if (slv_file_reader_change_count (ui->fr) == 0)
        return TRUE;
    if (!slv_file_reader_save (ui->fr, &error))
    {
        message (D_ERROR, _ ("Save"), "%s", error->message);
        g_error_free (error);
        return FALSE;
    }
    ui_update_heads (ui);
    widget_draw (WIDGET (ui->hex));
    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */

static gboolean
hex_on_edit (WHexStrip *h, off_t offset, unsigned char value, void *data)
{
    ui_t *ui = data;
    off_t cursor = h->cursor, top = h->top;
    gboolean in_text = h->in_text;

    ui_note_change (ui);
    slv_file_reader_set_byte (ui->fr, offset, value);
    /* the refresh re-syncs the mark from the tree, which moves the cursor to the
       start of the field; the user is typing here, so put it back */
    ui_refresh (ui);
    h->cursor = cursor;
    h->top = top;
    h->in_text = in_text;
    ui_update_heads (ui);
    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */

static gboolean
hex_is_changed (void *ctx, off_t offset)
{
    return slv_file_reader_is_changed (ctx, offset);
}

/* --------------------------------------------------------------------------------------------- */

static void
ui_cmd_help (void)
{
    ev_help_t event_data = { MC_PLUGIN_DIR "/mcstruct_panel.hlp", "[mcstruct]", "[main]" };

    mc_event_raise (MCEVENT_GROUP_CORE, "help", &event_data);
}

/* --------------------------------------------------------------------------------------------- */

static void
ui_cmd_quit (ui_t *ui)
{
    if (slv_file_reader_change_count (ui->fr) > 0)
    {
        int rc;

        rc = query_dialog (_ ("Quit"), _ ("File was modified. Save changes?"), D_NORMAL, 3,
                           _ ("&Yes"), _ ("&No"), _ ("&Cancel"));
        if (rc == 0 && !ui_cmd_save (ui))
            return;
        if (rc == 2 || rc == -1)
            return;
        if (rc == 1)
            slv_file_reader_discard (ui->fr);
    }
    ui->quit = TRUE;
    dlg_close (ui->dlg);
}

/* --------------------------------------------------------------------------------------------- */

static cb_ret_t
ui_execute (ui_t *ui, long command)
{
    switch (command)
    {
    case CMD_HELP:
        ui_cmd_help ();
        return MSG_HANDLED;
    case CMD_SAVE:
        ui_cmd_save (ui);
        return MSG_HANDLED;
    case CMD_STRUCT:
        ui_cmd_select_struct (ui);
        return MSG_HANDLED;
    case CMD_EDIT:
        ui_cmd_edit_field (ui);
        return MSG_HANDLED;
    case CMD_GOTO:
    {
        off_t offset;

        if (ui_input_offset (ui, _ ("Goto"), &offset))
            hexstrip_set_cursor (ui->hex, offset);
        return MSG_HANDLED;
    }
    case CMD_DEF:
        ui_cmd_select_def (ui);
        return MSG_HANDLED;
    case CMD_EDIT_DEF:
    {
        const row_t *r = ui_current_row (ui);

        ui_cmd_edit_def (ui, r != NULL ? r->node->line : 1);
        return MSG_HANDLED;
    }
    case CMD_SEARCH:
        ui_cmd_search (ui, FALSE);
        return MSG_HANDLED;
    case CMD_SEARCH_NEXT:
        ui_cmd_search (ui, TRUE);
        return MSG_HANDLED;
    case CMD_HEX:
        ui->hex_hidden = !ui->hex_hidden;
        ui_layout (ui);
        widget_draw (WIDGET (ui->dlg));
        return MSG_HANDLED;
    case CMD_ERRORS:
        ui_cmd_errors (ui);
        return MSG_HANDLED;
    case CMD_CALC:
        ui_cmd_calc (ui);
        return MSG_HANDLED;
    case CMD_QUIT:
        ui_cmd_quit (ui);
        return MSG_HANDLED;
    case CMD_BACK:
        ui_cmd_quit (ui);
        ui->back = ui->quit;
        return MSG_HANDLED;
    default:
        return MSG_NOT_HANDLED;
    }
}

/* --------------------------------------------------------------------------------------------- */

static cb_ret_t
ui_key (ui_t *ui, int key)
{
    long command;
    gboolean tree_focused = GROUP (ui->dlg)->current != NULL
        && WIDGET (GROUP (ui->dlg)->current->data) == WIDGET (ui->tree);

    command = keybind_lookup_keymap_command (bb_keymap, key);
    if (command != CK_IgnoreKey)
        return ui_execute (ui, command);

    if (key == ALT ('='))
        return ui_execute (ui, CMD_CALC);
    if (key == ui->zoom_key || key == KEY_F (19))
    {
        Widget *cur =
            GROUP (ui->dlg)->current != NULL ? WIDGET (GROUP (ui->dlg)->current->data) : NULL;
        int zone = cur == WIDGET (ui->hex) ? 2 : cur == WIDGET (ui->def) ? 3 : 1;

        ui->zoom = ui->zoom == zone ? 0 : zone;
        ui_layout (ui);
        ui_update_heads (ui);
        widget_draw (WIDGET (ui->dlg));
        return MSG_HANDLED;
    }
    if (key == '\t')
    {
        group_select_next_widget (GROUP (ui->dlg));
        return MSG_HANDLED;
    }
    if (ui->grid != NULL)
    {
        switch (key)
        {
        case ESC_CHAR:
        case KEY_BACKSPACE:
        case 127:
        case 8:
            ui_close_grid (ui);
            table_set_current (ui->tree, ui->tree_current);
            ui_sync_from_tree (ui);
            return MSG_HANDLED;
        case '<':
            ui_grid_scroll (ui, -1);
            return MSG_HANDLED;
        case '>':
            ui_grid_scroll (ui, 1);
            return MSG_HANDLED;
        case KEY_ENTER:
        case '\n':
        {
            slv_node_t *n =
                ui_grid_cell_node (ui, table_get_current (ui->grid), ui->grid->current_col);

            if (n != NULL && n->kind == SLV_NODE_JUMP)
            {
                ui_close_grid (ui);
                ui_follow_jump_node (ui, n);
            }
            return MSG_HANDLED;
        }
        default:
            return MSG_NOT_HANDLED;
        }
    }

    if (key == ESC_CHAR)
        return MSG_NOT_HANDLED;

    if (!tree_focused)
        return MSG_NOT_HANDLED;

    switch (key)
    {
    case KEY_RIGHT:
    case '+':
    {
        row_t *r = ui_current_row (ui);

        if (r != NULL && r->node->kind == SLV_NODE_JUMP)
            ui_follow_jump (ui);
        else
            ui_toggle_node (ui, TRUE);
    }
        return MSG_HANDLED;
    case KEY_LEFT:
    case '-':
    {
        row_t *r = ui_current_row (ui);

        if (r != NULL && node_has_children (r->node) && r->node->expanded)
            ui_toggle_node (ui, FALSE);
        else
            ui_go_parent (ui);
    }
        return MSG_HANDLED;
    case KEY_ENTER:
    case '\n':
        ui_activate_row (ui);
        return MSG_HANDLED;
    case KEY_BACKSPACE:
    case 127:
    case 8:
        ui_jump_back (ui);
        return MSG_HANDLED;
    case '*':
        ui_expand_to_depth (ui, 99);
        return MSG_HANDLED;
    default:
        if (key >= '1' && key <= '9')
        {
            ui_expand_to_depth (ui, key - '0');
            return MSG_HANDLED;
        }
        return MSG_NOT_HANDLED;
    }
}

/* --------------------------------------------------------------------------------------------- */

static cb_ret_t
ui_dialog_callback (Widget *w, Widget *sender, widget_msg_t msg, int parm, void *data)
{
    WDialog *dlg = DIALOG (w);
    ui_t *ui = dlg->data.p;

    switch (msg)
    {
    case MSG_RESIZE:
        dlg_default_callback (w, sender, msg, parm, data);
        ui_layout (ui);
        ui_update_heads (ui);
        return MSG_HANDLED;

    case MSG_DRAW:
    {
        const int *colors = widget_get_colors (w);
        int i;

        dlg_default_callback (w, sender, msg, parm, data);
        for (i = 0; i < 3; i++)
            if (ui->box[i].lines > 0)
            {
                /* by widget state: MSG_CHANGED_FOCUS arrives before the group's current moves */
                gboolean tree_active = widget_get_state (WIDGET (ui->tree), WST_FOCUSED)
                    || (ui->grid != NULL && widget_get_state (WIDGET (ui->grid), WST_FOCUSED));
                gboolean def_active = widget_get_state (WIDGET (ui->def), WST_FOCUSED);
                int fc = i == 0
                    ? (tree_active ? ui->colors.tree_frame_active : ui->colors.tree_frame)
                    : i == 1 ? (def_active ? ui->colors.def_frame_active : ui->colors.def_frame)
                             : -1;

                tty_setcolor (fc >= 0 ? fc : colors[DLG_COLOR_FRAME]);
                tty_draw_box (ui->box[i].y, ui->box[i].x, ui->box[i].lines, ui->box[i].cols, FALSE);
            }
        /* the tables paint their scrollbar column on the frame line, in its color */
        {
            gboolean tree_active = widget_get_state (WIDGET (ui->tree), WST_FOCUSED)
                || (ui->grid != NULL && widget_get_state (WIDGET (ui->grid), WST_FOCUSED));
            int tree_fc = tree_active ? ui->colors.tree_frame_active : ui->colors.tree_frame;

            ui->tree->scrollbar_color = tree_fc;
            if (ui->grid != NULL)
                ui->grid->scrollbar_color = tree_fc;
            ui->def->scrollbar_color = widget_get_state (WIDGET (ui->def), WST_FOCUSED)
                ? ui->colors.def_frame_active
                : ui->colors.def_frame;
        }
        if (ui->grid != NULL)
            widget_draw (WIDGET (ui->grid));
        else
            widget_draw (WIDGET (ui->tree));
        widget_draw (WIDGET (ui->def));
        return MSG_HANDLED;
    }

    case MSG_KEY:
        return ui_key (ui, parm);

    case MSG_ACTION:
        return ui_execute (ui, parm);

    case MSG_NOTIFY:
        if (sender == WIDGET (ui->tree))
        {
            if (parm == CK_Enter)
                ui_activate_row (ui);
            else
                ui_sync_from_tree (ui);
            return MSG_HANDLED;
        }
        if (ui->grid != NULL && sender == WIDGET (ui->grid))
        {
            ui_sync_from_grid (ui);
            return MSG_HANDLED;
        }
        if (sender == WIDGET (ui->def))
        {
            ui_update_heads (ui);
            return MSG_HANDLED;
        }
        return MSG_NOT_HANDLED;

    case MSG_CHANGED_FOCUS:
        /* the frame of the focused zone changes color */
        widget_draw (w);
        return MSG_HANDLED;

    case MSG_VALIDATE:
        /* Esc: same question as F10 */
        if (!ui->quit)
        {
            ui_cmd_quit (ui);
            if (!ui->quit)
                widget_set_state (w, WST_ACTIVE, TRUE);
        }
        return MSG_HANDLED;

    default:
        return dlg_default_callback (w, sender, msg, parm, data);
    }
}

/* --------------------------------------------------------------------------------------------- */

/* colors from the skin; a missing section falls back to the zone default, then core */
static void
ui_load_colors (ui_colors_t *c)
{
    c->tree_normal = mc_skin_color_get ("mcstruct-tree", "_default_");
    c->tree_frame = mc_skin_color_get ("mcstruct-tree", "frame");
    c->tree_frame_active = mc_skin_color_get ("mcstruct-tree", "frame-active");
    c->tree_head = mc_skin_color_get ("mcstruct-tree", "head");
    c->tree_selected = mc_skin_color_get ("mcstruct-tree", "selected");
    c->tree_offset = mc_skin_color_get ("mcstruct-tree", "offset");
    c->tree_name = mc_skin_color_get ("mcstruct-tree", "name");
    c->tree_type = mc_skin_color_get ("mcstruct-tree", "type");
    c->tree_value = mc_skin_color_get ("mcstruct-tree", "value");
    c->tree_struct = mc_skin_color_get ("mcstruct-tree", "struct");
    c->tree_jump = mc_skin_color_get ("mcstruct-tree", "jump");
    c->tree_remark = mc_skin_color_get ("mcstruct-tree", "remark");
    c->tree_error = mc_skin_color_get ("mcstruct-tree", "error");

    c->hex_head = mc_skin_color_get ("mcstruct-hex", "head");
    c->hex.normal = mc_skin_color_get ("mcstruct-hex", "_default_");
    c->hex.offset = mc_skin_color_get ("mcstruct-hex", "offset");
    c->hex.mark = mc_skin_color_get ("mcstruct-hex", "mark");
    c->hex.changed = mc_skin_color_get ("mcstruct-hex", "changed");
    c->hex.cursor = mc_skin_color_get ("mcstruct-hex", "cursor");
    c->hex.frame = mc_skin_color_get ("mcstruct-hex", "frame");

    c->def_normal = mc_skin_color_get ("mcstruct-def", "_default_");
    c->def_frame = mc_skin_color_get ("mcstruct-def", "frame");
    c->def_frame_active = mc_skin_color_get ("mcstruct-def", "frame-active");
    c->def_head = mc_skin_color_get ("mcstruct-def", "head");
    c->def_selected = mc_skin_color_get ("mcstruct-def", "selected");
    c->def_lineno = mc_skin_color_get ("mcstruct-def", "lineno");
    c->def_directive = mc_skin_color_get ("mcstruct-def", "directive");
    c->def_comment = mc_skin_color_get ("mcstruct-def", "comment");
    c->def_label = mc_skin_color_get ("mcstruct-def", "label");
}

/* --------------------------------------------------------------------------------------------- */

static void
ui_apply_colors (ui_t *ui)
{
    ui_load_colors (&ui->colors);
    ui->tree->normal_color = ui->colors.tree_normal;
    ui->tree->selected_color = ui->colors.tree_selected;
    ui->tree->scrollbar_color = ui->colors.tree_frame;
    ui->tree_head->color = ui->colors.tree_head;
    ui->def->normal_color = ui->colors.def_normal;
    ui->def->selected_color = ui->colors.def_selected;
    ui->def->scrollbar_color = ui->colors.def_frame;
    ui->def_head->color = ui->colors.def_head;
    ui->hex_head->color = ui->colors.hex_head;
    hexstrip_set_colors (ui->hex, &ui->colors.hex);
}

/* --------------------------------------------------------------------------------------------- */

static void
ui_create_widgets (ui_t *ui)
{
    WDialog *dlg;
    WGroup *g;
    Widget *wd;
    table_column_def_t tree_cols[TREE_COLS] = { { 9, J_LEFT, TABLE_COL_TEXT },
                                                { 20, J_LEFT_FIT, TABLE_COL_TEXT },
                                                { 8, J_LEFT, TABLE_COL_TEXT },
                                                { 30, J_LEFT_FIT, TABLE_COL_TEXT },
                                                { 0, J_LEFT_FIT, TABLE_COL_TEXT } };
    table_column_def_t def_cols[DEF_COLS] = { { 6, J_RIGHT, TABLE_COL_TEXT },
                                              { 70, J_LEFT_FIT, TABLE_COL_TEXT } };
    table_datasource_t tree_ds = { tree_get_nrows, tree_get_text, NULL, NULL, ui,
                                   NULL,           tree_get_color };
    table_datasource_t def_ds = {
        def_get_nrows, def_get_text, NULL, NULL, ui, NULL, def_get_color
    };
    hexstrip_source_t hex_src;

    dlg = dlg_create (FALSE, 0, 0, 1, 1, WPOS_FULLSCREEN, FALSE, listbox_colors, ui_dialog_callback,
                      NULL, NULL, NULL);
    dlg->data.p = ui;
    ui->dlg = dlg;
    wd = WIDGET (dlg);
    widget_want_tab (wd, TRUE);
    g = GROUP (dlg);

    ui->title = status_new (0, 0, 80);
    group_add_widget (g, ui->title);
    ui->tree_head = status_new (1, 0, 80);
    group_add_widget (g, ui->tree_head);

    ui->tree = table_new (2, 0, 5, 80, TREE_COLS, tree_cols);
    table_set_datasource (ui->tree, tree_ds);
    ui->tree->scrollbar = TRUE;
    ui->tree->scrollbar_on_frame = TRUE;
    group_add_widget (g, ui->tree);

    ui->hex_head = status_new (8, 0, 80);
    group_add_widget (g, ui->hex_head);
    ui->hex = hexstrip_new (9, 0, 4, 80);
    hex_src.get_size = ui->fr->reader.size;
    hex_src.read = ui->fr->reader.read;
    hex_src.is_changed = hex_is_changed;
    hex_src.ctx = ui->fr;
    hexstrip_set_source (ui->hex, &hex_src);
    hexstrip_set_handlers (ui->hex, hex_on_cursor, hex_on_edit, ui);
    group_add_widget (g, ui->hex);

    ui->def_head = status_new (14, 0, 80);
    group_add_widget (g, ui->def_head);
    ui->def = table_new (15, 0, 5, 80, DEF_COLS, def_cols);
    table_set_datasource (ui->def, def_ds);
    ui->def->scrollbar = TRUE;
    ui->def->scrollbar_on_frame = TRUE;
    group_add_widget (g, ui->def);

    ui->bb = buttonbar_new ();
    buttonbar_set_label (ui->bb, 1, Q_ ("ButtonBar|Help"), bb_keymap, wd);
    buttonbar_set_label (ui->bb, 2, Q_ ("ButtonBar|Save"), bb_keymap, wd);
    buttonbar_set_label (ui->bb, 3, Q_ ("ButtonBar|Struct"), bb_keymap, wd);
    buttonbar_set_label (ui->bb, 4, Q_ ("ButtonBar|Edit"), bb_keymap, wd);
    buttonbar_set_label (ui->bb, 5, Q_ ("ButtonBar|Goto"), bb_keymap, wd);
    buttonbar_set_label (ui->bb, 6, Q_ ("ButtonBar|DefFil"), bb_keymap, wd);
    buttonbar_set_label (ui->bb, 7, Q_ ("ButtonBar|Search"), bb_keymap, wd);
    buttonbar_set_label (ui->bb, 8, Q_ ("ButtonBar|Hex"), bb_keymap, wd);
    buttonbar_set_label (ui->bb, 9, Q_ ("ButtonBar|Errors"), bb_keymap, wd);
    buttonbar_set_label (ui->bb, 10, Q_ ("ButtonBar|Quit"), bb_keymap, wd);
    group_add_widget_autopos (g, ui->bb, WIDGET (ui->bb)->pos_flags, NULL);

    widget_select (WIDGET (ui->tree));
    ui->zoom_key = tty_keyname_to_keycode ("alt-f9", NULL);
    ui_apply_colors (ui);
}

/* --------------------------------------------------------------------------------------------- */
/*** public functions ****************************************************************************/
/* --------------------------------------------------------------------------------------------- */

gboolean
mcstruct_run (const char *path, const char *display_name, const char *hint, off_t *last_offset)
{
    ui_t *ui;
    GError *error = NULL;
    char *def_path;
    gboolean ok = TRUE;
    char *def_hint = NULL;
    off_t start_at = -1;

    ui = g_new0 (ui_t, 1);
    ui->display_name = g_strdup (display_name != NULL ? display_name : x_basename (path));
    ui->rows = g_array_new (FALSE, FALSE, sizeof (row_t));
    ui->jumps = g_ptr_array_new ();
    ui->cell = g_string_sized_new (128);
    slv_settings_load (&ui->settings);

    ui->fr = slv_file_reader_open (path, &error);
    if (ui->fr == NULL)
    {
        message (D_ERROR, _ ("Struct look"), "%s", error->message);
        g_error_free (error);
        ok = FALSE;
        goto out;
    }

    ui->vpath = vfs_path_from_str (path);
    ui->ev.reader = &ui->fr->reader;
    ui->ev.lazy_rows = ui->settings.lazy_rows;
    ui->ev.float_format = ui->settings.float_format;

    ui_create_widgets (ui);
    ui_layout (ui);

    /* hint: "name", "name@0x1c" or "@0x1c" (the offset to open on) */
    {
        const char *at = hint != NULL ? strchr (hint, '@') : NULL;

        if (at != NULL)
        {
            def_hint = g_strndup (hint, at - hint);
            start_at = (off_t) g_ascii_strtoll (at + 1, NULL, 0);
            hint = def_hint[0] != '\0' ? def_hint : NULL;
        }
    }
    def_path = slv_load_find_def (path, hint);
    if (def_path != NULL)
    {
        if (ui_load_def (ui, def_path))
            ui_build (ui, slv_file_first_struct (ui->file), 0);
        g_free (def_path);
    }
    ui_update_heads (ui);

    if (ui->file == NULL)
    {
        /* nothing matched: ask */
        ui_cmd_select_def (ui);
    }
    if (start_at >= 0 && start_at < ui->fr->size)
        hexstrip_set_cursor (ui->hex, start_at);

    dlg_run (ui->dlg);
    if (last_offset != NULL)
        *last_offset = ui->back ? hexstrip_get_cursor (ui->hex) : -1;
    if (widget_get_state (WIDGET (ui->dlg), WST_CLOSED))
        widget_destroy (WIDGET (ui->dlg));

out:
    if (ui->grid_cols != NULL)
        g_ptr_array_free (ui->grid_cols, TRUE);
    if (ui->grid_cache != NULL)
        g_ptr_array_free (ui->grid_cache, TRUE);
    g_free (ui->grid_path);
    if (ui->locked)
        unlock_file (ui->vpath);
    vfs_path_free (ui->vpath, TRUE);
    ui_clear_jumps (ui);
    g_ptr_array_free (ui->jumps, TRUE);
    if (ui->root != NULL)
        slv_node_free (ui->root);
    if (ui->file != NULL)
        slv_file_free (ui->file);
    slv_file_reader_free (ui->fr);
    g_array_free (ui->rows, TRUE);
    g_string_free (ui->cell, TRUE);
    slv_settings_free (&ui->settings);
    g_free (ui->def_path);
    g_free (ui->search);
    g_free (ui->display_name);
    g_free (ui);
    g_free (def_hint);
    return ok;
}

/* --------------------------------------------------------------------------------------------- */
