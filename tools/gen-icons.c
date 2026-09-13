#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DESIGN_GRID      64.0
#define MAX_ICON_SIZE    64
#define MAX_EDGES        4096
#define MAX_POINTS       2048
#define SUB_SCANLINES    4

typedef struct {
    double x0, y0, x1, y1;
    int winding;
} edge_t;

typedef struct {
    double red, green, blue, alpha;
} rgba_t;

typedef enum {
    PAINT_SOLID,
    PAINT_VERTICAL_GRADIENT
} paint_kind_t;

typedef struct {
    paint_kind_t kind;
    rgba_t start;
    rgba_t end;
    double top;
    double bottom;
} paint_t;

static edge_t edges[MAX_EDGES];
static int edge_count;
static int reverse_winding;

static double point_x[MAX_POINTS];
static double point_y[MAX_POINTS];
static int point_count;
static int contour_start;

static double canvas_red[MAX_ICON_SIZE * MAX_ICON_SIZE];
static double canvas_green[MAX_ICON_SIZE * MAX_ICON_SIZE];
static double canvas_blue[MAX_ICON_SIZE * MAX_ICON_SIZE];
static double canvas_alpha[MAX_ICON_SIZE * MAX_ICON_SIZE];
static int canvas_size;
static unsigned int tile_top_color;
static unsigned int tile_bottom_color;
static double canvas_scale;

static int errors;

static void fail(const char *what, const char *detail) {
    fprintf(stderr, "gen-icons: %s: %s\n", what, detail);
    errors++;
}

static rgba_t rgb(unsigned int packed) {
    rgba_t color;
    color.red = (double)((packed >> 16) & 0xFF) / 255.0;
    color.green = (double)((packed >> 8) & 0xFF) / 255.0;
    color.blue = (double)(packed & 0xFF) / 255.0;
    color.alpha = 1.0;
    return color;
}

static rgba_t rgba(unsigned int packed, double alpha) {
    rgba_t color = rgb(packed);
    color.alpha = alpha;
    return color;
}

static paint_t vertical_gradient(rgba_t start, rgba_t end, double top, double bottom) {
    paint_t paint;
    paint.kind = PAINT_VERTICAL_GRADIENT;
    paint.start = start;
    paint.end = end;
    paint.top = top;
    paint.bottom = bottom;
    return paint;
}

static void canvas_reset(int size) {
    canvas_size = size;
    canvas_scale = (double)size / DESIGN_GRID;
    memset(canvas_red, 0, sizeof(canvas_red));
    memset(canvas_green, 0, sizeof(canvas_green));
    memset(canvas_blue, 0, sizeof(canvas_blue));
    memset(canvas_alpha, 0, sizeof(canvas_alpha));
}

static void path_begin(void) {
    point_count = 0;
    contour_start = 0;
    edge_count = 0;
    reverse_winding = 0;
}

static void add_edge(double x0, double y0, double x1, double y1) {
    if (y0 == y1) {
        return;
    }
    if (edge_count >= MAX_EDGES) {
        fail("edge overflow", "too many edges in one path");
        return;
    }
    edge_t *edge = &edges[edge_count++];
    if (y0 < y1) {
        edge->x0 = x0;
        edge->y0 = y0;
        edge->x1 = x1;
        edge->y1 = y1;
        edge->winding = reverse_winding ? -1 : 1;
    } else {
        edge->x0 = x1;
        edge->y0 = y1;
        edge->x1 = x0;
        edge->y1 = y0;
        edge->winding = reverse_winding ? 1 : -1;
    }
}

static void path_point(double x, double y) {
    if (point_count >= MAX_POINTS) {
        fail("point overflow", "too many points in one contour");
        return;
    }
    point_x[point_count] = x * canvas_scale;
    point_y[point_count] = y * canvas_scale;
    point_count++;
}

static void path_close(void) {
    for (int i = contour_start; i < point_count; i++) {
        int next = (i + 1 < point_count) ? i + 1 : contour_start;
        add_edge(point_x[i], point_y[i], point_x[next], point_y[next]);
    }
    contour_start = point_count;
}

static void path_arc(double center_x, double center_y, double radius,
                     double from_degrees, double to_degrees, int steps) {
    for (int i = 0; i <= steps; i++) {
        double t = (double)i / (double)steps;
        double angle = (from_degrees + (to_degrees - from_degrees) * t) * M_PI / 180.0;
        path_point(center_x + radius * cos(angle), center_y + radius * sin(angle));
    }
}

static void path_rounded_rectangle(double x, double y, double width, double height, double radius) {
    double maximum = (width < height ? width : height) / 2.0;
    if (radius > maximum) {
        radius = maximum;
    }
    int steps = 12;
    path_arc(x + width - radius, y + height - radius, radius, 0.0, 90.0, steps);
    path_arc(x + radius, y + height - radius, radius, 90.0, 180.0, steps);
    path_arc(x + radius, y + radius, radius, 180.0, 270.0, steps);
    path_arc(x + width - radius, y + radius, radius, 270.0, 360.0, steps);
    path_close();
}

static void path_rectangle(double x, double y, double width, double height) {
    path_point(x, y);
    path_point(x + width, y);
    path_point(x + width, y + height);
    path_point(x, y + height);
    path_close();
}

static void path_circle(double center_x, double center_y, double radius) {
    path_arc(center_x, center_y, radius, 0.0, 360.0, 40);
    path_close();
}

static void path_ring(double center_x, double center_y, double outer, double inner) {
    path_arc(center_x, center_y, outer, 0.0, 360.0, 40);
    path_close();
    path_arc(center_x, center_y, inner, 360.0, 0.0, 40);
    path_close();
}

static void path_thick_line(double x0, double y0, double x1, double y1, double thickness) {
    double dx = x1 - x0;
    double dy = y1 - y0;
    double length = sqrt(dx * dx + dy * dy);
    if (length <= 0.0) {
        return;
    }
    double nx = -dy / length * thickness / 2.0;
    double ny = dx / length * thickness / 2.0;
    path_point(x0 + nx, y0 + ny);
    path_point(x1 + nx, y1 + ny);
    path_point(x1 - nx, y1 - ny);
    path_point(x0 - nx, y0 - ny);
    path_close();
}

static rgba_t paint_at(const paint_t *paint, double y_in_pixels) {
    if (paint->kind == PAINT_SOLID) {
        return paint->start;
    }
    double top = paint->top * canvas_scale;
    double bottom = paint->bottom * canvas_scale;
    double t = (bottom > top) ? (y_in_pixels - top) / (bottom - top) : 0.0;
    if (t < 0.0) {
        t = 0.0;
    }
    if (t > 1.0) {
        t = 1.0;
    }
    rgba_t color;
    color.red = paint->start.red + (paint->end.red - paint->start.red) * t;
    color.green = paint->start.green + (paint->end.green - paint->start.green) * t;
    color.blue = paint->start.blue + (paint->end.blue - paint->start.blue) * t;
    color.alpha = paint->start.alpha + (paint->end.alpha - paint->start.alpha) * t;
    return color;
}

static void blend_pixel(int x, int y, rgba_t color, double coverage) {
    if (x < 0 || y < 0 || x >= canvas_size || y >= canvas_size) {
        return;
    }
    double source_alpha = color.alpha * coverage;
    if (source_alpha <= 0.0) {
        return;
    }
    int index = y * canvas_size + x;
    double destination_alpha = canvas_alpha[index];
    double out_alpha = source_alpha + destination_alpha * (1.0 - source_alpha);
    if (out_alpha <= 0.0) {
        return;
    }
    canvas_red[index] = (color.red * source_alpha + canvas_red[index] * destination_alpha * (1.0 - source_alpha)) / out_alpha;
    canvas_green[index] = (color.green * source_alpha + canvas_green[index] * destination_alpha * (1.0 - source_alpha)) / out_alpha;
    canvas_blue[index] = (color.blue * source_alpha + canvas_blue[index] * destination_alpha * (1.0 - source_alpha)) / out_alpha;
    canvas_alpha[index] = out_alpha;
}

static double coverage_row[MAX_ICON_SIZE];

static void path_fill(const paint_t *paint) {
    if (edge_count == 0) {
        return;
    }
    double crossing_x[MAX_EDGES];
    int crossing_winding[MAX_EDGES];
    for (int row = 0; row < canvas_size; row++) {
        for (int i = 0; i < canvas_size; i++) {
            coverage_row[i] = 0.0;
        }
        int any = 0;
        for (int sub = 0; sub < SUB_SCANLINES; sub++) {
            double sample_y = (double)row + ((double)sub + 0.5) / (double)SUB_SCANLINES;
            int crossings = 0;
            for (int e = 0; e < edge_count; e++) {
                const edge_t *edge = &edges[e];
                if (sample_y < edge->y0 || sample_y >= edge->y1) {
                    continue;
                }
                double t = (sample_y - edge->y0) / (edge->y1 - edge->y0);
                crossing_x[crossings] = edge->x0 + (edge->x1 - edge->x0) * t;
                crossing_winding[crossings] = edge->winding;
                crossings++;
            }
            for (int i = 1; i < crossings; i++) {
                double key_x = crossing_x[i];
                int key_winding = crossing_winding[i];
                int j = i - 1;
                while (j >= 0 && crossing_x[j] > key_x) {
                    crossing_x[j + 1] = crossing_x[j];
                    crossing_winding[j + 1] = crossing_winding[j];
                    j--;
                }
                crossing_x[j + 1] = key_x;
                crossing_winding[j + 1] = key_winding;
            }
            int winding = 0;
            for (int i = 0; i + 1 < crossings; i++) {
                winding += crossing_winding[i];
                if (winding == 0) {
                    continue;
                }
                double span_start = crossing_x[i];
                double span_end = crossing_x[i + 1];
                if (span_end <= 0.0 || span_start >= (double)canvas_size) {
                    continue;
                }
                if (span_start < 0.0) {
                    span_start = 0.0;
                }
                if (span_end > (double)canvas_size) {
                    span_end = (double)canvas_size;
                }
                int first = (int)span_start;
                int last = (int)ceil(span_end) - 1;
                for (int column = first; column <= last && column < canvas_size; column++) {
                    if (column < 0) {
                        continue;
                    }
                    double left = (double)column;
                    double right = left + 1.0;
                    double covered_left = span_start > left ? span_start : left;
                    double covered_right = span_end < right ? span_end : right;
                    if (covered_right > covered_left) {
                        coverage_row[column] += (covered_right - covered_left) / (double)SUB_SCANLINES;
                        any = 1;
                    }
                }
            }
        }
        if (!any) {
            continue;
        }
        for (int column = 0; column < canvas_size; column++) {
            double coverage = coverage_row[column];
            if (coverage <= 0.0) {
                continue;
            }
            if (coverage > 1.0) {
                coverage = 1.0;
            }
            blend_pixel(column, row, paint_at(paint, (double)row + 0.5), coverage);
        }
    }
    path_begin();
}

static void fill_solid(unsigned int color, double alpha) {
    paint_t paint;
    paint.kind = PAINT_SOLID;
    paint.start = rgba(color, alpha);
    paint.end = paint.start;
    paint.top = 0.0;
    paint.bottom = 1.0;
    path_fill(&paint);
}

static void path_ellipse(double center_x, double center_y, double radius_x, double radius_y,
                         int reverse) {
    int steps = 44;
    for (int i = 0; i <= steps; i++) {
        double t = (double)i / (double)steps;
        double angle = (reverse ? (1.0 - t) : t) * 360.0 * M_PI / 180.0;
        path_point(center_x + radius_x * cos(angle), center_y + radius_y * sin(angle));
    }
    path_close();
}

static void path_ellipse_ring(double center_x, double center_y, double radius_x, double radius_y,
                              double thickness) {
    path_ellipse(center_x, center_y, radius_x, radius_y, 0);
    path_ellipse(center_x, center_y, radius_x - thickness, radius_y - thickness, 1);
}

static void fill_gradient(unsigned int top_color, double top_alpha,
                          unsigned int bottom_color, double bottom_alpha,
                          double y_top, double y_bottom) {
    paint_t paint = vertical_gradient(rgba(top_color, top_alpha), rgba(bottom_color, bottom_alpha),
                                      y_top, y_bottom);
    path_fill(&paint);
}

static void path_rotated_rectangle(double center_x, double center_y, double width, double height,
                                   double degrees) {
    double angle = degrees * M_PI / 180.0;
    double cosine = cos(angle);
    double sine = sin(angle);
    double half_width = width / 2.0;
    double half_height = height / 2.0;
    double corner_x[4] = {-half_width, half_width, half_width, -half_width};
    double corner_y[4] = {-half_height, -half_height, half_height, half_height};
    for (int i = 0; i < 4; i++) {
        path_point(center_x + corner_x[i] * cosine - corner_y[i] * sine,
                   center_y + corner_x[i] * sine + corner_y[i] * cosine);
    }
    path_close();
}

#define TILE_X       5.0
#define TILE_Y       4.0
#define TILE_SIDE    54.0
#define TILE_RADIUS  12.5

static void fill_tile_ground(void) {
    fill_gradient(tile_top_color, 1.0, tile_bottom_color, 1.0, TILE_Y, TILE_Y + TILE_SIDE);
}

static void draw_tile_shadow(void) {
    for (int i = 0; i < 4; i++) {
        double spread = (double)i;
        path_rounded_rectangle(TILE_X - spread, TILE_Y + 2.0 - spread * 0.25,
                               TILE_SIDE + 2.0 * spread, TILE_SIDE + 2.0 * spread,
                               TILE_RADIUS + spread);
        fill_solid(0x000000u, 0.055);
    }
}

static void draw_tile(unsigned int top_color, unsigned int bottom_color) {
    tile_top_color = top_color;
    tile_bottom_color = bottom_color;
    draw_tile_shadow();
    path_rounded_rectangle(TILE_X, TILE_Y, TILE_SIDE, TILE_SIDE, TILE_RADIUS);
    paint_t body = vertical_gradient(rgb(top_color), rgb(bottom_color), TILE_Y, TILE_Y + TILE_SIDE);
    path_fill(&body);

    path_rounded_rectangle(TILE_X, TILE_Y, TILE_SIDE, TILE_SIDE, TILE_RADIUS);
    reverse_winding = 1;
    path_rounded_rectangle(TILE_X + 1.0, TILE_Y + 1.0, TILE_SIDE - 2.0, TILE_SIDE - 2.0, TILE_RADIUS - 1.0);
    reverse_winding = 0;
    paint_t highlight = vertical_gradient(rgba(0xFFFFFFu, 0.40), rgba(0xFFFFFFu, 0.0),
                                          TILE_Y, TILE_Y + TILE_SIDE * 0.6);
    path_fill(&highlight);
}

static void draw_icon_terminal(void) {
    draw_tile(0x3C4250u, 0x1C1F28u);
    path_thick_line(20.0, 24.0, 28.0, 31.5, 3.4);
    path_thick_line(28.0, 31.5, 20.0, 39.0, 3.4);
    fill_solid(0x8CF0A0u, 1.0);
    path_rounded_rectangle(32.0, 37.0, 14.0, 3.2, 1.6);
    fill_solid(0xE6ECF5u, 1.0);
}

static void draw_icon_editor(void) {
    draw_tile(0xFDFDFEu, 0xE4E7EEu);
    for (int i = 0; i < 4; i++) {
        double y = 20.0 + (double)i * 7.0;
        double width = (i == 3) ? 14.0 : 24.0;
        path_rounded_rectangle(15.0, y, width, 3.0, 1.5);
        fill_solid(0x9AA4B4u, 1.0);
    }
    path_point(44.5, 17.0);
    path_point(48.5, 21.0);
    path_point(33.0, 36.5);
    path_point(28.0, 38.0);
    path_point(29.5, 33.0);
    path_close();
    fill_gradient(0xFFC85Cu, 1.0, 0xE8952Au, 1.0, 17.0, 38.0);
    path_point(28.0, 38.0);
    path_point(29.5, 33.0);
    path_point(31.6, 35.1);
    path_close();
    fill_solid(0x4A4F5Au, 1.0);
}

static void draw_icon_files(void) {
    draw_tile(0x63B8FFu, 0x2276D6u);
    path_rounded_rectangle(14.0, 19.0, 16.0, 6.0, 2.0);
    fill_solid(0xE8F3FFu, 0.95);
    path_rounded_rectangle(14.0, 22.0, 36.0, 23.0, 3.5);
    fill_gradient(0xFFFFFFu, 1.0, 0xDCE8F6u, 1.0, 22.0, 45.0);
    path_rounded_rectangle(14.0, 26.5, 36.0, 18.5, 3.5);
    fill_gradient(0xF4D98Cu, 1.0, 0xE0B45Au, 1.0, 26.5, 45.0);
}

static void draw_icon_settings(void) {
    draw_tile(0x98A3B5u, 0x545E72u);
    for (int i = 0; i < 6; i++) {
        path_rotated_rectangle(32.0, 32.0, 5.8, 33.0, (double)i * 30.0);
    }
    path_circle(32.0, 32.0, 12.0);
    fill_gradient(0xFFFFFFu, 1.0, 0xDCE2EAu, 1.0, 16.0, 48.0);
    path_circle(32.0, 32.0, 5.6);
    fill_tile_ground();
}

static void draw_icon_clock(void) {
    draw_tile(0xFBFCFDu, 0xDEE3EAu);
    path_ring(32.0, 32.0, 17.0, 14.4);
    fill_solid(0x2E3440u, 1.0);
    for (int i = 0; i < 12; i++) {
        double angle = (double)i * 30.0 * M_PI / 180.0;
        double length = (i % 3 == 0) ? 3.6 : 2.0;
        double outer = 13.2;
        path_thick_line(32.0 + cos(angle) * outer, 32.0 + sin(angle) * outer,
                        32.0 + cos(angle) * (outer - length), 32.0 + sin(angle) * (outer - length),
                        (i % 3 == 0) ? 2.0 : 1.2);
    }
    fill_solid(0x59637Au, 1.0);
    path_thick_line(32.0, 32.0, 32.0, 22.5, 2.6);
    path_thick_line(32.0, 32.0, 40.0, 35.0, 2.6);
    fill_solid(0x2E3440u, 1.0);
    path_circle(32.0, 32.0, 2.0);
    fill_solid(0xE0463Cu, 1.0);
}

static void draw_icon_paint(void) {
    draw_tile(0xFF8A62u, 0xDE3A75u);
    path_circle(30.5, 32.0, 15.0);
    fill_gradient(0xFFFFFFu, 1.0, 0xE7EBF1u, 1.0, 17.0, 47.0);
    path_circle(38.5, 37.5, 4.6);
    fill_tile_ground();
    static const unsigned int dot_color[4] = {0x2F7FE0u, 0x38B45Au, 0xF2B21Au, 0x8A46D4u};
    static const double dot_x[4] = {24.0, 30.5, 36.5, 23.5};
    static const double dot_y[4] = {26.5, 23.5, 27.0, 36.5};
    for (int i = 0; i < 4; i++) {
        path_circle(dot_x[i], dot_y[i], 3.1);
        fill_solid(dot_color[i], 1.0);
    }
}

static void draw_icon_tasks(void) {
    draw_tile(0x39405Bu, 0x1D2135u);
    static const unsigned int bar_color[3] = {0x4C99E6u, 0x46C07Au, 0xF2B21Au};
    static const double bar_height[3] = {12.0, 20.0, 27.0};
    for (int i = 0; i < 3; i++) {
        double x = 17.0 + (double)i * 11.0;
        double y = 45.0 - bar_height[i];
        path_rounded_rectangle(x, y, 8.0, bar_height[i], 2.2);
        fill_gradient(bar_color[i], 1.0, bar_color[i], 0.72, y, 45.0);
    }
    path_rounded_rectangle(15.0, 46.5, 34.0, 2.2, 1.1);
    fill_solid(0x6E7A90u, 1.0);
}

static void draw_icon_browser(void) {
    draw_tile(0x56C8F5u, 0x1A6FD0u);
    path_circle(32.0, 32.0, 16.0);
    fill_gradient(0xA6E6FBu, 1.0, 0x4796E4u, 1.0, 16.0, 48.0);

    path_ellipse_ring(32.0, 32.0, 16.0, 16.0, 1.7);
    path_ellipse_ring(32.0, 32.0, 6.4, 16.0, 1.5);
    path_rectangle(16.0, 31.2, 32.0, 1.6);
    for (int i = 0; i < 2; i++) {
        double dy = 8.0 + (double)i * 0.0;
        double half = sqrt(16.0 * 16.0 - dy * dy);
        path_rectangle(32.0 - half, 32.0 - dy - 0.7, 2.0 * half, 1.4);
        path_rectangle(32.0 - half, 32.0 + dy - 0.7, 2.0 * half, 1.4);
    }
    fill_solid(0xFFFFFFu, 0.92);
}

static void draw_icon_application(void) {
    draw_tile(0x7F8BA3u, 0x49536Bu);
    path_rounded_rectangle(15.0, 19.0, 34.0, 26.0, 4.0);
    fill_gradient(0xFFFFFFu, 1.0, 0xE2E7EFu, 1.0, 19.0, 45.0);
    path_rectangle(15.0, 19.0, 34.0, 7.0);
    fill_solid(0x5C6479u, 1.0);
    for (int i = 0; i < 3; i++) {
        path_circle(19.5 + (double)i * 5.0, 22.5, 1.5);
        fill_solid(0xE8ECF3u, 1.0);
    }
    path_rounded_rectangle(15.0, 26.0, 34.0, 1.2, 0.6);
    fill_solid(0xB9C2D0u, 1.0);
}

typedef struct {
    const char *symbol;
    const char *label;
    void (*draw)(void);
} icon_source_t;

static const icon_source_t ICON_SOURCES[] = {
    {"ICON_TERMINAL", "Terminal", draw_icon_terminal},
    {"ICON_EDITOR",   "Editor",   draw_icon_editor},
    {"ICON_FILES",    "Files",    draw_icon_files},
    {"ICON_SETTINGS", "Settings", draw_icon_settings},
    {"ICON_CLOCK",    "Clock",    draw_icon_clock},
    {"ICON_PAINT",    "Paint",    draw_icon_paint},
    {"ICON_TASKS",    "Tasks",    draw_icon_tasks},
    {"ICON_BROWSER",  "Browser",  draw_icon_browser},
    {"ICON_APPLICATION", "Application", draw_icon_application},
};
#define ICON_SOURCE_COUNT ((int)(sizeof(ICON_SOURCES) / sizeof(ICON_SOURCES[0])))

#define ICON_LARGE 48
#define ICON_SMALL 24

static unsigned char rendered[MAX_ICON_SIZE * MAX_ICON_SIZE * 4];
static int rendered_size;

static unsigned char to_byte(double value) {
    double scaled = value * 255.0 + 0.5;
    if (scaled < 0.0) {
        scaled = 0.0;
    }
    if (scaled > 255.0) {
        scaled = 255.0;
    }
    return (unsigned char)scaled;
}

static void render(const icon_source_t *source, int size) {
    canvas_reset(size);
    path_begin();
    source->draw();
    rendered_size = size;
    for (int i = 0; i < size * size; i++) {
        rendered[i * 4 + 0] = to_byte(canvas_red[i]);
        rendered[i * 4 + 1] = to_byte(canvas_green[i]);
        rendered[i * 4 + 2] = to_byte(canvas_blue[i]);
        rendered[i * 4 + 3] = to_byte(canvas_alpha[i]);
    }
}

static unsigned char alpha_at(int x, int y) {
    return rendered[(y * rendered_size + x) * 4 + 3];
}

static double opaque_fraction(void) {
    int opaque = 0;
    for (int i = 0; i < rendered_size * rendered_size; i++) {
        if (rendered[i * 4 + 3] >= 250) {
            opaque++;
        }
    }
    return (double)opaque / (double)(rendered_size * rendered_size);
}

static void check_icon(const icon_source_t *source, int size) {
    render(source, size);

    int partial = 0;
    for (int i = 0; i < size * size; i++) {
        unsigned char a = rendered[i * 4 + 3];
        if (a > 8 && a < 247) {
            partial++;
        }
    }
    if (partial < size) {
        fail(source->label, "not anti-aliased: too few partly covered pixels");
    }

    if (alpha_at(0, 0) != 0 || alpha_at(size - 1, 0) != 0 ||
        alpha_at(0, size - 1) != 0 || alpha_at(size - 1, size - 1) != 0) {
        fail(source->label, "corner pixel is not transparent: the tile is not rounded");
    }

    if (alpha_at(size / 2, size / 2) != 255) {
        fail(source->label, "centre pixel is not opaque");
    }

    for (int x = 0; x < size; x++) {
        if (alpha_at(x, 0) > 48 || alpha_at(x, size - 1) > 48) {
            fail(source->label, "hard ink on the outer row: artwork escapes its box");
            break;
        }
    }
    for (int y = 0; y < size; y++) {
        if (alpha_at(0, y) > 48 || alpha_at(size - 1, y) > 48) {
            fail(source->label, "hard ink on the outer column: artwork escapes its box");
            break;
        }
    }

    int distinct = 0;
    static unsigned char seen[1 << 15];
    memset(seen, 0, sizeof(seen));
    for (int i = 0; i < size * size; i++) {
        if (rendered[i * 4 + 3] < 250) {
            continue;
        }
        int key = ((rendered[i * 4 + 0] >> 3) << 10) | ((rendered[i * 4 + 1] >> 3) << 5) |
                  (rendered[i * 4 + 2] >> 3);
        if (!seen[key]) {
            seen[key] = 1;
            distinct++;
        }
    }
    if (distinct < 6) {
        fail(source->label, "fewer than six distinct colours: the artwork is flat");
    }
}

static void check_all(void) {
    for (int i = 0; i < ICON_SOURCE_COUNT; i++) {
        check_icon(&ICON_SOURCES[i], ICON_LARGE);
        double large_fraction = opaque_fraction();
        check_icon(&ICON_SOURCES[i], ICON_SMALL);
        double small_fraction = opaque_fraction();
        double difference = large_fraction - small_fraction;
        if (difference < 0.0) {
            difference = -difference;
        }
        if (difference > 0.06) {
            fail(ICON_SOURCES[i].label, "the two sizes do not agree on the silhouette");
        }
    }
}

static char out[1 << 22];
static size_t out_length;

static void emit(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void emit(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    out_length += (size_t)vsnprintf(out + out_length, sizeof(out) - out_length, fmt, args);
    va_end(args);
}

static void emit_blob(const char *symbol, const icon_source_t *source, int size) {
    render(source, size);
    emit("const uint8_t %s[] = {\n", symbol);
    emit("    0x4C, 0x49, 0x43, 0x32, 0x%02X, 0x%02X, 0x00, 0x00,\n", size, size);
    int total = size * size * 4;
    for (int i = 0; i < total; i++) {
        if (i % 12 == 0) {
            emit("   ");
        }
        emit(" 0x%02X,", rendered[i]);
        if (i % 12 == 11 || i == total - 1) {
            emit("\n");
        }
    }
    emit("};\n\n");
}

static int write_if_changed(const char *path) {
    FILE *existing = fopen(path, "rb");
    if (existing) {
        static char previous[1 << 22];
        size_t n = fread(previous, 1, sizeof(previous), existing);
        fclose(existing);
        if (n == out_length && memcmp(previous, out, n) == 0) {
            return 0;
        }
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        fail("write", path);
        return -1;
    }
    fwrite(out, 1, out_length, f);
    fclose(f);
    return 1;
}

static void build_source(void) {
    out_length = 0;
    emit("#include \"icons.h\"\n\n");
    for (int i = 0; i < ICON_SOURCE_COUNT; i++) {
        emit_blob(ICON_SOURCES[i].symbol, &ICON_SOURCES[i], ICON_LARGE);
        char small[64];
        snprintf(small, sizeof(small), "%s_SMALL", ICON_SOURCES[i].symbol);
        emit_blob(small, &ICON_SOURCES[i], ICON_SMALL);
    }
}

static void check_matches(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fail("check", path);
        return;
    }
    static char previous[1 << 22];
    size_t n = fread(previous, 1, sizeof(previous), f);
    fclose(f);
    if (n != out_length || memcmp(previous, out, n) != 0) {
        fail("stale, run make icons", path);
    }
}

static void build_header(void) {
    out_length = 0;
    emit("#pragma once\n\n");
    emit("#include <stdint.h>\n\n");
    emit("#include \"graphics.h\"\n\n");
    emit("#define ICON_LARGE_SIZE %d\n", ICON_LARGE);
    emit("#define ICON_SMALL_SIZE %d\n\n", ICON_SMALL);
    emit("void icon_draw(graphics_context_t *context, int32_t x, int32_t y, const uint8_t *blob, int32_t scale);\n\n");
    emit("void icon_draw_tinted(graphics_context_t *context, int32_t x, int32_t y, const uint8_t *blob,\n");
    emit("                      int32_t scale, uint32_t tint, uint32_t tint_percent);\n\n");
    for (int i = 0; i < ICON_SOURCE_COUNT; i++) {
        emit("extern const uint8_t %s[];\n", ICON_SOURCES[i].symbol);
        emit("extern const uint8_t %s_SMALL[];\n", ICON_SOURCES[i].symbol);
    }
}

int main(int argc, char **argv) {
    int write = 0;
    int check = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--write") == 0) {
            write = 1;
        } else if (strcmp(argv[i], "--check") == 0) {
            check = 1;
        }
    }
    if (!write && !check) {
        fprintf(stderr, "usage: gen-icons --write | --check\n");
        return 2;
    }

    check_all();
    if (errors) {
        fprintf(stderr, "gen-icons: %d error(s)\n", errors);
        return 1;
    }

    build_source();
    if (write) {
        int changed = write_if_changed("user_space/library/icons.c");
        build_header();
        changed |= write_if_changed("user_space/library/icons.h");
        printf("gen-icons: %d icons, two sizes each, %s\n", ICON_SOURCE_COUNT,
               changed ? "written" : "unchanged");
        return errors ? 1 : 0;
    }

    check_matches("user_space/library/icons.c");
    build_header();
    check_matches("user_space/library/icons.h");
    return errors ? 1 : 0;
}
