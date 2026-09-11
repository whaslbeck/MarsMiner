/* spectral.c — spectral flatness (Wiener entropy) for the DCS consistency check.
 *
 * A self-consistent DCS set decodes music/voiced speech with a peaky spectrum
 * (flatness ~0.03..0.18); a mismatched flash/sample set decodes white noise
 * (flatness ~0.7..0.85). Measuring flatness on a decoded reference sound tells the
 * two apart. Self-contained: a small iterative radix-2 FFT, no external FFT lib.
 */
#include "marsminer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* in-place iterative radix-2 Cooley-Tukey FFT (n a power of two) */
static void fft(double *re, double *im, int n) {
    /* bit-reversal permutation */
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            double t;
            t = re[i];
            re[i] = re[j];
            re[j] = t;
            t = im[i];
            im[i] = im[j];
            im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * M_PI / len;
        double wr = cos(ang), wi = sin(ang);
        for (int i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = i + k + len / 2;
                double xr = re[b] * cr - im[b] * ci;
                double xi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
                double ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

/* Spectral flatness of `n` samples (n forced to the largest power of two <= n,
 * capped at 8192), Hann-windowed — matching the reference measurement. Returns
 * geomean/arithmean of the magnitude spectrum over bins [0, N/2]; 0 on failure. */
double mm_spectral_flatness(const double *x, int n) {
    int N = 1;
    while (N * 2 <= n && N * 2 <= 8192)
        N <<= 1;
    if (N < 64)
        return 0.0;
    double *re = (double *)calloc(N, sizeof *re);
    double *im = (double *)calloc(N, sizeof *im);
    if (!re || !im) {
        free(re);
        free(im);
        return 0.0;
    }
    for (int i = 0; i < N; i++) {
        double w = 0.5 - 0.5 * cos(2.0 * M_PI * i / (N - 1)); /* Hann */
        re[i] = x[i] * w;
    }
    fft(re, im, N);
    int bins = N / 2 + 1;
    double sum_log = 0.0, sum = 0.0;
    for (int i = 0; i < bins; i++) {
        double mag = sqrt(re[i] * re[i] + im[i] * im[i]) + 1e-9;
        sum_log += log(mag);
        sum += mag;
    }
    double gm = exp(sum_log / bins);
    double am = sum / bins;
    free(re);
    free(im);
    return am > 0 ? gm / am : 0.0;
}
