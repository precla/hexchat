#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/hexchat.h"
#include "fe-gtk.h"
#include "preview.h"

static gboolean
preview_window_mouse(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
	if(event->type != GDK_MOTION_NOTIFY) {
		return FALSE;
	}
	struct preview_data *preview_data = user_data;
	if(preview_data->finalized) {
		return FALSE;
	}
	
	gdouble x = event->x;
	gdouble y = event->y;
	
	if(preview_data->pointer_start_x == 0 && preview_data->pointer_start_y == 0) {
		preview_data->pointer_start_x = x;
		preview_data->pointer_start_y = y;
		return FALSE;
	}
	
	gdouble distance = (preview_data->pointer_start_x - x) * (preview_data->pointer_start_x - x)
					   + (preview_data->pointer_start_y - y) * (preview_data->pointer_start_y - y);
	if(distance < 30 * 30) {
		return FALSE;
	}
	
	preview_end(preview_data->xtext);
	return FALSE;
}

static void
progressive_size_callback (GdkPixbufLoader *loader, gint width, gint height, gpointer data)
{
	struct preview_data *preview_data = data;
	if(preview_data->finalized) {
		return;
	}
	
	GtkWidget *base_window = gtk_widget_get_toplevel(GTK_WIDGET(preview_data->xtext));
	gint win_width, win_height;
	gtk_window_get_size(GTK_WINDOW(base_window), &win_width, &win_height);
	
	gfloat ratio = max((gfloat)width / (gfloat)win_width, (gfloat)height / (gfloat)win_height);
	if(ratio > 0.8) { // fit into 80% of the base window dimensions
		width = (gint)((gfloat)width / ratio * 0.8);
		height = (gint)((gfloat)height / ratio * 0.8);
	}
	gdk_pixbuf_loader_set_size(loader, width, height);
}

static void
progressive_prepared_callback (GdkPixbufLoader *loader, gpointer data)
{
	struct preview_data *preview_data = data;
	if(preview_data->finalized) {
		return;
	}
	
	GdkPixbuf *pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);

	// TODO gdk_pixbuf_fill (pixbuf, 0x000000ff); // TODO transparent
	
	GtkWidget *image = gtk_image_new_from_pixbuf(pixbuf);
	preview_data->image = image;
	
	preview_data->window = gtk_window_new(GTK_WINDOW_POPUP);
	GtkWidget *base_window = gtk_widget_get_toplevel(GTK_WIDGET(preview_data->xtext));
	gtk_window_set_transient_for(GTK_WINDOW(preview_data->window), GTK_WINDOW(base_window));
	gtk_window_set_keep_above(GTK_WINDOW(preview_data->window), TRUE);
	gtk_window_set_decorated(GTK_WINDOW(preview_data->window), FALSE);
	gtk_window_set_deletable(GTK_WINDOW(preview_data->window), FALSE);
	gtk_window_set_resizable(GTK_WINDOW(preview_data->window), FALSE);
	gtk_window_set_skip_taskbar_hint(GTK_WINDOW(preview_data->window), FALSE);
	gtk_window_set_skip_pager_hint(GTK_WINDOW(preview_data->window), FALSE);
	gtk_window_set_accept_focus(GTK_WINDOW(preview_data->window), FALSE);
	gtk_window_set_focus_on_map(GTK_WINDOW(preview_data->window), FALSE);
	gtk_window_set_position(GTK_WINDOW(preview_data->window), GTK_WIN_POS_CENTER_ON_PARENT);
	gtk_container_add(GTK_CONTAINER(preview_data->window), preview_data->image);
	gtk_widget_set_events(preview_data->window, GDK_POINTER_MOTION_MASK);
	g_signal_connect(preview_data->window, "motion-notify-event", G_CALLBACK(preview_window_mouse), preview_data);
	gtk_widget_show_all(preview_data->window);
}

static void
progressive_updated_callback (GdkPixbufLoader *loader, gint x, gint y, gint width, gint height, gpointer data)
{
	struct preview_data *preview_data = data;
	if(preview_data->finalized) {
		return;
	}
	
	gtk_widget_queue_draw_area(GTK_WIDGET(preview_data->image), x, y, width, height);
}

struct write_image_data {
	gboolean *finalized;
	GdkPixbufLoader *pixbuf_loader;
	void *buffer;
	size_t size;
	int success;
	GCond cond;
	GMutex mutex;
};

static gboolean
preview_write_image(gpointer user_data)
{
	struct write_image_data *write_image_data = user_data;
	g_mutex_lock(&write_image_data->mutex);
	if(*write_image_data->finalized) {
		write_image_data->success = 0;
	} else {
		GError *error = NULL;
		write_image_data->success = gdk_pixbuf_loader_write(write_image_data->pixbuf_loader, write_image_data->buffer, write_image_data->size, &error);
		if(!write_image_data->success) {
			g_warning("%s", error->message);
			g_error_free(error);
		}
	}
	g_cond_signal(&write_image_data->cond);
	g_mutex_unlock(&write_image_data->mutex);
	return FALSE;
}

static size_t
preview_write_data(void *buffer, size_t size, size_t nmemb, void *userp)
{
	size_t realsize = size * nmemb;
	struct preview_data *preview_data = userp;
	if(preview_data->finalized) {
		return 0;
	}
	struct write_image_data write_image_data = {
		.finalized = &preview_data->finalized,
		.pixbuf_loader = preview_data->pixbuf_loader,
		.buffer = buffer,
		.size = realsize,
		.success = -1,
	};
	g_mutex_init(&write_image_data.mutex);
	g_cond_init(&write_image_data.cond);
	
	gdk_threads_add_idle(preview_write_image, &write_image_data);
	
	g_mutex_lock(&write_image_data.mutex);
	while(write_image_data.success == -1) {
		g_cond_wait(&write_image_data.cond, &write_image_data.mutex);
	}
	g_mutex_unlock(&write_image_data.mutex);
	g_mutex_clear(&write_image_data.mutex);
	g_cond_clear(&write_image_data.cond);
	if(!write_image_data.success) {
		return 0;
	}
	return realsize;
}

static gboolean
preview_curl_complete(gpointer user_data)
{
	struct preview_data *preview_data = user_data;
	if(preview_data->finalized) {
		free(preview_data);
		return FALSE;
	}
	if(preview_data->code != 0) {
		preview_end(preview_data->xtext);
		free(preview_data);
		return FALSE;
	}
	gboolean success = gdk_pixbuf_loader_close(preview_data->pixbuf_loader, NULL);
	g_object_unref(preview_data->pixbuf_loader);
	preview_data->pixbuf_loader = NULL;
	if(!success || preview_data->window == NULL) {
		preview_end(preview_data->xtext);
		free(preview_data);
		return FALSE;
	}
	preview_data->complete = TRUE;
	return FALSE;
}

void
preview_thread(gpointer data, gpointer user_data)
{
	struct preview_data *preview_data = data;
	if(preview_data->finalized) {
		curl_slist_free_all(preview_data->headers);
		curl_easy_cleanup(preview_data->curl);
		free(preview_data);
		return;
	}
	
	int code = curl_easy_perform(preview_data->curl);
	curl_slist_free_all(preview_data->headers);
	curl_easy_cleanup(preview_data->curl);
	preview_data->code = code;
	
	gdk_threads_add_idle(preview_curl_complete, preview_data);
}

void
preview_start(GtkXText *xtext, unsigned char *word)
{
	char *ext = strrchr(word, '.');
	if(ext == NULL) {
		return;
	}
	if(g_ascii_strcasecmp(ext, ".jpg") && g_ascii_strcasecmp(ext, ".png")) {
		return;
	}
	
	if(xtext->preview_data != NULL) {
		preview_end(xtext);
	}
	
	struct preview_data *preview_data = calloc(1, sizeof(struct preview_data));
	xtext->preview_data = preview_data;
	preview_data->xtext = xtext;
	
	GdkPixbufLoader *pixbuf_loader = gdk_pixbuf_loader_new();
	preview_data->pixbuf_loader = pixbuf_loader;
	g_signal_connect (pixbuf_loader, "size-prepared", G_CALLBACK(progressive_size_callback), preview_data);
	g_signal_connect (pixbuf_loader, "area-prepared", G_CALLBACK(progressive_prepared_callback), preview_data);
	g_signal_connect (pixbuf_loader, "area-updated", G_CALLBACK(progressive_updated_callback), preview_data);
	
	CURL *curl = curl_easy_init();
	preview_data->curl = curl;
	curl_easy_setopt(curl, CURLOPT_URL, word);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, preview_write_data);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, preview_data);
	preview_data->headers = curl_slist_append(preview_data->headers, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:77.0) Gecko/20100101 Firefox/77.0");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, preview_data->headers);
	
	g_thread_pool_push(xtext->preview_thread_pool, preview_data, NULL);
}

void
preview_end(GtkXText *xtext)
{
	if(xtext->preview_data == NULL) {
		return;
	}
	xtext->preview_data->finalized = TRUE;
	if(xtext->preview_data->window != NULL) {
		gtk_widget_destroy(xtext->preview_data->window);
	}
	if(xtext->preview_data->complete) {
		free(xtext->preview_data);
	} else {
		if(xtext->preview_data->pixbuf_loader != NULL) {
			gdk_pixbuf_loader_close(xtext->preview_data->pixbuf_loader, NULL);
			g_object_unref(xtext->preview_data->pixbuf_loader);
		}
	}
	xtext->preview_data = NULL;
}
