// imagew-webp.c
// Part of ImageWorsener, Copyright (c) 2011 by Jason Summers.
// For more information, see the readme.txt file.

#include "imagew-config.h"

#if IW_SUPPORT_WEBP == 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <webp/decode.h>
#include <webp/encode.h>

#define IW_INCLUDE_UTIL_FUNCTIONS
#include "imagew.h"

enum iwwebp_string {
	iws_webp_read_error=1,
	iws_webp_write_error,
	iws_webp_enc_bad_imgtype
};

struct iw_stringtableentry iwwebp_stringtable[] = {
	{ iws_webp_read_error, "Failed to read webp file" },
	{ iws_webp_write_error, "Failed to write webp file" },
	{ iws_webp_enc_bad_imgtype, "Internal: WebP encoder doesn\xe2\x80\x99t support this image type (%d)" },
	{ 0, NULL }
};

static const char *iwwebp_get_string(struct iw_context *ctx, int n)
{
	return iw_get_string(ctx,IW_STRINGTABLENUM_WEBP,n);
}

struct iwwebpreadcontext {
	struct iw_iodescr *iodescr;
	struct iw_context *ctx;
	struct iw_image *img;
	int has_color;
	int has_transparency;
	struct iw_csdescr csdescr;
};

// Sets rctx->has_color and ->has_transparency if appropriate.
static void iwwebp_scan_pixels(struct iwwebpreadcontext *rctx, unsigned char *pixels, size_t npixels)
{
	size_t i;
	for(i=0;i<npixels;i++) {
		if(!rctx->has_transparency) {
			if(pixels[i*4+3]<255) {
				rctx->has_transparency=1;
			}
		}
		if(!rctx->has_color) {
			if(pixels[i*4+0]!=pixels[i*4+1] || pixels[i*4+0]!=pixels[i*4+2]) {
				rctx->has_color=1;
			}
		}
		if(rctx->has_color && rctx->has_transparency) {
			// Nothing left to detect
			return;
		}
	}
}

static void iwwebpr_convert_pixels_gray(struct iwwebpreadcontext *rctx,
 unsigned char *src, size_t nsrcpix)
{
	size_t i;
	for(i=0;i<nsrcpix;i++) {
		rctx->img->pixels[i]=src[i*4];
	}
}

static void iwwebpr_convert_pixels_graya(struct iwwebpreadcontext *rctx,
 unsigned char *src, size_t nsrcpix)
{
	size_t i;
	for(i=0;i<nsrcpix;i++) {
		rctx->img->pixels[i*2+0]=src[i*4+0]; // gray
		rctx->img->pixels[i*2+1]=src[i*4+3]; // alpha
	}
}

static void iwwebpr_convert_pixels_rgb(struct iwwebpreadcontext *rctx,
 unsigned char *src, size_t nsrcpix)
{
	size_t i;
	for(i=0;i<nsrcpix;i++) {
		rctx->img->pixels[i*3+0]=src[i*4+0]; // r
		rctx->img->pixels[i*3+1]=src[i*4+1]; // g
		rctx->img->pixels[i*3+2]=src[i*4+2]; // b
	}
}

static void iwwebpr_convert_pixels_rgba(struct iwwebpreadcontext *rctx,
 unsigned char *src, size_t nsrcpix)
{
	memcpy(rctx->img->pixels,src,nsrcpix*4);
}

static int iwwebp_read_main(struct iwwebpreadcontext *rctx)
{
	struct iw_image *img;
	int retval=0;
	void *webpimage=NULL;
	size_t webpimage_size=0;
	uint8_t* uncmpr_webp_pixels = NULL;
	int width, height;
	size_t npixels;
	int bytes_per_pixel;

	img = rctx->img;

	// TODO: This is really memory-inefficient.
	// Honestly, I just can't figure out libwebp.
	// It has several different ways to decode a webp image, but they don't
	// seem to add up to any way to do it without allocating at least one
	// more image's worth of memory than ought to be necessary.
	// It's like it expects you to know the width, height, and color format
	// of the image before that information has been read from the file.

	// Read the whole WebP file into a memory block.
	if(!iw_file_to_memory(rctx->ctx, rctx->iodescr, &webpimage, &webpimage_size)) {
		goto done;
	}

	// Have libwebp decode that memory block, to a memory block that
	// it allocates.
	width=height=0;
	uncmpr_webp_pixels = WebPDecodeRGBA(webpimage, (uint32_t)webpimage_size, &width, &height);
	if(!uncmpr_webp_pixels) goto done;

	if(!iw_check_image_dimensons(rctx->ctx,width,height))
		goto done;

	npixels = ((size_t)width)*height;

	// Figure out if the image has transparency, etc.
	iwwebp_scan_pixels(rctx,uncmpr_webp_pixels,npixels);

	// Choose the color format to use for IW's internal source image.
	if(rctx->has_color)
		img->imgtype = rctx->has_transparency ? IW_IMGTYPE_RGBA : IW_IMGTYPE_RGB;
	else
		img->imgtype = rctx->has_transparency ? IW_IMGTYPE_GRAYA : IW_IMGTYPE_GRAY;

	img->width = width;
	img->height = height;
	img->bit_depth = 8;
	bytes_per_pixel = iw_imgtype_num_channels(img->imgtype);
	img->bpr = bytes_per_pixel * img->width;
	img->pixels = (unsigned char*)iw_malloc_large(rctx->ctx, img->bpr, img->height);
	if(!img->pixels) goto done;

	switch(img->imgtype) {
	case IW_IMGTYPE_GRAY:  iwwebpr_convert_pixels_gray(rctx,(unsigned char*)uncmpr_webp_pixels,npixels); break;
	case IW_IMGTYPE_GRAYA: iwwebpr_convert_pixels_graya(rctx,(unsigned char*)uncmpr_webp_pixels,npixels); break;
	case IW_IMGTYPE_RGB:   iwwebpr_convert_pixels_rgb(rctx,(unsigned char*)uncmpr_webp_pixels,npixels); break;
	default:               iwwebpr_convert_pixels_rgba(rctx,(unsigned char*)uncmpr_webp_pixels,npixels); break;
	}

	retval=1;

done:
	if(webpimage) iw_free(webpimage);

	// Caution: We must use the right free() function: the one that corresponds
	// to the malloc function that libwebp used. But we can't always be sure
	// how to do that. It depends on how libwebp was compiled.
	if(uncmpr_webp_pixels) free(uncmpr_webp_pixels);

	return retval;
}

int iw_read_webp_file(struct iw_context *ctx, struct iw_iodescr *iodescr)
{
	struct iw_image img;
	struct iwwebpreadcontext rctx;
	int retval=0;

	memset(&rctx,0,sizeof(struct iwwebpreadcontext));
	memset(&img,0,sizeof(struct iw_image));

	iw_set_string_table(ctx,IW_STRINGTABLENUM_WEBP,iwwebp_stringtable);

	rctx.ctx = ctx;
	rctx.iodescr = iodescr;
	rctx.img = &img;

	// Assume WebP images are sRGB
	rctx.csdescr.cstype = IW_CSTYPE_SRGB;
	rctx.csdescr.sRGB_intent = IW_sRGB_INTENT_PERCEPTUAL;

	if(!iwwebp_read_main(&rctx))
		goto done;

	iw_set_input_image(ctx, &img);

	iw_set_input_colorspace(ctx,&rctx.csdescr);

	retval = 1;

done:
	if(!retval) {
		iw_seterror(ctx,iwwebp_get_string(ctx,iws_webp_read_error));
	}

	if(iodescr->close_fn)
		(*iodescr->close_fn)(ctx,iodescr);
	return retval;
}

struct iwwebpwritecontext {
	struct iw_iodescr *iodescr;
	struct iw_context *ctx;
	struct iw_image *img;
	unsigned char *tmppixels;
};

static void iwwebp_write(struct iwwebpwritecontext *wctx, const void *buf, size_t n)
{
	(*wctx->iodescr->write_fn)(wctx->ctx,wctx->iodescr,buf,n);
}

static void iwwebp_gray_to_rgb(struct iwwebpwritecontext *wctx)
{
	int i,j;
	struct iw_image *img = wctx->img;
	size_t bpr;
	unsigned char g;

	bpr = 3 * img->width;
	wctx->tmppixels = iw_malloc_large(wctx->ctx, img->height, bpr);
	if(!wctx) return;

	for(j=0;j<img->height;j++) {
		for(i=0;i<img->width;i++) {
			g = img->pixels[j*img->bpr+i];
			wctx->tmppixels[j*bpr+3*i+0]=g;
			wctx->tmppixels[j*bpr+3*i+1]=g;
			wctx->tmppixels[j*bpr+3*i+2]=g;
		}
	}
}

static int iwwebp_write_main(struct iwwebpwritecontext *wctx)
{
	struct iw_image *img;
	size_t ret;
	uint8_t *cmpr_webp_data = NULL;
	int retval=0;
	double quality;

	img = wctx->img;

	quality = iw_get_value_dbl(wctx->ctx,IW_VAL_WEBP_QUALITY);
	if(quality<0.0) {
		quality=80.0; // Default quality.
	}

	switch(img->imgtype) {
	case IW_IMGTYPE_GRAY:
		// IW requires encoders to support grayscale, but WebP doesn't (?)
		// support it. So, convert grayscale images to RGB.
		iwwebp_gray_to_rgb(wctx); // Allocates RGB image at wctx->tmppixels.
		if(!wctx->tmppixels) goto done;
		ret = WebPEncodeRGB(wctx->tmppixels, img->width, img->height, 3*img->width, (float)quality, &cmpr_webp_data);
		break;
	case IW_IMGTYPE_RGB:
		ret = WebPEncodeRGB(img->pixels, img->width, img->height, (int)img->bpr, (float)quality, &cmpr_webp_data);
		break;
	default:
		iw_seterror(wctx->ctx,iwwebp_get_string(wctx->ctx,iws_webp_enc_bad_imgtype),img->imgtype);
		goto done;
	}

	if(ret<1 || !cmpr_webp_data) {
		goto done;
	}
	iwwebp_write(wctx, cmpr_webp_data, ret);
	retval=1;

done:
	if(cmpr_webp_data) free(cmpr_webp_data);
	if(wctx->tmppixels) iw_free(wctx->tmppixels);
	return 1;
}

int iw_write_webp_file(struct iw_context *ctx, struct iw_iodescr *iodescr)
{
	struct iwwebpwritecontext wctx;
	int retval=0;
	struct iw_image img1;

	iw_set_string_table(ctx,IW_STRINGTABLENUM_WEBP,iwwebp_stringtable);

	memset(&img1,0,sizeof(struct iw_image));
	memset(&wctx,0,sizeof(struct iwwebpwritecontext));

	wctx.ctx = ctx;

	wctx.iodescr=iodescr;

	iw_get_output_image(ctx,&img1);
	wctx.img = &img1;

	if(!iwwebp_write_main(&wctx)) {
		goto done;
	}

	retval=1;

done:
	if(!retval) {
		iw_seterror(ctx,iwwebp_get_string(ctx,iws_webp_write_error));
	}

	if(wctx.iodescr->close_fn)
		(*wctx.iodescr->close_fn)(ctx,wctx.iodescr);

	return retval;
}

char *iw_get_libwebp_dec_version_string(char *s, int s_len)
{
	int v;
	v=WebPGetDecoderVersion();
	iw_snprintf(s,s_len,"%d.%d.%d",(v&0xff0000)>>16,
		(v&0xff00)>>8,v&0xff);
	return s;
}

char *iw_get_libwebp_enc_version_string(char *s, int s_len)
{
	int v;
	v=WebPGetEncoderVersion();
	iw_snprintf(s,s_len,"%d.%d.%d",(v&0xff0000)>>16,
		(v&0xff00)>>8,v&0xff);
	return s;
}

#endif // IW_SUPPORT_WEBP