/*
 * eqcurve.c - see eqcurve.h.
 */
#include "eqcurve.h"

static const double k_freqs[E1FX_EQ_BANDS] = E1FX_EQ_FREQS;

void eqc_peaking(E1BqCoef *c, double fs, double f0, double bw_hz, double gain_db)
{
    if (gain_db == 0.0) {
        e1_bq_identity(c);
        return;
    }
    /* H(z) = ((1 + A) + G (1 - A)) / 2 with the 2nd-order all-pass
       A(z) = (k + b(1+k) z^-1 + z^-2) / (1 + b(1+k) z^-1 + k z^-2), b = -cos(w0).
       Boost/cut symmetric bandwidth: t = tan(pi*bw/fs) / sqrt(G), k = (1 - t) / (1 + t). */
    double G = pow(10.0, gain_db / 20.0);
    double w0 = 2.0 * E1_PI * f0 / fs;
    double bw = 2.0 * E1_PI * bw_hz / fs;
    if (bw > 0.99 * E1_PI) bw = 0.99 * E1_PI;
    double t = tan(bw / 2.0) / sqrt(G);
    double k = (1.0 - t) / (1.0 + t);
    double beta = -cos(w0);
    c->b0 = (1.0 + G) / 2.0 + (1.0 - G) / 2.0 * k;
    c->b1 = beta * (1.0 + k);
    c->b2 = (1.0 + G) / 2.0 * k + (1.0 - G) / 2.0;
    c->a1 = beta * (1.0 + k);
    c->a2 = k;
}

void eqc_design(double fs, float preamp_db, const float gains_db[E1FX_EQ_BANDS], E1BqCoef out[E1FX_EQ_BANDS])
{
    const double shelf_q = 0.70710678118654752;
    for (int i = 0; i < E1FX_EQ_BANDS; ++i) {
        double g = (double)gains_db[i];
        double f0 = k_freqs[i];
        if (f0 > 0.45 * fs) f0 = 0.45 * fs;
        if (g == 0.0) e1_bq_identity(&out[i]);
        else if (i == 0) e1_bq_lowshelf(&out[i], fs, f0, shelf_q, g);
        else if (i == E1FX_EQ_BANDS - 1) e1_bq_highshelf(&out[i], fs, f0, shelf_q, g);
        else eqc_peaking(&out[i], fs, f0, 0.70710678118654752 * f0, g);
    }
    double pre = e1_db2lin((double)preamp_db);
    out[0].b0 *= pre;
    out[0].b1 *= pre;
    out[0].b2 *= pre;
}

double eqc_response_db(const E1BqCoef c[E1FX_EQ_BANDS], double fs, double f)
{
    double db = 0.0;
    for (int i = 0; i < E1FX_EQ_BANDS; ++i) db += e1_lin2db(e1_bq_mag(&c[i], fs, f));
    return db;
}
