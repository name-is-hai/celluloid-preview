/*
 * Copyright (c) 2016-2023 gnome-mpv
 *
 * This file is part of Celluloid.
 *
 * Celluloid is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Celluloid is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Celluloid.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <glib.h>
#include <glib-object.h>
#include <gtk/gtk.h>

#include "celluloid-seek-bar.h"
#include "celluloid-time-label.h"
#include "celluloid-common.h"

enum
{
	PROP_0,
	PROP_CHAPTER_LIST,
	PROP_DURATION,
	PROP_PAUSE,
	PROP_ENABLED,
	PROP_SHOW_LABEL,
	PROP_POPOVER_Y_OFFSET,
	PROP_POPOVER_VISIBLE,
	N_PROPERTIES
};

struct _CelluloidSeekBar
{
	GtkBox parent_instance;
	GtkWidget *seek_bar;
	GtkWidget *label;
	GtkWidget *popover;
	GtkWidget *popover_label;
	GtkWidget *preview_picture;
	gchar *preview_file;
	guint preview_timeout_id;
	guint preview_generation;
	gint preview_second;
	GSubprocess *preview_process;
	GHashTable *preview_cache;
	GQueue preview_lru;
	GPtrArray *chapter_list;
	gdouble pos;
	gdouble duration;
	gboolean pause;
	gboolean enabled;
	gboolean show_label;
	gint popover_y_offset;
	gboolean popover_visible;
	guint popover_timeout_id;
};

struct _CelluloidSeekBarClass
{
	GtkBoxClass parent_class;
};

static void
dispose(GObject *object);

static void
set_property(	GObject *object,
		guint property_id,
		const GValue *value,
		GParamSpec *pspec );

static void
get_property(	GObject *object,
		guint property_id,
		GValue *value,
		GParamSpec *pspec );

static void
change_value_handler(	GtkWidget *widget,
			GtkScrollType scroll,
			gdouble value,
			gpointer data );

static gboolean
enter_handler(	GtkEventControllerMotion *controller,
		gdouble x,
		gdouble y,
		gpointer data );

static gboolean
leave_handler(GtkEventControllerMotion *widget, gpointer data);

static gboolean
motion_handler(	GtkEventControllerMotion *controller,
		gdouble x,
		gdouble y,
		gpointer data );

static void
update_chapter_list(CelluloidSeekBar *bar);

static void
update_label(CelluloidSeekBar *bar);

static gboolean
update_popover_visibility(CelluloidSeekBar *bar);

G_DEFINE_TYPE(CelluloidSeekBar, celluloid_seek_bar, GTK_TYPE_BOX)

/* Local-file thumbnails never seek the playback mpv instance. All decoder I/O
 * is asynchronous; a weak widget reference and generation discard late frames. */
typedef struct
{
	GWeakRef bar;
	GSubprocess *process;
	guint generation;
	guint deadline;
	gint second;
} PreviewRequest;

static void
cancel_preview(CelluloidSeekBar *bar)
{
	bar->preview_generation++;
	if(bar->preview_timeout_id)
	{
		g_source_remove(bar->preview_timeout_id);
		bar->preview_timeout_id = 0;
	}
	if(bar->preview_process)
	{
		g_subprocess_force_exit(bar->preview_process);
		g_clear_object(&bar->preview_process);
	}
}

static gboolean
preview_deadline(gpointer data)
{
	PreviewRequest *request = data;
	request->deadline = 0;
	g_subprocess_force_exit(request->process);
	return G_SOURCE_REMOVE;
}

static gboolean start_preview(gpointer data);

static void
preview_ready(GObject *source, GAsyncResult *result, gpointer data)
{
	PreviewRequest *request = data;
	GBytes *bytes = NULL;
	GError *error = NULL;
	gboolean ok = g_subprocess_communicate_finish(
		G_SUBPROCESS(source), result, &bytes, NULL, &error);
	CelluloidSeekBar *bar = g_weak_ref_get(&request->bar);
	if(request->deadline) g_source_remove(request->deadline);

	if(bar && bar->preview_process == request->process)
	{
		g_clear_object(&bar->preview_process);
		if(bar->popover_visible && request->generation == bar->preview_generation)
		{
			GdkTexture *texture = NULL;
			if(ok && g_subprocess_get_successful(request->process)
				&& bytes && g_bytes_get_size(bytes) == 240 * 135 * 4)
			{
				texture = gdk_memory_texture_new(240, 135,
					GDK_MEMORY_R8G8B8A8, bytes, 240 * 4);
				gpointer key = GINT_TO_POINTER(request->second);
				if(g_hash_table_contains(bar->preview_cache, key))
					g_queue_remove(&bar->preview_lru, key);
				else if(g_hash_table_size(bar->preview_cache) >= 64)
					g_hash_table_remove(bar->preview_cache,
						g_queue_pop_head(&bar->preview_lru));
				g_hash_table_replace(bar->preview_cache, key, texture);
				g_queue_push_tail(&bar->preview_lru, key);
			}
			GdkTexture *wanted = g_hash_table_lookup(bar->preview_cache,
				GINT_TO_POINTER(bar->preview_second));
			if(wanted || texture)
			{
				gtk_picture_set_paintable(GTK_PICTURE(bar->preview_picture),
					GDK_PAINTABLE(wanted ? wanted : texture));
				gtk_widget_set_visible(bar->preview_picture, TRUE);
			}
			/* Finish useful work while the pointer moves, then serve only the
			 * most recent position. Never spawn a queue of obsolete requests. */
			if(!wanted && request->second != bar->preview_second
				&& !bar->preview_timeout_id)
				bar->preview_timeout_id = g_idle_add(start_preview, bar);
		}
	}
	g_clear_error(&error);
	g_clear_pointer(&bytes, g_bytes_unref);
	g_clear_object(&bar);
	g_weak_ref_clear(&request->bar);
	g_object_unref(request->process);
	g_free(request);
}

static gboolean
start_preview(gpointer data)
{
	CelluloidSeekBar *bar = data;
	bar->preview_timeout_id = 0;
	if(!bar->preview_file || !bar->popover_visible || bar->preview_process)
		return G_SOURCE_REMOVE;
	if(g_hash_table_contains(bar->preview_cache, GINT_TO_POINTER(bar->preview_second)))
		return G_SOURCE_REMOVE;

	gchar timestamp[32];
	g_ascii_formatd(timestamp, sizeof(timestamp), "%.3f", bar->preview_second);
	GError *error = NULL;
	bar->preview_process = g_subprocess_new(
		G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
		&error, "ffmpeg", "-v", "error", "-nostdin", "-threads", "1",
		"-filter_threads", "1", "-protocol_whitelist", "file,pipe",
		"-ss", timestamp, "-noaccurate_seek", "-skip_frame", "nokey", "-i", bar->preview_file,
		"-map", "0:v:0", "-frames:v", "1", "-vf",
		"scale=240:135:force_original_aspect_ratio=decrease,format=rgba,pad=240:135:(ow-iw)/2:(oh-ih)/2:color=black",
		"-an", "-sn", "-dn", "-pix_fmt", "rgba", "-threads", "1",
		"-f", "rawvideo", "pipe:1", NULL);
	if(!bar->preview_process)
	{
		g_debug("Seek preview unavailable: %s", error->message);
		g_clear_error(&error);
		return G_SOURCE_REMOVE;
	}
	PreviewRequest *request = g_new0(PreviewRequest, 1);
	g_weak_ref_init(&request->bar, bar);
	request->process = g_object_ref(bar->preview_process);
	request->generation = bar->preview_generation;
	request->second = bar->preview_second;
	request->deadline = g_timeout_add_seconds(8, preview_deadline, request);
	g_subprocess_communicate_async(bar->preview_process, NULL, NULL,
		preview_ready, request);
	return G_SOURCE_REMOVE;
}

static void
request_preview(CelluloidSeekBar *bar, gdouble time)
{
	if(!bar->preview_file || !bar->popover_visible || bar->duration <= 0) return;
	gint second = (gint)CLAMP(time, 0, MAX(0, bar->duration - 0.1));
	if(second == bar->preview_second) return;
	bar->preview_second = second;
	GdkTexture *cached = g_hash_table_lookup(bar->preview_cache,
		GINT_TO_POINTER(second));
	if(cached)
	{
		gpointer key = GINT_TO_POINTER(second);
		g_queue_remove(&bar->preview_lru, key);
		g_queue_push_tail(&bar->preview_lru, key);
		gtk_picture_set_paintable(GTK_PICTURE(bar->preview_picture), GDK_PAINTABLE(cached));
		gtk_widget_set_visible(bar->preview_picture, TRUE);
	}
	else if(!bar->preview_process && !bar->preview_timeout_id)
		bar->preview_timeout_id = g_timeout_add(40, start_preview, bar);
}

void
celluloid_seek_bar_set_preview_file(CelluloidSeekBar *bar, const gchar *path)
{
	GFile *file = path && *path ? g_file_new_for_commandline_arg(path) : NULL;
	gchar *local = file && g_file_is_native(file) ? g_file_get_path(file) : NULL;
	g_clear_object(&file);
	if(g_strcmp0(local, bar->preview_file) == 0)
	{
		g_free(local);
		return;
	}
	cancel_preview(bar);
	g_free(bar->preview_file);
	bar->preview_file = local;
	bar->preview_second = -1;
	g_hash_table_remove_all(bar->preview_cache);
	g_queue_clear(&bar->preview_lru);
	gtk_picture_set_paintable(GTK_PICTURE(bar->preview_picture), NULL);
	gtk_widget_set_visible(bar->preview_picture, FALSE);
}


static void
dispose(GObject *object)
{
	CelluloidSeekBar *bar = CELLULOID_SEEK_BAR(object);
	cancel_preview(bar);
	if(bar->popover_timeout_id)
	{
		g_source_remove(bar->popover_timeout_id);
		bar->popover_timeout_id = 0;
	}
	g_clear_pointer(&bar->preview_file, g_free);
	g_clear_pointer(&bar->preview_cache, g_hash_table_unref);
	g_queue_clear(&bar->preview_lru);
	g_clear_object(&bar->popover);

	G_OBJECT_CLASS(celluloid_seek_bar_parent_class)->dispose(object);
}

static void
set_property(	GObject *object,
		guint property_id,
		const GValue *value,
		GParamSpec *pspec )
{
	CelluloidSeekBar *self = CELLULOID_SEEK_BAR(object);

	switch(property_id)
	{
		case PROP_CHAPTER_LIST:
		self->chapter_list = g_value_get_pointer(value);
		update_chapter_list(self);
		break;

		case PROP_DURATION:
		self->duration = g_value_get_double(value);
		update_label(self);
		break;

		case PROP_PAUSE:
		self->pause = g_value_get_boolean(value);
		update_label(self);
		break;

		case PROP_ENABLED:
		self->enabled = g_value_get_boolean(value);
		update_label(self);
		break;

		case PROP_SHOW_LABEL:
		self->show_label = g_value_get_boolean(value);
		gtk_widget_set_visible(self->label, self->show_label);
		break;

		case PROP_POPOVER_Y_OFFSET:
		self->popover_y_offset = g_value_get_int(value);
		break;

		default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
		break;
	}
}

static void
get_property(	GObject *object,
		guint property_id,
		GValue *value,
		GParamSpec *pspec )
{
	CelluloidSeekBar *self = CELLULOID_SEEK_BAR(object);

	switch(property_id)
	{
		case PROP_CHAPTER_LIST:
		g_value_set_pointer(value, self->chapter_list);
		break;

		case PROP_DURATION:
		g_value_set_double(value, self->duration);
		break;

		case PROP_PAUSE:
		g_value_set_boolean(value, self->pause);
		break;

		case PROP_ENABLED:
		g_value_set_boolean(value, self->enabled);
		break;

		case PROP_SHOW_LABEL:
		g_value_set_boolean(value, self->show_label);
		break;

		case PROP_POPOVER_Y_OFFSET:
		g_value_set_int(value, self->popover_y_offset);
		break;

		case PROP_POPOVER_VISIBLE:
		g_value_set_boolean(value, self->popover_visible);
		break;

		default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
		break;
	}
}

static void
change_value_handler(	GtkWidget *widget,
			GtkScrollType scroll,
			gdouble value,
			gpointer data )
{
	CelluloidSeekBar *bar = data;

	if(bar->duration > 0)
	{
		g_object_set(	bar->label,
				"time", (gint)bar->pos,
				"duration", (gint)bar->duration,
				NULL );
		g_signal_emit_by_name(data, "seek", value);
	}
}

static gboolean
enter_handler(	GtkEventControllerMotion *controller,
		gdouble x,
		gdouble y,
		gpointer data )
{
	CelluloidSeekBar *bar = data;

	if(bar->popover_timeout_id > 0)
	{
		g_source_remove(bar->popover_timeout_id);
	}

	/* Don't show popover if duration is unknown */
	if(bar->duration > 0)
	{
		bar->popover_visible = TRUE;
		g_object_notify(G_OBJECT(bar), "popover-visible");

		bar->popover_timeout_id =
			g_timeout_add(	100,
					(GSourceFunc)update_popover_visibility,
					bar );
	}

	return FALSE;
}

static gboolean
leave_handler(GtkEventControllerMotion *widget, gpointer data)
{
	CelluloidSeekBar *bar = data;

	if(bar->popover_timeout_id > 0)
	{
		g_source_remove(bar->popover_timeout_id);
	}

	bar->popover_visible = FALSE;
	cancel_preview(bar);
	bar->preview_second = -1;
	g_object_notify(G_OBJECT(bar), "popover-visible");

	bar->popover_timeout_id =
		g_timeout_add(100, (GSourceFunc)update_popover_visibility, bar);

	return FALSE;
}

static gboolean
motion_handler(	GtkEventControllerMotion *controller,
		gdouble x,
		gdouble y,
		gpointer data )
{
	CelluloidSeekBar *bar = data;
	GtkRange *range = GTK_RANGE(bar->seek_bar);
	GdkRectangle popover_rect = {0};
	GdkRectangle range_rect = {0};

	gtk_range_get_range_rect(range, &range_rect);

	// TODO: Figure out how to get the actual margin
	const gint margin = 1;
	const gint trough_start = range_rect.x + margin;
	const gint trough_length = range_rect.width - 2 * margin;

	x = CLAMP(x, trough_start, trough_start + trough_length);

	graphene_point_t popover_offset =
		{0};
	const gboolean popover_offset_computed =
		gtk_widget_compute_point
		(	GTK_WIDGET(range),
			GTK_WIDGET(bar),
			&GRAPHENE_POINT_INIT((gfloat)x, 0),
			&popover_offset );

	if(popover_offset_computed)
	{
		popover_rect.x = (gint)popover_offset.x;
	}
	else
	{
		// This seems to work for Adwaita
		popover_rect.x = (gint)x + 13;
		g_warning("Failed to determine position for timestamp popover");
	}

	popover_rect.y += bar->popover_y_offset;

	gtk_popover_set_pointing_to(GTK_POPOVER(bar->popover), &popover_rect);

	GtkAdjustment *adjustment = gtk_range_get_adjustment(range);
	const gdouble lower = gtk_adjustment_get_lower(adjustment);
	const gdouble upper = gtk_adjustment_get_upper(adjustment);
	const gdouble page_size = gtk_adjustment_get_page_size(adjustment);

	const gdouble progress =
		trough_length > 0 ? (x - trough_start) / (gdouble)trough_length : 0;
	const gdouble time =
		lower + progress * (upper - lower - page_size);
	request_preview(bar, time);
	gchar *time_text =
		format_time((gint)time, bar->duration >= 3600);

	GPtrArray *chapter_list = bar->chapter_list;
	const gchar *title_text = NULL;

	if (chapter_list)
	{
		for(guint i = 0; !title_text && i < chapter_list->len; i++)
		{
			CelluloidChapter *chapter = g_ptr_array_index(chapter_list, i);

			if(chapter->time > time)
			{
				// At this point, we should be looking at the chapter
				// after the one we need. To get to the right chapter,
				// subtract the index by one. If we're already at the
				// first chapter, just pick that one.
				chapter = g_ptr_array_index(chapter_list, MAX(1, i) - 1);
				title_text = chapter->title;
			}
			else if(i == chapter_list->len - 1)
			{
				// The time should be beyond the last chapter's in this
				// case, meaning the last chapter (the current index)
				// is the one we need.
				chapter = g_ptr_array_index(chapter_list, i);
				title_text = chapter->title;
			}
		}
	}

	GtkLabel *label = GTK_LABEL(bar->popover_label);

	if(title_text)
	{
		gchar *markup =
			g_markup_printf_escaped
			(	"<b>%s</b>\r<span>%s</span>",
				title_text,
				time_text );

		gtk_label_set_markup(label, markup);
		gtk_label_set_justify(label, GTK_JUSTIFY_CENTER);

		g_free(markup);
	}
	else
	{
		gtk_label_set_text(label, time_text);
	}

	g_free(time_text);

	return FALSE;
}

static void
update_chapter_list(CelluloidSeekBar *bar)
{
	GtkScale *seek_bar = GTK_SCALE(bar->seek_bar);
	GPtrArray *chapter_list = bar->chapter_list;

	gtk_scale_clear_marks(seek_bar);

	for(guint i = 0; i < chapter_list->len; i++)
	{
		CelluloidChapter *chapter = g_ptr_array_index(chapter_list, i);

		gtk_scale_add_mark(seek_bar, chapter->time, GTK_POS_TOP, NULL);
	}
}

static void
update_label(CelluloidSeekBar *bar)
{
	// When disabled, the effective duration is zero regardless of what the
	// value of the duration property is.
	const gdouble duration = bar->enabled ? bar->duration : 0;

	g_object_set(bar->label, "duration", (gint)duration, NULL);
	gtk_range_set_range(GTK_RANGE(bar->seek_bar), 0, duration);
}

static gboolean
update_popover_visibility(CelluloidSeekBar *bar)
{
	if(bar->popover_visible)
	{
		gtk_popover_popup(GTK_POPOVER(bar->popover));
	}
	else
	{
		gtk_popover_popdown(GTK_POPOVER(bar->popover));
	}

	bar->popover_timeout_id = 0;

	return G_SOURCE_REMOVE;
}

static void
celluloid_seek_bar_class_init(CelluloidSeekBarClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
	GParamSpec *pspec = NULL;

	object_class->dispose = dispose;
	object_class->set_property = set_property;
	object_class->get_property = get_property;

	gtk_widget_class_set_css_name(widget_class, "celluloid-seek-bar");

	pspec = g_param_spec_pointer
		(	"chapter-list",
			"Chapter list",
			"The list of chapters in the current file",
			G_PARAM_READWRITE );
	g_object_class_install_property(object_class, PROP_CHAPTER_LIST, pspec);

	pspec = g_param_spec_double
		(	"duration",
			"Duration",
			"The duration the seek bar is using",
			0.0,
			G_MAXDOUBLE,
			0.0,
			G_PARAM_READWRITE );
	g_object_class_install_property(object_class, PROP_DURATION, pspec);

	pspec = g_param_spec_boolean
		(	"pause",
			"Pause",
			"Whether there is a file playing",
			TRUE,
			G_PARAM_READWRITE );
	g_object_class_install_property(object_class, PROP_PAUSE, pspec);

	pspec = g_param_spec_boolean
		(	"enabled",
			"Enabled",
			"Whether or not the widget is enabled",
			TRUE,
			G_PARAM_READWRITE );
	g_object_class_install_property(object_class, PROP_ENABLED, pspec);

	pspec = g_param_spec_boolean
		(	"show-label",
			"Shoe label",
			"Whether or not the timestamp/duration label is shown",
			TRUE,
			G_PARAM_READWRITE );
	g_object_class_install_property(object_class, PROP_SHOW_LABEL, pspec);

	pspec = g_param_spec_int
		(	"popover-y-offset",
			"Popover y-offset",
			"The offset to add to the y-position of the popover",
			G_MININT,
			G_MAXINT,
			0,
			G_PARAM_READWRITE );
	g_object_class_install_property(object_class, PROP_POPOVER_Y_OFFSET, pspec);

	pspec = g_param_spec_boolean
		(	"popover-visible",
			"Popover visible",
			"Whether the timestamp popover is visible",
			FALSE,
			G_PARAM_READABLE );
	g_object_class_install_property(object_class, PROP_POPOVER_VISIBLE, pspec);

	g_signal_new(	"seek",
			G_TYPE_FROM_CLASS(klass),
			G_SIGNAL_RUN_FIRST,
			0,
			NULL,
			NULL,
			g_cclosure_marshal_VOID__DOUBLE,
			G_TYPE_NONE,
			1,
			G_TYPE_DOUBLE );
}

static void
celluloid_seek_bar_init(CelluloidSeekBar *bar)
{
	bar->seek_bar = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, NULL);
	gtk_widget_set_direction(bar->seek_bar, GTK_TEXT_DIR_LTR);
	bar->label = celluloid_time_label_new();
	bar->popover = g_object_ref_sink(gtk_popover_new());
	bar->popover_label = gtk_label_new(NULL);
	bar->preview_picture = gtk_picture_new();
	gtk_widget_add_css_class(bar->popover, "seek-preview");
	gtk_popover_set_has_arrow(GTK_POPOVER(bar->popover), FALSE);
	bar->preview_second = -1;
	bar->preview_cache = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_object_unref);
	gtk_picture_set_content_fit(GTK_PICTURE(bar->preview_picture), GTK_CONTENT_FIT_CONTAIN);
	gtk_widget_set_size_request(bar->preview_picture, 240, 135);
	gtk_widget_set_visible(bar->preview_picture, FALSE);
	bar->chapter_list = NULL;
	bar->duration = 0;
	bar->pause = TRUE;
	bar->enabled = TRUE;
	bar->show_label = TRUE;
	bar->popover_y_offset = 0;
	bar->pos = 0;
	bar->popover_visible = FALSE;
	bar->popover_timeout_id = 0;

	g_object_set(	bar->label,
			"time", (gint)bar->pos,
			"duration", (gint)bar->duration,
			NULL );
	gtk_popover_set_autohide(GTK_POPOVER(bar->popover), FALSE);

	gtk_scale_set_draw_value(GTK_SCALE(bar->seek_bar), FALSE);
	gtk_range_set_increments(GTK_RANGE(bar->seek_bar), 10, 10);
	gtk_widget_set_can_focus(bar->seek_bar, FALSE);

	gtk_popover_set_position(GTK_POPOVER(bar->popover), GTK_POS_TOP);
	GtkWidget *preview_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_box_append(GTK_BOX(preview_box), bar->preview_picture);
	gtk_box_append(GTK_BOX(preview_box), bar->popover_label);
	gtk_popover_set_child(GTK_POPOVER(bar->popover), preview_box);
	gtk_widget_set_visible(bar->popover_label, TRUE);

	g_signal_connect(	bar->seek_bar,
				"change-value",
				G_CALLBACK(change_value_handler),
				bar );

	GtkEventController *motion_controller =
		gtk_event_controller_motion_new();

	gtk_widget_add_controller(GTK_WIDGET(bar->seek_bar), motion_controller);

	g_signal_connect(	motion_controller,
				"enter",
				G_CALLBACK(enter_handler),
				bar );
	g_signal_connect(	motion_controller,
				"leave",
				G_CALLBACK(leave_handler),
				bar );
	g_signal_connect(	motion_controller,
				"motion",
				G_CALLBACK(motion_handler),
				bar );

	gtk_widget_set_hexpand(bar->seek_bar, TRUE);

	gtk_box_append(GTK_BOX(bar), bar->seek_bar);
	gtk_box_append(GTK_BOX(bar), bar->label);
	gtk_box_append(GTK_BOX(bar), bar->popover);
}

GtkWidget *
celluloid_seek_bar_new()
{
	return GTK_WIDGET(g_object_new(celluloid_seek_bar_get_type(), NULL));
}

void
celluloid_seek_bar_set_duration(CelluloidSeekBar *bar, gdouble duration)
{
	g_object_set(bar, "duration", duration, NULL);
}

void
celluloid_seek_bar_set_pos(CelluloidSeekBar *bar, gdouble pos)
{
	gdouble old_pos = bar->pos;

	bar->pos = pos;

	gtk_range_set_value(GTK_RANGE(bar->seek_bar), pos);

	if((gint)old_pos != (gint)pos)
	{
		g_object_set(bar->label, "time", (gint)pos, NULL);
	}
}
