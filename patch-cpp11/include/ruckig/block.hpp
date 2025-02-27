#pragma once

#include <algorithm>
#include <limits>
#include <numeric>
#include <nonstd/optional.hpp>
#include <string>

#include <ruckig/profile.hpp>


namespace ruckig {

//! Which times are possible for synchronization?
class Block {
    struct Interval {
        double left, right; // [s]
        Profile profile; // Profile corresponding to right (end) time

        explicit Interval(double left, double right, const Profile& profile): left(left), right(right), profile(profile) { };
    };

    inline static void add_interval(nonstd::optional<Block::Interval>& interval, const Profile& left, const Profile& right) {
        const double left_duration = left.t_sum[6] + left.brake.duration;
        const double right_duraction = right.t_sum[6] + right.brake.duration;
        if (left_duration < right_duraction) {
            interval = Block::Interval(left_duration, right_duraction, right);
        } else {
            interval = Block::Interval(right_duraction, left_duration, left);
        }
    }

    template<size_t N>
    inline static void remove_profile(std::array<Profile, N>& valid_profiles, size_t& valid_profile_counter, size_t index) {
        for (size_t i = index; i < valid_profile_counter - 1; ++i) {
            valid_profiles[i] = valid_profiles[i + 1];
        }
        valid_profile_counter -= 1;
    }

public:
    Profile p_min; // Save min profile so that it doesn't need to be recalculated in Step2
    double t_min; // [s]

    // Max. 2 intervals can be blocked: called a and b with corresponding profiles, order does not matter
    nonstd::optional<Interval> a, b;

    explicit Block() { }
    explicit Block(const Profile& p_min): p_min(p_min), t_min(p_min.t_sum[6] + p_min.brake.duration) { }

    template<size_t N, bool numerical_robust = true>
    static bool calculate_block(Block& block, std::array<Profile, N>& valid_profiles, size_t valid_profile_counter) {
        // std::cout << "---\n " << valid_profile_counter << std::endl;
        // for (size_t i = 0; i < valid_profile_counter; ++i) {
        //     std::cout << valid_profiles[i].t_sum[6] << " " << valid_profiles[i].to_string() << std::endl;
        // }

        if (valid_profile_counter == 1) {
            block = Block(valid_profiles[0]);
            return true;

        } else if (valid_profile_counter == 2) {
            if (std::abs(valid_profiles[0].t_sum[6] - valid_profiles[1].t_sum[6]) < 8*std::numeric_limits<double>::epsilon()) {
                block = Block(valid_profiles[0]);
                return true;
            }

            if (numerical_robust) {
                const size_t idx_min = (valid_profiles[0].t_sum[6] < valid_profiles[1].t_sum[6]) ? 0 : 1;
                const size_t idx_else_1 = (idx_min + 1) % 2;

                block = Block(valid_profiles[idx_min]);
                Block::add_interval(block.a, valid_profiles[idx_min], valid_profiles[idx_else_1]);
                return true;
            }

        // Only happens due to numerical issues
        } else if (valid_profile_counter == 4) {
            // Find "identical" profiles
            if (std::abs(valid_profiles[0].t_sum[6] - valid_profiles[1].t_sum[6]) < 32*std::numeric_limits<double>::epsilon() && valid_profiles[0].direction != valid_profiles[1].direction) {
                remove_profile<N>(valid_profiles, valid_profile_counter, 1);
            } else if (std::abs(valid_profiles[2].t_sum[6] - valid_profiles[3].t_sum[6]) < 256*std::numeric_limits<double>::epsilon() && valid_profiles[2].direction != valid_profiles[3].direction) {
                remove_profile<N>(valid_profiles, valid_profile_counter, 3);
            } else if (std::abs(valid_profiles[0].t_sum[6] - valid_profiles[3].t_sum[6]) < 256*std::numeric_limits<double>::epsilon() && valid_profiles[0].direction != valid_profiles[3].direction) {
                remove_profile<N>(valid_profiles, valid_profile_counter, 3);
            } else {
                return false;
            }

        } else if (valid_profile_counter % 2 == 0) {
            return false;
        }

        // Find index of fastest profile
        const auto idx_min_it = std::min_element(valid_profiles.cbegin(), valid_profiles.cbegin() + valid_profile_counter, [](const Profile& a, const Profile& b) { return a.t_sum[6] < b.t_sum[6]; });
        const size_t idx_min = std::distance(valid_profiles.cbegin(), idx_min_it);

        block = Block(valid_profiles[idx_min]);

        if (valid_profile_counter == 3) {
            const size_t idx_else_1 = (idx_min + 1) % 3;
            const size_t idx_else_2 = (idx_min + 2) % 3;

            Block::add_interval(block.a, valid_profiles[idx_else_1], valid_profiles[idx_else_2]);
            return true;

        } else if (valid_profile_counter == 5) {
            const size_t idx_else_1 = (idx_min + 1) % 5;
            const size_t idx_else_2 = (idx_min + 2) % 5;
            const size_t idx_else_3 = (idx_min + 3) % 5;
            const size_t idx_else_4 = (idx_min + 4) % 5;

            if (valid_profiles[idx_else_1].direction == valid_profiles[idx_else_2].direction) {
                Block::add_interval(block.a, valid_profiles[idx_else_1], valid_profiles[idx_else_2]);
                Block::add_interval(block.b, valid_profiles[idx_else_3], valid_profiles[idx_else_4]);
            } else {
                Block::add_interval(block.a, valid_profiles[idx_else_1], valid_profiles[idx_else_4]);
                Block::add_interval(block.b, valid_profiles[idx_else_2], valid_profiles[idx_else_3]);
            }
            return true;
        }

        return false;
    }

    inline bool is_blocked(double t) const {
        return (t < t_min) || (a && a->left < t && t < a->right) || (b && b->left < t && t < b->right);
    }

    std::string to_string() const {
        std::string result = "[" + std::to_string(t_min) + " ";
        if (a) {
            result += std::to_string(a->left) + "] [" + std::to_string(a->right) + " ";
        }
        if (b) {
            result += std::to_string(b->left) + "] [" + std::to_string(b->right) + " ";
        }
        return result + "-";
    }
};

} // namespace ruckig
