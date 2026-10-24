/*
 * tui.cc
 * Copyright 2026 Erik Van Hamme
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the documentation
 *    provided with the distribution.
 *
 * This software is provided "as is" and without any warranty, express or
 * implied. In no event shall the authors be liable for any damages arising from
 * the use of this software.
 */

#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include <glib-unix.h>
#include <notcurses/notcurses.h>

#include <libaudcore/audstrings.h>
#include <libaudcore/drct.h>
#include <libaudcore/hook.h>
#include <libaudcore/i18n.h>
#include <libaudcore/interface.h>
#include <libaudcore/mainloop.h>
#include <libaudcore/objects.h>
#include <libaudcore/playlist.h>
#include <libaudcore/plugin.h>
#include <libaudcore/runtime.h>
#include <libaudcore/tuple.h>

class TUI : public IfacePlugin
{
public:
    static constexpr PluginInfo info = {N_("Terminal Interface"), PACKAGE,
                                        nullptr, nullptr, PluginTUIOnly};

    constexpr TUI() : IfacePlugin(info) {}

    bool init() override;
    void cleanup() override;

    // the terminal cannot be hidden
    void show(bool show) override {}

    void run() override;
    void quit() override { mainloop_quit(); }

    void show_about_window() override {}
    void hide_about_window() override {}
    void show_filebrowser(bool open) override {}
    void hide_filebrowser() override {}
    void show_jump_to_song() override {}
    void hide_jump_to_song() override {}
    void show_prefs_window() override {}
    void hide_prefs_window() override {}
    void plugin_menu_add(AudMenuID id, void func(), const char * name,
                         const char * icon) override
    {
    }
    void plugin_menu_remove(AudMenuID id, void func()) override {}
};

EXPORT TUI aud_plugin_instance;

static struct notcurses * nc;
static unsigned input_source, resize_source;

// Nerd Font glyphs (Font Awesome range)
#define ICON_PLAY "\uf04b"
#define ICON_PAUSE "\uf04c"

// A subtle shade for backgrounds (the focused playlist row and the unplayed
// part of the progress bar): the terminal's own background, shifted
// SUBTLE_MIX of the way towards its text colour.  That gives a grey that
// suits dark and light themes alike, on which the accent colour stays readable.
#define SUBTLE_MIX 0.15

// A muted colour for text that should recede (the playlist's column
// headers): the terminal's text colour, shifted MUTED_MIX of the way
// towards its background.
#define MUTED_MIX 0.4

// used if the terminal did not tell notcurses its default colours
#define SUBTLE_FALLBACK_RGB 0x262626
#define MUTED_FALLBACK_RGB 0x8a8a8a

static uint32_t subtle_rgb, muted_rgb;

// mixes the fraction f of colour b into colour a
static uint32_t mix_rgb(uint32_t a, uint32_t b, double f)
{
    uint32_t mixed = 0;

    for (int shift = 0; shift <= 16; shift += 8)
    {
        int ca = (a >> shift) & 0xff;
        int cb = (b >> shift) & 0xff;
        mixed |= (uint32_t)(ca + (cb - ca) * f + 0.5) << shift;
    }

    return mixed;
}

static void update_theme_colours()
{
    uint32_t bg, fg;

    if (notcurses_default_background(nc, &bg) == 0 &&
        notcurses_default_foreground(nc, &fg) == 0)
    {
        subtle_rgb = mix_rgb(bg, fg, SUBTLE_MIX);
        muted_rgb = mix_rgb(fg, bg, MUTED_MIX);
    }
    else
    {
        subtle_rgb = SUBTLE_FALLBACK_RGB;
        muted_rgb = MUTED_FALLBACK_RGB;
    }
}

// colour of the played part, as an index into the terminal's palette so that
// it follows the terminal's colour scheme (4 is blue)
#define ACCENT_PALETTE_INDEX 4

// used if the terminal did not tell notcurses what its palette is
#define ACCENT_FALLBACK_RGB 0x3080e0

// ncprogbar only takes RGB colours, so look up what the terminal's palette
// entry is set to (notcurses asks the terminal for its palette at startup)
static uint32_t accent_channel()
{
    uint32_t rgb = 0;
    ncpalette * palette = ncpalette_new(nc);

    if (palette)
    {
        ncpalette_get(palette, ACCENT_PALETTE_INDEX, &rgb);
        ncpalette_free(palette);
    }

    if (!rgb)
        rgb = ACCENT_FALLBACK_RGB;

    uint32_t channel = 0;
    ncchannel_set(&channel, rgb);
    return channel;
}

// information about the current song, empty when nothing is playing
static String song_title, song_artist, song_album;

#define TOP_BOX_HEIGHT 4
#define BOTTOM_BOX_HEIGHT 5

// Each box is a plane with a rounded border along its edges, plus an inner
// plane one cell inside the border for its contents.  Both planes resize
// themselves when the terminal is resized, through their resize callbacks.
// (NCPLANE_OPTION_MARGINALIZED would do the same, but in notcurses 3.0.17
// ncplane_resize_marginalized() moves the plane away from its margins.)
struct Box
{
    ncplane * outer = nullptr;
    ncplane * inner = nullptr;
};

static Box top_box, main_box, bottom_box;

// on the top box's inner plane
static ncprogbar * progbar;

// keeps the top box TOP_BOX_HEIGHT rows tall and as wide as the terminal
// moves and resizes a plane, keeping it at least one cell in size
static int place_plane(ncplane * plane, int y, int x, int rows, int cols)
{

    if (ncplane_move_yx(plane, y, x) < 0)
        return -1;

    return ncplane_resize_simple(plane, aud::max(rows, 1), aud::max(cols, 1));
}

static int top_box_resized(ncplane * plane)
{
    unsigned cols;
    ncplane_dim_yx(ncplane_parent(plane), nullptr, &cols);

    return place_plane(plane, 0, 0, TOP_BOX_HEIGHT, cols);
}

// keeps the main box filling the space between the other two boxes
static int main_box_resized(ncplane * plane)
{
    unsigned rows, cols;
    ncplane_dim_yx(ncplane_parent(plane), &rows, &cols);

    return place_plane(plane, TOP_BOX_HEIGHT, 0,
                       (int)rows - TOP_BOX_HEIGHT - BOTTOM_BOX_HEIGHT, cols);
}

// keeps the bottom box BOTTOM_BOX_HEIGHT rows tall, at the bottom of the
// terminal and as wide as it
static int bottom_box_resized(ncplane * plane)
{
    unsigned rows, cols;
    ncplane_dim_yx(ncplane_parent(plane), &rows, &cols);

    int height = aud::min((int)rows, BOTTOM_BOX_HEIGHT);

    return place_plane(plane, (int)rows - height, 0, height, cols);
}

// keeps an inner plane one cell inside its box's border
static int inner_resized(ncplane * plane)
{
    unsigned rows, cols;
    ncplane_dim_yx(ncplane_parent(plane), &rows, &cols);

    return place_plane(plane, 1, 1, (int)rows - 2, (int)cols - 2);
}

// Creates a box whose outer plane is placed by the given resize callback.
// Both planes start out as a single cell and are then put into place by
// calling their callbacks, just as when the terminal is resized.
static bool create_box(Box & box, ncplane * parent,
                       int (*resized)(ncplane *))
{
    ncplane_options opts = {};
    opts.rows = 1;
    opts.cols = 1;

    opts.resizecb = resized;
    if (!(box.outer = ncplane_create(parent, &opts)))
        return false;

    opts.resizecb = inner_resized;
    if (!(box.inner = ncplane_create(box.outer, &opts)))
        return false;

    return resized(box.outer) == 0 && inner_resized(box.inner) == 0;
}

static bool create_boxes()
{
    ncplane * parent = notcurses_stdplane(nc);

    return create_box(top_box, parent, top_box_resized) &&
           create_box(main_box, parent, main_box_resized) &&
           create_box(bottom_box, parent, bottom_box_resized);
}

static void destroy_boxes()
{
    if (progbar)
    {
        ncprogbar_destroy(progbar);
        progbar = nullptr;
    }

    for (Box * box : {&top_box, &main_box, &bottom_box})
    {
        if (box->outer)
            ncplane_family_destroy(box->outer);

        *box = Box();
    }
}

static void draw_border(ncplane * plane)
{
    unsigned rows, cols;
    ncplane_dim_yx(plane, &rows, &cols);

    ncplane_erase(plane);

    // a box needs at least two rows and columns for its corners
    if (rows < 2 || cols < 2)
        return;

    ncplane_cursor_move_yx(plane, 0, 0);
    ncplane_rounded_box_sized(plane, 0, 0, rows, cols, 0);
}

// Shows the progress bar (an ncprogbar on its own plane) at the given place,
// creating it the first time.  It is destroyed when there is no room for it.
static void update_progress_bar(ncplane * parent, int y, int x, int width,
                                double progress)
{
    if (width < 1)
    {
        if (progbar)
        {
            ncprogbar_destroy(progbar);
            progbar = nullptr;
        }

        return;
    }

    if (progbar)
    {
        ncplane * plane = ncprogbar_plane(progbar);
        ncplane_move_yx(plane, y, x);
        ncplane_resize_simple(plane, 1, width);
    }
    else
    {
        ncplane_options popts = {};
        popts.y = y;
        popts.x = x;
        popts.rows = 1;
        popts.cols = width;

        ncplane * plane = ncplane_create(parent, &popts);
        if (!plane)
            return;

        // the unplayed part, which ncprogbar leaves alone; as a background
        // colour it also fills the rest of the cell where the bar ends
        uint64_t track = 0;
        ncchannels_set_bg_rgb(&track, subtle_rgb);
        ncplane_set_base(plane, " ", 0, track);

        uint32_t accent = accent_channel();

        ncprogbar_options opts = {};
        opts.ulchannel = opts.urchannel = accent;
        opts.blchannel = opts.brchannel = accent;

        // takes ownership of the plane
        if (!(progbar = ncprogbar_create(plane, &opts)))
            return;
    }

    ncprogbar_set_progress(progbar, aud::clamp(progress, 0.0, 1.0));
}

// play/pause, progress bar, played / duration on row y, with one column of
// space at each end
static void draw_controls(ncplane * plane, int y, unsigned cols)
{
    // paused and stopped count as not playing, so the button offers to play
    bool playing = aud_drct_get_playing() && !aud_drct_get_paused();

    ncplane_printf_yx(plane, y, 1, "%s ", playing ? ICON_PAUSE : ICON_PLAY);

    unsigned bar_x;
    ncplane_cursor_yx(plane, nullptr, &bar_x);

    int position = 0, length = 0;

    if (aud_drct_get_playing())
    {
        position = aud_drct_get_time();
        length = aud_drct_get_length(); // negative if unknown (e.g. streams)
    }

    StringBuf time = str_printf(
        "%s / %s", (const char *)str_format_time(position),
        (length >= 0) ? (const char *)str_format_time(length) : "-:--");

    int time_x = (int)cols - 1 - (int)strlen(time);
    int bar_width = time_x - 1 - (int)bar_x;

    update_progress_bar(plane, y, bar_x, bar_width,
                        (length > 0) ? (double)position / length : 0);

    // leave out the timer rather than draw it over the button
    if (time_x >= (int)bar_x)
        ncplane_putstr_yx(plane, y, time_x, time);
}

// returns text cut short with an ellipsis so that it fits in the given number
// of columns, and stores the number of columns it takes up in text_width
static StringBuf fit_text(const char * text, int width, int * text_width)
{
    int full_width = ncstrwidth(text, nullptr, nullptr);

    if (full_width >= 0 && full_width <= width)
    {
        *text_width = full_width;
        return str_copy(text);
    }

    StringBuf cut(0);
    int cut_width = 0;

    if (width < 1)
    {
        *text_width = 0;
        return cut;
    }

    for (const char * p = text; *p; p = g_utf8_next_char(p))
    {
        StringBuf c = str_copy(p, g_utf8_next_char(p) - p);
        int c_width = aud::max(ncstrwidth(c, nullptr, nullptr), 0);

        if (cut_width + c_width > width - 1)
            break;

        cut.insert(-1, c);
        cut_width += c_width;
    }

    cut.insert(-1, "\u2026"); // …
    *text_width = cut_width + 1;
    return cut;
}

// writes text centered on row y within the given columns
static void put_centered(ncplane * plane, int y, int x, int width,
                         const char * text)
{
    if (!text)
        return;

    int text_width;
    StringBuf fitted = fit_text(text, width, &text_width);

    if (text_width > 0)
        ncplane_putstr_yx(plane, y, x + (width - text_width) / 2, fitted);
}

// writes text on row y, aligned left or right within the given columns
static void put_aligned(ncplane * plane, int y, int x, int width,
                        const char * text, bool right)
{
    // e.g. a tag of an entry that has not been scanned yet
    if (!text)
        return;

    int text_width;
    StringBuf fitted = fit_text(text, width, &text_width);

    if (text_width > 0)
        ncplane_putstr_yx(plane, y, right ? x + width - text_width : x, fitted);
}

enum Column
{
    ColTitle,
    ColArtist,
    ColAlbum,
    ColDisc,
    ColTrack,
    n_columns
};

static const char * const column_headers[n_columns] = {
    N_("Title"), N_("Artist"), N_("Album"), N_("Disc"), N_("Trk")};

#define NUMBER_COLUMN_WIDTH 4
#define MIN_TEXT_COLUMN_WIDTH 15

struct ColumnLayout
{
    bool visible;
    int x, width;
};

// The width each text column (title, artist, album) would ideally get: the
// width that WIDTH_PERCENTILE of the playlist's values fit in, plus a column
// for the gap.  Using a percentile rather than the widest value keeps a few
// very long titles from taking all the space; those are cut short instead.
// Zero until measured (see measure_columns()).
#define WIDTH_PERCENTILE 0.9

static int wanted_width[n_columns];

static bool is_number_column(int c) { return c == ColDisc || c == ColTrack; }

// Splits the given columns between the playlist columns.  Disc and track are
// fixed-width; title, artist and album share the rest according to their
// wanted widths.  When the text columns would get narrower than
// MIN_TEXT_COLUMN_WIDTH on average, columns are hidden in this order: disc,
// track, album, artist.  The title is always shown.
static void layout_columns(int x, int width, ColumnLayout layout[n_columns])
{
    static const Column hide_order[] = {ColDisc, ColTrack, ColAlbum,
                                        ColArtist};

    for (int c = 0; c < n_columns; c++)
        layout[c] = {true, 0, 0};

    int text_space = 0, n_text = 0;

    auto count_space = [&]() {
        text_space = width;
        n_text = 0;

        for (int c = 0; c < n_columns; c++)
        {
            if (!layout[c].visible)
                continue;

            if (is_number_column(c))
                text_space -= NUMBER_COLUMN_WIDTH;
            else
                n_text++;
        }
    };

    for (Column c : hide_order)
    {
        count_space();

        if (text_space / n_text >= MIN_TEXT_COLUMN_WIDTH)
            break;

        layout[c].visible = false;
    }

    count_space();

    // what each visible text column would like, and the least it gets
    int want[n_columns] = {}, floor[n_columns] = {};
    int total_want = 0;

    for (int c = 0; c < n_columns; c++)
    {
        if (!layout[c].visible || is_number_column(c))
            continue;

        int header = ncstrwidth(_(column_headers[c]), nullptr, nullptr) + 1;
        want[c] = aud::max(wanted_width[c], header);
        floor[c] = aud::min(want[c], MIN_TEXT_COLUMN_WIDTH);
        total_want += want[c];
    }

    int used = 0;

    for (int c = 0; c < n_columns; c++)
    {
        if (!layout[c].visible)
            continue;

        if (is_number_column(c))
            layout[c].width = NUMBER_COLUMN_WIDTH;
        else if (total_want <= text_space)
        {
            // enough room: everyone gets what they want, and the space left
            // over is shared in proportion
            layout[c].width = (int64_t)want[c] * text_space / total_want;
        }
        else
        {
            // not enough room: everyone gets their floor, and the rest is
            // shared in proportion to what each still wants beyond it
            int floors = 0, beyond = 0;

            for (int d = 0; d < n_columns; d++)
            {
                floors += floor[d];
                beyond += want[d] - floor[d];
            }

            int rest = aud::max(text_space - floors, 0);
            layout[c].width = floor[c];

            if (beyond > 0)
                layout[c].width += (int64_t)(want[c] - floor[c]) * rest / beyond;
        }

        used += layout[c].width;
    }

    // whatever rounding left over goes to the title
    layout[ColTitle].width += width - used;

    for (int c = 0; c < n_columns; c++)
    {
        if (!layout[c].visible)
            continue;

        layout[c].x = x;
        x += layout[c].width;
    }
}

// Writes one cell of a playlist row.  Text columns keep their last column
// free as a gap; the number columns are right-aligned instead.
static void put_column(ncplane * plane, int y, const ColumnLayout & layout,
                       Column column, const char * text)
{
    if (!layout.visible)
        return;

    if (column == ColDisc || column == ColTrack)
        put_aligned(plane, y, layout.x, layout.width, text, true);
    else
        put_aligned(plane, y, layout.x, layout.width - 1, text, false);
}

// The playlist pane is an nctree with one item per entry of the active
// playlist.  nctree does not accept NULL curries, so each item's curry is its
// entry number plus one (see entry_curry()).  The tree lives on its own
// plane below the column headers, which the tree owns.  An nctree cannot be
// empty, so there is no tree while the playlist is empty.
static nctree * tree;
static Playlist tree_playlist; // the playlist whose entries the tree shows
static std::vector<nctree_item> tree_items;

static void * entry_curry(int entry) { return (void *)(intptr_t)(entry + 1); }
static int curry_entry(void * curry) { return (int)(intptr_t)curry - 1; }

// keeps the tree's plane filling the main box below the column headers
static int tree_plane_resized(ncplane * plane)
{
    unsigned rows, cols;
    ncplane_dim_yx(ncplane_parent(plane), &rows, &cols);

    return place_plane(plane, 1, 0, (int)rows - 1, cols);
}

// draws one row of the playlist; distance is 0 for the focused row
static int draw_entry(ncplane * plane, void * curry, int distance)
{
    // the entry has scrolled out of view
    if (!plane)
        return 0;

    int entry = curry_entry(curry);
    bool focused = (distance == 0);
    bool playing = (tree_playlist == Playlist::playing_playlist() &&
                    entry == tree_playlist.get_position());

    // the tree does not resize the rows' planes, so make each row one line
    // as wide as the tree
    unsigned cols;
    ncplane_dim_yx(ncplane_parent(plane), nullptr, &cols);
    ncplane_resize_simple(plane, 1, cols);

    uint64_t channels = 0;
    if (focused)
        ncchannels_set_bg_rgb(&channels, subtle_rgb);
    if (playing)
        ncchannels_set_fg_palindex(&channels, ACCENT_PALETTE_INDEX);

    // erasing also resets the colours and styles for writing
    ncplane_set_base(plane, " ", 0, channels);
    ncplane_erase(plane);
    ncplane_set_channels(plane, channels);
    ncplane_set_styles(plane, playing ? NCSTYLE_BOLD : NCSTYLE_NONE);

    // don't wait for the entry to be scanned; the tree is redrawn when the
    // tags have been read
    Tuple tuple = tree_playlist.entry_tuple(entry, Playlist::NoWait);
    int disc = tuple.get_int(Tuple::Disc);
    int track = tuple.get_int(Tuple::Track);

    ColumnLayout layout[n_columns];
    layout_columns(0, cols, layout);

    put_column(plane, 0, layout[ColTitle], ColTitle,
               tuple.get_str(Tuple::Title));
    put_column(plane, 0, layout[ColArtist], ColArtist,
               tuple.get_str(Tuple::Artist));
    put_column(plane, 0, layout[ColAlbum], ColAlbum,
               tuple.get_str(Tuple::Album));
    put_column(plane, 0, layout[ColDisc], ColDisc,
               (disc > 0) ? (const char *)int_to_str(disc) : "");
    put_column(plane, 0, layout[ColTrack], ColTrack,
               (track > 0) ? (const char *)int_to_str(track) : "");

    return 0;
}

// returns the width that WIDTH_PERCENTILE of the given widths fit in
static int percentile_width(std::vector<int> & widths)
{
    if (widths.empty())
        return 0;

    auto nth = widths.begin() + (int)((widths.size() - 1) * WIDTH_PERCENTILE);
    std::nth_element(widths.begin(), nth, widths.end());
    return *nth;
}

// measures the wanted widths of the text columns over the tree's playlist
static void measure_columns()
{
    static const Tuple::Field fields[n_columns] = {
        Tuple::Title, Tuple::Artist, Tuple::Album};

    std::vector<int> widths[n_columns];
    int n_entries = tree ? tree_playlist.n_entries() : 0;

    for (int entry = 0; entry < n_entries; entry++)
    {
        Tuple tuple = tree_playlist.entry_tuple(entry, Playlist::NoWait);

        for (int c = 0; c < n_columns; c++)
        {
            if (is_number_column(c))
                continue;

            // empty tags (or tags not read yet) don't count
            String value = tuple.get_str(fields[c]);
            if (value && value[0])
                widths[c].push_back(aud::max(ncstrwidth(value, nullptr, nullptr), 0));
        }
    }

    for (int c = 0; c < n_columns; c++)
    {
        int typical = percentile_width(widths[c]);
        wanted_width[c] = typical ? typical + 1 : 0; // + 1 for the gap
    }
}

static void destroy_tree()
{
    // also destroys the tree's plane
    if (tree)
        nctree_destroy(tree);

    tree = nullptr;
    tree_items.clear();

    for (int & width : wanted_width)
        width = 0;
}

// (Re)creates the tree for the active playlist.  The focus stays on the same
// entry if the playlist is the same one; otherwise it starts on the
// playlist's current entry.
static void build_tree()
{
    Playlist playlist = Playlist::active_playlist();
    int focus = -1;

    if (tree && playlist == tree_playlist)
        focus = curry_entry(nctree_focused(tree));

    destroy_tree();

    tree_playlist = playlist;
    int n_entries = playlist.n_entries();

    if (n_entries < 1)
        return;

    if (focus < 0)
        focus = playlist.get_position();

    focus = aud::clamp(focus, 0, n_entries - 1);

    for (int i = 0; i < n_entries; i++)
        tree_items.push_back({entry_curry(i), nullptr, 0});

    ncplane_options popts = {};
    popts.rows = 1;
    popts.cols = 1;
    popts.resizecb = tree_plane_resized;

    ncplane * plane = ncplane_create(main_box.inner, &popts);
    if (!plane || tree_plane_resized(plane) < 0)
        return;

    nctree_options opts = {};
    opts.items = tree_items.data();
    opts.count = n_entries;
    opts.nctreecb = draw_entry;

    // takes ownership of the plane, even if it fails
    if (!(tree = nctree_create(plane, &opts)))
    {
        AUDERR("Failed to create the playlist view.\n");
        tree_items.clear();
        return;
    }

    measure_columns();

    unsigned spec[] = {(unsigned)focus, UINT_MAX};
    int failspec;
    nctree_goto(tree, spec, &failspec);
}

// the title, then the controls
static void draw_top()
{
    ncplane * plane = top_box.inner;
    unsigned cols;

    ncplane_erase(plane);
    ncplane_dim_yx(plane, nullptr, &cols);

    put_centered(plane, 0, 0, cols, _("Audacious TUI"));
    draw_controls(plane, 1, cols);
}

// the playlist column headers and the playlist
static void draw_main()
{
    ncplane * plane = main_box.inner;
    unsigned rows, cols;

    ncplane_erase(plane);
    ncplane_dim_yx(plane, &rows, &cols);

    ColumnLayout layout[n_columns];
    layout_columns(0, cols, layout);

    ncplane_set_fg_rgb(plane, muted_rgb);
    ncplane_on_styles(plane, NCSTYLE_BOLD);

    for (int c = 0; c < n_columns; c++)
        put_column(plane, 0, layout[c], (Column)c, _(column_headers[c]));

    ncplane_off_styles(plane, NCSTYLE_BOLD);
    ncplane_set_fg_default(plane);

    if (tree)
        nctree_redraw(tree);
    else if (rows > 1)
        put_centered(plane, 1, 0, cols, _("The playlist is empty."));
}

// title, artist and album of the current song
static void draw_bottom()
{
    ncplane * plane = bottom_box.inner;
    unsigned cols;

    ncplane_erase(plane);
    ncplane_dim_yx(plane, nullptr, &cols);

    put_centered(plane, 0, 0, cols, song_title);
    put_centered(plane, 1, 0, cols, song_artist);
    put_centered(plane, 2, 0, cols, song_album);
}

static void draw()
{
    for (Box * box : {&top_box, &main_box, &bottom_box})
        draw_border(box->outer);

    draw_top();
    draw_main();
    draw_bottom();

    notcurses_render(nc);
}

static void update_song_info(void * data, void * user)
{
    Tuple tuple = aud_drct_get_tuple();

    song_title = tuple.get_str(Tuple::Title);
    song_artist = tuple.get_str(Tuple::Artist);
    song_album = tuple.get_str(Tuple::Album);

    draw_bottom();
    notcurses_render(nc);
}

#define SEEK_STEP_MS 5000

static void tick(void * data)
{
    draw_top();
    notcurses_render(nc);
}

// keeps the elapsed time and progress bar moving while a song is playing
static void update_timer()
{
    if (aud_drct_get_playing() && !aud_drct_get_paused())
        timer_add(TimerRate::Hz4, tick);
    else
        timer_remove(TimerRate::Hz4, tick);
}

static void playback_state_changed(void * data, void * user)
{
    update_timer();
    draw_top();
    notcurses_render(nc);
}

static void position_changed(void * data, void * user)
{
    draw_top();
    notcurses_render(nc);
}

static void clear_song_info(void * data, void * user)
{
    song_title = String();
    song_artist = String();
    song_album = String();

    update_timer();
    draw_top();
    draw_bottom();
    notcurses_render(nc);
}

// Moves the focus a page up or down.  nctree's own handling of page up and
// page down (in notcurses 3.0.17) moves only one item, like up and down.
static void page_tree(bool down)
{
    unsigned rows;
    ncplane_dim_yx(nctree_plane(tree), &rows, nullptr);

    // keep one row of the previous page in view
    for (unsigned i = 1; i < rows; i++)
    {
        if (down)
            nctree_next(tree);
        else
            nctree_prev(tree);
    }

    nctree_redraw(tree);
    notcurses_render(nc);
}

// switches playback to the focused entry
static void play_focused()
{
    int entry = curry_entry(nctree_focused(tree));

    tree_playlist.set_position(entry);
    tree_playlist.start_playback();
}

static void playlist_updated(void * data, void * user)
{
    auto level = Playlist::active_playlist().update_detail().level;

    // entries added, removed or moved: the tree needs new items
    if (level == Playlist::Structure)
        build_tree();
    // new tags: the columns may need different widths
    else if (level == Playlist::Metadata)
        measure_columns();
    else if (level == Playlist::NoUpdate)
        return;

    draw_main();
    notcurses_render(nc);
}

static void playlist_activated(void * data, void * user)
{
    build_tree();
    draw_main();
    notcurses_render(nc);
}

// the playing entry is highlighted
static void playing_entry_changed(void * data, void * user)
{
    draw_main();
    notcurses_render(nc);
}

// seeks relative to the current position, staying within the song
static void seek_relative(int delta_ms)
{
    if (!aud_drct_get_playing())
        return;

    int length = aud_drct_get_length();

    // songs of unknown length (e.g. streams) cannot be seeked
    if (length <= 0)
        return;

    aud_drct_seek(aud::clamp(aud_drct_get_time() + delta_ms, 0, length));
}

// Refreshing makes notcurses pick up the new terminal size and resize the
// standard plane, whose resize callbacks then resize the boxes.  It also
// repaints the whole screen, so nothing is left over from before the resize.
// Then everything is drawn again at the new size.
static gboolean terminal_resized(void * user)
{
    notcurses_refresh(nc, nullptr, nullptr);
    draw();

    return G_SOURCE_CONTINUE;
}

static gboolean input_cb(int fd, GIOCondition condition, void * user)
{
    ncinput ni;
    uint32_t key;

    while ((key = notcurses_get_nblock(nc, &ni)) != 0 && key != (uint32_t)-1)
    {
        // key releases are only reported by terminals using the kitty protocol
        if (ni.evtype == NCTYPE_RELEASE)
            continue;

        if (key == NCKEY_RESIZE)
            terminal_resized(nullptr);
        else if (ncinput_ctrl_p(&ni) || ncinput_alt_p(&ni))
            continue;
        else if (tree && (key == NCKEY_PGDOWN || key == NCKEY_PGUP))
            page_tree(key == NCKEY_PGDOWN);
        // up and down (and the mouse wheel) move the focus in the playlist
        else if (tree && nctree_offer_input(tree, &ni))
        {
            nctree_redraw(tree);
            notcurses_render(nc);
        }
        else if (tree && key == NCKEY_ENTER)
            play_focused();
        else if (key == ' ')
            aud_drct_play_pause();
        else if (key == NCKEY_LEFT)
            seek_relative(-SEEK_STEP_MS);
        else if (key == NCKEY_RIGHT)
            seek_relative(SEEK_STEP_MS);
        else if (key == 'q')
        {
            aud_quit();
            break;
        }
    }

    return G_SOURCE_CONTINUE;
}

bool TUI::init()
{
    // Audacious already sets the locale and handles the quit signals.
    // SIGWINCH is watched from the main loop instead (see run()).
    notcurses_options opts = {};
    opts.flags = NCOPTION_INHIBIT_SETLOCALE | NCOPTION_NO_QUIT_SIGHANDLERS |
                 NCOPTION_NO_WINCH_SIGHANDLER | NCOPTION_SUPPRESS_BANNERS;

    nc = notcurses_init(&opts, nullptr);
    if (!nc)
    {
        AUDERR("Failed to initialize notcurses.\n");
        return false;
    }

    update_theme_colours();

    if (!create_boxes())
    {
        AUDERR("Failed to create the interface planes.\n");
        destroy_boxes();
        notcurses_stop(nc);
        nc = nullptr;
        return false;
    }

    // "playback ready" is sent once the song's information is available,
    // "tuple change" when it changes during playback (e.g. streams)
    hook_associate("playback ready", update_song_info, nullptr);
    hook_associate("tuple change", update_song_info, nullptr);
    hook_associate("playback stop", clear_song_info, nullptr);

    // the play/pause button and the timer follow the playback state
    hook_associate("playback begin", playback_state_changed, nullptr);
    hook_associate("playback pause", playback_state_changed, nullptr);
    hook_associate("playback unpause", playback_state_changed, nullptr);
    hook_associate("playback seek", position_changed, nullptr);

    hook_associate("playlist update", playlist_updated, nullptr);
    hook_associate("playlist activate", playlist_activated, nullptr);
    hook_associate("playlist position", playing_entry_changed, nullptr);
    hook_associate("playlist set playing", playing_entry_changed, nullptr);

    return true;
}

void TUI::cleanup()
{
    hook_dissociate("playback ready", update_song_info);
    hook_dissociate("tuple change", update_song_info);
    hook_dissociate("playback stop", clear_song_info);
    hook_dissociate("playback begin", playback_state_changed);
    hook_dissociate("playback pause", playback_state_changed);
    hook_dissociate("playback unpause", playback_state_changed);
    hook_dissociate("playback seek", position_changed);
    hook_dissociate("playlist update", playlist_updated);
    hook_dissociate("playlist activate", playlist_activated);
    hook_dissociate("playlist position", playing_entry_changed);
    hook_dissociate("playlist set playing", playing_entry_changed);
    timer_remove(TimerRate::Hz4, tick);

    song_title = String();
    song_artist = String();
    song_album = String();

    // the tree's plane is inside the main box
    destroy_tree();
    destroy_boxes();

    notcurses_stop(nc);
    nc = nullptr;
}

void TUI::run()
{
    // a song may already be playing, e.g. when resuming playback at startup
    update_timer();

    if (aud_drct_get_ready())
    {
        Tuple tuple = aud_drct_get_tuple();

        song_title = tuple.get_str(Tuple::Title);
        song_artist = tuple.get_str(Tuple::Artist);
        song_album = tuple.get_str(Tuple::Album);
    }

    build_tree();
    draw();

    input_source = g_unix_fd_add(notcurses_inputready_fd(nc), G_IO_IN,
                                 input_cb, nullptr);

    // With its own SIGWINCH handler, notcurses only reports a resize once
    // the signal happens to reach its input thread, which in a program with
    // as many threads as Audacious is often only at the next key press.
    // GLib's signal source wakes the main loop whichever thread gets it.
    resize_source = g_unix_signal_add(SIGWINCH, terminal_resized, nullptr);

    mainloop_run();

    g_source_remove(resize_source);
    g_source_remove(input_source);
    resize_source = input_source = 0;
}
