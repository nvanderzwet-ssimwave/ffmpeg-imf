/*
 * This file is part of FFmpeg.
 *
 * Copyright (c) Sandflow Consulting LLC
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * Demuxes an IMF Composition
 *
 * References
 * OV 2067-0:2018 - SMPTE Overview Document - Interoperable Master Format
 * ST 2067-2:2020 - SMPTE Standard - Interoperable Master Format — Core Constraints
 * ST 2067-3:2020 - SMPTE Standard - Interoperable Master Format — Composition Playlist
 * ST 2067-5:2020 - SMPTE Standard - Interoperable Master Format — Essence Component
 * ST 2067-20:2016 - SMPTE Standard - Interoperable Master Format — Application #2
 * ST 2067-21:2020 - SMPTE Standard - Interoperable Master Format — Application #2 Extended
 * ST 2067-102:2017 - SMPTE Standard - Interoperable Master Format — Common Image Pixel Color Schemes
 * ST 429-9:2007 - SMPTE Standard - D-Cinema Packaging — Asset Mapping and File Segmentation
 *
 * @author Marc-Antoine Arnaud
 * @author Valentin Noel
 * @author Nicholas Vanderzwet
 * @file
 * @ingroup lavu_imf
 */

#include "imf.h"
#include "internal.h"
#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/opt.h"
#include "mxf.h"
#include "url.h"
#include "avio_internal.h"
#include <inttypes.h>
#include <libxml/parser.h>

#define MAX_BPRINT_READ_SIZE (UINT_MAX - 1)
#define DEFAULT_ASSETMAP_SIZE 8 * 1024
#define AVRATIONAL_FORMAT "%d/%d"
#define AVRATIONAL_ARG(rational) rational.num, rational.den

/**
 * IMF Asset locator
 */
typedef struct IMFAssetLocator {
    FFUUID uuid;
    char *absolute_uri;
} IMFAssetLocator;

/**
 * IMF Asset locator map
 * Results from the parsing of one or more ASSETMAP XML files
 */
typedef struct IMFAssetLocatorMap {
    uint32_t asset_count;
    IMFAssetLocator *assets;
} IMFAssetLocatorMap;

typedef struct IMFVirtualTrackResourcePlaybackCtx {
    IMFAssetLocator *locator;
    FFIMFTrackFileResource *resource;
    AVFormatContext *ctx;
} IMFVirtualTrackResourcePlaybackCtx;

typedef struct IMFVirtualTrackPlaybackCtx {
    // Track index in playlist
    int32_t index;
    // Time counters
    AVRational current_timestamp;
    AVRational duration;
    // Resources
    uint32_t resource_count;
    uint32_t resources_alloc_sz;
    IMFVirtualTrackResourcePlaybackCtx *resources;
    // Decoding cursors
    uint32_t current_resource_index;
    int64_t last_pts;
} IMFVirtualTrackPlaybackCtx;

typedef struct IMFContext {
    const AVClass *class;
    const char *base_url;
    char *asset_map_paths;
    AVIOInterruptCB *interrupt_callback;
    AVDictionary *avio_opts;
    FFIMFCPL *cpl;
    IMFAssetLocatorMap asset_locator_map;
    uint32_t track_count;
    IMFVirtualTrackPlaybackCtx **tracks;
} IMFContext;

static int imf_uri_is_url(const char *string)
{
    return strstr(string, "://") != NULL;
}

static int imf_uri_is_unix_abs_path(const char *string)
{
    return string[0] == '/';
}

static int imf_uri_is_dos_abs_path(const char *string)
{
    /* Absolute path case: `C:\path\to\somwhere` */
    if (string[1] == ':' && string[2] == '\\')
        return 1;

    /* Absolute path case: `C:/path/to/somwhere` */
    if (string[1] == ':' && string[2] == '/')
        return 1;

    /* Network path case: `\\path\to\somwhere` */
    if (string[0] == '\\' && string[1] == '\\')
        return 1;

    return 0;
}

/**
 * Parse a ASSETMAP XML file to extract the UUID-URI mapping of assets.
 * @param s the current format context, if any (can be NULL).
 * @param doc the XML document to be parsed.
 * @param asset_map pointer on the IMFAssetLocatorMap to fill.
 * @param base_url the url of the asset map XML file, if any (can be NULL).
 * @return a negative value in case of error, 0 otherwise.
 */
static int parse_imf_asset_map_from_xml_dom(AVFormatContext *s,
    xmlDocPtr doc,
    IMFAssetLocatorMap *asset_map,
    const char *base_url)
{
    xmlNodePtr asset_map_element = NULL;
    xmlNodePtr node = NULL;
    xmlNodePtr asset_element = NULL;
    char *uri;
    int ret = 0;
    IMFAssetLocator *asset = NULL;
    void *tmp;

    asset_map_element = xmlDocGetRootElement(doc);

    if (!asset_map_element) {
        av_log(s, AV_LOG_ERROR, "Unable to parse asset map XML - missing root node\n");
        return AVERROR_INVALIDDATA;
    }

    if (asset_map_element->type != XML_ELEMENT_NODE || av_strcasecmp(asset_map_element->name, "AssetMap")) {
        av_log(s,
            AV_LOG_ERROR,
            "Unable to parse asset map XML - wrong root node name[%s] type[%d]\n",
            asset_map_element->name,
            (int)asset_map_element->type);
        return AVERROR_INVALIDDATA;
    }

    /* parse asset locators */
    if (!(node = ff_xml_get_child_element_by_name(asset_map_element, "AssetList"))) {
        av_log(s, AV_LOG_ERROR, "Unable to parse asset map XML - missing AssetList node\n");
        return AVERROR_INVALIDDATA;
    }
    tmp = av_realloc(asset_map->assets,
        (xmlChildElementCount(node) + asset_map->asset_count)
            * sizeof(IMFAssetLocator));
    if (!tmp) {
        av_log(NULL, AV_LOG_ERROR, "Cannot allocate IMF asset locators\n");
        return AVERROR(ENOMEM);
    }
    asset_map->assets = tmp;

    asset_element = xmlFirstElementChild(node);
    while (asset_element) {
        if (av_strcasecmp(asset_element->name, "Asset") != 0)
            continue;

        asset = &(asset_map->assets[asset_map->asset_count]);

        if (ff_xml_read_uuid(ff_xml_get_child_element_by_name(asset_element, "Id"), asset->uuid)) {
            av_log(s, AV_LOG_ERROR, "Could not parse UUID from asset in asset map.\n");
            return AVERROR_INVALIDDATA;
        }

        av_log(s, AV_LOG_DEBUG, "Found asset id: " FF_UUID_FORMAT "\n", UID_ARG(asset->uuid));

        if (!(node = ff_xml_get_child_element_by_name(asset_element, "ChunkList"))) {
            av_log(s, AV_LOG_ERROR, "Unable to parse asset map XML - missing ChunkList node\n");
            return AVERROR_INVALIDDATA;
        }

        if (!(node = ff_xml_get_child_element_by_name(node, "Chunk"))) {
            av_log(s, AV_LOG_ERROR, "Unable to parse asset map XML - missing Chunk node\n");
            return AVERROR_INVALIDDATA;
        }

        uri = xmlNodeGetContent(ff_xml_get_child_element_by_name(node, "Path"));
        if (!imf_uri_is_url(uri) && !imf_uri_is_unix_abs_path(uri) && !imf_uri_is_dos_abs_path(uri))
            asset->absolute_uri = av_append_path_component(base_url, uri);
        else
            asset->absolute_uri = av_strdup(uri);
        xmlFree(uri);
        if (!asset->absolute_uri) {
            return AVERROR(ENOMEM);
        }

        av_log(s, AV_LOG_DEBUG, "Found asset absolute URI: %s\n", asset->absolute_uri);

        asset_map->asset_count++;
        asset_element = xmlNextElementSibling(asset_element);
    }

    return ret;
}

/**
 * Initializes an IMFAssetLocatorMap structure.
 */
static void imf_asset_locator_map_init(IMFAssetLocatorMap *asset_map)
{
    asset_map->assets = NULL;
    asset_map->asset_count = 0;
}

/**
 * Free a IMFAssetLocatorMap pointer.
 */
static void imf_asset_locator_map_deinit(IMFAssetLocatorMap *asset_map)
{
    for (uint32_t i = 0; i < asset_map->asset_count; ++i)
        av_freep(&asset_map->assets[i].absolute_uri);
    av_freep(&asset_map->assets);
}

static int parse_assetmap(AVFormatContext *s, const char *url, AVIOContext *in)
{
    IMFContext *c = s->priv_data;
    struct AVBPrint buf;
    AVDictionary *opts = NULL;
    xmlDoc *doc = NULL;
    const char *base_url;
    char *tmp_str = NULL;
    int close_in = 0;
    int ret;
    int64_t filesize;

    av_log(s, AV_LOG_DEBUG, "Asset Map URL: %s\n", url);

    if (!in) {
        close_in = 1;

        av_dict_copy(&opts, c->avio_opts, 0);
        ret = s->io_open(s, &in, url, AVIO_FLAG_READ, &opts);
        av_dict_free(&opts);
        if (ret < 0)
            return ret;
    }

    filesize = avio_size(in);
    filesize = filesize > 0 ? filesize : DEFAULT_ASSETMAP_SIZE;

    av_bprint_init(&buf, filesize + 1, AV_BPRINT_SIZE_UNLIMITED);

    ret = avio_read_to_bprint(in, &buf, MAX_BPRINT_READ_SIZE);
    if (ret < 0 || !avio_feof(in) || (filesize = buf.len) == 0) {
        av_log(s, AV_LOG_ERROR, "Unable to read to asset map '%s'\n", url);
        if (ret == 0)
            ret = AVERROR_INVALIDDATA;
        goto clean_up;
    }

    LIBXML_TEST_VERSION

    tmp_str = av_strdup(url);
    if (!tmp_str) {
        ret = AVERROR(ENOMEM);
        goto clean_up;
    }
    base_url = av_dirname(tmp_str);

    doc = xmlReadMemory(buf.str, filesize, url, NULL, 0);

    ret = parse_imf_asset_map_from_xml_dom(s, doc, &c->asset_locator_map, base_url);
    if (!ret)
        av_log(s,
            AV_LOG_DEBUG,
            "Found %d assets from %s\n",
            c->asset_locator_map.asset_count,
            url);

    xmlFreeDoc(doc);

clean_up:
    if (tmp_str)
        av_freep(&tmp_str);
    if (close_in)
        avio_close(in);
    av_bprint_finalize(&buf, NULL);

    return ret;
}

static IMFAssetLocator *find_asset_map_locator(IMFAssetLocatorMap *asset_map, FFUUID uuid)
{
    for (uint32_t i = 0; i < asset_map->asset_count; ++i)
        if (memcmp(asset_map->assets[i].uuid, uuid, 16) == 0)
            return &(asset_map->assets[i]);
    return NULL;
}

static int open_track_resource_context(AVFormatContext *s,
    IMFVirtualTrackResourcePlaybackCtx *track_resource)
{
    IMFContext *c = s->priv_data;
    int ret = 0;
    int64_t entry_point;
    AVDictionary *opts = NULL;

    if (!track_resource->ctx) {
        track_resource->ctx = avformat_alloc_context();
        if (!track_resource->ctx)
            return AVERROR(ENOMEM);
    }

    if (track_resource->ctx->iformat) {
        av_log(s,
            AV_LOG_DEBUG,
            "Input context already opened for %s.\n",
            track_resource->locator->absolute_uri);
        return 0;
    }

    track_resource->ctx->io_open = s->io_open;
    track_resource->ctx->io_close = s->io_close;
    track_resource->ctx->flags |= s->flags & ~AVFMT_FLAG_CUSTOM_IO;

    if ((ret = ff_copy_whiteblacklists(track_resource->ctx, s)) < 0)
        goto cleanup;

    av_dict_copy(&opts, c->avio_opts, 0);
    ret = avformat_open_input(&track_resource->ctx,
        track_resource->locator->absolute_uri,
        NULL,
        &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        av_log(s,
            AV_LOG_ERROR,
            "Could not open %s input context: %s\n",
            track_resource->locator->absolute_uri,
            av_err2str(ret));
        return ret;
    }

    ret = avformat_find_stream_info(track_resource->ctx, NULL);
    if (ret < 0) {
        av_log(s,
            AV_LOG_ERROR,
            "Could not find %s stream information: %s\n",
            track_resource->locator->absolute_uri,
            av_err2str(ret));
        goto cleanup;
    }

    /* Compare the source timebase to the resource edit rate, considering the first stream of the source file */
    if (av_cmp_q(track_resource->ctx->streams[0]->time_base, av_inv_q(track_resource->resource->base.edit_rate)))
        av_log(s,
            AV_LOG_WARNING,
            "Incoherent source stream timebase %d/%d regarding resource edit rate: %d/%d",
            track_resource->ctx->streams[0]->time_base.num,
            track_resource->ctx->streams[0]->time_base.den,
            track_resource->resource->base.edit_rate.den,
            track_resource->resource->base.edit_rate.num);

    entry_point = (int64_t)track_resource->resource->base.entry_point
        * track_resource->resource->base.edit_rate.den
        * AV_TIME_BASE
        / track_resource->resource->base.edit_rate.num;

    if (entry_point) {
        av_log(s,
            AV_LOG_DEBUG,
            "Seek at resource %s entry point: %" PRIu32 "\n",
            track_resource->locator->absolute_uri,
            track_resource->resource->base.entry_point);
        ret = avformat_seek_file(track_resource->ctx, -1, entry_point, entry_point, entry_point, 0);
        if (ret < 0) {
            av_log(s,
                AV_LOG_ERROR,
                "Could not seek at %" PRId64 "on %s: %s\n",
                entry_point,
                track_resource->locator->absolute_uri,
                av_err2str(ret));
            goto cleanup;
        }
    }

    return ret;
cleanup:
    avformat_free_context(track_resource->ctx);
    track_resource->ctx = NULL;
    return ret;
}

static int open_track_file_resource(AVFormatContext *s,
    FFIMFTrackFileResource *track_file_resource,
    IMFVirtualTrackPlaybackCtx *track)
{
    IMFContext *c = s->priv_data;
    IMFAssetLocator *asset_locator;
    IMFVirtualTrackResourcePlaybackCtx vt_ctx;
    void *tmp;
    int ret;

    asset_locator = find_asset_map_locator(&c->asset_locator_map, track_file_resource->track_file_uuid);
    if (!asset_locator) {
        av_log(s,
            AV_LOG_ERROR,
            "Could not find asset locator for UUID: " FF_UUID_FORMAT "\n",
            UID_ARG(track_file_resource->track_file_uuid));
        return AVERROR_INVALIDDATA;
    }

    av_log(s,
        AV_LOG_DEBUG,
        "Found locator for " FF_UUID_FORMAT ": %s\n",
        UID_ARG(asset_locator->uuid),
        asset_locator->absolute_uri);
    tmp = av_fast_realloc(track->resources,
        &track->resources_alloc_sz,
        (track->resource_count + track_file_resource->base.repeat_count)
            * sizeof(IMFVirtualTrackResourcePlaybackCtx));
    if (!tmp)
        return AVERROR(ENOMEM);
    track->resources = tmp;

    for (uint32_t i = 0; i < track_file_resource->base.repeat_count; ++i) {
        vt_ctx.locator = asset_locator;
        vt_ctx.resource = track_file_resource;
        vt_ctx.ctx = NULL;
        if ((ret = open_track_resource_context(s, &vt_ctx)) != 0)
            return ret;
        track->resources[track->resource_count++] = vt_ctx;
        track->duration = av_add_q(track->duration,
            av_make_q((int)track_file_resource->base.duration * track_file_resource->base.edit_rate.den,
                track_file_resource->base.edit_rate.num));
    }

    return ret;
}

static void imf_virtual_track_playback_context_deinit(IMFVirtualTrackPlaybackCtx *track)
{
    for (uint32_t i = 0; i < track->resource_count; ++i)
        if (track->resources[i].ctx && track->resources[i].ctx->iformat)
            avformat_close_input(&track->resources[i].ctx);

    av_freep(&track->resources);
}

static int open_virtual_track(AVFormatContext *s,
    FFIMFTrackFileVirtualTrack *virtual_track,
    int32_t track_index)
{
    IMFContext *c = s->priv_data;
    IMFVirtualTrackPlaybackCtx *track = NULL;
    void *tmp;
    int ret = 0;

    if (!(track = av_mallocz(sizeof(IMFVirtualTrackPlaybackCtx))))
        return AVERROR(ENOMEM);
    track->index = track_index;
    track->duration = av_make_q(0, 1);

    for (uint32_t i = 0; i < virtual_track->resource_count; i++) {
        av_log(s,
            AV_LOG_DEBUG,
            "Open stream from file " FF_UUID_FORMAT ", stream %d\n",
            UID_ARG(virtual_track->resources[i].track_file_uuid),
            i);
        if ((ret = open_track_file_resource(s, &virtual_track->resources[i], track)) != 0) {
            av_log(s,
                AV_LOG_ERROR,
                "Could not open image track resource " FF_UUID_FORMAT "\n",
                UID_ARG(virtual_track->resources[i].track_file_uuid));
            goto clean_up;
        }
    }

    track->current_timestamp = av_make_q(0, track->duration.den);

    tmp = av_realloc(c->tracks, (c->track_count + 1) * sizeof(IMFVirtualTrackPlaybackCtx *));
    if (!tmp) {
        ret = AVERROR(ENOMEM);
        goto clean_up;
    }
    c->tracks = tmp;
    c->tracks[c->track_count++] = track;

    return 0;

clean_up:
    imf_virtual_track_playback_context_deinit(track);
    av_free(track);
    return ret;
}

static int set_context_streams_from_tracks(AVFormatContext *s)
{
    IMFContext *c = s->priv_data;

    AVStream *asset_stream;
    AVStream *first_resource_stream;

    int ret = 0;

    for (uint32_t i = 0; i < c->track_count; ++i) {
        /* Open the first resource of the track to get stream information */
        first_resource_stream = c->tracks[i]->resources[0].ctx->streams[0];
        av_log(s, AV_LOG_DEBUG, "Open the first resource of track %d\n", c->tracks[i]->index);

        /* Copy stream information */
        asset_stream = avformat_new_stream(s, NULL);
        if (!asset_stream) {
            av_log(s, AV_LOG_ERROR, "Could not create stream\n");
            break;
        }
        asset_stream->id = i;
        ret = avcodec_parameters_copy(asset_stream->codecpar, first_resource_stream->codecpar);
        if (ret < 0) {
            av_log(s, AV_LOG_ERROR, "Could not copy stream parameters\n");
            break;
        }
        avpriv_set_pts_info(asset_stream,
            first_resource_stream->pts_wrap_bits,
            first_resource_stream->time_base.num,
            first_resource_stream->time_base.den);
        asset_stream->duration = (int64_t)av_q2d(av_mul_q(c->tracks[i]->duration,
            av_inv_q(asset_stream->time_base)));
    }

    return ret;
}

static int open_cpl_tracks(AVFormatContext *s)
{
    IMFContext *c = s->priv_data;
    int32_t track_index = 0;
    int ret;

    if (c->cpl->main_image_2d_track)
        if ((ret = open_virtual_track(s, c->cpl->main_image_2d_track, track_index++)) != 0) {
            av_log(s,
                AV_LOG_ERROR,
                "Could not open image track " FF_UUID_FORMAT "\n",
                UID_ARG(c->cpl->main_image_2d_track->base.id_uuid));
            return ret;
        }

    for (uint32_t i = 0; i < c->cpl->main_audio_track_count; ++i)
        if ((ret = open_virtual_track(s, &c->cpl->main_audio_tracks[i], track_index++)) != 0) {
            av_log(s,
                AV_LOG_ERROR,
                "Could not open audio track " FF_UUID_FORMAT "\n",
                UID_ARG(c->cpl->main_audio_tracks[i].base.id_uuid));
            return ret;
        }

    return set_context_streams_from_tracks(s);
}

static int imf_read_header(AVFormatContext *s)
{
    IMFContext *c = s->priv_data;
    char *asset_map_path;
    char *tmp_str;
    int ret = 0;

    c->interrupt_callback = &s->interrupt_callback;
    tmp_str = av_strdup(s->url);
    if (!tmp_str) {
        ret = AVERROR(ENOMEM);
        return ret;
    }
    c->base_url = av_dirname(tmp_str);
    if ((ret = ffio_copy_url_options(s->pb, &c->avio_opts)) < 0)
        return ret;

    av_log(s, AV_LOG_DEBUG, "start parsing IMF CPL: %s\n", s->url);

    if ((ret = ff_parse_imf_cpl(s->pb, &c->cpl)) < 0)
        return ret;

    av_log(s,
        AV_LOG_DEBUG,
        "parsed IMF CPL: " FF_UUID_FORMAT "\n",
        UID_ARG(c->cpl->id_uuid));

    if (!c->asset_map_paths) {
        c->asset_map_paths = av_append_path_component(c->base_url, "ASSETMAP.xml");
        if (!c->asset_map_paths) {
            ret = AVERROR(ENOMEM);
            return ret;
        }
        av_log(s, AV_LOG_DEBUG, "No asset maps provided, using the default ASSETMAP.xml\n");
    }

    /* Parse each asset map XML file */
    imf_asset_locator_map_init(&c->asset_locator_map);
    asset_map_path = av_strtok(c->asset_map_paths, ",", &tmp_str);
    while (asset_map_path != NULL) {
        av_log(s, AV_LOG_DEBUG, "start parsing IMF Asset Map: %s\n", asset_map_path);

        if (ret = parse_assetmap(s, asset_map_path, NULL))
            return ret;

        asset_map_path = av_strtok(NULL, ",", &tmp_str);
    }

    av_log(s, AV_LOG_DEBUG, "parsed IMF Asset Maps\n");

    if (ret = open_cpl_tracks(s))
        return ret;

    av_log(s, AV_LOG_DEBUG, "parsed IMF package\n");

    return 0;
}

static IMFVirtualTrackPlaybackCtx *get_next_track_with_minimum_timestamp(AVFormatContext *s)
{
    IMFContext *c = s->priv_data;
    IMFVirtualTrackPlaybackCtx *track;

    AVRational minimum_timestamp = av_make_q(INT32_MAX, 1);
    for (uint32_t i = 0; i < c->track_count; ++i) {
        av_log(
            s,
            AV_LOG_DEBUG,
            "Compare track %d timestamp " AVRATIONAL_FORMAT
            " to minimum " AVRATIONAL_FORMAT
            " (over duration: " AVRATIONAL_FORMAT
            ")\n",
            i,
            AVRATIONAL_ARG(c->tracks[i]->current_timestamp),
            AVRATIONAL_ARG(minimum_timestamp),
            AVRATIONAL_ARG(c->tracks[i]->duration));
        if (av_cmp_q(c->tracks[i]->current_timestamp, minimum_timestamp) < 0) {
            track = c->tracks[i];
            minimum_timestamp = track->current_timestamp;
        }
    }

    av_log(s,
        AV_LOG_DEBUG,
        "Found next track to read: %d (timestamp: %lf / %lf)\n",
        track->index,
        av_q2d(track->current_timestamp),
        av_q2d(minimum_timestamp));
    return track;
}

static IMFVirtualTrackResourcePlaybackCtx *get_resource_context_for_timestamp(AVFormatContext *s,
    IMFVirtualTrackPlaybackCtx *track)
{
    AVRational edit_unit_duration = av_inv_q(track->resources[0].resource->base.edit_rate);
    AVRational cumulated_duration = av_make_q(0, edit_unit_duration.den);

    av_log(s,
        AV_LOG_DEBUG,
        "Looking for track %d resource for timestamp = %lf / %lf\n",
        track->index,
        av_q2d(track->current_timestamp),
        av_q2d(track->duration));
    for (uint32_t i = 0; i < track->resource_count; ++i) {
        cumulated_duration = av_add_q(cumulated_duration,
            av_make_q((int)track->resources[i].resource->base.duration * edit_unit_duration.num,
                edit_unit_duration.den));

        if (av_cmp_q(av_add_q(track->current_timestamp, edit_unit_duration), cumulated_duration) <= 0) {
            av_log(s,
                AV_LOG_DEBUG,
                "Found resource %d in track %d to read for timestamp %lf "
                "(on cumulated=%lf): entry=%" PRIu32
                ", duration=%" PRIu32
                ", editrate=" AVRATIONAL_FORMAT
                " | edit_unit_duration=%lf\n",
                i,
                track->index,
                av_q2d(track->current_timestamp),
                av_q2d(cumulated_duration),
                track->resources[i].resource->base.entry_point,
                track->resources[i].resource->base.duration,
                AVRATIONAL_ARG(track->resources[i].resource->base.edit_rate),
                av_q2d(edit_unit_duration));

            if (track->current_resource_index != i) {
                av_log(s,
                    AV_LOG_DEBUG,
                    "Switch resource on track %d: re-open context\n",
                    track->index);
                avformat_close_input(&(track->resources[track->current_resource_index].ctx));
                if (open_track_resource_context(s, &(track->resources[i])) != 0)
                    return NULL;
                track->current_resource_index = i;
            }
            return &(track->resources[track->current_resource_index]);
        }
    }
    return NULL;
}

static int imf_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    IMFContext *c = s->priv_data;
    IMFVirtualTrackResourcePlaybackCtx *resource_to_read = NULL;
    AVRational edit_unit_duration;
    int ret = 0;
    IMFVirtualTrackPlaybackCtx *track;
    FFStream *track_stream;

    track = get_next_track_with_minimum_timestamp(s);

    if (av_cmp_q(track->current_timestamp, track->duration) == 0)
        return AVERROR_EOF;

    resource_to_read = get_resource_context_for_timestamp(s, track);

    if (!resource_to_read) {
        edit_unit_duration = av_inv_q(track->resources[track->current_resource_index].resource->base.edit_rate);
        if (av_cmp_q(av_add_q(track->current_timestamp, edit_unit_duration),
                track->duration)
            > 0)
            return AVERROR_EOF;

        av_log(s, AV_LOG_ERROR, "Could not find IMF track resource to read\n");
        return AVERROR_STREAM_NOT_FOUND;
    }

    while (!ff_check_interrupt(c->interrupt_callback) && !ret) {
        ret = av_read_frame(resource_to_read->ctx, pkt);
        av_log(s,
            AV_LOG_DEBUG,
            "Got packet: pts=%" PRId64
            ", dts=%" PRId64
            ", duration=%" PRId64
            ", stream_index=%d, pos=%" PRId64
            "\n",
            pkt->pts,
            pkt->dts,
            pkt->duration,
            pkt->stream_index,
            pkt->pos);
        track_stream = ffstream(s->streams[track->index]);
        if (ret >= 0) {
            /* Update packet info from track */
            if (pkt->dts < track_stream->cur_dts && track->last_pts > 0)
                pkt->dts = track_stream->cur_dts;

            pkt->pts = track->last_pts;
            pkt->dts = pkt->dts
                - (int64_t)track->resources[track->current_resource_index].resource->base.entry_point;
            pkt->stream_index = track->index;

            /* Update track cursors */
            track->current_timestamp = av_add_q(track->current_timestamp,
                av_make_q((int)pkt->duration * resource_to_read->ctx->streams[0]->time_base.num,
                    resource_to_read->ctx->streams[0]->time_base.den));
            track->last_pts += pkt->duration;

            return 0;
        } else if (ret != AVERROR_EOF) {
            av_log(s,
                AV_LOG_ERROR,
                "Could not get packet from track %d: %s\n",
                track->index,
                av_err2str(ret));
            return ret;
        }
    }

    return AVERROR_EOF;
}

static int imf_close(AVFormatContext *s)
{
    IMFContext *c = s->priv_data;

    av_log(s, AV_LOG_DEBUG, "Close IMF package\n");
    av_dict_free(&c->avio_opts);
    av_freep(&c->base_url);
    imf_asset_locator_map_deinit(&c->asset_locator_map);
    ff_imf_cpl_free(c->cpl);

    for (uint32_t i = 0; i < c->track_count; ++i) {
        imf_virtual_track_playback_context_deinit(c->tracks[i]);
        av_freep(&c->tracks[i]);
    }

    av_freep(&c->tracks);

    return 0;
}

// clang-format off
static const AVOption imf_options[] = {
    {
        .name        = "assetmaps",
        .help        = "Comma-separated paths to ASSETMAP files."
                       "If not specified, the `ASSETMAP.xml` file in the same directory as the CPL is used.",
        .offset      = offsetof(IMFContext, asset_map_paths),
        .type        = AV_OPT_TYPE_STRING,
        .default_val = {.str = NULL},
        .flags       = AV_OPT_FLAG_DECODING_PARAM,
    },
    {NULL},
};

static const AVClass imf_class = {
    .class_name = "imf",
    .item_name  = av_default_item_name,
    .option     = imf_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const AVInputFormat ff_imf_demuxer = {
    .name           = "imf",
    .long_name      = NULL_IF_CONFIG_SMALL("IMF (Interoperable Master Format)"),
    .flags_internal = FF_FMT_INIT_CLEANUP,
    .priv_class     = &imf_class,
    .priv_data_size = sizeof(IMFContext),
    .read_header    = imf_read_header,
    .read_packet    = imf_read_packet,
    .read_close     = imf_close,
    .extensions     = "xml",
    .mime_type      = "application/xml,text/xml",
};
// clang-format on