/****************************************************************/
/* AdavaCashReg cash register program for Linux                 */
/* (c) 2026 Adam Cír (Adava), Adava Software, Adava Development */
/* This program is under GPL v3.0 license                       */
/* Github: https://github.com/adamcir/AdavaCashReg	         	*/
/* E-mail: adam.cir@adava.cz									*/
/****************************************************************/

#include <gtk/gtk.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

typedef struct {
    GtkWidget *window;
    GtkWidget *treeview;
    GtkListStore *store;
    GtkWidget *total_label;
    GtkWidget *status_label;

    double total;
} App;

enum {
    COL_NAME,
    COL_PRICE,
    COL_QUANTITY,
    COL_TOTAL,
    NUM_COLS
};

typedef struct {
    char *name;
    double price;
} Product;

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

static gboolean load_products(const char *path, GError **error)
{
    gchar *content = NULL;
    gsize length = 0;

    if (!g_file_get_contents(path, &content, &length, error))
        return FALSE;

    gchar **lines = g_strsplit(content, "\n", -1);

    for (int i = 0; lines[i] != NULL; i++) {
        gchar *line = g_strstrip(lines[i]);
        if (*line == '\0' || *line == '#')
            continue;
        if (g_strcmp0(line, "ACRI1") == 0)
            continue;

        gchar **parts = g_strsplit(line, "|", 2);
        if (!parts[0] || !parts[1]) {
            g_strfreev(parts);
            continue;
        }

        gchar *name = g_strstrip(parts[0]);
        gchar *price_text = g_strstrip(parts[1]);
        gchar *end = NULL;
        double price = g_ascii_strtod(price_text, &end);

        if (*name != '\0' && end != price_text && price >= 0.0) {
            products = g_realloc(products, sizeof(Product) * (product_count + 1));
            products[product_count].name = g_strdup(name);
            products[product_count].price = price;
            product_count++;
        }

        g_strfreev(parts);
    }

    g_strfreev(lines);
    g_free(content);
    return TRUE;
}

static void update_total(App *app)
{
    char text[128];

    snprintf(
        text,
        sizeof(text),
        "<span size=\"30000\" weight=\"bold\">%.2f Kč</span>",
        app->total
    );

    gtk_label_set_markup(GTK_LABEL(app->total_label), text);
}

static gboolean find_product_in_cart(App *app,
                                     const char *name,
                                     GtkTreeIter *found_iter)
{
    GtkTreeIter iter;
    gboolean valid;

    valid = gtk_tree_model_get_iter_first(
        GTK_TREE_MODEL(app->store),
        &iter
    );

    while (valid) {
        gchar *item_name = NULL;

        gtk_tree_model_get(
            GTK_TREE_MODEL(app->store),
            &iter,
            COL_NAME, &item_name,
            -1
        );

        gboolean same = g_strcmp0(item_name, name) == 0;

        g_free(item_name);

        if (same) {
            *found_iter = iter;
            return TRUE;
        }

        valid = gtk_tree_model_iter_next(
            GTK_TREE_MODEL(app->store),
            &iter
        );
    }

    return FALSE;
}

static void add_product(GtkWidget *button, gpointer data)
{
    App *app = data;

    int index =
        GPOINTER_TO_INT(
            g_object_get_data(G_OBJECT(button), "product-index")
        );

    Product *product = &products[index];

    GtkTreeIter iter;

    if (find_product_in_cart(app, product->name, &iter)) {

        int quantity;
        double current_total;

        gtk_tree_model_get(
            GTK_TREE_MODEL(app->store),
            &iter,
            COL_QUANTITY, &quantity,
            COL_TOTAL, &current_total,
            -1
        );

        quantity++;
        current_total = quantity * product->price;

        gtk_list_store_set(
            app->store,
            &iter,
            COL_QUANTITY, quantity,
            COL_TOTAL, current_total,
            -1
        );

    } else {

        gtk_list_store_append(app->store, &iter);

        gtk_list_store_set(
            app->store,
            &iter,
            COL_NAME, product->name,
            COL_PRICE, product->price,
            COL_QUANTITY, 1,
            COL_TOTAL, product->price,
            -1
        );
    }

    app->total += product->price;

    update_total(app);

    char status[128];
    snprintf(
        status,
        sizeof(status),
        "Přidáno: %s — %.2f Kč",
        product->name,
        product->price
    );

    gtk_label_set_text(GTK_LABEL(app->status_label), status);
}

static void remove_selected(GtkWidget *button, gpointer data)
{
    App *app = data;

    GtkTreeSelection *selection =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(app->treeview));

    GtkTreeIter iter;

    if (!gtk_tree_selection_get_selected(
            selection,
            NULL,
            &iter
        )) {

        gtk_label_set_text(
            GTK_LABEL(app->status_label),
            "Nejdříve vyber položku v seznamu."
        );

        return;
    }

    int quantity;
    double price;

    gtk_tree_model_get(
        GTK_TREE_MODEL(app->store),
        &iter,
        COL_PRICE, &price,
        COL_QUANTITY, &quantity,
        -1
    );

    if (quantity > 1) {

        quantity--;

        gtk_list_store_set(
            app->store,
            &iter,
            COL_QUANTITY, quantity,
            COL_TOTAL, price * quantity,
            -1
        );

    } else {

        gtk_list_store_remove(app->store, &iter);
    }

    app->total -= price;

    if (app->total < 0.001)
        app->total = 0.0;

    update_total(app);

    gtk_label_set_text(
        GTK_LABEL(app->status_label),
        "Položka odebrána."
    );
}

static void clear_cart(GtkWidget *button, gpointer data)
{
    App *app = data;

    gtk_list_store_clear(app->store);

    app->total = 0.0;

    update_total(app);

    gtk_label_set_text(
        GTK_LABEL(app->status_label),
        "Nákup byl zrušen."
    );
}

static void show_receipt(App *app,
                         double paid,
                         double change)
{
    GtkWidget *dialog =
        gtk_message_dialog_new(
            GTK_WINDOW(app->window),
            GTK_DIALOG_MODAL,
            GTK_MESSAGE_INFO,
            GTK_BUTTONS_OK,
            "Platba dokončena"
        );

    char message[512];

    snprintf(
        message,
        sizeof(message),

        "============================\n"
        "       Adava Store :)\n"
        "============================\n\n"
        "Celkem:       %.2f Kč\n"
        "Zaplaceno:    %.2f Kč\n"
        "Vrátit:       %.2f Kč\n\n"
        "Děkujeme za nákup!\n"
        "============================",

        app->total,
        paid,
        change
    );

    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(dialog),
        "%s",
        message
    );

    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static void payment_changed(GtkEditable *editable, gpointer data)
{
    GtkWidget *change_label =
        g_object_get_data(G_OBJECT(editable), "change-label");

    double total =
        *(double *)g_object_get_data(
            G_OBJECT(editable),
            "total-pointer"
        );

    const char *text =
        gtk_entry_get_text(GTK_ENTRY(editable));

    char *end;

    double paid = g_ascii_strtod(text, &end);

    char output[128];

    if (text[0] == '\0') {

        snprintf(
            output,
            sizeof(output),
            "<span size=\"18000\">Vrátit: --</span>"
        );

    } else if (paid < total) {

        snprintf(
            output,
            sizeof(output),
            "<span size=\"18000\" foreground=\"red\">"
            "Chybí: %.2f Kč"
            "</span>",
            total - paid
        );

    } else {

        snprintf(
            output,
            sizeof(output),
            "<span size=\"18000\" foreground=\"green\" weight=\"bold\">"
            "Vrátit: %.2f Kč"
            "</span>",
            paid - total
        );
    }

    gtk_label_set_markup(
        GTK_LABEL(change_label),
        output
    );
}

static void pay(GtkWidget *button, gpointer data)
{
    App *app = data;

    if (app->total <= 0.0) {

        gtk_label_set_text(
            GTK_LABEL(app->status_label),
            "Košík je prázdný."
        );

        return;
    }

    GtkWidget *dialog =
        gtk_dialog_new_with_buttons(
            "Platba",
            GTK_WINDOW(app->window),
            GTK_DIALOG_MODAL |
            GTK_DIALOG_DESTROY_WITH_PARENT,

            "_Zrušit",
            GTK_RESPONSE_CANCEL,

            "_Zaplatit",
            GTK_RESPONSE_OK,

            NULL
        );

    gtk_window_set_default_size(
        GTK_WINDOW(dialog),
        420,
        300
    );

    GtkWidget *content =
        gtk_dialog_get_content_area(GTK_DIALOG(dialog));

    GtkWidget *box =
        gtk_box_new(GTK_ORIENTATION_VERTICAL, 15);

    gtk_container_set_border_width(
        GTK_CONTAINER(box),
        20
    );

    gtk_box_pack_start(
        GTK_BOX(content),
        box,
        TRUE,
        TRUE,
        0
    );

    GtkWidget *title =
        gtk_label_new(NULL);

    char total_text[128];

    snprintf(
        total_text,
        sizeof(total_text),
        "<span size=\"15000\">K zaplacení</span>\n"
        "<span size=\"30000\" weight=\"bold\">%.2f Kč</span>",
        app->total
    );

    gtk_label_set_markup(
        GTK_LABEL(title),
        total_text
    );

    gtk_box_pack_start(
        GTK_BOX(box),
        title,
        FALSE,
        FALSE,
        0
    );

    GtkWidget *paid_label =
        gtk_label_new("Zákazník zaplatil:");

    gtk_widget_set_halign(
        paid_label,
        GTK_ALIGN_START
    );

    gtk_box_pack_start(
        GTK_BOX(box),
        paid_label,
        FALSE,
        FALSE,
        0
    );

    GtkWidget *entry =
        gtk_entry_new();

    gtk_entry_set_placeholder_text(
        GTK_ENTRY(entry),
        "např. 500"
    );

    gtk_entry_set_input_purpose(
        GTK_ENTRY(entry),
        GTK_INPUT_PURPOSE_NUMBER
    );

    gtk_box_pack_start(
        GTK_BOX(box),
        entry,
        FALSE,
        FALSE,
        0
    );

    GtkWidget *change_label =
        gtk_label_new(NULL);

    gtk_label_set_markup(
        GTK_LABEL(change_label),
        "<span size=\"18000\">Vrátit: --</span>"
    );

    gtk_box_pack_start(
        GTK_BOX(box),
        change_label,
        FALSE,
        FALSE,
        10
    );

    g_object_set_data(
        G_OBJECT(entry),
        "change-label",
        change_label
    );

    g_object_set_data(
        G_OBJECT(entry),
        "total-pointer",
        &app->total
    );

    g_signal_connect(
        entry,
        "changed",
        G_CALLBACK(payment_changed),
        NULL
    );

    gtk_widget_show_all(dialog);

    while (1) {

        int response =
            gtk_dialog_run(GTK_DIALOG(dialog));

        if (response != GTK_RESPONSE_OK)
            break;

        const char *text =
            gtk_entry_get_text(GTK_ENTRY(entry));

        double paid =
            g_ascii_strtod(text, NULL);

        if (paid < app->total) {

            GtkWidget *error =
                gtk_message_dialog_new(
                    GTK_WINDOW(dialog),
                    GTK_DIALOG_MODAL,
                    GTK_MESSAGE_ERROR,
                    GTK_BUTTONS_OK,
                    "Zákazník zaplatil málo."
                );

            gtk_message_dialog_format_secondary_text(
                GTK_MESSAGE_DIALOG(error),
                "Chybí %.2f Kč.",
                app->total - paid
            );

            gtk_dialog_run(GTK_DIALOG(error));
            gtk_widget_destroy(error);

            continue;
        }

        double change =
            paid - app->total;

        gtk_widget_hide(dialog);

        show_receipt(
            app,
            paid,
            change
        );

        gtk_list_store_clear(app->store);

        app->total = 0.0;

        update_total(app);

        gtk_label_set_text(
            GTK_LABEL(app->status_label),
            "Platba dokončena. Připraven nový nákup."
        );

        break;
    }

    gtk_widget_destroy(dialog);
}

static void price_cell_func(GtkTreeViewColumn *column,
                            GtkCellRenderer *renderer,
                            GtkTreeModel *model,
                            GtkTreeIter *iter,
                            gpointer data)
{
    int index =
        GPOINTER_TO_INT(data);

    double value;

    gtk_tree_model_get(
        model,
        iter,
        index,
        &value,
        -1
    );

    char text[64];

    snprintf(
        text,
        sizeof(text),
        "%.2f Kč",
        value
    );

    g_object_set(
        renderer,
        "text",
        text,
        NULL
    );
}

static void add_text_column(GtkTreeView *view,
                            const char *title,
                            int index)
{
    GtkCellRenderer *renderer =
        gtk_cell_renderer_text_new();

    GtkTreeViewColumn *column =
        gtk_tree_view_column_new_with_attributes(
            title,
            renderer,
            "text",
            index,
            NULL
        );

    gtk_tree_view_append_column(
        view,
        column
    );
}

static void add_price_column(GtkTreeView *view,
                             const char *title,
                             int index)
{
    GtkCellRenderer *renderer =
        gtk_cell_renderer_text_new();

    GtkTreeViewColumn *column =
        gtk_tree_view_column_new();

    gtk_tree_view_column_set_title(
        column,
        title
    );

    gtk_tree_view_column_pack_start(
        column,
        renderer,
        TRUE
    );

    gtk_tree_view_column_set_cell_data_func(
        column,
        renderer,
        price_cell_func,
        GINT_TO_POINTER(index),
        NULL
    );

    gtk_tree_view_append_column(
        view,
        column
    );
}

static void activate(GtkApplication *application,
                     gpointer user_data)
{
    App *app = g_malloc0(sizeof(App));

    char *items_path = find_items_file();
    GError *load_error = NULL;
    if (!load_products(items_path, &load_error)) {
        g_printerr("Nelze načíst %s: %s\n", items_path,
                   load_error ? load_error->message : "neznámá chyba");
        g_clear_error(&load_error);
    }
    g_free(items_path);

    app->window =
        gtk_application_window_new(application);

    gtk_window_set_title(
        GTK_WINDOW(app->window),
        "AdavaCashReg"
    );

    gtk_window_set_default_size(
        GTK_WINDOW(app->window),
        1100,
        700
    );

    GtkWidget *main_box =
        gtk_box_new(
            GTK_ORIENTATION_HORIZONTAL,
            15
        );

    gtk_container_set_border_width(
        GTK_CONTAINER(main_box),
        15
    );

    gtk_container_add(
        GTK_CONTAINER(app->window),
        main_box
    );

    GtkWidget *left =
        gtk_box_new(
            GTK_ORIENTATION_VERTICAL,
            10
        );

    gtk_widget_set_size_request(
        left,
        430,
        -1
    );

    gtk_box_pack_start(
        GTK_BOX(main_box),
        left,
        FALSE,
        FALSE,
        0
    );

    GtkWidget *products_title =
        gtk_label_new(NULL);

    gtk_label_set_markup(
        GTK_LABEL(products_title),
        "<span size=\"20000\" weight=\"bold\">Produkty</span>"
    );

    gtk_widget_set_halign(
        products_title,
        GTK_ALIGN_START
    );

    gtk_box_pack_start(
        GTK_BOX(left),
        products_title,
        FALSE,
        FALSE,
        5
    );

    GtkWidget *grid =
        gtk_grid_new();

    gtk_grid_set_row_spacing(
        GTK_GRID(grid),
        10
    );

    gtk_grid_set_column_spacing(
        GTK_GRID(grid),
        10
    );

    gtk_box_pack_start(
        GTK_BOX(left),
        grid,
        TRUE,
        TRUE,
        0
    );

    for (int i = 0; i < product_count; i++) {

        char text[128];

        snprintf(
            text,
            sizeof(text),
            "%s\n%.2f Kč",
            products[i].name,
            products[i].price
        );

        GtkWidget *button =
            gtk_button_new_with_label(text);

        gtk_widget_set_size_request(
            button,
            130,
            85
        );

        g_object_set_data(
            G_OBJECT(button),
            "product-index",
            GINT_TO_POINTER(i)
        );

        g_signal_connect(
            button,
            "clicked",
            G_CALLBACK(add_product),
            app
        );

        int column = i % 3;
        int row = i / 3;

        gtk_grid_attach(
            GTK_GRID(grid),
            button,
            column,
            row,
            1,
            1
        );
    }

    GtkWidget *right =
        gtk_box_new(
            GTK_ORIENTATION_VERTICAL,
            12
        );

    gtk_box_pack_start(
        GTK_BOX(main_box),
        right,
        TRUE,
        TRUE,
        0
    );

    GtkWidget *total_title =
        gtk_label_new("CELKEM");

    gtk_widget_set_halign(
        total_title,
        GTK_ALIGN_END
    );

    gtk_box_pack_start(
        GTK_BOX(right),
        total_title,
        FALSE,
        FALSE,
        0
    );

    app->total_label =
        gtk_label_new(NULL);

    gtk_widget_set_halign(
        app->total_label,
        GTK_ALIGN_END
    );

    gtk_box_pack_start(
        GTK_BOX(right),
        app->total_label,
        FALSE,
        FALSE,
        0
    );

    update_total(app);

    GtkWidget *separator =
        gtk_separator_new(
            GTK_ORIENTATION_HORIZONTAL
        );

    gtk_box_pack_start(
        GTK_BOX(right),
        separator,
        FALSE,
        FALSE,
        5
    );

    GtkWidget *cart_title =
        gtk_label_new(NULL);

    gtk_label_set_markup(
        GTK_LABEL(cart_title),
        "<span size=\"16000\" weight=\"bold\">Nákup</span>"
    );

    gtk_widget_set_halign(
        cart_title,
        GTK_ALIGN_START
    );

    gtk_box_pack_start(
        GTK_BOX(right),
        cart_title,
        FALSE,
        FALSE,
        0
    );

    app->store =
        gtk_list_store_new(
            NUM_COLS,
            G_TYPE_STRING,
            G_TYPE_DOUBLE,
            G_TYPE_INT,
            G_TYPE_DOUBLE
        );

    app->treeview =
        gtk_tree_view_new_with_model(
            GTK_TREE_MODEL(app->store)
        );

    add_text_column(
        GTK_TREE_VIEW(app->treeview),
        "Položka",
        COL_NAME
    );

    add_price_column(
        GTK_TREE_VIEW(app->treeview),
        "Cena",
        COL_PRICE
    );

    add_text_column(
        GTK_TREE_VIEW(app->treeview),
        "Ks",
        COL_QUANTITY
    );

    add_price_column(
        GTK_TREE_VIEW(app->treeview),
        "Celkem",
        COL_TOTAL
    );

    GtkWidget *scroll =
        gtk_scrolled_window_new(
            NULL,
            NULL
        );

    gtk_container_add(
        GTK_CONTAINER(scroll),
        app->treeview
    );

    gtk_box_pack_start(
        GTK_BOX(right),
        scroll,
        TRUE,
        TRUE,
        0
    );

    app->status_label =
        gtk_label_new(
            "Pokladna připravena."
        );

    gtk_widget_set_halign(
        app->status_label,
        GTK_ALIGN_START
    );

    gtk_box_pack_start(
        GTK_BOX(right),
        app->status_label,
        FALSE,
        FALSE,
        5
    );

    GtkWidget *buttons =
        gtk_box_new(
            GTK_ORIENTATION_HORIZONTAL,
            10
        );

    gtk_box_pack_start(
        GTK_BOX(right),
        buttons,
        FALSE,
        FALSE,
        0
    );

    GtkWidget *remove =
        gtk_button_new_with_label(
            "− Odebrat"
        );

    GtkWidget *clear =
        gtk_button_new_with_label(
            "Zrušit nákup"
        );

    GtkWidget *pay_button =
        gtk_button_new_with_label(
            "ZAPLATIT"
        );

    gtk_widget_set_size_request(
        pay_button,
        180,
        60
    );

    gtk_box_pack_start(
        GTK_BOX(buttons),
        remove,
        FALSE,
        FALSE,
        0
    );

    gtk_box_pack_start(
        GTK_BOX(buttons),
        clear,
        FALSE,
        FALSE,
        0
    );

    gtk_box_pack_end(
        GTK_BOX(buttons),
        pay_button,
        FALSE,
        FALSE,
        0
    );

    g_signal_connect(
        remove,
        "clicked",
        G_CALLBACK(remove_selected),
        app
    );

    g_signal_connect(
        clear,
        "clicked",
        G_CALLBACK(clear_cart),
        app
    );

    g_signal_connect(
        pay_button,
        "clicked",
        G_CALLBACK(pay),
        app
    );

    gtk_widget_show_all(app->window);
}

int main(int argc, char **argv)
{
    GtkApplication *application =
        gtk_application_new(
            "cz.adava.pos",
            G_APPLICATION_DEFAULT_FLAGS
        );

    g_signal_connect(
        application,
        "activate",
        G_CALLBACK(activate),
        NULL
    );

    int status =
        g_application_run(
            G_APPLICATION(application),
            argc,
            argv
        );

    g_object_unref(application);

    return status;
}
