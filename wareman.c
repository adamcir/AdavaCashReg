#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

enum {
    COL_NAME,
    COL_PRICE,
    COL_STOCK,
    COL_CATEGORY,
    COL_UNIT,
    NUM_COLS
};

typedef struct {
    GtkWidget *window;
    GtkWidget *treeview;
    GtkListStore *store;
    GtkWidget *status_label;
    char *items_path;
    gboolean dirty;
} WareManApp;

static const char *UNITS[] = {"ks", "kg", "g", "l", "ml", "m", NULL};

static char *find_items_file(void)
{
    if (g_file_test("items.acri", G_FILE_TEST_IS_REGULAR))
        return g_strdup("items.acri");

    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *dir = g_path_get_dirname(exe);
        char *path = g_build_filename(dir, "items.acri", NULL);
        g_free(dir);
        if (g_file_test(path, G_FILE_TEST_IS_REGULAR))
            return path;
        g_free(path);
    }
    return g_strdup("items.acri");
}

static gboolean parse_cz_number(const char *text, double *value)
{
    if (!text || !value) return FALSE;

    gchar *copy = g_strdup(text);
    gchar *s = g_strstrip(copy);
    if (!*s || strchr(s, '.') != NULL) {
        g_free(copy);
        return FALSE;
    }

    int commas = 0;
    for (char *p = s; *p; p++) {
        if (*p == ',') {
            commas++;
            *p = '.';
        }
    }
    if (commas > 1) {
        g_free(copy);
        return FALSE;
    }

    gchar *end = NULL;
    double v = g_ascii_strtod(s, &end);
    gboolean ok = end != s && *end == '\0' && isfinite(v) && v >= 0.0;
    if (ok) *value = v;
    g_free(copy);
    return ok;
}

static void format_cz(double value, char *buf, gsize size)
{
    g_ascii_formatd(buf, size, "%.2f", value);
    for (char *p = buf; *p; p++)
        if (*p == '.') *p = ',';
}

static void format_json_price(double value, char *buf, gsize size)
{
    g_ascii_formatd(buf, size, "%.2f", value);
}

static void format_json_quantity(double value, char *buf, gsize size)
{
    g_ascii_formatd(buf, size, "%.6f", value);

    char *end = buf + strlen(buf) - 1;
    while (end > buf && *end == '0') {
        *end = '\0';
        end--;
    }

    if (end > buf && *end == '.')
        g_strlcat(buf, "0", size);
}

static void append_json_string(GString *out, const char *text)
{
    const unsigned char *p = (const unsigned char *)(text ? text : "");

    g_string_append_c(out, '"');

    while (*p) {
        switch (*p) {
        case '"':
            g_string_append(out, "\\\"");
            break;
        case '\\':
            g_string_append(out, "\\\\");
            break;
        case '\b':
            g_string_append(out, "\\b");
            break;
        case '\f':
            g_string_append(out, "\\f");
            break;
        case '\n':
            g_string_append(out, "\\n");
            break;
        case '\r':
            g_string_append(out, "\\r");
            break;
        case '\t':
            g_string_append(out, "\\t");
            break;
        default:
            if (*p < 0x20)
                g_string_append_printf(out, "\\u%04x", (unsigned int)*p);
            else
                g_string_append_c(out, (char)*p);
            break;
        }
        p++;
    }

    g_string_append_c(out, '"');
}

static gboolean names_equal(const char *a, const char *b)
{
    gchar *aa = g_utf8_casefold(a ? a : "", -1);
    gchar *bb = g_utf8_casefold(b ? b : "", -1);
    gboolean same = g_strcmp0(aa, bb) == 0;
    g_free(aa);
    g_free(bb);
    return same;
}

static gboolean name_exists(WareManApp *app, const char *name)
{
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->store), &iter);
    while (valid) {
        gchar *existing = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(app->store), &iter, COL_NAME, &existing, -1);
        gboolean same = names_equal(existing, name);
        g_free(existing);
        if (same) return TRUE;
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(app->store), &iter);
    }
    return FALSE;
}

static gboolean category_exists_in_combo(GtkComboBoxText *combo,
                                         const char *category)
{
    GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(combo));
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(model, &iter);

    while (valid) {
        gchar *existing = NULL;
        gtk_tree_model_get(model, &iter, 0, &existing, -1);
        gboolean same = g_strcmp0(existing, category) == 0;
        g_free(existing);
        if (same)
            return TRUE;
        valid = gtk_tree_model_iter_next(model, &iter);
    }
    return FALSE;
}

static void fill_category_combo(WareManApp *app, GtkComboBoxText *combo)
{
    GtkTreeIter iter;
    gboolean valid =
        gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->store), &iter);

    while (valid) {
        gchar *category = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(app->store), &iter,
                           COL_CATEGORY, &category, -1);
        if (category && *category &&
            !category_exists_in_combo(combo, category))
            gtk_combo_box_text_append_text(combo, category);
        g_free(category);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(app->store), &iter);
    }
}

static void price_cell(GtkTreeViewColumn *column, GtkCellRenderer *renderer,
                       GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    (void)column; (void)data;
    double price = 0.0;
    gtk_tree_model_get(model, iter, COL_PRICE, &price, -1);
    char n[64], text[80];
    format_cz(price, n, sizeof(n));
    g_snprintf(text, sizeof(text), "%s Kč", n);
    g_object_set(renderer, "text", text, NULL);
}

static void stock_cell(GtkTreeViewColumn *column, GtkCellRenderer *renderer,
                       GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    (void)column; (void)data;
    double stock = 0.0;
    gchar *unit = NULL;
    gtk_tree_model_get(model, iter, COL_STOCK, &stock, COL_UNIT, &unit, -1);
    char n[64], text[96];
    format_cz(stock, n, sizeof(n));
    g_snprintf(text, sizeof(text), "%s %s", n, unit ? unit : "");
    g_object_set(renderer, "text", text, NULL);
    g_free(unit);
}

static void configure_dynamic_column(GtkTreeViewColumn *column)
{
    gtk_tree_view_column_set_expand(column, TRUE);
    gtk_tree_view_column_set_resizable(column, TRUE);
    gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_GROW_ONLY);
}

static void add_columns(GtkTreeView *view)
{
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *name =
        gtk_tree_view_column_new_with_attributes("Položka", r, "text", COL_NAME, NULL);
    configure_dynamic_column(name);
    gtk_tree_view_append_column(view, name);

    r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *price = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(price, "Cena");
    gtk_tree_view_column_pack_start(price, r, TRUE);
    gtk_tree_view_column_set_cell_data_func(price, r, price_cell, NULL, NULL);
    configure_dynamic_column(price);
    gtk_tree_view_append_column(view, price);

    r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *stock = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(stock, "Skladem");
    gtk_tree_view_column_pack_start(stock, r, TRUE);
    gtk_tree_view_column_set_cell_data_func(stock, r, stock_cell, NULL, NULL);
    configure_dynamic_column(stock);
    gtk_tree_view_append_column(view, stock);

    r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *category =
        gtk_tree_view_column_new_with_attributes("Kategorie", r, "text", COL_CATEGORY, NULL);
    configure_dynamic_column(category);
    gtk_tree_view_append_column(view, category);

    r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *unit =
        gtk_tree_view_column_new_with_attributes("Jednotka", r, "text", COL_UNIT, NULL);
    configure_dynamic_column(unit);
    gtk_tree_view_append_column(view, unit);
}

static gboolean load_items(WareManApp *app)
{
    gtk_list_store_clear(app->store);

    if (!g_file_test(app->items_path, G_FILE_TEST_IS_REGULAR)) {
        gtk_label_set_text(GTK_LABEL(app->status_label), "items.acri zatím neexistuje.");
        app->dirty = FALSE;
        return TRUE;
    }

    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    if (!json_parser_load_from_file(parser, app->items_path, &error)) {
        char msg[512];
        g_snprintf(msg, sizeof(msg), "Chyba ACRI JSON: %s", error->message);
        gtk_label_set_text(GTK_LABEL(app->status_label), msg);
        g_clear_error(&error);
        g_object_unref(parser);
        return FALSE;
    }

    JsonNode *root_node = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root_node)) {
        gtk_label_set_text(GTK_LABEL(app->status_label), "ACRI: kořen musí být JSON objekt.");
        g_object_unref(parser);
        return FALSE;
    }

    JsonObject *root = json_node_get_object(root_node);
    const char *format = json_object_get_string_member_with_default(root, "format", "");
    gint64 version = json_object_get_int_member_with_default(root, "version", 0);
    if (g_strcmp0(format, "ACRI") != 0 || version != 1) {
        gtk_label_set_text(GTK_LABEL(app->status_label), "Nepodporovaný ACRI formát/verze.");
        g_object_unref(parser);
        return FALSE;
    }

    JsonArray *items = json_object_get_array_member(root, "items");
    guint count = items ? json_array_get_length(items) : 0;
    for (guint i = 0; i < count; i++) {
        JsonObject *o = json_array_get_object_element(items, i);
        if (!o) continue;

        const char *name = json_object_get_string_member_with_default(o, "name", "");
        const char *category = json_object_get_string_member_with_default(o, "category", "Ostatní");
        const char *unit = json_object_get_string_member_with_default(o, "unit", "ks");
        double price = json_object_get_double_member_with_default(o, "price", 0.0);
        double stock = json_object_get_double_member_with_default(o, "stock", 0.0);

        if (!*name || name_exists(app, name)) continue;

        GtkTreeIter iter;
        gtk_list_store_append(app->store, &iter);
        gtk_list_store_set(app->store, &iter,
                           COL_NAME, name,
                           COL_PRICE, price,
                           COL_STOCK, stock,
                           COL_CATEGORY, category,
                           COL_UNIT, unit,
                           -1);
    }

    char msg[128];
    g_snprintf(msg, sizeof(msg), "Načteno %u položek.", count);
    gtk_label_set_text(GTK_LABEL(app->status_label), msg);
    app->dirty = FALSE;
    g_object_unref(parser);
    return TRUE;
}

static gboolean save_items(WareManApp *app)
{
    GString *out = g_string_new(
        "{\n"
        "  \"format\" : \"ACRI\",\n"
        "  \"version\" : 1,\n"
        "  \"items\" : [\n"
    );

    GtkTreeIter iter;
    gboolean valid =
        gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->store), &iter);
    gboolean first = TRUE;

    while (valid) {
        gchar *name = NULL, *category = NULL, *unit = NULL;
        double price = 0.0, stock = 0.0;
        char price_text[64];
        char stock_text[64];

        gtk_tree_model_get(GTK_TREE_MODEL(app->store), &iter,
                           COL_NAME, &name,
                           COL_PRICE, &price,
                           COL_STOCK, &stock,
                           COL_CATEGORY, &category,
                           COL_UNIT, &unit,
                           -1);

        format_json_price(price, price_text, sizeof(price_text));
        format_json_quantity(stock, stock_text, sizeof(stock_text));

        if (!first)
            g_string_append(out, ",\n");
        first = FALSE;

        g_string_append(out, "    {\n      \"name\" : ");
        append_json_string(out, name);
        g_string_append_printf(out,
            ",\n"
            "      \"price\" : %s,\n"
            "      \"stock\" : %s,\n"
            "      \"category\" : ",
            price_text, stock_text);
        append_json_string(out, category);
        g_string_append(out, ",\n      \"unit\" : ");
        append_json_string(out, unit);
        g_string_append(out, "\n    }");

        g_free(name);
        g_free(category);
        g_free(unit);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(app->store), &iter);
    }

    g_string_append(out, "\n  ]\n}\n");

    GError *error = NULL;
    gboolean ok = g_file_set_contents(
        app->items_path,
        out->str,
        (gssize)out->len,
        &error
    );

    g_string_free(out, TRUE);

    if (!ok) {
        char msg[512];
        g_snprintf(msg, sizeof(msg), "Nelze uložit: %s",
                   error ? error->message : "neznámá chyba");
        gtk_label_set_text(GTK_LABEL(app->status_label), msg);
        g_clear_error(&error);
    } else {
        gtk_label_set_text(GTK_LABEL(app->status_label), "items.acri uložen.");
        app->dirty = FALSE;
    }

    return ok;
}

static void scroll_to_last(WareManApp *app, GtkTreeIter *iter)
{
    GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(app->store), iter);
    gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(app->treeview), path, NULL, TRUE, 1.0, 0.0);
    gtk_tree_path_free(path);
}

static void show_error(GtkWindow *parent, const char *text)
{
    GtkWidget *d = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL,
                                          GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", text);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

static void item_dialog(WareManApp *app, GtkTreeIter *edit_iter)
{
    gboolean editing = edit_iter != NULL;
    gchar *old_name = NULL, *old_category = NULL, *old_unit = NULL;
    double old_price = 0.0, old_stock = 0.0;

    if (editing) {
        gtk_tree_model_get(GTK_TREE_MODEL(app->store), edit_iter,
                           COL_NAME, &old_name,
                           COL_PRICE, &old_price,
                           COL_STOCK, &old_stock,
                           COL_CATEGORY, &old_category,
                           COL_UNIT, &old_unit,
                           -1);
    }

    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        editing ? "Upravit položku" : "Nová položka",
        GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Zrušit", GTK_RESPONSE_CANCEL,
        editing ? "_Uložit změny" : "_Přidat", GTK_RESPONSE_OK,
        NULL);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 16);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),
                       grid, TRUE, TRUE, 0);

    GtkWidget *name = gtk_entry_new();
    GtkWidget *price = gtk_entry_new();
    GtkWidget *stock = gtk_entry_new();
    GtkWidget *category = gtk_combo_box_text_new_with_entry();
    GtkWidget *category_entry = gtk_bin_get_child(GTK_BIN(category));
    fill_category_combo(app, GTK_COMBO_BOX_TEXT(category));
    GtkWidget *unit = gtk_combo_box_text_new();

    gtk_entry_set_placeholder_text(GTK_ENTRY(name), "např. Rohlík");
    gtk_entry_set_placeholder_text(GTK_ENTRY(price), "např. 3,90");
    gtk_entry_set_placeholder_text(GTK_ENTRY(stock), "např. 120 nebo 12,5");
    gtk_entry_set_placeholder_text(GTK_ENTRY(category_entry), "např. Pečivo");

    for (int i = 0; UNITS[i]; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(unit), UNITS[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(unit), 0);

    if (editing) {
        char ptxt[64], stxt[64];
        format_cz(old_price, ptxt, sizeof(ptxt));
        format_cz(old_stock, stxt, sizeof(stxt));
        gtk_entry_set_text(GTK_ENTRY(name), old_name ? old_name : "");
        gtk_entry_set_text(GTK_ENTRY(price), ptxt);
        gtk_entry_set_text(GTK_ENTRY(stock), stxt);
        gtk_entry_set_text(GTK_ENTRY(category_entry), old_category ? old_category : "");
        for (int i = 0; UNITS[i]; i++)
            if (g_strcmp0(UNITS[i], old_unit) == 0)
                gtk_combo_box_set_active(GTK_COMBO_BOX(unit), i);
    }

    const char *labels[] = {"Název:", "Cena (Kč):", "Počet:", "Kategorie:", "Jednotka:"};
    GtkWidget *widgets[] = {name, price, stock, category, unit};
    for (int i = 0; i < 5; i++) {
        GtkWidget *l = gtk_label_new(labels[i]);
        gtk_widget_set_halign(l, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(grid), l, 0, i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), widgets[i], 1, i, 1, 1);
    }

    gtk_widget_show_all(dialog);

    while (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        const char *n = gtk_entry_get_text(GTK_ENTRY(name));
        const char *cat = gtk_entry_get_text(GTK_ENTRY(category_entry));
        gchar *u = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(unit));
        double p = 0.0, st = 0.0;

        if (!n || !*n) {
            show_error(GTK_WINDOW(dialog), "Zadej název položky.");
            g_free(u); continue;
        }
        if ((!editing || !names_equal(n, old_name)) && name_exists(app, n)) {
            show_error(GTK_WINDOW(dialog), "Položka s tímto názvem už existuje.");
            g_free(u); continue;
        }
        if (!parse_cz_number(gtk_entry_get_text(GTK_ENTRY(price)), &p)) {
            show_error(GTK_WINDOW(dialog), "Cena není platná. Použij desetinnou čárku.");
            g_free(u); continue;
        }
        if (!parse_cz_number(gtk_entry_get_text(GTK_ENTRY(stock)), &st)) {
            show_error(GTK_WINDOW(dialog), "Počet není platný.");
            g_free(u); continue;
        }
        if (g_strcmp0(u, "ks") == 0 && fabs(st - floor(st)) > 0.000001) {
            show_error(GTK_WINDOW(dialog), "U jednotky ks musí být počet celé číslo.");
            g_free(u); continue;
        }

        const char *cat_final = (cat && *cat) ? cat : "Ostatní";
        GtkTreeIter iter;
        if (editing) {
            iter = *edit_iter;
        } else {
            gtk_list_store_append(app->store, &iter);
        }

        gtk_list_store_set(app->store, &iter,
                           COL_NAME, n, COL_PRICE, p, COL_STOCK, st,
                           COL_CATEGORY, cat_final, COL_UNIT, u, -1);
        app->dirty = TRUE;

        if (!editing)
            scroll_to_last(app, &iter);

        gtk_label_set_text(GTK_LABEL(app->status_label),
            editing ? "Položka upravena. Nezapomeň uložit."
                    : "Položka přidána. Nezapomeň uložit.");
        g_free(u);
        break;
    }

    g_free(old_name);
    g_free(old_category);
    g_free(old_unit);
    gtk_widget_destroy(dialog);
}

static void add_item_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    item_dialog((WareManApp *)data, NULL);
}

static void row_activated(GtkTreeView *view,
                          GtkTreePath *path,
                          GtkTreeViewColumn *column,
                          gpointer data)
{
    (void)view;
    (void)column;
    WareManApp *app = data;
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(app->store), &iter, path))
        item_dialog(app, &iter);
}

static void delete_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    WareManApp *app = data;

    GtkTreeSelection *selection =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(app->treeview));
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(selection, NULL, &iter)) {
        gtk_label_set_text(GTK_LABEL(app->status_label),
                           "Vyber položku, kterou chceš odstranit.");
        return;
    }

    gchar *name = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(app->store), &iter,
                       COL_NAME, &name, -1);

    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_WARNING,
        GTK_BUTTONS_NONE,
        "Opravdu odstranit položku?");

    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(dialog),
        "Položka „%s“ bude odstraněna ze skladu. Změnu je potom nutné uložit.",
        name ? name : "");

    gtk_dialog_add_buttons(GTK_DIALOG(dialog),
                           "_Zrušit", GTK_RESPONSE_CANCEL,
                           "_Odstranit", GTK_RESPONSE_ACCEPT,
                           NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);

    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (response == GTK_RESPONSE_ACCEPT) {
        gtk_list_store_remove(app->store, &iter);
        app->dirty = TRUE;
        gtk_label_set_text(GTK_LABEL(app->status_label),
                           "Položka odstraněna. Nezapomeň uložit.");
    }

    g_free(name);
}

static void save_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    save_items((WareManApp *)data);
}

static void reload_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    WareManApp *app = data;

    if (!app->dirty) {
        load_items(app);
        return;
    }

    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "Máš neuložené změny.");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
        "Pokud znovu načteš items.acri, neuložené změny se ztratí.");
    gtk_dialog_add_buttons(GTK_DIALOG(d),
        "_Zrušit", GTK_RESPONSE_CANCEL,
        "_Zahodit změny a načíst", GTK_RESPONSE_REJECT,
        "_Uložit a načíst", GTK_RESPONSE_ACCEPT, NULL);

    gint r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);

    if (r == GTK_RESPONSE_ACCEPT) {
        if (save_items(app)) load_items(app);
    } else if (r == GTK_RESPONSE_REJECT) {
        load_items(app);
    }
}

static gboolean on_delete(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    (void)widget; (void)event;
    WareManApp *app = data;
    if (!app->dirty) return FALSE;

    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "Máš neuložené změny.");
    gtk_dialog_add_buttons(GTK_DIALOG(d),
        "_Zrušit", GTK_RESPONSE_CANCEL,
        "_Neuložit", GTK_RESPONSE_REJECT,
        "_Uložit", GTK_RESPONSE_ACCEPT, NULL);

    gint r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    if (r == GTK_RESPONSE_ACCEPT) return !save_items(app);
    if (r == GTK_RESPONSE_REJECT) return FALSE;
    return TRUE;
}

static void activate(GtkApplication *application, gpointer user_data)
{
    (void)user_data;
    WareManApp *app = g_new0(WareManApp, 1);
    app->items_path = find_items_file();

    app->window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(app->window), "AdavaWarehouseManagment");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 850, 560);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(root), 12);
    gtk_container_add(GTK_CONTAINER(app->window), root);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title),
        "<span size=\"20000\" weight=\"bold\">Správa skladu systému AdavaCashReg</span>\n"
        "<span size=\"11000\">Editor souborů ARCI v1 / JSON</span>");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(root), title, FALSE, FALSE, 0);

    app->store = gtk_list_store_new(NUM_COLS,
        G_TYPE_STRING, G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_STRING, G_TYPE_STRING);
    app->treeview = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->store));
    add_columns(GTK_TREE_VIEW(app->treeview));
    g_signal_connect(app->treeview, "row-activated",
                     G_CALLBACK(row_activated), app);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(app->treeview, TRUE);
    gtk_widget_set_vexpand(app->treeview, TRUE);
    gtk_container_add(GTK_CONTAINER(scroll), app->treeview);
    gtk_box_pack_start(GTK_BOX(root), scroll, TRUE, TRUE, 0);

    app->status_label = gtk_label_new("Připraveno.");
    gtk_widget_set_halign(app->status_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(root), app->status_label, FALSE, FALSE, 0);

    GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(root), bottom, FALSE, FALSE, 0);

    GtkWidget *add = gtk_button_new_with_label("Přidat novou");
    GtkWidget *remove = gtk_button_new_with_label("Odstranit");
    GtkWidget *reload = gtk_button_new_with_label("Znovu načíst");
    GtkWidget *save = gtk_button_new_with_label("Uložit");

    gtk_box_pack_start(GTK_BOX(bottom), add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bottom), remove, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bottom), reload, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bottom), save, FALSE, FALSE, 0);

    g_signal_connect(add, "clicked", G_CALLBACK(add_item_clicked), app);
    g_signal_connect(remove, "clicked", G_CALLBACK(delete_clicked), app);
    g_signal_connect(reload, "clicked", G_CALLBACK(reload_clicked), app);
    g_signal_connect(save, "clicked", G_CALLBACK(save_clicked), app);
    g_signal_connect(app->window, "delete-event", G_CALLBACK(on_delete), app);

    load_items(app);
    gtk_widget_show_all(app->window);
}

int main(int argc, char **argv)
{
    GtkApplication *application =
        gtk_application_new("cz.adava.wareman", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    return status;
}
