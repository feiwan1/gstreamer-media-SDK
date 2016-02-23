#include "gstmfxvideobufferpool.h"
#include "gstmfxvideomemory.h"
#include "gstmfxdisplay.h"

GST_DEBUG_CATEGORY_STATIC(gst_debug_mfxvideopool);
#define GST_CAT_DEFAULT gst_debug_mfxvideopool

G_DEFINE_TYPE(GstMfxVideoBufferPool,
	gst_mfx_video_buffer_pool, GST_TYPE_BUFFER_POOL);

struct _GstMfxVideoBufferPoolPrivate
{
	GstVideoInfo video_info[2];
	guint video_info_index;
	GstAllocator *allocator;
	GstVideoInfo alloc_info;
	GstMfxContextAllocator *ctx;
	guint has_video_meta : 1;
	guint has_video_alignment : 1;
};

#define GST_MFX_VIDEO_BUFFER_POOL_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_MFX_TYPE_VIDEO_BUFFER_POOL, \
	GstMfxVideoBufferPoolPrivate))

static void
gst_mfx_video_buffer_pool_finalize(GObject * object)
{
	GstMfxVideoBufferPoolPrivate *const priv =
		GST_MFX_VIDEO_BUFFER_POOL(object)->priv;

	//gst_mfx_context_replace(&priv->ctx, NULL);
	priv->ctx = NULL;
	g_clear_object(&priv->allocator);

	G_OBJECT_CLASS(gst_mfx_video_buffer_pool_parent_class)->finalize(object);
}

static void
fill_video_alignment(GstMfxVideoBufferPool * pool, GstVideoAlignment * align)
{
	GstVideoInfo *const vip = &pool->priv->alloc_info;
	guint i;

	gst_video_alignment_reset(align);
	for (i = 0; i < GST_VIDEO_INFO_N_PLANES(vip); i++)
		align->stride_align[i] =
		(1U << g_bit_nth_lsf(GST_VIDEO_INFO_PLANE_STRIDE(vip, i), 0)) - 1;
}

static const gchar **
gst_mfx_video_buffer_pool_get_options(GstBufferPool * pool)
{
	static const gchar *g_options[] = {
		GST_BUFFER_POOL_OPTION_VIDEO_META,
		GST_BUFFER_POOL_OPTION_MFX_VIDEO_META,
		GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT,
		NULL,
	};

	return g_options;
}

static gboolean
gst_mfx_video_buffer_pool_set_config(GstBufferPool * pool,
	GstStructure * config)
{
	GstMfxVideoBufferPoolPrivate *const priv =
		GST_MFX_VIDEO_BUFFER_POOL(pool)->priv;
	GstCaps *caps = NULL;
	GstVideoInfo *const cur_vip = &priv->video_info[priv->video_info_index];
	GstVideoInfo *const new_vip = &priv->video_info[!priv->video_info_index];
	GstVideoAlignment align;
	GstAllocator *allocator;
	gboolean changed_caps;

	if (!gst_buffer_pool_config_get_params(config, &caps, NULL, NULL, NULL))
		goto error_invalid_config;
	if (!caps || !gst_video_info_from_caps(new_vip, caps))
		goto error_no_caps;

	changed_caps = !priv->allocator ||
		GST_VIDEO_INFO_FORMAT(cur_vip) != GST_VIDEO_INFO_FORMAT(new_vip) ||
		GST_VIDEO_INFO_WIDTH(cur_vip) != GST_VIDEO_INFO_WIDTH(new_vip) ||
		GST_VIDEO_INFO_HEIGHT(cur_vip) != GST_VIDEO_INFO_HEIGHT(new_vip);

	if (changed_caps) {
		const GstVideoInfo *alloc_vip;

		allocator = gst_mfx_video_allocator_new(priv->ctx, new_vip);

		if (!allocator)
			goto error_create_allocator;
		gst_object_replace((GstObject **)& priv->allocator,
			GST_OBJECT_CAST(allocator));
		gst_object_unref(allocator);
		priv->video_info_index ^= 1;

		alloc_vip = gst_allocator_get_mfx_video_info(allocator, NULL);
		if (!alloc_vip)
			goto error_create_allocator_info;
		priv->alloc_info = *alloc_vip;
	}

	if (!gst_buffer_pool_config_has_option(config,
            GST_BUFFER_POOL_OPTION_MFX_VIDEO_META))
		goto error_no_mfx_video_meta_option;

	priv->has_video_meta = gst_buffer_pool_config_has_option(config,
		GST_BUFFER_POOL_OPTION_VIDEO_META);

	priv->has_video_alignment = gst_buffer_pool_config_has_option(config,
		GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT);
	if (priv->has_video_alignment) {
		fill_video_alignment(GST_MFX_VIDEO_BUFFER_POOL(pool), &align);
		gst_buffer_pool_config_set_video_alignment(config, &align);
	}

	return
		GST_BUFFER_POOL_CLASS
		(gst_mfx_video_buffer_pool_parent_class)->set_config(pool, config);

	/* ERRORS */
error_invalid_config:
	{
		GST_ERROR("invalid config");
		return FALSE;
	}
error_no_caps:
	{
		GST_ERROR("no valid caps in config");
		return FALSE;
	}
error_create_allocator:
	{
		GST_ERROR("failed to create GstMfxVideoAllocator object");
		return FALSE;
	}
error_create_allocator_info:
	{
		GST_ERROR("failed to create GstMfxVideoAllocator `video-info'");
		return FALSE;
	}
error_no_mfx_video_meta_option:
	{
		GST_ERROR("no GstMfxVideoMeta option");
		return FALSE;
	}
}

static GstFlowReturn
gst_mfx_video_buffer_pool_alloc_buffer(GstBufferPool * pool,
	GstBuffer ** out_buffer_ptr, GstBufferPoolAcquireParams * params)
{
	GstMfxVideoBufferPoolPrivate *const priv =
		GST_MFX_VIDEO_BUFFER_POOL(pool)->priv;
	GstMfxVideoMeta *meta;
	GstMemory *mem;
	GstBuffer *buffer;

	if (!priv->allocator)
		goto error_no_allocator;

    meta = gst_mfx_video_meta_new();
    if (!meta)
        goto error_create_meta;

    buffer = gst_buffer_new ();
    if (buffer)
        gst_buffer_set_mfx_video_meta (buffer, meta);

	if (!buffer)
		goto error_create_buffer;

	mem = gst_mfx_video_memory_new(priv->allocator, meta);
	if (!mem)
		goto error_create_memory;

	gst_mfx_video_meta_replace(&meta, NULL);
	gst_buffer_append_memory(buffer, mem);

	if (priv->has_video_meta) {
		GstVideoInfo *const vip = &priv->alloc_info;
		GstVideoMeta *vmeta;

		vmeta = gst_buffer_add_video_meta_full(buffer, 0,
			GST_VIDEO_INFO_FORMAT(vip), GST_VIDEO_INFO_WIDTH(vip),
			GST_VIDEO_INFO_HEIGHT(vip), GST_VIDEO_INFO_N_PLANES(vip),
			&GST_VIDEO_INFO_PLANE_OFFSET(vip, 0),
			&GST_VIDEO_INFO_PLANE_STRIDE(vip, 0));

		if (GST_MFX_IS_VIDEO_MEMORY(mem)) {
			vmeta->map = gst_video_meta_map_mfx_surface;
			vmeta->unmap = gst_video_meta_unmap_mfx_surface;
		}
	}

	*out_buffer_ptr = buffer;
	return GST_FLOW_OK;

	/* ERRORS */
error_no_allocator:
	{
		GST_ERROR("no GstAllocator in buffer pool");
		return GST_FLOW_ERROR;
	}
error_create_meta:
	{
		GST_ERROR("failed to allocate mfx video meta");
		return GST_FLOW_ERROR;
	}
error_create_buffer:
	{
		GST_ERROR("failed to create video buffer");
		gst_mfx_video_meta_unref(meta);
		return GST_FLOW_ERROR;
	}
error_create_memory:
	{
		GST_ERROR("failed to create video memory");
		gst_buffer_unref(buffer);
		gst_mfx_video_meta_unref(meta);
		return GST_FLOW_ERROR;
	}
}

static void
gst_mfx_video_buffer_pool_reset_buffer(GstBufferPool * pool,
	GstBuffer * buffer)
{
	GstMemory *const mem = gst_buffer_peek_memory(buffer, 0);

	/* Release the underlying surface proxy */
	if (GST_MFX_IS_VIDEO_MEMORY(mem))
		gst_mfx_video_memory_reset_surface(GST_MFX_VIDEO_MEMORY_CAST(mem));

	GST_BUFFER_POOL_CLASS(gst_mfx_video_buffer_pool_parent_class)->reset_buffer
		(pool, buffer);
}

static void
gst_mfx_video_buffer_pool_class_init(GstMfxVideoBufferPoolClass * klass)
{
	GObjectClass *const object_class = G_OBJECT_CLASS(klass);
	GstBufferPoolClass *const pool_class = GST_BUFFER_POOL_CLASS(klass);

	GST_DEBUG_CATEGORY_INIT(gst_debug_mfxvideopool,
		"mfxvideopool", 0, "MFX video pool");

	g_type_class_add_private(klass, sizeof (GstMfxVideoBufferPoolPrivate));

	object_class->finalize = gst_mfx_video_buffer_pool_finalize;
	pool_class->get_options = gst_mfx_video_buffer_pool_get_options;
	pool_class->set_config = gst_mfx_video_buffer_pool_set_config;
	pool_class->alloc_buffer = gst_mfx_video_buffer_pool_alloc_buffer;
	pool_class->reset_buffer = gst_mfx_video_buffer_pool_reset_buffer;

}

static void
gst_mfx_video_buffer_pool_init(GstMfxVideoBufferPool * pool)
{
	GstMfxVideoBufferPoolPrivate *const priv =
		GST_MFX_VIDEO_BUFFER_POOL_GET_PRIVATE(pool);

	pool->priv = priv;

	gst_video_info_init(&priv->video_info[0]);
	gst_video_info_init(&priv->video_info[1]);
	gst_video_info_init(&priv->alloc_info);
}

GstBufferPool *
gst_mfx_video_buffer_pool_new(GstMfxContextAllocator * ctx)
{
    GstMfxVideoBufferPool *pool;

    pool = g_object_new(GST_MFX_TYPE_VIDEO_BUFFER_POOL, NULL);

    pool->priv->ctx = ctx;

	return GST_BUFFER_POOL_CAST(pool);
}
