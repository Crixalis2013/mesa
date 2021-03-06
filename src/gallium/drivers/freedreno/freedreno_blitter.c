/*
 * Copyright (C) 2017 Rob Clark <robclark@freedesktop.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include "util/u_blitter.h"
#include "util/u_surface.h"

#include "freedreno_blitter.h"
#include "freedreno_context.h"
#include "freedreno_resource.h"

/* generic blit using u_blitter.. slightly modified version of util_blitter_blit
 * which also handles PIPE_BUFFER:
 */

static void
default_dst_texture(struct pipe_surface *dst_templ, struct pipe_resource *dst,
		unsigned dstlevel, unsigned dstz)
{
	memset(dst_templ, 0, sizeof(*dst_templ));
	if (dst->target == PIPE_BUFFER)
		dst_templ->format = PIPE_FORMAT_R8_UINT;
	else
		dst_templ->format = util_format_linear(dst->format);
	dst_templ->u.tex.level = dstlevel;
	dst_templ->u.tex.first_layer = dstz;
	dst_templ->u.tex.last_layer = dstz;
}

static void
default_src_texture(struct pipe_sampler_view *src_templ,
		struct pipe_resource *src, unsigned srclevel)
{
	bool cube_as_2darray = src->screen->get_param(src->screen,
			PIPE_CAP_SAMPLER_VIEW_TARGET);

	memset(src_templ, 0, sizeof(*src_templ));

	if (cube_as_2darray && (src->target == PIPE_TEXTURE_CUBE ||
			src->target == PIPE_TEXTURE_CUBE_ARRAY))
		src_templ->target = PIPE_TEXTURE_2D_ARRAY;
	else
		src_templ->target = src->target;

	if (src->target  == PIPE_BUFFER) {
		src_templ->target = PIPE_TEXTURE_1D;
		src_templ->format = PIPE_FORMAT_R8_UINT;
	} else {
		src_templ->format = util_format_linear(src->format);
	}
	src_templ->u.tex.first_level = srclevel;
	src_templ->u.tex.last_level = srclevel;
	src_templ->u.tex.first_layer = 0;
	src_templ->u.tex.last_layer =
			src->target == PIPE_TEXTURE_3D ? u_minify(src->depth0, srclevel) - 1
					: (unsigned)(src->array_size - 1);
	src_templ->swizzle_r = PIPE_SWIZZLE_X;
	src_templ->swizzle_g = PIPE_SWIZZLE_Y;
	src_templ->swizzle_b = PIPE_SWIZZLE_Z;
	src_templ->swizzle_a = PIPE_SWIZZLE_W;
}

bool
fd_blitter_blit(struct fd_context *ctx, const struct pipe_blit_info *info)
{
	struct pipe_resource *dst = info->dst.resource;
	struct pipe_resource *src = info->src.resource;
	struct pipe_context *pipe = &ctx->base;
	struct pipe_surface *dst_view, dst_templ;
	struct pipe_sampler_view src_templ, *src_view;
	bool discard = false;

	if (!info->scissor_enable && !info->alpha_blend) {
		discard = util_texrange_covers_whole_level(info->dst.resource,
				info->dst.level, info->dst.box.x, info->dst.box.y,
				info->dst.box.z, info->dst.box.width,
				info->dst.box.height, info->dst.box.depth);
	}

	fd_blitter_pipe_begin(ctx, info->render_condition_enable, discard, FD_STAGE_BLIT);

	/* Initialize the surface. */
	default_dst_texture(&dst_templ, dst, info->dst.level,
			info->dst.box.z);
	dst_templ.format = info->dst.format;
	dst_view = pipe->create_surface(pipe, dst, &dst_templ);

	/* Initialize the sampler view. */
	default_src_texture(&src_templ, src, info->src.level);
	src_templ.format = info->src.format;
	src_view = pipe->create_sampler_view(pipe, src, &src_templ);

	/* Copy. */
	util_blitter_blit_generic(ctx->blitter, dst_view, &info->dst.box,
			src_view, &info->src.box, src->width0, src->height0,
			info->mask, info->filter,
			info->scissor_enable ? &info->scissor : NULL,
			info->alpha_blend);

	pipe_surface_reference(&dst_view, NULL);
	pipe_sampler_view_reference(&src_view, NULL);

	fd_blitter_pipe_end(ctx);

	/* The fallback blitter must never fail: */
	return true;
}

/**
 * _copy_region using pipe (3d engine)
 */
static bool
fd_blitter_pipe_copy_region(struct fd_context *ctx,
		struct pipe_resource *dst,
		unsigned dst_level,
		unsigned dstx, unsigned dsty, unsigned dstz,
		struct pipe_resource *src,
		unsigned src_level,
		const struct pipe_box *src_box)
{
	/* not until we allow rendertargets to be buffers */
	if (dst->target == PIPE_BUFFER || src->target == PIPE_BUFFER)
		return false;

	if (!util_blitter_is_copy_supported(ctx->blitter, dst, src))
		return false;

	/* TODO we could discard if dst box covers dst level fully.. */
	fd_blitter_pipe_begin(ctx, false, false, FD_STAGE_BLIT);
	util_blitter_copy_texture(ctx->blitter,
			dst, dst_level, dstx, dsty, dstz,
			src, src_level, src_box);
	fd_blitter_pipe_end(ctx);

	return true;
}

/**
 * Copy a block of pixels from one resource to another.
 * The resource must be of the same format.
 * Resources with nr_samples > 1 are not allowed.
 */
void
fd_resource_copy_region(struct pipe_context *pctx,
		struct pipe_resource *dst,
		unsigned dst_level,
		unsigned dstx, unsigned dsty, unsigned dstz,
		struct pipe_resource *src,
		unsigned src_level,
		const struct pipe_box *src_box)
{
	struct fd_context *ctx = fd_context(pctx);

	if (ctx->blit) {
		struct pipe_blit_info info;

		memset(&info, 0, sizeof info);
		info.dst.resource = dst;
		info.dst.level = dst_level;
		info.dst.box.x = dstx;
		info.dst.box.y = dsty;
		info.dst.box.z = dstz;
		info.dst.box.width = src_box->width;
		info.dst.box.height = src_box->height;
		assert(info.dst.box.width >= 0);
		assert(info.dst.box.height >= 0);
		info.dst.box.depth = 1;
		info.dst.format = dst->format;
		info.src.resource = src;
		info.src.level = src_level;
		info.src.box = *src_box;
		info.src.format = src->format;
		info.mask = util_format_get_mask(src->format);
		info.filter = PIPE_TEX_FILTER_NEAREST;
		info.scissor_enable = 0;

		if (ctx->blit(ctx, &info))
			return;
	}

	/* TODO if we have 2d core, or other DMA engine that could be used
	 * for simple copies and reasonably easily synchronized with the 3d
	 * core, this is where we'd plug it in..
	 */

	/* try blit on 3d pipe: */
	if (fd_blitter_pipe_copy_region(ctx,
			dst, dst_level, dstx, dsty, dstz,
			src, src_level, src_box))
		return;

	/* else fallback to pure sw: */
	util_resource_copy_region(pctx,
			dst, dst_level, dstx, dsty, dstz,
			src, src_level, src_box);
}
