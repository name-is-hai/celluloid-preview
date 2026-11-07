/* GPL-3.0-or-later. Exercise the production widget and real FFmpeg decoder. */
#include "../src/celluloid-seek-bar.c"

static guint seek_count;
static void on_seek(CelluloidSeekBar *bar, gdouble value, gpointer data)
{
	seek_count++;
}

static void drain(guint milliseconds)
{
	gint64 until = g_get_monotonic_time() + milliseconds * 1000;
	while(g_get_monotonic_time() < until)
	{
		while(g_main_context_iteration(NULL, FALSE));
		g_usleep(1000);
	}
}

static GdkTexture *wait_preview(CelluloidSeekBar *bar)
{
	for(guint i = 0; i < 10000; i += 10)
	{
		drain(10);
		GdkPaintable *paintable = gtk_picture_get_paintable(GTK_PICTURE(bar->preview_picture));
		if(paintable && paintable == g_hash_table_lookup(bar->preview_cache,
			GINT_TO_POINTER(bar->preview_second))) return GDK_TEXTURE(paintable);
	}
	g_error("Thumbnail did not arrive");
	return NULL;
}

static void test_preview(void)
{
	const gchar *path = g_getenv("CELLULOID_PREVIEW_TEST_VIDEO");
	g_assert_nonnull(path);
	CelluloidSeekBar *bar = CELLULOID_SEEK_BAR(g_object_ref_sink(celluloid_seek_bar_new()));
	celluloid_seek_bar_set_duration(bar, 4);
	celluloid_seek_bar_set_pos(bar, 1);
	g_signal_connect(bar, "seek", G_CALLBACK(on_seek), NULL);
	celluloid_seek_bar_set_preview_file(bar, path);
	bar->popover_visible = TRUE;
	request_preview(bar, 0.5);
	GdkTexture *first = g_object_ref(wait_preview(bar));
	g_assert_cmpint(gdk_texture_get_width(first), <=, 240);
	g_assert_cmpint(gdk_texture_get_height(first), <=, 135);
	g_assert_cmpfloat(bar->pos, ==, 1);
	g_assert_cmpuint(seek_count, ==, 0);

	request_preview(bar, 3);
	g_assert_true(gtk_picture_get_paintable(GTK_PICTURE(bar->preview_picture)) == GDK_PAINTABLE(first));
	GdkTexture *second = g_object_ref(wait_preview(bar));
	gsize stride = (gsize)gdk_texture_get_width(first) * 4;
	gsize size = stride * (gsize)gdk_texture_get_height(first);
	guchar *pixels1 = g_malloc(size), *pixels2 = g_malloc(size);
	gdk_texture_download(first, pixels1, stride);
	gdk_texture_download(second, pixels2, stride);
	g_assert_cmpint(memcmp(pixels1, pixels2, size), !=, 0);
	g_free(pixels1); g_free(pixels2);
	request_preview(bar, 0.5);
	g_assert_true(gtk_picture_get_paintable(GTK_PICTURE(bar->preview_picture)) == GDK_PAINTABLE(first));
	g_assert_null(bar->preview_process);
	g_assert_cmpuint(bar->preview_timeout_id, ==, 0);

	/* Rapid motion coalesces into one request. Leaving cancels pending work. */
	request_preview(bar, 1);
	request_preview(bar, 2);
	g_assert_null(bar->preview_process);
	leave_handler(NULL, bar);
	g_assert_cmpuint(bar->preview_timeout_id, ==, 0);
	drain(200);
	g_assert_null(bar->preview_process);

	/* Changing file clears frames; remote streams retain timestamp-only hover. */
	celluloid_seek_bar_set_preview_file(bar, "https://example.invalid/video.mp4");
	g_assert_null(bar->preview_file);
	g_assert_cmpuint(g_hash_table_size(bar->preview_cache), ==, 0);
	g_assert_null(gtk_picture_get_paintable(GTK_PICTURE(bar->preview_picture)));
	celluloid_seek_bar_set_preview_file(bar, "/no/such/video.mp4");
	bar->popover_visible = TRUE;
	request_preview(bar, 1);
	drain(1000);
	g_assert_null(bar->preview_process);
	g_assert_null(gtk_picture_get_paintable(GTK_PICTURE(bar->preview_picture)));

	/* Destroy with a live request: late output must never touch the widget. */
	celluloid_seek_bar_set_preview_file(bar, path);
	bar->popover_visible = TRUE;
	request_preview(bar, 2);
	drain(160);
	g_object_unref(bar);
	drain(1000);
	g_object_unref(first); g_object_unref(second);
}

static void test_motion(void)
{
	CelluloidSeekBar *bar = CELLULOID_SEEK_BAR(g_object_ref_sink(celluloid_seek_bar_new()));
	celluloid_seek_bar_set_duration(bar, 4);
	celluloid_seek_bar_set_preview_file(bar, g_getenv("CELLULOID_PREVIEW_TEST_VIDEO"));
	bar->popover_visible = TRUE;
	request_preview(bar, 0);
	for(guint i = 0; i < 1000 && !bar->preview_process; i++) drain(1);
	g_assert_nonnull(bar->preview_process);
	GSubprocess *active = bar->preview_process;
	request_preview(bar, 1);
	request_preview(bar, 3);
	g_assert_true(bar->preview_process == active);
	wait_preview(bar);
	g_assert_true(g_hash_table_contains(bar->preview_cache, GINT_TO_POINTER(0)));
	g_assert_true(g_hash_table_contains(bar->preview_cache, GINT_TO_POINTER(3)));
	g_assert_false(g_hash_table_contains(bar->preview_cache, GINT_TO_POINTER(1)));
	g_assert_null(bar->preview_process);
	g_object_unref(bar);
	drain(50);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	gtk_init();
	g_test_add_func("/seek-preview/local-and-lifecycle", test_preview);
	g_test_add_func("/seek-preview/motion-retains-decoder", test_motion);
	return g_test_run();
}
