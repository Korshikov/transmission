/*
 * This file Copyright (C) 2007-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#include <limits.h> /* INT_MAX */
#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <libtransmission/transmission.h>
#include <libtransmission/utils.h> /* tr_truncd() */
#include "hig.h"
#include "icons.h"
#include "torrent-cell-renderer.h"
#include "util.h"

/* #define TEST_RTL */

enum
{
    P_TORRENT = 1,
    P_UPLOAD_SPEED,
    P_DOWNLOAD_SPEED,
    P_BAR_HEIGHT,
    P_COMPACT
};

#define DEFAULT_BAR_HEIGHT 12
#define SMALL_SCALE 0.9
#define COMPACT_ICON_SIZE GTK_ICON_SIZE_MENU
#define FULL_ICON_SIZE GTK_ICON_SIZE_DND

/***
****
***/

static void getProgressString(GString* gstr, tr_torrent const* tor, tr_info const* info, tr_stat const* st)
{
    bool const isDone = st->leftUntilDone == 0;
    uint64_t const haveTotal = st->haveUnchecked + st->haveValid;
    bool const isSeed = st->haveValid >= info->totalSize;
    char buf1[32];
    char buf2[32];
    char buf3[32];
    char buf4[32];
    char buf5[32];
    char buf6[32];
    double seedRatio;
    bool const hasSeedRatio = tr_torrentGetSeedRatio(tor, &seedRatio);

    if (!isDone) /* downloading */
    {
        g_string_append_printf(
            gstr,
            /* %1$s is how much we've got,
               %2$s is how much we'll have when done,
               %3$s%% is a percentage of the two */
            _("%1$s of %2$s (%3$s%%)"),
            tr_strlsize(buf1, haveTotal, sizeof(buf1)),
            tr_strlsize(buf2, st->sizeWhenDone, sizeof(buf2)),
            tr_strlpercent(buf3, st->percentDone * 100.0, sizeof(buf3)));
    }
    else if (!isSeed) /* partial seeds */
    {
        if (hasSeedRatio)
        {
            g_string_append_printf(
                gstr,
                /* %1$s is how much we've got,
                   %2$s is the torrent's total size,
                   %3$s%% is a percentage of the two,
                   %4$s is how much we've uploaded,
                   %5$s is our upload-to-download ratio,
                   %6$s is the ratio we want to reach before we stop uploading */
                _("%1$s of %2$s (%3$s%%), uploaded %4$s (Ratio: %5$s Goal: %6$s)"),
                tr_strlsize(buf1, haveTotal, sizeof(buf1)),
                tr_strlsize(buf2, info->totalSize, sizeof(buf2)),
                tr_strlpercent(buf3, st->percentComplete * 100.0, sizeof(buf3)),
                tr_strlsize(buf4, st->uploadedEver, sizeof(buf4)),
                tr_strlratio(buf5, st->ratio, sizeof(buf5)),
                tr_strlratio(buf6, seedRatio, sizeof(buf6)));
        }
        else
        {
            g_string_append_printf(
                gstr,
                /* %1$s is how much we've got,
                   %2$s is the torrent's total size,
                   %3$s%% is a percentage of the two,
                   %4$s is how much we've uploaded,
                   %5$s is our upload-to-download ratio */
                _("%1$s of %2$s (%3$s%%), uploaded %4$s (Ratio: %5$s)"),
                tr_strlsize(buf1, haveTotal, sizeof(buf1)),
                tr_strlsize(buf2, info->totalSize, sizeof(buf2)),
                tr_strlpercent(buf3, st->percentComplete * 100.0, sizeof(buf3)),
                tr_strlsize(buf4, st->uploadedEver, sizeof(buf4)),
                tr_strlratio(buf5, st->ratio, sizeof(buf5)));
        }
    }
    else /* seeding */
    {
        if (hasSeedRatio)
        {
            g_string_append_printf(
                gstr,
                /* %1$s is the torrent's total size,
                   %2$s is how much we've uploaded,
                   %3$s is our upload-to-download ratio,
                   %4$s is the ratio we want to reach before we stop uploading */
                _("%1$s, uploaded %2$s (Ratio: %3$s Goal: %4$s)"),
                tr_strlsize(buf1, info->totalSize, sizeof(buf1)),
                tr_strlsize(buf2, st->uploadedEver, sizeof(buf2)),
                tr_strlratio(buf3, st->ratio, sizeof(buf3)),
                tr_strlratio(buf4, seedRatio, sizeof(buf4)));
        }
        else /* seeding w/o a ratio */
        {
            g_string_append_printf(
                gstr,
                /* %1$s is the torrent's total size,
                   %2$s is how much we've uploaded,
                   %3$s is our upload-to-download ratio */
                _("%1$s, uploaded %2$s (Ratio: %3$s)"),
                tr_strlsize(buf1, info->totalSize, sizeof(buf1)),
                tr_strlsize(buf2, st->uploadedEver, sizeof(buf2)),
                tr_strlratio(buf3, st->ratio, sizeof(buf3)));
        }
    }

    /* add time when downloading */
    if (st->activity == TR_STATUS_DOWNLOAD || (hasSeedRatio && st->activity == TR_STATUS_SEED))
    {
        int const eta = st->eta;
        g_string_append(gstr, " - ");

        if (eta < 0)
        {
            g_string_append(gstr, _("Remaining time unknown"));
        }
        else
        {
            char timestr[128];
            tr_strltime(timestr, eta, sizeof(timestr));
            /* time remaining */
            g_string_append_printf(gstr, _("%s remaining"), timestr);
        }
    }
}

static char* getShortTransferString(
    tr_torrent const* tor,
    tr_stat const* st,
    double uploadSpeed_KBps,
    double downloadSpeed_KBps,
    char* buf,
    size_t buflen)
{
    bool const haveMeta = tr_torrentHasMetadata(tor);
    bool const haveUp = haveMeta && st->peersGettingFromUs > 0;
    bool const haveDown = haveMeta && (st->peersSendingToUs > 0 || st->webseedsSendingToUs > 0);

    if (haveDown)
    {
        char dnStr[32];
        char upStr[32];
        tr_formatter_speed_KBps(dnStr, downloadSpeed_KBps, sizeof(dnStr));
        tr_formatter_speed_KBps(upStr, uploadSpeed_KBps, sizeof(upStr));

        /* down speed, down symbol, up speed, up symbol */
        g_snprintf(
            buf,
            buflen,
            _("%1$s %2$s  %3$s %4$s"),
            dnStr,
            gtr_get_unicode_string(GTR_UNICODE_DOWN),
            upStr,
            gtr_get_unicode_string(GTR_UNICODE_UP));
    }
    else if (haveUp)
    {
        char upStr[32];
        tr_formatter_speed_KBps(upStr, uploadSpeed_KBps, sizeof(upStr));

        /* up speed, up symbol */
        g_snprintf(buf, buflen, _("%1$s  %2$s"), upStr, gtr_get_unicode_string(GTR_UNICODE_UP));
    }
    else if (st->isStalled)
    {
        g_strlcpy(buf, _("Stalled"), buflen);
    }
    else
    {
        *buf = '\0';
    }

    return buf;
}

static void getShortStatusString(
    GString* gstr,
    tr_torrent const* tor,
    tr_stat const* st,
    double uploadSpeed_KBps,
    double downloadSpeed_KBps)
{
    switch (st->activity)
    {
    case TR_STATUS_STOPPED:
        g_string_append(gstr, st->finished ? _("Finished") : _("Paused"));
        break;

    case TR_STATUS_CHECK_WAIT:
        g_string_append(gstr, _("Queued for verification"));
        break;

    case TR_STATUS_DOWNLOAD_WAIT:
        g_string_append(gstr, _("Queued for download"));
        break;

    case TR_STATUS_SEED_WAIT:
        g_string_append(gstr, _("Queued for seeding"));
        break;

    case TR_STATUS_CHECK:
        g_string_append_printf(gstr, _("Verifying local data (%.1f%% tested)"), tr_truncd(st->recheckProgress * 100.0, 1));
        break;

    case TR_STATUS_DOWNLOAD:
    case TR_STATUS_SEED:
        {
            char speedStr[64];
            char ratioStr[64];
            tr_strlratio(ratioStr, st->ratio, sizeof(ratioStr));
            getShortTransferString(tor, st, uploadSpeed_KBps, downloadSpeed_KBps, speedStr, sizeof(speedStr));
            /* download/upload speed, ratio */
            g_string_append_printf(gstr, "%s  ", speedStr);
            g_string_append_printf(gstr, _("Ratio: %s"), ratioStr);
            break;
        }

    default:
        break;
    }
}

static void getStatusString(
    GString* gstr,
    tr_torrent const* tor,
    tr_stat const* st,
    double const uploadSpeed_KBps,
    double const downloadSpeed_KBps)
{
    if (st->error != 0)
    {
        char const* fmt[] = {
            nullptr,
            N_("Tracker gave a warning: \"%s\""),
            N_("Tracker gave an error: \"%s\""),
            N_("Error: %s"),
        };

        g_string_append_printf(gstr, _(fmt[st->error]), st->errorString);
    }
    else
    {
        switch (st->activity)
        {
        case TR_STATUS_STOPPED:
        case TR_STATUS_CHECK_WAIT:
        case TR_STATUS_CHECK:
        case TR_STATUS_DOWNLOAD_WAIT:
        case TR_STATUS_SEED_WAIT:
            {
                getShortStatusString(gstr, tor, st, uploadSpeed_KBps, downloadSpeed_KBps);
                break;
            }

        case TR_STATUS_DOWNLOAD:
            {
                if (!tr_torrentHasMetadata(tor))
                {
                    /* Downloading metadata from 2 peer (s)(50% done) */
                    g_string_append_printf(
                        gstr,
                        _("Downloading metadata from %1$'d %2$s (%3$d%% done)"),
                        st->peersConnected,
                        ngettext("peer", "peers", st->peersConnected),
                        (int)(100.0 * st->metadataPercentComplete));
                }
                else if (st->peersSendingToUs != 0 && st->webseedsSendingToUs != 0)
                {
                    /* Downloading from 2 of 3 peer (s) and 2 webseed (s) */
                    g_string_append_printf(
                        gstr,
                        _("Downloading from %1$'d of %2$'d %3$s and %4$'d %5$s"),
                        st->peersSendingToUs,
                        st->peersConnected,
                        ngettext("peer", "peers", st->peersConnected),
                        st->webseedsSendingToUs,
                        ngettext("web seed", "web seeds", st->webseedsSendingToUs));
                }
                else if (st->webseedsSendingToUs != 0)
                {
                    /* Downloading from 3 web seed (s) */
                    g_string_append_printf(
                        gstr,
                        _("Downloading from %1$'d %2$s"),
                        st->webseedsSendingToUs,
                        ngettext("web seed", "web seeds", st->webseedsSendingToUs));
                }
                else
                {
                    /* Downloading from 2 of 3 peer (s) */
                    g_string_append_printf(
                        gstr,
                        _("Downloading from %1$'d of %2$'d %3$s"),
                        st->peersSendingToUs,
                        st->peersConnected,
                        ngettext("peer", "peers", st->peersConnected));
                }

                break;
            }

        case TR_STATUS_SEED:
            g_string_append_printf(
                gstr,
                ngettext(
                    "Seeding to %1$'d of %2$'d connected peer",
                    "Seeding to %1$'d of %2$'d connected peers",
                    st->peersConnected),
                st->peersGettingFromUs,
                st->peersConnected);
            break;
        }
    }

    if (st->activity != TR_STATUS_CHECK_WAIT && st->activity != TR_STATUS_CHECK && st->activity != TR_STATUS_DOWNLOAD_WAIT &&
        st->activity != TR_STATUS_SEED_WAIT && st->activity != TR_STATUS_STOPPED)
    {
        char buf[256];
        getShortTransferString(tor, st, uploadSpeed_KBps, downloadSpeed_KBps, buf, sizeof(buf));

        if (!tr_str_is_empty(buf))
        {
            g_string_append_printf(gstr, " - %s", buf);
        }
    }
}

/***
****
***/

typedef struct TorrentCellRendererPrivate
{
    tr_torrent* tor;
    GtkCellRenderer* text_renderer;
    GtkCellRenderer* progress_renderer;
    GtkCellRenderer* icon_renderer;
    GString* gstr1;
    GString* gstr2;
    int bar_height;

    /* Use this instead of tr_stat.pieceUploadSpeed so that the model can
       control when the speed displays get updated. This is done to keep
       the individual torrents' speeds and the status bar's overall speed
       in sync even if they refresh at slightly different times */
    double upload_speed_KBps;

    /* @see upload_speed_Bps */
    double download_speed_KBps;

    gboolean compact;
} TorrentCellRendererPrivate;

/***
****
***/

static GdkPixbuf* get_icon(tr_torrent const* tor, GtkIconSize icon_size, GtkWidget* for_widget)
{
    char const* mime_type;
    tr_info const* info = tr_torrentInfo(tor);

    if (info->fileCount == 0)
    {
        mime_type = UNKNOWN_MIME_TYPE;
    }
    else if (info->fileCount > 1)
    {
        mime_type = DIRECTORY_MIME_TYPE;
    }
    else if (strchr(info->files[0].name, '/') != nullptr)
    {
        mime_type = DIRECTORY_MIME_TYPE;
    }
    else
    {
        mime_type = gtr_get_mime_type_from_filename(info->files[0].name);
    }

    return gtr_get_mime_type_icon(mime_type, icon_size, for_widget);
}

/***
****
***/

static void gtr_cell_renderer_get_preferred_size(
    GtkCellRenderer* renderer,
    GtkWidget* widget,
    GtkRequisition* minimum_size,
    GtkRequisition* natural_size)
{
    gtk_cell_renderer_get_preferred_size(renderer, widget, minimum_size, natural_size);
}

static void get_size_compact(TorrentCellRenderer* cell, GtkWidget* widget, gint* width, gint* height)
{
    int xpad;
    int ypad;
    GtkRequisition icon_size;
    GtkRequisition name_size;
    GtkRequisition stat_size;
    char const* name;
    GdkPixbuf* icon;

    struct TorrentCellRendererPrivate* p = cell->priv;
    tr_torrent* tor = p->tor;
    tr_stat const* st = tr_torrentStatCached(tor);
    GString* gstr_stat = p->gstr1;

    icon = get_icon(tor, COMPACT_ICON_SIZE, widget);
    name = tr_torrentName(tor);
    g_string_truncate(gstr_stat, 0);
    getShortStatusString(gstr_stat, tor, st, p->upload_speed_KBps, p->download_speed_KBps);
    gtk_cell_renderer_get_padding(GTK_CELL_RENDERER(cell), &xpad, &ypad);

    /* get the idealized cell dimensions */
    g_object_set(p->icon_renderer, "pixbuf", icon, nullptr);
    gtr_cell_renderer_get_preferred_size(p->icon_renderer, widget, nullptr, &icon_size);
    g_object_set(p->text_renderer, "text", name, "ellipsize", PANGO_ELLIPSIZE_NONE, "scale", 1.0, nullptr);
    gtr_cell_renderer_get_preferred_size(p->text_renderer, widget, nullptr, &name_size);
    g_object_set(p->text_renderer, "text", gstr_stat->str, "scale", SMALL_SCALE, nullptr);
    gtr_cell_renderer_get_preferred_size(p->text_renderer, widget, nullptr, &stat_size);

    /**
    *** LAYOUT
    **/

#define BAR_WIDTH 50

    if (width != nullptr)
    {
        *width = xpad * 2 + icon_size.width + GUI_PAD + BAR_WIDTH + GUI_PAD + stat_size.width;
    }

    if (height != nullptr)
    {
        *height = ypad * 2 + MAX(name_size.height, p->bar_height);
    }

    /* cleanup */
    g_object_unref(icon);
}

static void get_size_full(TorrentCellRenderer* cell, GtkWidget* widget, gint* width, gint* height)
{
    int xpad;
    int ypad;
    GtkRequisition icon_size;
    GtkRequisition name_size;
    GtkRequisition stat_size;
    GtkRequisition prog_size;
    char const* name;
    GdkPixbuf* icon;

    struct TorrentCellRendererPrivate* p = cell->priv;
    tr_torrent* tor = p->tor;
    tr_stat const* st = tr_torrentStatCached(tor);
    tr_info const* inf = tr_torrentInfo(tor);
    GString* gstr_prog = p->gstr1;
    GString* gstr_stat = p->gstr2;

    icon = get_icon(tor, FULL_ICON_SIZE, widget);
    name = tr_torrentName(tor);
    g_string_truncate(gstr_stat, 0);
    getStatusString(gstr_stat, tor, st, p->upload_speed_KBps, p->download_speed_KBps);
    g_string_truncate(gstr_prog, 0);
    getProgressString(gstr_prog, tor, inf, st);
    gtk_cell_renderer_get_padding(GTK_CELL_RENDERER(cell), &xpad, &ypad);

    /* get the idealized cell dimensions */
    g_object_set(p->icon_renderer, "pixbuf", icon, nullptr);
    gtr_cell_renderer_get_preferred_size(p->icon_renderer, widget, nullptr, &icon_size);
    g_object_set(
        p->text_renderer,
        TR_ARG_TUPLE("text", name),
        TR_ARG_TUPLE("weight", PANGO_WEIGHT_BOLD),
        TR_ARG_TUPLE("scale", 1.0),
        TR_ARG_TUPLE("ellipsize", PANGO_ELLIPSIZE_NONE),
        nullptr);
    gtr_cell_renderer_get_preferred_size(p->text_renderer, widget, nullptr, &name_size);
    g_object_set(p->text_renderer, "text", gstr_prog->str, "weight", PANGO_WEIGHT_NORMAL, "scale", SMALL_SCALE, nullptr);
    gtr_cell_renderer_get_preferred_size(p->text_renderer, widget, nullptr, &prog_size);
    g_object_set(p->text_renderer, "text", gstr_stat->str, nullptr);
    gtr_cell_renderer_get_preferred_size(p->text_renderer, widget, nullptr, &stat_size);

    /**
    *** LAYOUT
    **/

    if (width != nullptr)
    {
        *width = xpad * 2 + icon_size.width + GUI_PAD + MAX(prog_size.width, stat_size.width);
    }

    if (height != nullptr)
    {
        *height = ypad * 2 + name_size.height + prog_size.height + GUI_PAD_SMALL + p->bar_height + GUI_PAD_SMALL +
            stat_size.height;
    }

    /* cleanup */
    g_object_unref(icon);
}

static void torrent_cell_renderer_get_size(
    GtkCellRenderer* cell,
    GtkWidget* widget,
    GdkRectangle const* cell_area,
    gint* x_offset,
    gint* y_offset,
    gint* width,
    gint* height)
{
    TorrentCellRenderer const* const self = TORRENT_CELL_RENDERER(cell);

    if (self != nullptr && self->priv->tor != nullptr)
    {
        int w;
        int h;
        struct TorrentCellRendererPrivate const* const p = self->priv;

        if (p->compact)
        {
            get_size_compact(TORRENT_CELL_RENDERER(cell), widget, &w, &h);
        }
        else
        {
            get_size_full(TORRENT_CELL_RENDERER(cell), widget, &w, &h);
        }

        if (width != nullptr)
        {
            *width = w;
        }

        if (height != nullptr)
        {
            *height = h;
        }

        if (x_offset != nullptr)
        {
            *x_offset = cell_area ? cell_area->x : 0;
        }

        if (y_offset != nullptr)
        {
            int xpad;
            int ypad;
            gtk_cell_renderer_get_padding(cell, &xpad, &ypad);
            *y_offset = cell_area ? (int)((cell_area->height - (ypad * 2 + h)) / 2.0) : 0;
        }
    }
}

typedef GdkRGBA GtrColor;
#define FOREGROUND_COLOR_KEY "foreground-rgba"

static void get_text_color(GtkWidget* w, tr_stat const* st, GtrColor* setme)
{
    static GdkRGBA const red = { 1.0, 0, 0, 0 };

    if (st->error != 0)
    {
        *setme = red;
    }
    else if (st->activity == TR_STATUS_STOPPED)
    {
        gtk_style_context_get_color(gtk_widget_get_style_context(w), GTK_STATE_FLAG_INSENSITIVE, setme);
    }
    else
    {
        gtk_style_context_get_color(gtk_widget_get_style_context(w), GTK_STATE_FLAG_NORMAL, setme);
    }
}

static double get_percent_done(tr_torrent const* tor, tr_stat const* st, bool* seed)
{
    double d;

    if (st->activity == TR_STATUS_SEED && tr_torrentGetSeedRatio(tor, &d))
    {
        *seed = true;
        d = MAX(0.0, st->seedRatioPercentDone);
    }
    else
    {
        *seed = false;
        d = MAX(0.0, st->percentDone);
    }

    return d;
}

typedef cairo_t GtrDrawable;

static void gtr_cell_renderer_render(
    GtkCellRenderer* renderer,
    GtrDrawable* drawable,
    GtkWidget* widget,
    GdkRectangle const* area,
    GtkCellRendererState flags)
{
    gtk_cell_renderer_render(renderer, drawable, widget, area, area, flags);
}

static void render_compact(
    TorrentCellRenderer* cell,
    GtrDrawable* window,
    GtkWidget* widget,
    GdkRectangle const* background_area,
    GdkRectangle const* cell_area,
    GtkCellRendererState flags)
{
    TR_UNUSED(cell_area);

    int xpad;
    int ypad;
    GtkRequisition size;
    GdkRectangle icon_area;
    GdkRectangle name_area;
    GdkRectangle stat_area;
    GdkRectangle prog_area;
    GdkRectangle fill_area;
    char const* name;
    GdkPixbuf* icon;
    GtrColor text_color;
    bool seed;

    struct TorrentCellRendererPrivate* p = cell->priv;
    tr_torrent* tor = p->tor;
    tr_stat const* st = tr_torrentStatCached(tor);
    gboolean const active = st->activity != TR_STATUS_STOPPED && st->activity != TR_STATUS_DOWNLOAD_WAIT &&
        st->activity != TR_STATUS_SEED_WAIT;
    double const percentDone = get_percent_done(tor, st, &seed);
    gboolean const sensitive = active || st->error;
    GString* gstr_stat = p->gstr1;

    icon = get_icon(tor, COMPACT_ICON_SIZE, widget);
    name = tr_torrentName(tor);
    g_string_truncate(gstr_stat, 0);
    getShortStatusString(gstr_stat, tor, st, p->upload_speed_KBps, p->download_speed_KBps);
    gtk_cell_renderer_get_padding(GTK_CELL_RENDERER(cell), &xpad, &ypad);
    get_text_color(widget, st, &text_color);

    fill_area = *background_area;
    fill_area.x += xpad;
    fill_area.y += ypad;
    fill_area.width -= xpad * 2;
    fill_area.height -= ypad * 2;
    icon_area = name_area = stat_area = prog_area = fill_area;

    g_object_set(p->icon_renderer, "pixbuf", icon, nullptr);
    gtr_cell_renderer_get_preferred_size(p->icon_renderer, widget, nullptr, &size);
    icon_area.width = size.width;
    g_object_set(p->text_renderer, "text", name, "ellipsize", PANGO_ELLIPSIZE_NONE, "scale", 1.0, nullptr);
    gtr_cell_renderer_get_preferred_size(p->text_renderer, widget, nullptr, &size);
    name_area.width = size.width;
    g_object_set(p->text_renderer, "text", gstr_stat->str, "scale", SMALL_SCALE, nullptr);
    gtr_cell_renderer_get_preferred_size(p->text_renderer, widget, nullptr, &size);
    stat_area.width = size.width;

    icon_area.x = fill_area.x;
    prog_area.x = fill_area.x + fill_area.width - BAR_WIDTH;
    prog_area.width = BAR_WIDTH;
    stat_area.x = prog_area.x - GUI_PAD - stat_area.width;
    name_area.x = icon_area.x + icon_area.width + GUI_PAD;
    name_area.y = fill_area.y;
    name_area.width = stat_area.x - GUI_PAD - name_area.x;

    /**
    *** RENDER
    **/

    g_object_set(p->icon_renderer, "pixbuf", icon, "sensitive", sensitive, nullptr);
    gtr_cell_renderer_render(p->icon_renderer, window, widget, &icon_area, flags);
    g_object_set(p->progress_renderer, "value", (int)(percentDone * 100.0), "text", nullptr, "sensitive", sensitive, nullptr);
    gtr_cell_renderer_render(p->progress_renderer, window, widget, &prog_area, flags);
    g_object_set(
        p->text_renderer,
        TR_ARG_TUPLE("text", gstr_stat->str),
        TR_ARG_TUPLE("scale", SMALL_SCALE),
        TR_ARG_TUPLE("ellipsize", PANGO_ELLIPSIZE_END),
        TR_ARG_TUPLE(FOREGROUND_COLOR_KEY, &text_color),
        nullptr);
    gtr_cell_renderer_render(p->text_renderer, window, widget, &stat_area, flags);
    g_object_set(p->text_renderer, "text", name, "scale", 1.0, FOREGROUND_COLOR_KEY, &text_color, nullptr);
    gtr_cell_renderer_render(p->text_renderer, window, widget, &name_area, flags);

    /* cleanup */
    g_object_unref(icon);
}

static void render_full(
    TorrentCellRenderer* cell,
    GtrDrawable* window,
    GtkWidget* widget,
    GdkRectangle const* background_area,
    GdkRectangle const* cell_area,
    GtkCellRendererState flags)
{
    TR_UNUSED(cell_area);

    int xpad;
    int ypad;
    GtkRequisition size;
    GdkRectangle fill_area;
    GdkRectangle icon_area;
    GdkRectangle name_area;
    GdkRectangle stat_area;
    GdkRectangle prog_area;
    GdkRectangle prct_area;
    char const* name;
    GdkPixbuf* icon;
    GtrColor text_color;
    bool seed;

    struct TorrentCellRendererPrivate* p = cell->priv;
    tr_torrent* tor = p->tor;
    tr_stat const* st = tr_torrentStatCached(tor);
    tr_info const* inf = tr_torrentInfo(tor);
    gboolean const active = st->activity != TR_STATUS_STOPPED && st->activity != TR_STATUS_DOWNLOAD_WAIT &&
        st->activity != TR_STATUS_SEED_WAIT;
    double const percentDone = get_percent_done(tor, st, &seed);
    gboolean const sensitive = active || st->error;
    GString* gstr_prog = p->gstr1;
    GString* gstr_stat = p->gstr2;

    icon = get_icon(tor, FULL_ICON_SIZE, widget);
    name = tr_torrentName(tor);
    g_string_truncate(gstr_prog, 0);
    getProgressString(gstr_prog, tor, inf, st);
    g_string_truncate(gstr_stat, 0);
    getStatusString(gstr_stat, tor, st, p->upload_speed_KBps, p->download_speed_KBps);
    gtk_cell_renderer_get_padding(GTK_CELL_RENDERER(cell), &xpad, &ypad);
    get_text_color(widget, st, &text_color);

    /* get the idealized cell dimensions */
    g_object_set(p->icon_renderer, "pixbuf", icon, nullptr);
    gtr_cell_renderer_get_preferred_size(p->icon_renderer, widget, nullptr, &size);
    icon_area.width = size.width;
    icon_area.height = size.height;
    g_object_set(
        p->text_renderer,
        TR_ARG_TUPLE("text", name),
        TR_ARG_TUPLE("weight", PANGO_WEIGHT_BOLD),
        TR_ARG_TUPLE("ellipsize", PANGO_ELLIPSIZE_NONE),
        TR_ARG_TUPLE("scale", 1.0),
        nullptr);
    gtr_cell_renderer_get_preferred_size(p->text_renderer, widget, nullptr, &size);
    name_area.width = size.width;
    name_area.height = size.height;
    g_object_set(p->text_renderer, "text", gstr_prog->str, "weight", PANGO_WEIGHT_NORMAL, "scale", SMALL_SCALE, nullptr);
    gtr_cell_renderer_get_preferred_size(p->text_renderer, widget, nullptr, &size);
    prog_area.width = size.width;
    prog_area.height = size.height;
    g_object_set(p->text_renderer, "text", gstr_stat->str, nullptr);
    gtr_cell_renderer_get_preferred_size(p->text_renderer, widget, nullptr, &size);
    stat_area.width = size.width;
    stat_area.height = size.height;

    /**
    *** LAYOUT
    **/

    fill_area = *background_area;
    fill_area.x += xpad;
    fill_area.y += ypad;
    fill_area.width -= xpad * 2;
    fill_area.height -= ypad * 2;

    /* icon */
    icon_area.x = fill_area.x;
    icon_area.y = fill_area.y + (fill_area.height - icon_area.height) / 2;

    /* name */
    name_area.x = icon_area.x + icon_area.width + GUI_PAD;
    name_area.y = fill_area.y;
    name_area.width = fill_area.width - GUI_PAD - icon_area.width - GUI_PAD_SMALL;

    /* prog */
    prog_area.x = name_area.x;
    prog_area.y = name_area.y + name_area.height;
    prog_area.width = name_area.width;

    /* progressbar */
    prct_area.x = prog_area.x;
    prct_area.y = prog_area.y + prog_area.height + GUI_PAD_SMALL;
    prct_area.width = prog_area.width;
    prct_area.height = p->bar_height;

    /* status */
    stat_area.x = prct_area.x;
    stat_area.y = prct_area.y + prct_area.height + GUI_PAD_SMALL;
    stat_area.width = prct_area.width;

    /**
    *** RENDER
    **/

    g_object_set(p->icon_renderer, "pixbuf", icon, "sensitive", sensitive, nullptr);
    gtr_cell_renderer_render(p->icon_renderer, window, widget, &icon_area, flags);
    g_object_set(
        p->text_renderer,
        TR_ARG_TUPLE("text", name),
        TR_ARG_TUPLE("scale", 1.0),
        TR_ARG_TUPLE(FOREGROUND_COLOR_KEY, &text_color),
        TR_ARG_TUPLE("ellipsize", PANGO_ELLIPSIZE_END),
        TR_ARG_TUPLE("weight", PANGO_WEIGHT_BOLD),
        nullptr);
    gtr_cell_renderer_render(p->text_renderer, window, widget, &name_area, flags);
    g_object_set(p->text_renderer, "text", gstr_prog->str, "scale", SMALL_SCALE, "weight", PANGO_WEIGHT_NORMAL, nullptr);
    gtr_cell_renderer_render(p->text_renderer, window, widget, &prog_area, flags);
    g_object_set(p->progress_renderer, "value", (int)(percentDone * 100.0), "text", "", "sensitive", sensitive, nullptr);
    gtr_cell_renderer_render(p->progress_renderer, window, widget, &prct_area, flags);
    g_object_set(p->text_renderer, "text", gstr_stat->str, FOREGROUND_COLOR_KEY, &text_color, nullptr);
    gtr_cell_renderer_render(p->text_renderer, window, widget, &stat_area, flags);

    /* cleanup */
    g_object_unref(icon);
}

static void torrent_cell_renderer_render(
    GtkCellRenderer* cell,
    GtrDrawable* window,
    GtkWidget* widget,
    GdkRectangle const* background_area,
    GdkRectangle const* cell_area,
    GtkCellRendererState flags)
{
    TorrentCellRenderer* self = TORRENT_CELL_RENDERER(cell);

#ifdef TEST_RTL
    GtkTextDirection real_dir = gtk_widget_get_direction(widget);
    gtk_widget_set_direction(widget, GTK_TEXT_DIR_RTL);
#endif

    if (self != nullptr && self->priv->tor != nullptr)
    {
        struct TorrentCellRendererPrivate const* const p = self->priv;

        if (p->compact)
        {
            render_compact(self, window, widget, background_area, cell_area, flags);
        }
        else
        {
            render_full(self, window, widget, background_area, cell_area, flags);
        }
    }

#ifdef TEST_RTL
    gtk_widget_set_direction(widget, real_dir);
#endif
}

static void torrent_cell_renderer_set_property(GObject* object, guint property_id, GValue const* v, GParamSpec* pspec)
{
    TorrentCellRenderer* self = TORRENT_CELL_RENDERER(object);
    struct TorrentCellRendererPrivate* p = self->priv;

    switch (property_id)
    {
    case P_TORRENT:
        p->tor = static_cast<tr_torrent*>(g_value_get_pointer(v));
        break;

    case P_UPLOAD_SPEED:
        p->upload_speed_KBps = g_value_get_double(v);
        break;

    case P_DOWNLOAD_SPEED:
        p->download_speed_KBps = g_value_get_double(v);
        break;

    case P_BAR_HEIGHT:
        p->bar_height = g_value_get_int(v);
        break;

    case P_COMPACT:
        p->compact = g_value_get_boolean(v);
        break;

    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void torrent_cell_renderer_get_property(GObject* object, guint property_id, GValue* v, GParamSpec* pspec)
{
    TorrentCellRenderer const* self = TORRENT_CELL_RENDERER(object);
    struct TorrentCellRendererPrivate* p = self->priv;

    switch (property_id)
    {
    case P_TORRENT:
        g_value_set_pointer(v, p->tor);
        break;

    case P_UPLOAD_SPEED:
        g_value_set_double(v, p->upload_speed_KBps);
        break;

    case P_DOWNLOAD_SPEED:
        g_value_set_double(v, p->download_speed_KBps);
        break;

    case P_BAR_HEIGHT:
        g_value_set_int(v, p->bar_height);
        break;

    case P_COMPACT:
        g_value_set_boolean(v, p->compact);
        break;

    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

G_DEFINE_TYPE_WITH_CODE(TorrentCellRenderer, torrent_cell_renderer, GTK_TYPE_CELL_RENDERER, G_ADD_PRIVATE(TorrentCellRenderer))

static void torrent_cell_renderer_dispose(GObject* o)
{
    TorrentCellRenderer* r = TORRENT_CELL_RENDERER(o);

    if (r != nullptr && r->priv != nullptr)
    {
        g_string_free(r->priv->gstr1, TRUE);
        g_string_free(r->priv->gstr2, TRUE);
        g_object_unref(G_OBJECT(r->priv->text_renderer));
        g_object_unref(G_OBJECT(r->priv->progress_renderer));
        g_object_unref(G_OBJECT(r->priv->icon_renderer));
        r->priv = nullptr;
    }

    G_OBJECT_CLASS(torrent_cell_renderer_parent_class)->dispose(o);
}

static void torrent_cell_renderer_class_init(TorrentCellRendererClass* klass)
{
    GObjectClass* gobject_class = G_OBJECT_CLASS(klass);
    GtkCellRendererClass* cell_class = GTK_CELL_RENDERER_CLASS(klass);

    cell_class->render = torrent_cell_renderer_render;
    cell_class->get_size = torrent_cell_renderer_get_size;
    gobject_class->set_property = torrent_cell_renderer_set_property;
    gobject_class->get_property = torrent_cell_renderer_get_property;
    gobject_class->dispose = torrent_cell_renderer_dispose;

    g_object_class_install_property(
        gobject_class,
        P_TORRENT,
        g_param_spec_pointer("torrent", nullptr, "tr_torrent*", G_PARAM_READWRITE));

    g_object_class_install_property(
        gobject_class,
        P_UPLOAD_SPEED,
        g_param_spec_double("piece-upload-speed", nullptr, "tr_stat.pieceUploadSpeed_KBps", 0, INT_MAX, 0, G_PARAM_READWRITE));

    g_object_class_install_property(
        gobject_class,
        P_DOWNLOAD_SPEED,
        g_param_spec_double(
            "piece-download-speed",
            nullptr,
            "tr_stat.pieceDownloadSpeed_KBps",
            0,
            INT_MAX,
            0,
            G_PARAM_READWRITE));

    g_object_class_install_property(
        gobject_class,
        P_BAR_HEIGHT,
        g_param_spec_int("bar-height", nullptr, "Bar Height", 1, INT_MAX, DEFAULT_BAR_HEIGHT, G_PARAM_READWRITE));

    g_object_class_install_property(
        gobject_class,
        P_COMPACT,
        g_param_spec_boolean("compact", nullptr, "Compact Mode", FALSE, G_PARAM_READWRITE));
}

static void torrent_cell_renderer_init(TorrentCellRenderer* self)
{
    struct TorrentCellRendererPrivate* p;

#if GLIB_CHECK_VERSION(2, 58, 0)
    p = self->priv = static_cast<TorrentCellRendererPrivate*>(torrent_cell_renderer_get_instance_private(self));
#else
    p = self->priv = G_TYPE_INSTANCE_GET_PRIVATE(self, TORRENT_CELL_RENDERER_TYPE, struct TorrentCellRendererPrivate);
#endif

    p->tor = nullptr;
    p->gstr1 = g_string_new(nullptr);
    p->gstr2 = g_string_new(nullptr);
    p->text_renderer = gtk_cell_renderer_text_new();
    g_object_set(p->text_renderer, "xpad", 0, "ypad", 0, nullptr);
    p->progress_renderer = gtk_cell_renderer_progress_new();
    p->icon_renderer = gtk_cell_renderer_pixbuf_new();
    g_object_ref_sink(p->text_renderer);
    g_object_ref_sink(p->progress_renderer);
    g_object_ref_sink(p->icon_renderer);

    p->bar_height = DEFAULT_BAR_HEIGHT;
}

GtkCellRenderer* torrent_cell_renderer_new(void)
{
    return (GtkCellRenderer*)g_object_new(TORRENT_CELL_RENDERER_TYPE, nullptr);
}
