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
} WarehouseApp;

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

static gboolean names_equal(const char *a, const char *b)
{
    gchar *aa = g_utf8_casefold(a ? a : "", -1);
    gchar *bb = g_utf8_casefold(b ? b : "", -1);
    gboolean same = g_strcmp0(aa, bb) == 0;
    g_free(aa);
    g_free(bb);
    return same;
}

static gboolean name_exists(WarehouseApp *app, const char *name)
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

static void add_columns(GtkTreeView *view)
{
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    gtk_tree_view_append_column(view,
        gtk_tree_view_column_new_with_attributes("Položka", r, "text", COL_NAME, NULL));

    r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *price = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(price, "Cena");
    gtk_tree_view_column_pack_start(price, r, TRUE);
    gtk_tree_view_column_set_cell_data_func(price, r, price_cell, NULL, NULL);
    gtk_tree_view_append_column(view, price);

    r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *stock = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(stock, "Skladem");
    gtk_tree_view_column_pack_start(stock, r, TRUE);
    gtk_tree_view_column_set_cell_data_func(stock, r, stock_cell, NULL, NULL);
    gtk_tree_view_append_column(view, stock);

    r = gtk_cell_renderer_text_new();
    gtk_tree_view_append_column(view,
        gtk_tree_view_column_new_with_attributes("Kategorie", r, "text", COL_CATEGORY, NULL));

    r = gtk_cell_renderer_text_new();
    gtk_tree_view_append_column(view,
        gtk_tree_view_column_new_with_attributes("Jednotka", r, "text", COL_UNIT, NULL));
}

static gboolean load_items(WarehouseApp *app)
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

static gboolean save_items(WarehouseApp *app)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "format");
    json_builder_add_string_value(b, "ACRI");
    json_builder_set_member_name(b, "version");
    json_builder_add_int_value(b, 1);
    json_builder_set_member_name(b, "items");
    json_builder_begin_array(b);

    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->store), &iter);
    while (valid) {
        gchar *name = NULL, *category = NULL, *unit = NULL;
        double price = 0.0, stock = 0.0;
        gtk_tree_model_get(GTK_TREE_MODEL(app->store), &iter,
                           COL_NAME, &name, COL_PRICE, &price, COL_STOCK, &stock,
                           COL_CATEGORY, &category, COL_UNIT, &unit, -1);

        json_builder_begin_object(b);
        json_builder_set_member_name(b, "name"); json_builder_add_string_value(b, name);
        json_builder_set_member_name(b, "price"); json_builder_add_double_value(b, price);
        json_builder_set_member_name(b, "stock"); json_builder_add_double_value(b, stock);
        json_builder_set_member_name(b, "category"); json_builder_add_string_value(b, category);
        json_builder_set_member_name(b, "unit"); json_builder_add_string_value(b, unit);
        json_builder_end_object(b);

        g_free(name); g_free(category); g_free(unit);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(app->store), &iter);
    }

    json_builder_end_array(b);
    json_builder_end_object(b);

    JsonGenerator *g = json_generator_new();
    JsonNode *root = json_builder_get_root(b);
    json_generator_set_root(g, root);
    json_generator_set_pretty(g, TRUE);

    GError *error = NULL;
    gboolean ok = json_generator_to_file(g, app->items_path, &error);
    if (!ok) {
        char msg[512];
        g_snprintf(msg, sizeof(msg), "Nelze uložit: %s", error->message);
        gtk_label_set_text(GTK_LABEL(app->status_label), msg);
        g_clear_error(&error);
    } else {
        gtk_label_set_text(GTK_LABEL(app->status_label), "items.acri uložen.");
        app->dirty = FALSE;
    }

    json_node_free(root);
    g_object_unref(g);
    g_object_unref(b);
    return ok;
}

static void scroll_to_last(WarehouseApp *app, GtkTreeIter *iter)
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

static void item_dialog(WarehouseApp *app, GtkTreeIter *edit_iter)
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
    GtkWidget *category = gtk_entry_new();
    GtkWidget *unit = gtk_combo_box_text_new();

    gtk_entry_set_placeholder_text(GTK_ENTRY(name), "např. Rohlík");
    gtk_entry_set_placeholder_text(GTK_ENTRY(price), "např. 3,90");
    gtk_entry_set_placeholder_text(GTK_ENTRY(stock), "např. 120 nebo 12,5");
    gtk_entry_set_placeholder_text(GTK_ENTRY(category), "např. Pečivo");

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
        gtk_entry_set_text(GTK_ENTRY(category), old_category ? old_category : "");
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
        const char *cat = gtk_entry_get_text(GTK_ENTRY(category));
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
    item_dialog((WarehouseApp *)data, NULL);
}

static void row_activated(GtkTreeView *view,
                          GtkTreePath *path,
                          GtkTreeViewColumn *column,
                          gpointer data)
{
    (void)view;
    (void)column;
    WarehouseApp *app = data;
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(app->store), &iter, path))
        item_dialog(app, &iter);
}

static void save_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    save_items((WarehouseApp *)data);
}

static void reload_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    WarehouseApp *app = data;

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
    WarehouseApp *app = data;
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
    WarehouseApp *app = g_new0(WarehouseApp, 1);
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
        "<span size=\"11000\">ACRI v1 / JSON</span>");
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
    gtk_container_add(GTK_CONTAINER(scroll), app->treeview);
    gtk_box_pack_start(GTK_BOX(root), scroll, TRUE, TRUE, 0);

    app->status_label = gtk_label_new("Připraveno.");
    gtk_widget_set_halign(app->status_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(root), app->status_label, FALSE, FALSE, 0);

    GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(root), bottom, FALSE, FALSE, 0);

    GtkWidget *add = gtk_button_new_with_label("Přidat novou");
    GtkWidget *save = gtk_button_new_with_label("Uložit");
    GtkWidget *reload = gtk_button_new_with_label("Znovu načíst");

    gtk_box_pack_start(GTK_BOX(bottom), add, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bottom), save, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bottom), reload, TRUE, TRUE, 0);

    g_signal_connect(add, "clicked", G_CALLBACK(add_item_clicked), app);
    g_signal_connect(save, "clicked", G_CALLBACK(save_clicked), app);
    g_signal_connect(reload, "clicked", G_CALLBACK(reload_clicked), app);
    g_signal_connect(app->window, "delete-event", G_CALLBACK(on_delete), app);

    load_items(app);
    gtk_widget_show_all(app->window);
}

int main(int argc, char **argv)
{
    GtkApplication *application =
        gtk_application_new("cz.adava.warehousemanagment", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    return status;
}
