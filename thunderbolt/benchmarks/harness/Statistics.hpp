// Thunderbolt HT - summary statistics for benchmark samples.
//
// Median and IQR rather than mean and standard deviation, and that is not a
// stylistic preference. The reference machine is a 15 W part whose sustained
// clock drifts under load, so a run's timing distribution is skewed by thermal
// throttling rather than normally distributed. A mean is dragged by the throttled
// tail; a standard deviation implies a symmetry that is not there. The median is
// the honest central estimate, and the interquartile range says how much spread
// there was without pretending to know its shape.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace thunderbolt::bench {

struct Summary {
    std::size_t sample_count = 0;

    double min    = 0.0;
    double p25    = 0.0;
    double median = 0.0;
    double p75    = 0.0;
    double max    = 0.0;

    // p75 - p25. Reported instead of a standard deviation.
    double iqr = 0.0;

    // Mean is recorded too, but only so a reader can SEE the skew by comparing it
    // against the median. It is never the headline number.
    double mean = 0.0;
};

// Linear-interpolated percentile over a sorted range.
[[nodiscard]] inline double percentile_sorted(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty()) {
        return 0.0;
    }
    if (sorted.size() == 1) {
        return sorted.front();
    }
    const double position = fraction * static_cast<double>(sorted.size() - 1);
    const auto   lower    = static_cast<std::size_t>(std::floor(position));
    const auto   upper    = static_cast<std::size_t>(std::ceil(position));
    const double weight   = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - weight) + sorted[upper] * weight;
}

[[nodiscard]] inline Summary summarize(std::vector<double> samples) {
    Summary summary;
    summary.sample_count = samples.size();
    if (samples.empty()) {
        return summary;
    }

    std::sort(samples.begin(), samples.end());

    summary.min    = samples.front();
    summary.max    = samples.back();
    summary.p25    = percentile_sorted(samples, 0.25);
    summary.median = percentile_sorted(samples, 0.50);
    summary.p75    = percentile_sorted(samples, 0.75);
    summary.iqr    = summary.p75 - summary.p25;

    double total = 0.0;
    for (double value : samples) {
        total += value;
    }
    summary.mean = total / static_cast<double>(samples.size());

    return summary;
}

// Least-squares fit of y = intercept + slope * x.
//
// Used by the granularity experiment to recover per-task scheduler cost as the
// SLOPE of total time against task count. That measurement is why the profiler
// does not need to timestamp every dispatch: instrumenting a 200 ns task with a
// 30 ns clock read would perturb the very quantity being measured, whereas a
// slope over a range of task counts costs nothing at run time.
struct LinearFit {
    double intercept = 0.0;
    double slope     = 0.0;
    // Coefficient of determination. A poor fit means the linear-overhead model
    // does not describe the data, and the slope should not be quoted as a
    // per-task cost.
    double r_squared = 0.0;
    bool   valid     = false;
};

[[nodiscard]] inline LinearFit fit_linear(const std::vector<double>& x,
                                          const std::vector<double>& y) {
    LinearFit fit;
    if (x.size() != y.size() || x.size() < 2) {
        return fit;
    }

    const auto n = static_cast<double>(x.size());
    double     sum_x = 0.0, sum_y = 0.0, sum_xx = 0.0, sum_xy = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        sum_x += x[i];
        sum_y += y[i];
        sum_xx += x[i] * x[i];
        sum_xy += x[i] * y[i];
    }

    const double denominator = n * sum_xx - sum_x * sum_x;
    if (std::abs(denominator) < 1e-12) {
        return fit;  // all x identical: slope undefined
    }

    fit.slope     = (n * sum_xy - sum_x * sum_y) / denominator;
    fit.intercept = (sum_y - fit.slope * sum_x) / n;

    const double mean_y = sum_y / n;
    double       ss_total = 0.0, ss_residual = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double predicted = fit.intercept + fit.slope * x[i];
        ss_total += (y[i] - mean_y) * (y[i] - mean_y);
        ss_residual += (y[i] - predicted) * (y[i] - predicted);
    }
    fit.r_squared = (ss_total > 0.0) ? (1.0 - ss_residual / ss_total) : 0.0;
    fit.valid     = true;
    return fit;
}

} // namespace thunderbolt::bench
