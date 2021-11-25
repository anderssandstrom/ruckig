#pragma once

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <tuple>
#include <type_traits>

#include <ruckig/block.hpp>
#include <ruckig/brake.hpp>
#include <ruckig/input_parameter.hpp>
#include <ruckig/profile.hpp>
#include <ruckig/position.hpp>
#include <ruckig/velocity.hpp>


namespace ruckig {

// Forward declare alternative OTG algorithms for friend class
template <size_t> class Reflexxes;


//! Interface for the generated trajectory.
template<size_t DOFs>
class Trajectory {
    template<class T> using Vector = typename std::conditional<DOFs >= 1, std::array<T, DOFs>, std::vector<T>>::type;
    template<class T> using VectorIntervals = typename std::conditional<DOFs >= 1, std::array<T, 3*DOFs+1>, std::vector<T>>::type;

    // Allow alternative OTG algorithms to directly access members (i.e. duration)
    friend class Reflexxes<DOFs>;

    constexpr static double eps {std::numeric_limits<double>::epsilon()};

    // Set of current profiles for each DoF
    Vector<Profile> profiles;

    double duration {0.0};
    Vector<double> independent_min_durations;

    Vector<double> pd;

    VectorIntervals<double> possible_t_syncs;
    VectorIntervals<int> idx;

    Vector<Block> blocks;
    Vector<double> p0s, v0s, a0s; // Starting point of profiles without brake trajectory
    Vector<double> inp_min_velocity, inp_min_acceleration;

    Vector<ControlInterface> inp_per_dof_control_interface;
    Vector<Synchronization> inp_per_dof_synchronization;

    Vector<double> new_max_jerk; // For phase synchronization

    Vector<PositionExtrema> position_extrema;

    //! Is the trajectory (in principle) phase synchronizable?
    bool is_input_collinear(const InputParameter<DOFs>& inp, const Vector<double>& jMax, Profile::Direction limiting_direction, size_t limiting_dof, Vector<double>& new_max_jerk) {
        // Get scaling factor of first DoF
        bool pd_found_nonzero {false};
        double v0_scale, a0_scale, vf_scale, af_scale;
        for (size_t dof = 0; dof < pd.size(); ++dof) {
            if (inp_per_dof_synchronization[dof] != Synchronization::Phase) {
                continue;
            }

            pd[dof] = inp.target_position[dof] - inp.current_position[dof];

            if (!pd_found_nonzero && std::abs(pd[dof]) > eps) {
                v0_scale = inp.current_velocity[dof] / pd[dof];
                a0_scale = inp.current_acceleration[dof] / pd[dof];
                vf_scale = inp.target_velocity[dof] / pd[dof];
                af_scale = inp.target_acceleration[dof] / pd[dof];
                pd_found_nonzero = true;
            }
        }

        if (!pd_found_nonzero) { // position difference is zero everywhere...
            return false;
        }

        const double max_jerk_limiting = (limiting_direction == Profile::Direction::UP) ? jMax[limiting_dof] : -jMax[limiting_dof];
        constexpr double eps_colinear {10 * eps};
        
        for (size_t dof = 0; dof < pd.size(); ++dof) {
            if (dof == limiting_dof || inp_per_dof_synchronization[dof] != Synchronization::Phase) {
                continue;
            }

            // Are the vectors collinear?
            if (
                std::abs(inp.current_velocity[dof] - v0_scale * pd[dof]) > eps_colinear
                || std::abs(inp.current_acceleration[dof] - a0_scale * pd[dof]) > eps_colinear
                || std::abs(inp.target_velocity[dof] - vf_scale * pd[dof]) > eps_colinear
                || std::abs(inp.target_acceleration[dof] - af_scale * pd[dof]) > eps_colinear
            ) {
                return false;
            }

            const double scale = pd[dof] / pd[limiting_dof];
            new_max_jerk[dof] = scale * max_jerk_limiting;
        }

        return true;
    }

    bool synchronize(const Vector<Block>& blocks, nonstd::optional<double> t_min, double& t_sync, int& limiting_dof, Vector<Profile>& profiles, bool discrete_duration, double delta_time) {
        if (degrees_of_freedom == 1 && !t_min && !discrete_duration) {
            limiting_dof = 0;
            t_sync = blocks[0].t_min;
            profiles[0] = blocks[0].p_min;
            return true;
        }

        // Possible t_syncs are the start times of the intervals and optional t_min
        bool any_interval {false};
        for (size_t dof = 0; dof < degrees_of_freedom; ++dof) {
            possible_t_syncs[dof] = blocks[dof].t_min;
            possible_t_syncs[degrees_of_freedom + dof] = blocks[dof].a ? blocks[dof].a->right : std::numeric_limits<double>::infinity();
            possible_t_syncs[2 * degrees_of_freedom + dof] = blocks[dof].b ? blocks[dof].b->right : std::numeric_limits<double>::infinity();
            any_interval |= blocks[dof].a || blocks[dof].b;
        }
        possible_t_syncs[3 * degrees_of_freedom] = t_min.value_or(std::numeric_limits<double>::infinity());
        any_interval |= t_min.has_value();

        if (discrete_duration) {
            for (size_t i = 0; i < possible_t_syncs.size(); ++i) {
                possible_t_syncs[i] = std::ceil(possible_t_syncs[i] / delta_time) * delta_time;
            }
        }

        // Test them in sorted order
        auto idx_end = any_interval ? idx.end() : idx.begin() + degrees_of_freedom;
        std::iota(idx.begin(), idx_end, 0);
        std::sort(idx.begin(), idx_end, [&possible_t_syncs=possible_t_syncs](size_t i, size_t j) { return possible_t_syncs[i] < possible_t_syncs[j]; });

        // Start at last tmin (or worse)
        for (auto i = idx.begin() + degrees_of_freedom - 1; i != idx_end; ++i) {
            const double possible_t_sync = possible_t_syncs[*i];
            if (std::any_of(blocks.begin(), blocks.end(), [possible_t_sync](const Block& block){ return block.is_blocked(possible_t_sync); }) || possible_t_sync < t_min.value_or(0.0)) {
                continue;
            }

            t_sync = possible_t_sync;
            if (*i == 3*degrees_of_freedom) { // Optional t_min
                limiting_dof = -1;
                return true;
            }

            const auto div = std::div(*i, degrees_of_freedom);
            limiting_dof = div.rem;
            switch (div.quot) {
                case 0: {
                    profiles[limiting_dof] = blocks[limiting_dof].p_min;
                } break;
                case 1: {
                    profiles[limiting_dof] = blocks[limiting_dof].a->profile;
                } break;
                case 2: {
                    profiles[limiting_dof] = blocks[limiting_dof].b->profile;
                } break;
            }
            return true;
        }

        return false;
    }

public:
    size_t degrees_of_freedom;

    template <size_t D = DOFs, typename std::enable_if<D >= 1, int>::type = 0>
    Trajectory(): degrees_of_freedom(DOFs) { }

    template <size_t D = DOFs, typename std::enable_if<D == 0, int>::type = 0>
    Trajectory(size_t dofs): degrees_of_freedom(dofs) {
        blocks.resize(dofs);
        p0s.resize(dofs);
        v0s.resize(dofs);
        a0s.resize(dofs);
        inp_min_velocity.resize(dofs);
        inp_min_acceleration.resize(dofs);
        inp_per_dof_control_interface.resize(dofs);
        inp_per_dof_synchronization.resize(dofs);
        profiles.resize(dofs);
        independent_min_durations.resize(dofs);
        pd.resize(dofs);

        new_max_jerk.resize(dofs);

        position_extrema.resize(dofs);

        possible_t_syncs.resize(3*dofs+1);
        idx.resize(3*dofs+1);
    }

    //! Calculate the time-optimal waypoint-based trajectory
    template<bool throw_error, bool return_error_at_maximal_duration>
    Result calculate(const InputParameter<DOFs>& inp, double delta_time, bool& was_interrupted) {
        was_interrupted = false;

        for (size_t dof = 0; dof < profiles.size(); ++dof) {
            auto& p = profiles[dof];
            if (!inp.enabled[dof]) {
                p.pf = inp.current_position[dof];
                p.vf = inp.current_velocity[dof];
                p.af = inp.current_acceleration[dof];
                p.t_sum[6] = 0.0;
                continue;
            }

            inp_min_velocity[dof] = inp.min_velocity ? inp.min_velocity.value()[dof] : -inp.max_velocity[dof];
            inp_min_acceleration[dof] = inp.min_acceleration ? inp.min_acceleration.value()[dof] : -inp.max_acceleration[dof];
            inp_per_dof_control_interface[dof] = inp.per_dof_control_interface ? inp.per_dof_control_interface.value()[dof] : inp.control_interface;
            inp_per_dof_synchronization[dof] = inp.per_dof_synchronization ? inp.per_dof_synchronization.value()[dof] : inp.synchronization;

            // Calculate brake (if input exceeds or will exceed limits)
            switch (inp_per_dof_control_interface[dof]) {
                case ControlInterface::Position: {
                    BrakeProfile::get_position_brake_trajectory(inp.current_velocity[dof], inp.current_acceleration[dof], inp.max_velocity[dof], inp_min_velocity[dof], inp.max_acceleration[dof], inp_min_acceleration[dof], inp.max_jerk[dof], p.brake.t, p.brake.j);
                } break;
                case ControlInterface::Velocity: {
                    BrakeProfile::get_velocity_brake_trajectory(inp.current_acceleration[dof], inp.max_acceleration[dof], inp_min_acceleration[dof], inp.max_jerk[dof], p.brake.t, p.brake.j);
                } break;
            }

            p.brake.duration = p.brake.t[0] + p.brake.t[1];
            p0s[dof] = inp.current_position[dof];
            v0s[dof] = inp.current_velocity[dof];
            a0s[dof] = inp.current_acceleration[dof];

            // Integrate brake pre-trajectory
            for (size_t i = 0; i < 2 && p.brake.t[i] > 0; ++i) {
                p.brake.p[i] = p0s[dof];
                p.brake.v[i] = v0s[dof];
                p.brake.a[i] = a0s[dof];
                std::tie(p0s[dof], v0s[dof], a0s[dof]) = Profile::integrate(p.brake.t[i], p0s[dof], v0s[dof], a0s[dof], p.brake.j[i]);
            }

            bool found_profile;
            switch (inp_per_dof_control_interface[dof]) {
                case ControlInterface::Position: {
                    PositionStep1 step1 {p0s[dof], v0s[dof], a0s[dof], inp.target_position[dof], inp.target_velocity[dof], inp.target_acceleration[dof], inp.max_velocity[dof], inp_min_velocity[dof], inp.max_acceleration[dof], inp_min_acceleration[dof], inp.max_jerk[dof]};
                    found_profile = step1.get_profile(p, blocks[dof]);
                } break;
                case ControlInterface::Velocity: {
                    VelocityStep1 step1 {p0s[dof], v0s[dof], a0s[dof], inp.target_velocity[dof], inp.target_acceleration[dof], inp.max_acceleration[dof], inp_min_acceleration[dof], inp.max_jerk[dof]};
                    found_profile = step1.get_profile(p, blocks[dof]);
                } break;
            }

            if (!found_profile) {
                if (throw_error) {
                    throw std::runtime_error("[ruckig] error in step 1, dof: " + std::to_string(dof) + " input: " + inp.to_string());
                }
                return Result::ErrorExecutionTimeCalculation;
            }

            independent_min_durations[dof] = blocks[dof].p_min.brake.duration + blocks[dof].t_min;
            // std::cout << dof << " profile step1: " << blocks[dof].to_string() << std::endl;
        }

        int limiting_dof; // The DoF that doesn't need step 2
        const bool discrete_duration = (inp.duration_discretization == DurationDiscretization::Discrete);
        const bool found_synchronization = synchronize(blocks, inp.minimum_duration, duration, limiting_dof, profiles, discrete_duration, delta_time);
        if (!found_synchronization) {
            if (throw_error) {
                throw std::runtime_error("[ruckig] error in time synchronization: " + std::to_string(duration));
            }
            return Result::ErrorSynchronizationCalculation;
        }

        if (return_error_at_maximal_duration) {
            if (duration > 7.6e3) {
                return Result::ErrorTrajectoryDuration;
            }
        }

        if (duration == 0.0) {
            return Result::Working;
        }

        // None Synchronization
        for (size_t dof = 0; dof < blocks.size(); ++dof) {
            if (inp.enabled[dof] && dof != limiting_dof && inp_per_dof_synchronization[dof] == Synchronization::None) {
                profiles[dof] = blocks[dof].p_min;
            }
        }
        if (std::all_of(inp_per_dof_synchronization.begin(), inp_per_dof_synchronization.end(), [](Synchronization s){ return s == Synchronization::None; })) {
            return Result::Working;
        }

        // Phase Synchronization
        if (std::any_of(inp_per_dof_synchronization.begin(), inp_per_dof_synchronization.end(), [](Synchronization s){ return s == Synchronization::Phase; }) && std::all_of(inp_per_dof_control_interface.begin(), inp_per_dof_control_interface.end(), [](ControlInterface s){ return s == ControlInterface::Position; })) {
            if (is_input_collinear(inp, inp.max_jerk, profiles[limiting_dof].direction, limiting_dof, new_max_jerk)) {
                bool found_time_synchronization {true};
                for (size_t dof = 0; dof < profiles.size(); ++dof) {
                    if (!inp.enabled[dof] || dof == limiting_dof || inp_per_dof_synchronization[dof] != Synchronization::Phase) {
                        continue;
                    }

                    Profile& p = profiles[dof];
                    const double t_profile = duration - p.brake.duration;

                    p.t = profiles[limiting_dof].t; // Copy timing information from limiting DoF
                    p.jerk_signs = profiles[limiting_dof].jerk_signs;
                    p.set_boundary(inp.current_position[dof], inp.current_velocity[dof], inp.current_acceleration[dof], inp.target_position[dof], inp.target_velocity[dof], inp.target_acceleration[dof]);

                    // Profile::Limits::NONE is a small hack, as there is no specialization for that in the check function
                    switch (p.jerk_signs) {
                        case Profile::JerkSigns::UDDU: {
                            if (!p.check_with_timing<Profile::JerkSigns::UDDU, Profile::Limits::NONE>(t_profile, new_max_jerk[dof], inp.max_velocity[dof], inp_min_velocity[dof], inp.max_acceleration[dof], inp_min_acceleration[dof], inp.max_jerk[dof])) {
                                found_time_synchronization = false;
                            }
                        } break;
                        case Profile::JerkSigns::UDUD: {
                            if (!p.check_with_timing<Profile::JerkSigns::UDUD, Profile::Limits::NONE>(t_profile, new_max_jerk[dof], inp.max_velocity[dof], inp_min_velocity[dof], inp.max_acceleration[dof], inp_min_acceleration[dof], inp.max_jerk[dof])) {
                                found_time_synchronization = false;
                            }
                        } break;
                    }

                    p.limits = profiles[limiting_dof].limits; // After check method call to set correct limits
                }

                if (found_time_synchronization && std::all_of(inp_per_dof_synchronization.begin(), inp_per_dof_synchronization.end(), [](Synchronization s){ return s == Synchronization::Phase || s == Synchronization::None; })) {
                    return Result::Working;
                }
            }
        }

        // Time Synchronization
        for (size_t dof = 0; dof < profiles.size(); ++dof) {
            if (!inp.enabled[dof] || dof == limiting_dof || inp_per_dof_synchronization[dof] == Synchronization::None) {
                continue;
            }

            Profile& p = profiles[dof];
            const double t_profile = duration - p.brake.duration;

            if (inp_per_dof_synchronization[dof] == Synchronization::TimeIfNecessary && std::abs(inp.target_velocity[dof]) < eps && std::abs(inp.target_acceleration[dof]) < eps) {
                p = blocks[dof].p_min;
                continue;
            }

            // Check if the final time corresponds to an extremal profile calculated in step 1
            if (std::abs(t_profile - blocks[dof].t_min) < eps) {
                p = blocks[dof].p_min;
                continue;
            } else if (blocks[dof].a && std::abs(t_profile - blocks[dof].a->right) < eps) {
                p = blocks[dof].a->profile;
                continue;
            } else if (blocks[dof].b && std::abs(t_profile - blocks[dof].b->right) < eps) {
                p = blocks[dof].b->profile;
                continue;
            }

            bool found_time_synchronization;
            switch (inp_per_dof_control_interface[dof]) {
                case ControlInterface::Position: {
                    PositionStep2 step2 {t_profile, p0s[dof], v0s[dof], a0s[dof], inp.target_position[dof], inp.target_velocity[dof], inp.target_acceleration[dof], inp.max_velocity[dof], inp_min_velocity[dof], inp.max_acceleration[dof], inp_min_acceleration[dof], inp.max_jerk[dof]};
                    found_time_synchronization = step2.get_profile(p);
                } break;
                case ControlInterface::Velocity: {
                    VelocityStep2 step2 {t_profile, p0s[dof], v0s[dof], a0s[dof], inp.target_velocity[dof], inp.target_acceleration[dof], inp.max_acceleration[dof], inp_min_acceleration[dof], inp.max_jerk[dof]};
                    found_time_synchronization = step2.get_profile(p);
                } break;
            }
            if (!found_time_synchronization) {
                if (throw_error) {
                    throw std::runtime_error("[ruckig] error in step 2 in dof: " + std::to_string(dof) + " for t sync: " + std::to_string(duration) + " input: " + inp.to_string());
                }
                return Result::ErrorSynchronizationCalculation;
            }
            // std::cout << dof << " profile step2: " << p.to_string() << std::endl;
        }

        return Result::Working;
    }

    //! Continue the trajectory calculation
    template<bool throw_error, bool return_error_at_maximal_duration>
    Result continue_calculation(const InputParameter<DOFs>&, double, bool&) {
        return Result::Error;
    }

    //! Get the kinematic state at a given time

    //! The Python wrapper takes `time` as an argument, and returns `new_position`, `new_velocity`, and `new_acceleration` instead.
    void at_time(double time, Vector<double>& new_position, Vector<double>& new_velocity, Vector<double>& new_acceleration, size_t& new_section) const {
        if (DOFs == 0) {
            if (degrees_of_freedom != new_position.size() || degrees_of_freedom != new_velocity.size() || degrees_of_freedom != new_acceleration.size()) {
                throw std::runtime_error("[ruckig] mismatch in degrees of freedom (vector size).");
            }
        }

        if (time >= duration) {
            // Keep constant acceleration
            new_section = 1;
            for (size_t dof = 0; dof < profiles.size(); ++dof) {
                const double t_diff = time - (profiles[dof].brake.duration + profiles[dof].t_sum[6]);
                std::tie(new_position[dof], new_velocity[dof], new_acceleration[dof]) = Profile::integrate(t_diff, profiles[dof].pf, profiles[dof].vf, profiles[dof].af, 0);
            }
            return;
        }

        new_section = 0;
        for (size_t dof = 0; dof < profiles.size(); ++dof) {
            const Profile& p = profiles[dof];

            double t_diff = time;
            if (p.brake.duration > 0) {
                if (t_diff < p.brake.duration) {
                    const size_t index = (t_diff < p.brake.t[0]) ? 0 : 1;
                    if (index > 0) {
                        t_diff -= p.brake.t[index - 1];
                    }

                    std::tie(new_position[dof], new_velocity[dof], new_acceleration[dof]) = Profile::integrate(t_diff, p.brake.p[index], p.brake.v[index], p.brake.a[index], p.brake.j[index]);
                    continue;
                } else {
                    t_diff -= p.brake.duration;
                }
            }

            // Non-time synchronization
            if (t_diff >= p.t_sum[6]) {
                // Keep constant acceleration
                std::tie(new_position[dof], new_velocity[dof], new_acceleration[dof]) = Profile::integrate(t_diff - p.t_sum[6], p.pf, p.vf, p.af, 0);
                continue;
            }

            const auto index_ptr = std::upper_bound(p.t_sum.begin(), p.t_sum.end(), t_diff);
            const size_t index = std::distance(p.t_sum.begin(), index_ptr);

            if (index > 0) {
                t_diff -= p.t_sum[index - 1];
            }

            std::tie(new_position[dof], new_velocity[dof], new_acceleration[dof]) = Profile::integrate(t_diff, p.p[index], p.v[index], p.a[index], p.j[index]);
        }
    }

    //! Get the kinematic state and current section at a given time
    void at_time(double time, Vector<double>& new_position, Vector<double>& new_velocity, Vector<double>& new_acceleration) const {
        size_t new_section;
        at_time(time, new_position, new_velocity, new_acceleration, new_section);
    }

    //! Get the duration of the (synchronized) trajectory
    double get_duration() const {
        return duration;
    }

    //! Get the durations when the intermediate waypoints are reached
    std::vector<double> get_intermediate_durations() const {
        return {duration};
    }

    //! Get the minimum duration of each independent DoF
    Vector<double> get_independent_min_durations() const {
        return independent_min_durations;
    }

    //! Get the min/max values of the position for each DoF
    Vector<PositionExtrema> get_position_extrema() {
        for (size_t dof = 0; dof < profiles.size(); ++dof) {
            position_extrema[dof] = profiles[dof].get_position_extrema();
        }
        return position_extrema;
    }

    //! Get the time that this trajectory passes a specific position of a given DoF the first time

    //! If the position is passed, this method returns true, otherwise false
    //! The Python wrapper takes `dof` and `position` as arguments and returns `time` (or `None`) instead
    bool get_first_time_at_position(size_t dof, double position, double& time) const {
        if (dof >= degrees_of_freedom) {
            return false;
        }

        double v, a;
        return profiles[dof].get_first_state_at_position(position, time, v, a);
    }
};

} // namespace ruckig
