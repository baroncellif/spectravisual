#include "../algorithms.h"
#include "../loader.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int failures = 0;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (message)); \
        failures++; \
    } \
} while (0)

static int near(double actual, double expected, double tolerance) {
    return fabs(actual - expected) <= tolerance;
}

static void test_spectrum_loader(void) {
    Point *points = NULL;
    double xmin, xmax, ymin, ymax;
    int n = read_data_alloc("tests/fixtures/spectrum_basic.csv", &points,
                           &xmin, &xmax, &ymin, &ymax);

    CHECK(n == 5, "CSV fixture should yield five points");
    CHECK(points != NULL, "CSV fixture should allocate points");
    if (points) {
        CHECK(near(xmin, 1000.0, 1e-9), "loader xmin");
        CHECK(near(xmax, 1002.0, 1e-9), "loader xmax");
        CHECK(near(ymin, 1.0, 1e-9), "loader ymin");
        CHECK(near(ymax, 5.0, 1e-9), "loader ymax");
        CHECK(near(points[2].x, 1001.0, 1e-9), "middle frequency");
        CHECK(near(points[2].y, 5.0, 1e-9), "middle intensity");
    }
    free(points);
}

static void test_prediction_loader(void) {
    PredLine *lines = NULL;
    double xmin, xmax, max_intensity;
    int n = read_pred_cat_alloc("tests/fixtures/prediction_basic.cat", &lines,
                                &xmin, &xmax, &max_intensity);

    CHECK(n == 2, "CAT fixture should yield two prediction lines");
    CHECK(lines != NULL, "CAT fixture should allocate lines");
    if (lines) {
        CHECK(near(xmin, 1000.0, 1e-9), "prediction xmin");
        CHECK(near(xmax, 1001.25, 1e-9), "prediction xmax");
        CHECK(near(lines[1].lgint, -1.0, 1e-9), "prediction intensity log");
        CHECK(near(max_intensity, 0.1, 1e-12), "prediction linear maximum");
    }
    free(lines);
}

static void test_search_and_average(void) {
    Point source[] = {{0, 0}, {1, 1}, {2, 3}, {3, 1}, {4, 0}};
    Point smoothed[5] = {{0}};

    CHECK(binary_search_lower(source, 5, 1.0) == 1, "lower bound exact");
    CHECK(binary_search_upper(source, 5, 1.0) == 2, "upper bound exact");
    CHECK(binary_search_lower(source, 5, 2.4) == 3, "lower bound between values");

    apply_rolling_average(source, smoothed, 5, 3);
    CHECK(near(smoothed[0].y, 0.5, 1e-12), "rolling average left edge");
    CHECK(near(smoothed[2].y, 5.0 / 3.0, 1e-12), "rolling average centre");
    CHECK(near(smoothed[4].y, 0.5, 1e-12), "rolling average right edge");
}

static void test_peak_finder(void) {
    Point trace[11];
    Peak peaks[MAX_PEAKS];
    int n_peaks = 0;
    for (int i = 0; i < 11; i++) trace[i] = (Point){(double)i, 0.0};
    trace[5].y = 1.0;

    run_peak_finder(trace, 11, 0.0, 10.0, 1, 5, 0.5, peaks, &n_peaks);
    CHECK(n_peaks == 1, "isolated peak should be found once");
    if (n_peaks == 1) {
        CHECK(near(peaks[0].x, 5.0, 1e-12), "interpolated peak frequency");
        CHECK(near(peaks[0].y, 1.0, 1e-12), "interpolated peak intensity");
    }
}

int main(void) {
    test_spectrum_loader();
    test_prediction_loader();
    test_search_and_average();
    test_peak_finder();

    if (failures) {
        fprintf(stderr, "%d test assertion(s) failed.\n", failures);
        return EXIT_FAILURE;
    }
    puts("All core tests passed.");
    return EXIT_SUCCESS;
}
