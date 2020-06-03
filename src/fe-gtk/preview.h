#ifndef HEXCHAT_PREVIEW_H
#define HEXCHAT_PREVIEW_H

#include <curl/curl.h>
#include <gtk/gtk.h>

#include "xtext.h"

typedef struct _GtkXText GtkXText;

struct preview_data {
	GtkXText *xtext;
	GtkWidget *window;
	GtkWidget *image;
	GdkPixbufLoader *pixbuf_loader;
	CURL *curl;
	struct curl_slist *headers;
	int code;
	gboolean complete;
	gboolean finalized;
	gdouble pointer_start_x;
	gdouble pointer_start_y;
};

void
preview_thread(gpointer data, gpointer user_data);

void
preview_start(GtkXText *xtext, unsigned char *word);

void
preview_end(GtkXText *xtext);

#endif
