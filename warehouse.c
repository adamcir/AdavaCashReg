/****************************************************************/
/* This is part of AdavaCashReg program    				  		*/
/* See more in copyright section in main.c 				  		*/
/* AdavaWarehouseManagment managment program for warehouse		*/
/* (c) 2026 Adam Cír (Adava), Adava Software, Adava Development */
/****************************************************************/

#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

enum {
    COL_NAME,
    COL_PRICE,
    NUM_COLS
};

typedef struct {
    GtkWidget *window;
    GtkWidget *treeview;
    GtkListStore *store;
    GtkWidget *name_entry;
    GtkWidget *price_entry;
    GtkWidget *status_label;
    char *items_path;
    gboolean dirty;
} WarehouseApp;

static gboolean parse_price_cz(const char *text, double *value)
{
    if (!text || !value)
        return FALSE;

    gchar *copy = g_strdup(text);
    gchar *trimmed = g_strstrip(copy);

    if (*trimmed == '\0' || strchr(trimmed, '.') != NULL) {
        g_free(copy);
        return FALSE;
    }

    int commas = 0;
    for (char *p = trimmed; *p; p++) {
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
    double parsed = g_ascii_strtod(trimmed, &end);
    gboolean ok = end != trimmed && *end == '\0' && parsed >= 0.0;

    if (ok)
        *value = parsed;

    g_free(copy);
    return ok;
}

static void format_price_cz(double value, char *buffer, gsize size)
{
    g_ascii_formatd(buffer, size, "%.2f", value);
    for (char *p = buffer; *p; p++)
        if (*p == '.')
            *p = ',';
}

static gboolean names_equal(const char *a, const char *b)
{
    gchar *a_copy = g_strdup(a ? a : "");
    gchar *b_copy = g_strdup(b ? b : "");
    gchar *a_fold = g_utf8_casefold(g_strstrip(a_copy), -1);
    gchar *b_fold = g_utf8_casefold(g_strstrip(b_copy), -1);

    gboolean same = g_strcmp0(a_fold, b_fold) == 0;

    g_free(a_fold);
    g_free(b_fold);
    g_free(a_copy);
    g_free(b_copy);
    return same;
}

static gboolean name_exists_except(WarehouseApp *app,
                                   const char *name,
                                   GtkTreeIter *except_iter)
{
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(
        GTK_TREE_MODEL(app->store), &iter);

    GtkTreePath *except_path = except_iter
        ? gtk_tree_model_get_path(GTK_TREE_MODEL(app->store), except_iter)
        : NULL;

    while (valid) {
        gboolean is_except = FALSE;

        if (except_path) {
            GtkTreePath *path = gtk_tree_model_get_path(
                GTK_TREE_MODEL(app->store), &iter);
            is_except = gtk_tree_path_compare(path, except_path) == 0;
            gtk_tree_path_free(path);
        }

        if (!is_except) {
            gchar *existing = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(app->store), &iter,
                               COL_NAME, &existing, -1);

            gboolean same = names_equal(existing, name);
            g_free(existing);

            if (same) {
                if (except_path)
                    gtk_tree_path_free(except_path);
                return TRUE;
            }
        }

        valid = gtk_tree_model_iter_next(
            GTK_TREE_MODEL(app->store), &iter);
    }

    if (except_path)
        gtk_tree_path_free(except_path);

    return FALSE;
}

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

static void price_cell_func(GtkTreeViewColumn *column,
                            GtkCellRenderer *renderer,
                            GtkTreeModel *model,
                            GtkTreeIter *iter,
                            gpointer data)
{
    (void)column;
    (void)data;
    double price = 0.0;
    gtk_tree_model_get(model, iter, COL_PRICE, &price, -1);
    char price_text[64];
    char text[80];
    format_price_cz(price, price_text, sizeof(price_text));
    g_snprintf(text, sizeof(text), "%s Kč", price_text);
    g_object_set(renderer, "text", text, NULL);
}

static void add_columns(GtkTreeView *view)
{
    GtkCellRenderer *name_renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *name_col = gtk_tree_view_column_new_with_attributes(
        "Položka", name_renderer, "text", COL_NAME, NULL);
    gtk_tree_view_column_set_expand(name_col, TRUE);
    gtk_tree_view_append_column(view, name_col);

    GtkCellRenderer *price_renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *price_col = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(price_col, "Cena");
    gtk_tree_view_column_pack_start(price_col, price_renderer, TRUE);
    gtk_tree_view_column_set_cell_data_func(price_col, price_renderer,
                                            price_cell_func, NULL, NULL);
    gtk_tree_view_append_column(view, price_col);
}

static gboolean load_items(WarehouseApp *app)
{
    gchar *content = NULL;
    gsize length = 0;
    GError *error = NULL;

    gtk_list_store_clear(app->store);

    if (!g_file_get_contents(app->items_path, &content, &length, &error)) {
        if (g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            gtk_label_set_text(GTK_LABEL(app->status_label),
                               "items.acri zatím neexistuje. Ulož první položku.");
            app->dirty = FALSE;
            g_clear_error(&error);
            return TRUE;
        }

        char msg[512];
        g_snprintf(msg, sizeof(msg), "Chyba při načítání: %s",
                   error ? error->message : "neznámá chyba");
        gtk_label_set_text(GTK_LABEL(app->status_label), msg);
        g_clear_error(&error);
        return FALSE;
    }

    gchar **lines = g_strsplit(content, "\n", -1);
    int count = 0;
    int duplicate_count = 0;

    for (int i = 0; lines[i] != NULL; i++) {
        gchar *line = g_strstrip(lines[i]);
        if (*line == '\0' || *line == '#' || g_strcmp0(line, "ACRI1") == 0)
            continue;

        gchar **parts = g_strsplit(line, "|", 2);
        if (!parts[0] || !parts[1]) {
            g_strfreev(parts);
            continue;
        }

        gchar *name = g_strstrip(parts[0]);
        gchar *price_text = g_strstrip(parts[1]);
        double price = 0.0;

        if (*name && parse_price_cz(price_text, &price)) {
            if (name_exists_except(app, name, NULL)) {
                duplicate_count++;
            } else {
                GtkTreeIter iter;
                gtk_list_store_append(app->store, &iter);
                gtk_list_store_set(app->store, &iter,
                                   COL_NAME, name,
                                   COL_PRICE, price,
                                   -1);
                count++;
            }
        }

        g_strfreev(parts);
    }

    g_strfreev(lines);
    g_free(content);

    char status[320];
    if (duplicate_count > 0) {
        g_snprintf(status, sizeof(status),
                   "Načteno %d položek z %s. Přeskočeno %d duplicitních názvů.",
                   count, app->items_path, duplicate_count);
    } else {
        g_snprintf(status, sizeof(status),
                   "Načteno %d položek z %s",
                   count, app->items_path);
    }
    gtk_label_set_text(GTK_LABEL(app->status_label), status);
    app->dirty = FALSE;
    return TRUE;
}

static gboolean save_items(WarehouseApp *app)
{
    GString *out = g_string_new("ACRI1\n# AdavaCashReg Items\n# Název|Cena\n");
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->store), &iter);

    while (valid) {
        gchar *name = NULL;
        double price = 0.0;
        gtk_tree_model_get(GTK_TREE_MODEL(app->store), &iter,
                           COL_NAME, &name,
                           COL_PRICE, &price,
                           -1);

        /* Znak | je oddělovač formátu, proto ho v názvu nahradíme lomítkem. */
        for (char *p = name; p && *p; p++)
            if (*p == '|') *p = '/';

        char price_text[64];
        format_price_cz(price, price_text, sizeof(price_text));
        g_string_append_printf(out, "%s|%s\n", name ? name : "", price_text);
        g_free(name);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(app->store), &iter);
    }

    GError *error = NULL;
    gboolean ok = g_file_set_contents(app->items_path, out->str, -1, &error);
    g_string_free(out, TRUE);

    if (!ok) {
        char msg[512];
        g_snprintf(msg, sizeof(msg), "Nelze uložit: %s",
                   error ? error->message : "neznámá chyba");
        gtk_label_set_text(GTK_LABEL(app->status_label), msg);
        g_clear_error(&error);
        return FALSE;
    }

    char msg[256];
    g_snprintf(msg, sizeof(msg), "Uloženo do %s", app->items_path);
    gtk_label_set_text(GTK_LABEL(app->status_label), msg);
    app->dirty = FALSE;
    return TRUE;
}

static gboolean read_form(WarehouseApp *app, const char **name, double *price)
{
    *name = gtk_entry_get_text(GTK_ENTRY(app->name_entry));
    const char *price_text = gtk_entry_get_text(GTK_ENTRY(app->price_entry));

    if (!*name || !**name) {
        gtk_label_set_text(GTK_LABEL(app->status_label), "Zadej název položky.");
        return FALSE;
    }

    if (!parse_price_cz(price_text, price)) {
        gtk_label_set_text(
            GTK_LABEL(app->status_label),
            "Cena není platná. Použij českou desetinnou čárku, např. 19,90."
        );
        return FALSE;
    }
    return TRUE;
}

static void clear_form(WarehouseApp *app)
{
    gtk_entry_set_text(GTK_ENTRY(app->name_entry), "");
    gtk_entry_set_text(GTK_ENTRY(app->price_entry), "");
}

static void add_item(GtkButton *button, gpointer data)
{
    (void)button;
    WarehouseApp *app = data;
    const char *name;
    double price;
    if (!read_form(app, &name, &price)) return;

    if (name_exists_except(app, name, NULL)) {
        gtk_label_set_text(
            GTK_LABEL(app->status_label),
            "Položka s tímto názvem už existuje. Každá položka musí mít originální název."
        );
        return;
    }

    GtkTreeIter iter;
    gtk_list_store_append(app->store, &iter);
    gtk_list_store_set(app->store, &iter,
                       COL_NAME, name,
                       COL_PRICE, price,
                       -1);
    clear_form(app);
    app->dirty = TRUE;
    gtk_label_set_text(GTK_LABEL(app->status_label), "Položka přidána. Nezapomeň uložit.");
}

static void update_item(GtkButton *button, gpointer data)
{
    (void)button;
    WarehouseApp *app = data;
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->treeview));
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(selection, NULL, &iter)) {
        gtk_label_set_text(GTK_LABEL(app->status_label), "Vyber položku, kterou chceš upravit.");
        return;
    }

    const char *name;
    double price;
    if (!read_form(app, &name, &price)) return;

    if (name_exists_except(app, name, &iter)) {
        gtk_label_set_text(
            GTK_LABEL(app->status_label),
            "Položka s tímto názvem už existuje. Zvol jiné originální jméno."
        );
        return;
    }

    gtk_list_store_set(app->store, &iter,
                       COL_NAME, name,
                       COL_PRICE, price,
                       -1);
    app->dirty = TRUE;
    gtk_label_set_text(GTK_LABEL(app->status_label), "Položka upravena. Nezapomeň uložit.");
}

static void delete_item(GtkButton *button, gpointer data)
{
    (void)button;
    WarehouseApp *app = data;
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->treeview));
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(selection, NULL, &iter)) {
        gtk_label_set_text(GTK_LABEL(app->status_label), "Vyber položku, kterou chceš smazat.");
        return;
    }
    gtk_list_store_remove(app->store, &iter);
    clear_form(app);
    app->dirty = TRUE;
    gtk_label_set_text(GTK_LABEL(app->status_label), "Položka odstraněna. Nezapomeň uložit.");
}

static void selection_changed(GtkTreeSelection *selection, gpointer data)
{
    WarehouseApp *app = data;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(selection, NULL, &iter)) return;

    gchar *name = NULL;
    double price = 0.0;
    gtk_tree_model_get(GTK_TREE_MODEL(app->store), &iter,
                       COL_NAME, &name,
                       COL_PRICE, &price,
                       -1);

    char price_text[64];
    format_price_cz(price, price_text, sizeof(price_text));
    gtk_entry_set_text(GTK_ENTRY(app->name_entry), name ? name : "");
    gtk_entry_set_text(GTK_ENTRY(app->price_entry), price_text);
    g_free(name);
}

static void save_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    save_items((WarehouseApp *)data);
}

static void reload_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    load_items((WarehouseApp *)data);
}

static gboolean on_window_delete(GtkWidget *widget,
                                 GdkEvent *event,
                                 gpointer data)
{
    (void)widget;
    (void)event;

    WarehouseApp *app = data;

    if (!app->dirty)
        return FALSE;

    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_WARNING,
        GTK_BUTTONS_NONE,
        "Máš neuložené změny."
    );

    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(dialog),
        "Položky v items.acri byly změněny. Opravdu je nechceš před ukončením uložit?"
    );

    gtk_dialog_add_buttons(
        GTK_DIALOG(dialog),
        "_Zrušit", GTK_RESPONSE_CANCEL,
        "_Neuložit", GTK_RESPONSE_REJECT,
        "_Uložit", GTK_RESPONSE_ACCEPT,
        NULL
    );

    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);

    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (response == GTK_RESPONSE_ACCEPT) {
        if (save_items(app))
            return FALSE;
        return TRUE;
    }

    if (response == GTK_RESPONSE_REJECT)
        return FALSE;

    return TRUE;
}

static void activate(GtkApplication *application, gpointer user_data)
{
    (void)user_data;
    WarehouseApp *app = g_new0(WarehouseApp, 1);
    app->items_path = find_items_file();

    app->window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(app->window), "AdavaWarehouseManagment");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 760, 520);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(root), 12);
    gtk_container_add(GTK_CONTAINER(app->window), root);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title),
                         "<span size=\"20000\" weight=\"bold\">Správa skladu systému AdavaCashReg</span>\n"
                         "<span size=\"11000\">Editor AdavaCashReg Items (.acri)</span>");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(root), title, FALSE, FALSE, 0);

    app->store = gtk_list_store_new(NUM_COLS, G_TYPE_STRING, G_TYPE_DOUBLE);
    app->treeview = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->store));
    add_columns(GTK_TREE_VIEW(app->treeview));

    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->treeview));
    g_signal_connect(selection, "changed", G_CALLBACK(selection_changed), app);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scroll), app->treeview);
    gtk_box_pack_start(GTK_BOX(root), scroll, TRUE, TRUE, 0);

    GtkWidget *form = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(form), 10);
    gtk_grid_set_row_spacing(GTK_GRID(form), 8);
    gtk_box_pack_start(GTK_BOX(root), form, FALSE, FALSE, 0);

    GtkWidget *name_label = gtk_label_new("Název:");
    GtkWidget *price_label = gtk_label_new("Cena (Kč):");
    app->name_entry = gtk_entry_new();
    app->price_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->name_entry), "např. Rohlík");
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->price_entry), "např. 3,50");
    gtk_widget_set_hexpand(app->name_entry, TRUE);

    gtk_grid_attach(GTK_GRID(form), name_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(form), app->name_entry, 1, 0, 3, 1);
    gtk_grid_attach(GTK_GRID(form), price_label, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(form), app->price_entry, 1, 1, 1, 1);

    GtkWidget *add_btn = gtk_button_new_with_label("Přidat");
    GtkWidget *update_btn = gtk_button_new_with_label("Upravit vybranou");
    GtkWidget *delete_btn = gtk_button_new_with_label("Smazat vybranou");
    gtk_grid_attach(GTK_GRID(form), add_btn, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(form), update_btn, 1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(form), delete_btn, 2, 2, 1, 1);

    GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(root), bottom, FALSE, FALSE, 0);

    GtkWidget *reload_btn = gtk_button_new_with_label("Znovu načíst");
    GtkWidget *save_btn = gtk_button_new_with_label("ULOŽIT items.acri");
    gtk_widget_set_size_request(save_btn, 180, 44);
    gtk_box_pack_start(GTK_BOX(bottom), reload_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bottom), save_btn, FALSE, FALSE, 0);

    app->status_label = gtk_label_new("Připraveno.");
    gtk_widget_set_halign(app->status_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(root), app->status_label, FALSE, FALSE, 0);

    g_signal_connect(add_btn, "clicked", G_CALLBACK(add_item), app);
    g_signal_connect(update_btn, "clicked", G_CALLBACK(update_item), app);
    g_signal_connect(delete_btn, "clicked", G_CALLBACK(delete_item), app);
    g_signal_connect(save_btn, "clicked", G_CALLBACK(save_clicked), app);
    g_signal_connect(reload_btn, "clicked", G_CALLBACK(reload_clicked), app);
    g_signal_connect(app->window, "delete-event", G_CALLBACK(on_window_delete), app);

    load_items(app);
    gtk_widget_show_all(app->window);
}

int main(int argc, char **argv)
{
    GtkApplication *application = gtk_application_new(
        "cz.adava.warehousemanagment", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    return status;
}
