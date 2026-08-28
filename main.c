#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

enum {
    COL_NAME,
    COL_PRICE,
    COL_QUANTITY,
    COL_UNIT,
    COL_TOTAL,
    NUM_COLS
};

typedef struct {
    char *name;
    double price;
    double stock;
    char *category;
    char *unit;
} Product;

typedef struct {
    GtkWidget *window;
    GtkWidget *treeview;
    GtkListStore *store;
    GtkWidget *total_label;
    GtkWidget *status_label;
    char *items_path;
    double total;
} App;

static Product *products = NULL;
static int product_count = 0;

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

static gboolean load_products(const char *path, GError **error)
{
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_file(parser, path, error)) {
        g_object_unref(parser);
        return FALSE;
    }

    JsonNode *root_node = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root_node)) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "ACRI kořen není JSON objekt.");
        g_object_unref(parser);
        return FALSE;
    }

    JsonObject *root = json_node_get_object(root_node);
    const char *format = json_object_get_string_member_with_default(root, "format", "");
    gint64 version = json_object_get_int_member_with_default(root, "version", 0);
    if (g_strcmp0(format, "ACRI") != 0 || version != 1) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "Nepodporovaný ACRI formát/verze.");
        g_object_unref(parser);
        return FALSE;
    }

    JsonArray *items = json_object_get_array_member(root, "items");
    guint n = items ? json_array_get_length(items) : 0;
    products = g_new0(Product, n);
    product_count = 0;

    for (guint i = 0; i < n; i++) {
        JsonObject *o = json_array_get_object_element(items, i);
        if (!o) continue;
        const char *name = json_object_get_string_member_with_default(o, "name", "");
        if (!*name) continue;

        Product *p = &products[product_count++];
        p->name = g_strdup(name);
        p->price = json_object_get_double_member_with_default(o, "price", 0.0);
        p->stock = json_object_get_double_member_with_default(o, "stock", 0.0);
        p->category = g_strdup(json_object_get_string_member_with_default(o, "category", "Ostatní"));
        p->unit = g_strdup(json_object_get_string_member_with_default(o, "unit", "ks"));
    }

    g_object_unref(parser);
    return TRUE;
}

static Product *find_product(const char *name)
{
    for (int i = 0; i < product_count; i++)
        if (g_strcmp0(products[i].name, name) == 0)
            return &products[i];
    return NULL;
}

static gboolean save_products(App *app, GError **error)
{
    GString *out = g_string_new(
        "{\n"
        "  \"format\" : \"ACRI\",\n"
        "  \"version\" : 1,\n"
        "  \"items\" : [\n"
    );

    for (int i = 0; i < product_count; i++) {
        Product *p = &products[i];
        char price[64];
        char stock[64];

        format_json_price(p->price, price, sizeof(price));
        format_json_quantity(p->stock, stock, sizeof(stock));

        g_string_append(out, "    {\n      \"name\" : ");
        append_json_string(out, p->name);
        g_string_append_printf(out,
            ",\n"
            "      \"price\" : %s,\n"
            "      \"stock\" : %s,\n"
            "      \"category\" : ",
            price, stock);
        append_json_string(out, p->category);
        g_string_append(out, ",\n      \"unit\" : ");
        append_json_string(out, p->unit);
        g_string_append(out, "\n    }");

        if (i + 1 < product_count)
            g_string_append_c(out, ',');

        g_string_append_c(out, '\n');
    }

    g_string_append(out, "  ]\n}\n");

    gboolean ok = g_file_set_contents(
        app->items_path,
        out->str,
        (gssize)out->len,
        error
    );

    g_string_free(out, TRUE);
    return ok;
}

static void update_total(App *app)
{
    char n[64], text[128];
    format_cz(app->total, n, sizeof(n));
    g_snprintf(text, sizeof(text),
        "<span size=\"30000\" weight=\"bold\">%s Kč</span>", n);
    gtk_label_set_markup(GTK_LABEL(app->total_label), text);
}

static gboolean cart_has_items(App *app)
{
    GtkTreeIter iter;
    return gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->store), &iter);
}

static gboolean find_cart_item(App *app, const char *name, GtkTreeIter *found)
{
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->store), &iter);
    while (valid) {
        gchar *existing = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(app->store), &iter, COL_NAME, &existing, -1);
        gboolean same = g_strcmp0(existing, name) == 0;
        g_free(existing);
        if (same) {
            *found = iter;
            return TRUE;
        }
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(app->store), &iter);
    }
    return FALSE;
}

static void show_error(GtkWindow *parent, const char *text)
{
    GtkWidget *d = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL,
        GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", text);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

static gboolean ask_quantity(App *app, Product *p, double initial, double *quantity)
{
    GtkWidget *d = gtk_dialog_new_with_buttons(
        p->name, GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Zrušit", GTK_RESPONSE_CANCEL,
        "_Přidat", GTK_RESPONSE_OK, NULL);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))),
                       box, TRUE, TRUE, 0);

    char prompt[256], stock_text[64];
    format_cz(p->stock, stock_text, sizeof(stock_text));
    g_snprintf(prompt, sizeof(prompt),
        "Zadej množství (%s)\nSkladem: %s %s",
        p->unit, stock_text, p->unit);
    GtkWidget *label = gtk_label_new(prompt);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry),
        g_strcmp0(p->unit, "ks") == 0 ? "např. 2" : "např. 1,5");

    if (initial > 0.0) {
        char initial_text[64];
        format_cz(initial, initial_text, sizeof(initial_text));
        gtk_entry_set_text(GTK_ENTRY(entry), initial_text);
        gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
    }

    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
    gtk_widget_show_all(d);

    gboolean ok = FALSE;
    while (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
        double q = 0.0;
        if (!parse_cz_number(gtk_entry_get_text(GTK_ENTRY(entry)), &q) || q <= 0.0) {
            show_error(GTK_WINDOW(d), "Zadej kladné množství.");
            continue;
        }
        if (g_strcmp0(p->unit, "ks") == 0 && fabs(q - floor(q)) > 0.000001) {
            show_error(GTK_WINDOW(d), "U jednotky ks musí být množství celé číslo.");
            continue;
        }
        if (q > p->stock) {
            show_error(GTK_WINDOW(d), "Požadované množství je větší než stav skladu.");
            continue;
        }
        *quantity = q;
        ok = TRUE;
        break;
    }

    gtk_widget_destroy(d);
    return ok;
}

static void add_product(GtkWidget *button, gpointer data)
{
    App *app = data;
    int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "product-index"));
    Product *p = &products[index];

    double q = 0.0;
    if (!ask_quantity(app, p, 0.0, &q)) return;

    GtkTreeIter iter;
    if (find_cart_item(app, p->name, &iter)) {
        double old_q = 0.0;
        gtk_tree_model_get(GTK_TREE_MODEL(app->store), &iter, COL_QUANTITY, &old_q, -1);
        double new_q = old_q + q;
        if (new_q > p->stock) {
            show_error(GTK_WINDOW(app->window), "Celkové množství by překročilo stav skladu.");
            return;
        }
        gtk_list_store_set(app->store, &iter,
                           COL_QUANTITY, new_q,
                           COL_TOTAL, new_q * p->price, -1);
    } else {
        gtk_list_store_append(app->store, &iter);
        gtk_list_store_set(app->store, &iter,
                           COL_NAME, p->name,
                           COL_PRICE, p->price,
                           COL_QUANTITY, q,
                           COL_UNIT, p->unit,
                           COL_TOTAL, q * p->price, -1);
    }

    app->total += q * p->price;
    update_total(app);

    char qtxt[64], msg[192];
    format_cz(q, qtxt, sizeof(qtxt));
    g_snprintf(msg, sizeof(msg), "Přidáno: %s — %s %s", p->name, qtxt, p->unit);
    gtk_label_set_text(GTK_LABEL(app->status_label), msg);
}

static void cart_row_activated(GtkTreeView *view,
                               GtkTreePath *path,
                               GtkTreeViewColumn *column,
                               gpointer data)
{
    (void)view;
    (void)column;
    App *app = data;

    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(app->store), &iter, path))
        return;

    gchar *name = NULL;
    double old_q = 0.0;
    double price = 0.0;
    gtk_tree_model_get(GTK_TREE_MODEL(app->store), &iter,
                       COL_NAME, &name,
                       COL_PRICE, &price,
                       COL_QUANTITY, &old_q,
                       -1);

    Product *p = find_product(name);
    if (!p) {
        g_free(name);
        return;
    }

    double new_q = old_q;
    if (ask_quantity(app, p, old_q, &new_q)) {
        app->total += (new_q - old_q) * price;
        if (app->total < 0.001) app->total = 0.0;

        gtk_list_store_set(app->store, &iter,
                           COL_QUANTITY, new_q,
                           COL_TOTAL, new_q * price,
                           -1);
        update_total(app);

        char qtxt[64], msg[192];
        format_cz(new_q, qtxt, sizeof(qtxt));
        g_snprintf(msg, sizeof(msg), "Množství upraveno: %s — %s %s",
                   p->name, qtxt, p->unit);
        gtk_label_set_text(GTK_LABEL(app->status_label), msg);
    }

    g_free(name);
}

static void remove_selected(GtkWidget *button, gpointer data)
{
    (void)button;
    App *app = data;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->treeview));
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(sel, NULL, &iter)) {
        gtk_label_set_text(GTK_LABEL(app->status_label), "Vyber položku.");
        return;
    }

    double row_total = 0.0;
    gtk_tree_model_get(GTK_TREE_MODEL(app->store), &iter, COL_TOTAL, &row_total, -1);
    gtk_list_store_remove(app->store, &iter);
    app->total -= row_total;
    if (app->total < 0.001) app->total = 0.0;
    update_total(app);
}

static void clear_cart(GtkWidget *button, gpointer data)
{
    (void)button;
    App *app = data;
    gtk_list_store_clear(app->store);
    app->total = 0.0;
    update_total(app);
}

static gboolean on_delete(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    (void)widget; (void)event;
    App *app = data;
    if (!cart_has_items(app)) return FALSE;

    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "Je rozdělaný nákup.");
    gtk_dialog_add_buttons(GTK_DIALOG(d),
        "_Pokračovat", GTK_RESPONSE_CANCEL,
        "_Ukončit bez dokončení", GTK_RESPONSE_ACCEPT, NULL);
    gint r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    return r != GTK_RESPONSE_ACCEPT;
}

static void price_cell(GtkTreeViewColumn *column, GtkCellRenderer *renderer,
                       GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    int col = GPOINTER_TO_INT(data);
    double value = 0.0;
    gtk_tree_model_get(model, iter, col, &value, -1);
    char n[64], text[80];
    format_cz(value, n, sizeof(n));
    g_snprintf(text, sizeof(text), "%s Kč", n);
    g_object_set(renderer, "text", text, NULL);
}

static void quantity_cell(GtkTreeViewColumn *column, GtkCellRenderer *renderer,
                          GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    (void)column; (void)data;
    double q = 0.0;
    gchar *unit = NULL;
    gtk_tree_model_get(model, iter, COL_QUANTITY, &q, COL_UNIT, &unit, -1);
    char n[64], text[96];
    format_cz(q, n, sizeof(n));
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

static void add_text_column(GtkTreeView *view, const char *title, int col)
{
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *c =
        gtk_tree_view_column_new_with_attributes(title, r, "text", col, NULL);
    configure_dynamic_column(c);
    gtk_tree_view_append_column(view, c);
}

static void add_price_column(GtkTreeView *view, const char *title, int col)
{
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *c = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(c, title);
    gtk_tree_view_column_pack_start(c, r, TRUE);
    gtk_tree_view_column_set_cell_data_func(c, r, price_cell, GINT_TO_POINTER(col), NULL);
    configure_dynamic_column(c);
    gtk_tree_view_append_column(view, c);
}

static void add_quantity_column(GtkTreeView *view)
{
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *c = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(c, "Množství");
    gtk_tree_view_column_pack_start(c, r, TRUE);
    gtk_tree_view_column_set_cell_data_func(c, r, quantity_cell, NULL, NULL);
    configure_dynamic_column(c);
    gtk_tree_view_append_column(view, c);
}

static void show_receipt(App *app, double charged, double paid, double change)
{
    (void)app;
    char a[64], p[64], c[64], text[256];
    format_cz(charged, a, sizeof(a));
    format_cz(paid, p, sizeof(p));
    format_cz(change, c, sizeof(c));
    g_snprintf(text, sizeof(text), "Celkem: %s Kč\nZaplaceno: %s Kč\nVrátit: %s Kč",
               a, p, c);

    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "Platba dokončena");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", text);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

static gboolean deduct_cart_from_stock(App *app)
{
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->store), &iter);

    while (valid) {
        gchar *name = NULL;
        double q = 0.0;
        gtk_tree_model_get(GTK_TREE_MODEL(app->store), &iter,
                           COL_NAME, &name, COL_QUANTITY, &q, -1);
        Product *p = find_product(name);
        g_free(name);

        if (!p || q > p->stock + 0.000001) {
            show_error(GTK_WINDOW(app->window),
                       "Sklad se mezitím změnil nebo není dostatečné množství.");
            return FALSE;
        }
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(app->store), &iter);
    }

    valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->store), &iter);
    while (valid) {
        gchar *name = NULL;
        double q = 0.0;
        gtk_tree_model_get(GTK_TREE_MODEL(app->store), &iter,
                           COL_NAME, &name, COL_QUANTITY, &q, -1);
        Product *p = find_product(name);
        if (p) p->stock -= q;
        g_free(name);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(app->store), &iter);
    }

    GError *error = NULL;
    if (save_products(app, &error))
        return TRUE;

    valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->store), &iter);
    while (valid) {
        gchar *name = NULL;
        double q = 0.0;
        gtk_tree_model_get(GTK_TREE_MODEL(app->store), &iter,
                           COL_NAME, &name, COL_QUANTITY, &q, -1);
        Product *p = find_product(name);
        if (p) p->stock += q;
        g_free(name);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(app->store), &iter);
    }

    char msg[512];
    g_snprintf(msg, sizeof(msg), "Nepodařilo se uložit stav skladu: %s",
               error ? error->message : "neznámá chyba");
    show_error(GTK_WINDOW(app->window), msg);
    g_clear_error(&error);
    return FALSE;
}

static void payment_changed(GtkEditable *editable, gpointer data)
{
    (void)data;

    g_print("payment_changed: %s\n",
            gtk_entry_get_text(GTK_ENTRY(editable)));

    GtkWidget *change_label =
        g_object_get_data(
            G_OBJECT(editable),
            "change-label"
        );

    double *amount_due =
        g_object_get_data(
            G_OBJECT(editable),
            "amount-due-pointer"
        );

    if (!change_label || !amount_due) {
        g_print("CHYBA: change_label=%p amount_due=%p\n",
                (void *)change_label,
                (void *)amount_due);
        return;
    }

    const char *text =
        gtk_entry_get_text(GTK_ENTRY(editable));

    double paid = 0.0;
    char output[160];

    if (!text || text[0] == '\0') {
        snprintf(
            output,
            sizeof(output),
            "<span size=\"18000\">Vrátit: --</span>"
        );
    } else if (!parse_cz_number(text, &paid)) {
        snprintf(
            output,
            sizeof(output),
            "<span size=\"18000\" foreground=\"red\">"
            "Neplatná částka"
            "</span>"
        );
    } else if (paid < *amount_due) {
        char missing[64];

        format_cz(
            *amount_due - paid,
            missing,
            sizeof(missing)
        );

        snprintf(
            output,
            sizeof(output),
            "<span size=\"18000\" foreground=\"red\" weight=\"bold\">"
            "Chybí: %s Kč"
            "</span>",
            missing
        );
    } else {
        char change[64];

        format_cz(
            paid - *amount_due,
            change,
            sizeof(change)
        );

        snprintf(
            output,
            sizeof(output),
            "<span size=\"18000\" foreground=\"green\" weight=\"bold\">"
            "Vrátit: %s Kč"
            "</span>",
            change
        );
    }

    gtk_label_set_markup(
        GTK_LABEL(change_label),
        output
    );
}

static void pay(GtkWidget *button, gpointer data)
{
    (void)button;
    App *app = data;
    if (!cart_has_items(app)) {
        gtk_label_set_text(GTK_LABEL(app->status_label), "Košík je prázdný.");
        return;
    }

    const gint RESPONSE_ROUND = 1001;
    GtkWidget *d = gtk_dialog_new_with_buttons("Platba", GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Zrušit", GTK_RESPONSE_CANCEL,
        "_Zaplatit", GTK_RESPONSE_OK, NULL);
    gtk_dialog_add_button(GTK_DIALOG(d), "_Zaokrouhlit", RESPONSE_ROUND);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), box, TRUE, TRUE, 0);

    GtkWidget *title = gtk_label_new(NULL);
    GtkWidget *entry = gtk_entry_new();
    GtkWidget *change_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(change_label),
        "<span size=\"18000\">Vrátit: --</span>");
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "např. 500,00");
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), change_label, FALSE, FALSE, 0);

    double amount_due = app->total;
    g_object_set_data(G_OBJECT(entry), "change-label", change_label);
    g_object_set_data(G_OBJECT(entry), "amount-due-pointer", &amount_due);
    g_signal_connect(entry, "changed", G_CALLBACK(payment_changed), NULL);
    char due[64], markup[160];
    format_cz(amount_due, due, sizeof(due));
    g_snprintf(markup, sizeof(markup),
        "<span size=\"24000\" weight=\"bold\">K zaplacení: %s Kč</span>", due);
    gtk_label_set_markup(GTK_LABEL(title), markup);
    gtk_widget_show_all(d);

    while (1) {
        gint r = gtk_dialog_run(GTK_DIALOG(d));
        if (r == RESPONSE_ROUND) {
            gint64 cents = (gint64)(app->total * 100.0 + 0.5);
            amount_due = (double)((cents + 50) / 100);
            format_cz(amount_due, due, sizeof(due));
            g_snprintf(markup, sizeof(markup),
                "<span size=\"24000\" weight=\"bold\">K zaplacení (zaokrouhleno): %s Kč</span>", due);
            gtk_label_set_markup(GTK_LABEL(title), markup);
            payment_changed(GTK_EDITABLE(entry), NULL);
            continue;
        }
        if (r != GTK_RESPONSE_OK) break;

        double paid = 0.0;
        if (!parse_cz_number(gtk_entry_get_text(GTK_ENTRY(entry)), &paid)) {
            show_error(GTK_WINDOW(d), "Neplatná částka.");
            continue;
        }
        if (paid < amount_due) {
            show_error(GTK_WINDOW(d), "Zákazník zaplatil málo.");
            continue;
        }

        double change = paid - amount_due;

        if (!deduct_cart_from_stock(app))
            continue;

        show_receipt(app, amount_due, paid, change);
        gtk_list_store_clear(app->store);
        app->total = 0.0;
        update_total(app);
        gtk_label_set_text(GTK_LABEL(app->status_label), "Platba dokončena.");
        break;
    }
    gtk_widget_destroy(d);
}

static GtkWidget *build_category_page(App *app, const char *category)
{
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_widget_set_vexpand(scroll, TRUE);

    GtkWidget *flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 8);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 8);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flow), 1);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 20);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), FALSE);
    gtk_widget_set_hexpand(flow, TRUE);
    gtk_widget_set_valign(flow, GTK_ALIGN_START);
    gtk_container_add(GTK_CONTAINER(scroll), flow);

    for (int i = 0; i < product_count; i++) {
        if (g_strcmp0(products[i].category, category) != 0) continue;
        if (products[i].stock <= 0.000001) continue;
        char price[64], text[160];
        format_cz(products[i].price, price, sizeof(price));
        g_snprintf(text, sizeof(text), "%s\n%s Kč / %s",
                   products[i].name, price, products[i].unit);

        GtkWidget *b = gtk_button_new_with_label(text);
        gtk_widget_set_size_request(b, 135, 85);
        gtk_widget_set_hexpand(b, FALSE);
        gtk_widget_set_vexpand(b, FALSE);
        g_object_set_data(G_OBJECT(b), "product-index", GINT_TO_POINTER(i));
        g_signal_connect(b, "clicked", G_CALLBACK(add_product), app);
        gtk_container_add(GTK_CONTAINER(flow), b);
    }
    return scroll;
}

static void activate(GtkApplication *application, gpointer user_data)
{
    (void)user_data;
    App *app = g_new0(App, 1);

    app->items_path = find_items_file();
    GError *error = NULL;
    if (!load_products(app->items_path, &error)) {
        g_printerr("Nelze načíst %s: %s\n", app->items_path,
                   error ? error->message : "chyba");
        g_clear_error(&error);
    }

    app->window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(app->window), "AdavaCashRegister");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 1100, 700);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(main_box), 12);
    gtk_widget_set_hexpand(main_box, TRUE);
    gtk_widget_set_vexpand(main_box, TRUE);
    gtk_container_add(GTK_CONTAINER(app->window), main_box);

    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_hexpand(left, TRUE);
    gtk_widget_set_vexpand(left, TRUE);
    gtk_box_pack_start(GTK_BOX(main_box), left, TRUE, TRUE, 0);

    GtkWidget *heading = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(heading),
        "<span size=\"20000\" weight=\"bold\">Produkty</span>");
    gtk_widget_set_halign(heading, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(left), heading, FALSE, FALSE, 0);

    GtkWidget *notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook), TRUE);
    gtk_box_pack_start(GTK_BOX(left), notebook, TRUE, TRUE, 0);

    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    for (int i = 0; i < product_count; i++) {
        if (products[i].stock <= 0.000001) continue;
        if (g_hash_table_contains(seen, products[i].category)) continue;
        g_hash_table_add(seen, products[i].category);
        GtkWidget *page = build_category_page(app, products[i].category);
        GtkWidget *tab = gtk_label_new(products[i].category);
        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), page, tab);
    }
    g_hash_table_destroy(seen);

    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_hexpand(right, TRUE);
    gtk_widget_set_vexpand(right, TRUE);
    gtk_box_pack_start(GTK_BOX(main_box), right, TRUE, TRUE, 0);

    GtkWidget *total_title = gtk_label_new("CELKEM");
    gtk_widget_set_halign(total_title, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(right), total_title, FALSE, FALSE, 0);

    app->total_label = gtk_label_new(NULL);
    gtk_widget_set_halign(app->total_label, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(right), app->total_label, FALSE, FALSE, 0);
    update_total(app);

    app->store = gtk_list_store_new(NUM_COLS,
        G_TYPE_STRING, G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_STRING, G_TYPE_DOUBLE);
    app->treeview = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->store));
    g_signal_connect(app->treeview, "row-activated",
                     G_CALLBACK(cart_row_activated), app);
    add_text_column(GTK_TREE_VIEW(app->treeview), "Položka", COL_NAME);
    add_price_column(GTK_TREE_VIEW(app->treeview), "Cena", COL_PRICE);
    add_quantity_column(GTK_TREE_VIEW(app->treeview));
    add_price_column(GTK_TREE_VIEW(app->treeview), "Celkem", COL_TOTAL);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(app->treeview, TRUE);
    gtk_widget_set_vexpand(app->treeview, TRUE);
    gtk_container_add(GTK_CONTAINER(scroll), app->treeview);
    gtk_box_pack_start(GTK_BOX(right), scroll, TRUE, TRUE, 0);

    app->status_label = gtk_label_new("Pokladna připravena.");
    gtk_widget_set_halign(app->status_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(right), app->status_label, FALSE, FALSE, 0);

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(right), buttons, FALSE, FALSE, 0);
    GtkWidget *remove = gtk_button_new_with_label("Odebrat");
    GtkWidget *clear = gtk_button_new_with_label("Zrušit nákup");
    GtkWidget *pay_btn = gtk_button_new_with_label("Zaplatit");
    gtk_box_pack_start(GTK_BOX(buttons), remove, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), clear, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(buttons), pay_btn, FALSE, FALSE, 0);

    g_signal_connect(remove, "clicked", G_CALLBACK(remove_selected), app);
    g_signal_connect(clear, "clicked", G_CALLBACK(clear_cart), app);
    g_signal_connect(pay_btn, "clicked", G_CALLBACK(pay), app);
    g_signal_connect(app->window, "delete-event", G_CALLBACK(on_delete), app);

    gtk_widget_show_all(app->window);
}

int main(int argc, char **argv)
{
    GtkApplication *application =
        gtk_application_new("cz.adava.pos", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    return status;
}
