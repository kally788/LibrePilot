/**
 ******************************************************************************
 * @addtogroup OpenPilot Math Utilities
 * @{
 * @addtogroup Sine and cosine methods that use a cached lookup table
 * @{
 *
 * @file       pid.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2012.
 * @brief      Methods to work with PID structure
 *
 * @see        The GNU Public License (GPL) Version 3
 *
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "openpilot.h"
#include "pid.h"
#include <mathmisc.h>
#include <pios_math.h>

// ! Store the shared time constant for the derivative cutoff.
static float deriv_tau   = 7.9577e-3f;

// ! Store the setpoint weight to apply for the derivative term
static float deriv_gamma = 1.0f;

/**
 * Update the PID computation
 * @param[in] pid The PID struture which stores temporary information
 * @param[in] err The error term
 * @param[in] dT  The time step
 * @returns Output the computed controller value
 */
float pid_apply(struct pid *pid, const float err, float dT)
{
    // Scale up accumulator by 1000 while computing to avoid losing precision
    pid->iAccumulator += err * (pid->i * dT * 1000.0f);
    pid->iAccumulator  = boundf(pid->iAccumulator, pid->iLim * -1000.0f, pid->iLim * 1000.0f);

    // Calculate DT1 term
    float diff  = (err - pid->lastErr);
    float dterm = 0;
    pid->lastErr = err;
    if (pid->d > 0.0f && dT > 0.0f) {
        dterm = pid->lastDer + dT / (dT + deriv_tau) * ((diff * pid->d / dT) - pid->lastDer);
        pid->lastDer = dterm; // ^ set constant to 1/(2*pi*f_cutoff)
    } // 7.9577e-3  means 20 Hz f_cutoff

    return (err * pid->p) + pid->iAccumulator / 1000.0f + dterm;
}

/**
 * Update the PID computation with setpoint weighting on the derivative
 * @param[in] pid The PID struture which stores temporary information
 * @param[in] factor A dynamic factor to scale pid's by, to compensate nonlinearities
 * @param[in] setpoint The setpoint to use
 * @param[in] measured The measured value of output
 * @param[in] dT  The time step
 * @returns Output the computed controller value
 *
 * This version of apply uses setpoint weighting for the derivative component so the gain
 * on the gyro derivative can be different than the gain on the setpoint derivative
 */
float pid_apply_setpoint(struct pid *pid, const float factor, const float setpoint, const float measured, float dT)
{
    float err = setpoint - measured;

    // Scale up accumulator by 1000 while computing to avoid losing precision
    pid->iAccumulator += err * (factor * pid->i * dT * 1000.0f);
    pid->iAccumulator  = boundf(pid->iAccumulator, pid->iLim * -1000.0f, pid->iLim * 1000.0f);

    // Calculate DT1 term,
    float dterm = 0;
    float diff  = ((deriv_gamma * setpoint - measured) - pid->lastErr);
    pid->lastErr = (deriv_gamma * setpoint - measured);
    if (pid->d > 0.0f && dT > 0.0f) {
        dterm = pid->lastDer + dT / (dT + deriv_tau) * ((factor * diff * pid->d / dT) - pid->lastDer);
        pid->lastDer = dterm; // ^ set constant to 1/(2*pi*f_cutoff)
    } // 7.9577e-3  means 20 Hz f_cutoff

    return (err * factor * pid->p) + pid->iAccumulator / 1000.0f + dterm;
}

/**
 * Reset a bit
 * @param[in] pid The pid to reset
 */
void pid_zero(struct pid *pid)
{
    if (!pid) {
        return;
    }

    pid->iAccumulator = 0;
    pid->lastErr = 0;
    pid->lastDer = 0;
}

/**
 * @brief Configure the common terms that alter ther derivative
 * @param[in] cutoff The cutoff frequency (in Hz)
 * @param[in] gamma The gamma term for setpoint shaping (unsused now)
 */
void pid_configure_derivative(float cutoff, float g)
{
    deriv_tau   = 1.0f / (2 * M_PI_F * cutoff);
    deriv_gamma = g;
}

/**
 * Configure the settings for a pid structure
 * @param[out] pid The PID structure to configure
 * @param[in] p The proportional term
 * @param[in] i The integral term
 * @param[in] d The derivative term
 */
void pid_configure(struct pid *pid, float p, float i, float d, float iLim)
{
    if (!pid) {
        return;
    }

    pid->p    = p;
    pid->i    = i;
    pid->d    = d;
    pid->iLim = iLim;
}

float pid_scale_factor_from_line(float x, struct point *p0, struct point *p1)
{
    // Setup line y = m * x + b.
    const float dY1 = p1->y - p0->y;
    const float dX1 = p1->x - p0->x;
    const float m   = dY1 / dX1; // == dY0 / dX0 == (p0.y - b) / (p0.x - 0.0f) ==>
    const float b   = p0->y - m * p0->x;

    // Scale according to given x.
    float y = m * x + b;

    return 1.0f + y;
}

float pid_scale_factor(pid_scaler *scaler)
{
    const int length = sizeof(scaler->points) / sizeof(typeof(scaler->points[0]));

    // Find the two points where is within scaler->x. Use the outer points if
    // scaler->x is smaller than the first x value or larger than the last x value.
    int end_point = length - 1;

    for (int i = 1; i < length; i++) {
        if (scaler->x < scaler->points[i].x) {
            end_point = i;
            break;
        }
    }

    return pid_scale_factor_from_line(scaler->x, &scaler->points[end_point - 1], &scaler->points[end_point]);
}

float pid_apply_setpoint_scaled(struct pid *pid, const float factor, const float setpoint, const float measured, float dT,
                                pid_scaler *scaler)
{
    // Save PD values.
    float p     = pid->p;
    float d     = pid->d;

    // Scale PD values.
    float scale = pid_scale_factor(scaler);

    pid->p *= scale;
    pid->d *= scale;

    float value = pid_apply_setpoint(pid, factor, setpoint, measured, dT);

    // Restore PD values.
    pid->p = p;
    pid->d = d;

    return value;
}
