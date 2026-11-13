#pragma once
#define NOMINMAX
#include <cmath>
#include <algorithm>
#include <immintrin.h>
#include <intrin.h>

class BSPricer {
public:
    // ============================================================
    // 1. 标量计算：标准正态分布 CDF
    // ============================================================
    static inline double norm_cdf(double x) {
        const double a1 = 0.254829592, a2 = -0.284496736, a3 = 1.421413741;
        const double a4 = -1.453152027, a5 = 1.061405429, p = 0.3275911;
        int sign = 1; if (x < 0.0) sign = -1;
        x = std::abs(x) / std::sqrt(2.0);
        double t = 1.0 / (1.0 + p * x);
        double y = 1.0 - (((((a5 * t + a4) * t) + a3) * t + a2) * t + a1) * t * std::exp(-x * x);
        return 0.5 * (1.0 + sign * y);
    }

    // ============================================================
    // 2. 标量计算：Black-Scholes 期权理论价格
    // ============================================================
    static inline double calculate_price(double S, double K, double T, double r, double sigma, bool is_call) {
        if (T <= 0.0) {
            double diff = is_call ? (S - K) : (K - S);
            return diff > 0.0 ? diff : 0.0;
        }
        double d1 = (std::log(S / K) + (r + 0.5 * sigma * sigma) * T) / (sigma * std::sqrt(T));
        double d2 = d1 - sigma * std::sqrt(T);
        if (is_call) {
            return S * norm_cdf(d1) - K * std::exp(-r * T) * norm_cdf(d2);
        }
        else {
            return K * std::exp(-r * T) * norm_cdf(-d2) - S * norm_cdf(-d1);
        }
    }

    // ============================================================
    // 3. 标量计算：Delta
    // ============================================================
    static inline double calculate_delta(double S, double K, double T, double r, double sigma, bool is_call) {
        if (T <= 0.0) return 0.0;
        double d1 = (std::log(S / K) + (r + 0.5 * sigma * sigma) * T) / (sigma * std::sqrt(T));
        return is_call ? norm_cdf(d1) : norm_cdf(d1) - 1.0;
    }

    // ============================================================
    // 4. AVX2 基础运算 (批量)
    // ============================================================
    static inline __m256d norm_cdf_avx(__m256d x) {
        __m256d sqrt2 = _mm256_set1_pd(1.4142135623730951);
        __m256d div = _mm256_div_pd(x, sqrt2);
        __m256d erf_val = _mm256_erf_pd(div);
        __m256d one = _mm256_set1_pd(1.0);
        __m256d half = _mm256_set1_pd(0.5);
        return _mm256_mul_pd(half, _mm256_add_pd(one, erf_val));
    }

    static inline __m256d norm_pdf_avx(__m256d x) {
        __m256d neg_half_x2 = _mm256_mul_pd(_mm256_set1_pd(-0.5), _mm256_mul_pd(x, x));
        __m256d exp_val = _mm256_exp_pd(neg_half_x2);
        __m256d sqrt2pi = _mm256_set1_pd(2.5066282746310002);
        return _mm256_div_pd(exp_val, sqrt2pi);
    }

    // 标准 AVX2 计算 (处理满 4 个)
    static inline void calculate_greeks_avx(
        __m256d S, __m256d K, __m256d T, __m256d r, __m256d sigma, __m256d is_call,
        __m256d& out_delta, __m256d& out_gamma, __m256d& out_vega) {

        __m256d sqrtT = _mm256_sqrt_pd(T);
        __m256d s_k = _mm256_div_pd(S, K);
        __m256d log_sk = _mm256_log_pd(s_k);
        __m256d sigma_sq = _mm256_mul_pd(sigma, sigma);
        __m256d half_sigma_sq = _mm256_mul_pd(sigma_sq, _mm256_set1_pd(0.5));
        __m256d r_plus = _mm256_add_pd(r, half_sigma_sq);
        __m256d num = _mm256_add_pd(log_sk, _mm256_mul_pd(r_plus, T));
        __m256d denom = _mm256_mul_pd(sigma, sqrtT);
        __m256d d1 = _mm256_div_pd(num, denom);
        __m256d d2 = _mm256_sub_pd(d1, denom);

        __m256d cdf_d1 = norm_cdf_avx(d1);
        __m256d one = _mm256_set1_pd(1.0);
        __m256d delta_put = _mm256_sub_pd(cdf_d1, one);
        out_delta = _mm256_blendv_pd(delta_put, cdf_d1, is_call);

        __m256d pdf_d1 = norm_pdf_avx(d1);
        __m256d gamma_denom = _mm256_mul_pd(S, _mm256_mul_pd(sigma, sqrtT));
        out_gamma = _mm256_div_pd(pdf_d1, gamma_denom);

        __m256d vega_num = _mm256_mul_pd(S, _mm256_mul_pd(pdf_d1, sqrtT));
        out_vega = _mm256_div_pd(vega_num, _mm256_set1_pd(100.0));
    }

    // ============================================================
    // 5. 【核心修正】带掩码的 AVX2 计算 (处理尾部 1~3 个)
    // 注意掩码类型是 __m256i，不是 __m256d
    // ============================================================
    static inline void calculate_greeks_avx_masked(
        __m256d S, const double* K_ptr, const double* T_ptr, __m256d r,
        const double* sigma_ptr, const double* is_call_ptr, __m256i mask,
        __m256d& out_delta, __m256d& out_gamma, __m256d& out_vega) {

        // 使用 _mm256_maskload_pd 只加载掩码为 1 的对应位置 (类型是 __m256i)
        __m256d K = _mm256_maskload_pd(K_ptr, mask);
        __m256d T = _mm256_maskload_pd(T_ptr, mask);
        __m256d sigma = _mm256_maskload_pd(sigma_ptr, mask);
        __m256d is_call = _mm256_maskload_pd(is_call_ptr, mask);

        // 为了防止无效位置产生除零或 NaN，把无效位置的 T 和 sigma 稍微兜底一下
        __m256d mask_f = _mm256_castsi256_pd(mask); // 转成浮点掩码用于 blend
        __m256d safe_T = _mm256_blendv_pd(_mm256_set1_pd(0.0001), T, mask_f);
        __m256d safe_sigma = _mm256_blendv_pd(_mm256_set1_pd(0.01), sigma, mask_f);

        __m256d d_out, g_out, v_out;
        calculate_greeks_avx(S, K, safe_T, r, safe_sigma, is_call, d_out, g_out, v_out);

        // 关键步骤：把无效位置的结果归零，防止污染累加和
        out_delta = _mm256_and_pd(d_out, mask_f);
        out_gamma = _mm256_and_pd(g_out, mask_f);
        out_vega = _mm256_and_pd(v_out, mask_f);
    }
};