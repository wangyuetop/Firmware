/****************************************************************************
 *
 *   Copyright (C) 2013 Anton Babushkin. All rights reserved.
 *   Author: 	Anton Babushkin	<rk3dov@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file position_estimator_inav_main.c
 * Model-identification based position estimator for multirotors
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <fcntl.h>
#include <float.h>
#include <string.h>
#include <nuttx/config.h>
#include <nuttx/sched.h>
#include <sys/prctl.h>
#include <termios.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <uORB/uORB.h>
#include <uORB/topics/sensor_combined.h>
#include <uORB/topics/parameter_update.h>
#include <uORB/topics/vehicle_attitude.h>
#include <uORB/topics/vehicle_status.h>
#include <uORB/topics/vehicle_local_position.h>
#include <uORB/topics/vehicle_gps_position.h>
#include <mavlink/mavlink_log.h>
#include <poll.h>
#include <systemlib/geo/geo.h>
#include <systemlib/systemlib.h>
#include <systemlib/conversions.h>
#include <drivers/drv_hrt.h>

#include "position_estimator_inav_params.h"
#include "kalman_filter_inertial.h"

static bool thread_should_exit = false; /**< Deamon exit flag */
static bool thread_running = false; /**< Deamon status flag */
static int position_estimator_inav_task; /**< Handle of deamon task / thread */
static bool verbose_mode = false;

__EXPORT int position_estimator_inav_main(int argc, char *argv[]);

int position_estimator_inav_thread_main(int argc, char *argv[]);

static void usage(const char *reason);

/**
 * Print the correct usage.
 */
static void usage(const char *reason) {
	if (reason)
		fprintf(stderr, "%s\n", reason);
		fprintf(stderr,
				"usage: position_estimator_inav {start|stop|status} [-v]\n\n");
		exit(1);
	}

/**
 * The position_estimator_inav_thread only briefly exists to start
 * the background job. The stack size assigned in the
 * Makefile does only apply to this management task.
 *
 * The actual stack size should be set in the call
 * to task_create().
 */
int position_estimator_inav_main(int argc, char *argv[]) {
	if (argc < 1)
		usage("missing command");

	if (!strcmp(argv[1], "start")) {
		if (thread_running) {
			printf("position_estimator_inav already running\n");
			/* this is not an error */
			exit(0);
		}
		if (argc > 1)
			if (!strcmp(argv[2], "-v"))
				verbose_mode = true;

		thread_should_exit = false;
		position_estimator_inav_task = task_spawn("position_estimator_inav",
				SCHED_RR, SCHED_PRIORITY_MAX - 5, 4096,
				position_estimator_inav_thread_main,
				(argv) ? (const char **) &argv[2] : (const char **) NULL );
		exit(0);
	}
	if (!strcmp(argv[1], "stop")) {
		thread_should_exit = true;
		exit(0);
	}

	if (!strcmp(argv[1], "status")) {
		if (thread_running) {
			printf("\tposition_estimator_inav is running\n");
		} else {
			printf("\tposition_estimator_inav not started\n");
		}
		exit(0);
	}

	usage("unrecognized command");
	exit(1);
}

/****************************************************************************
 * main
 ****************************************************************************/
int position_estimator_inav_thread_main(int argc, char *argv[]) {
	/* welcome user */
	printf("[position_estimator_inav] started\n");
	static int mavlink_fd;
	mavlink_fd = open(MAVLINK_LOG_DEVICE, 0);
	mavlink_log_info(mavlink_fd, "[position_estimator_inav] started");

	/* initialize values */
	float x_est[3] = { 0.0f, 0.0f, 0.0f };
	float y_est[3] = { 0.0f, 0.0f, 0.0f };
	float z_est[3] = { 0.0f, 0.0f, 0.0f };

	int baro_loop_cnt = 0;
	int baro_loop_end = 70; /* measurement for 1 second */
	float baro_alt0 = 0.0f; /* to determine while start up */

	static double lat_current = 0.0; //[°]] --> 47.0
	static double lon_current = 0.0; //[°]] -->8.5
	static double alt_current = 0.0; //[m] above MSL

	/* declare and safely initialize all structs */
	struct vehicle_status_s vehicle_status;
	memset(&vehicle_status, 0, sizeof(vehicle_status));
	/* make sure that baroINITdone = false */
	struct sensor_combined_s sensor;
	memset(&sensor, 0, sizeof(sensor));
	struct vehicle_gps_position_s gps;
	memset(&gps, 0, sizeof(gps));
	struct vehicle_attitude_s att;
	memset(&att, 0, sizeof(att));
	struct vehicle_local_position_s pos;
	memset(&pos, 0, sizeof(pos));

	/* subscribe */
	int parameter_update_sub = orb_subscribe(ORB_ID(parameter_update));
	int vehicle_status_sub = orb_subscribe(ORB_ID(vehicle_status));
	int sensor_combined_sub = orb_subscribe(ORB_ID(sensor_combined));
	int vehicle_attitude_sub = orb_subscribe(ORB_ID(vehicle_attitude));
	int vehicle_gps_position_sub = orb_subscribe(ORB_ID(vehicle_gps_position));

	/* advertise */
	orb_advert_t vehicle_local_position_pub = orb_advertise(ORB_ID(vehicle_local_position), &pos);

	struct position_estimator_inav_params params;
	struct position_estimator_inav_param_handles pos_inav_param_handles;
	/* initialize parameter handles */
	parameters_init(&pos_inav_param_handles);

	bool local_flag_baroINITdone = false; /* in any case disable baroINITdone */
	/* FIRST PARAMETER READ at START UP*/
	struct parameter_update_s param_update;
	orb_copy(ORB_ID(parameter_update), parameter_update_sub, &param_update); /* read from param to clear updated flag */
	/* FIRST PARAMETER UPDATE */
	parameters_update(&pos_inav_param_handles, &params);
	/* END FIRST PARAMETER UPDATE */

	/* wait until gps signal turns valid, only then can we initialize the projection */
	if (params.use_gps) {
		struct pollfd fds_init[1] = {
				{ .fd = vehicle_gps_position_sub, .events = POLLIN }
		};

		while (gps.fix_type < 3) {
			if (poll(fds_init, 1, 5000)) { /* poll only two first subscriptions */
				if (fds_init[0].revents & POLLIN) {
					/* Wait for the GPS update to propagate (we have some time) */
					usleep(5000);
					orb_copy(ORB_ID(vehicle_gps_position), vehicle_gps_position_sub, &gps);
				}
			}
			static int printcounter = 0;
			if (printcounter == 100) {
				printcounter = 0;
				printf("[position_estimator_inav] wait for GPS fix type 3\n");
			}
			printcounter++;
		}

		/* get GPS position for first initialization */
		orb_copy(ORB_ID(vehicle_gps_position), vehicle_gps_position_sub, &gps);
		lat_current = ((double) (gps.lat)) * 1e-7;
		lon_current = ((double) (gps.lon)) * 1e-7;
		alt_current = gps.alt * 1e-3;

		pos.home_lat = lat_current * 1e7;
		pos.home_lon = lon_current * 1e7;
		pos.home_timestamp = hrt_absolute_time();

		/* initialize coordinates */
		map_projection_init(lat_current, lon_current);
		/* publish global position messages only after first GPS message */
	}
	printf(
			"[position_estimator_inav] initialized projection with: lat: %.10f,  lon:%.10f\n",
			lat_current, lon_current);

	hrt_abstime last_time = 0;
	thread_running = true;
	uint32_t accelerometer_counter = 0;
	uint32_t baro_counter = 0;
	uint16_t accelerometer_updates = 0;
	uint16_t baro_updates = 0;
	uint16_t gps_updates = 0;
	uint16_t attitude_updates = 0;
	hrt_abstime updates_counter_start = hrt_absolute_time();
	uint32_t updates_counter_len = 1000000;
	hrt_abstime pub_last = hrt_absolute_time();
	uint32_t pub_interval = 4000;	// limit publish rate to 250 Hz

	/* main loop */
	struct pollfd fds[5] = {
			{ .fd = parameter_update_sub, .events = POLLIN },
			{ .fd = vehicle_status_sub, .events = POLLIN },
			{ .fd = vehicle_attitude_sub, .events = POLLIN },
			{ .fd = sensor_combined_sub, .events = POLLIN },
			{ .fd = vehicle_gps_position_sub, .events = POLLIN }
	};
	printf("[position_estimator_inav] main loop started\n");
	while (!thread_should_exit) {
		bool accelerometer_updated = false;
		bool baro_updated = false;
		bool gps_updated = false;
		float local_pos_gps[3] = { 0.0f, 0.0f, 0.0f };

		int ret = poll(fds, params.use_gps ? 5 : 4, 10); // wait maximal this 10 ms = 100 Hz minimum rate
		if (ret < 0) {
			/* poll error */
			printf("[position_estimator_inav] subscriptions poll error\n");
			thread_should_exit = true;
			continue;
		} else if (ret > 0) {
			/* parameter update */
			if (fds[0].revents & POLLIN) {
				/* read from param to clear updated flag */
				struct parameter_update_s update;
				orb_copy(ORB_ID(parameter_update), parameter_update_sub,
						&update);
				/* update parameters */
				parameters_update(&pos_inav_param_handles, &params);
			}
			/* vehicle status */
			if (fds[1].revents & POLLIN) {
				orb_copy(ORB_ID(vehicle_status), vehicle_status_sub,
						&vehicle_status);
			}
			/* vehicle attitude */
			if (fds[2].revents & POLLIN) {
				orb_copy(ORB_ID(vehicle_attitude), vehicle_attitude_sub, &att);
				attitude_updates++;
			}
			/* sensor combined */
			if (fds[3].revents & POLLIN) {
				orb_copy(ORB_ID(sensor_combined), sensor_combined_sub, &sensor);
				if (sensor.accelerometer_counter > accelerometer_counter) {
					accelerometer_updated = true;
					accelerometer_counter = sensor.accelerometer_counter;
					accelerometer_updates++;
				}
				if (sensor.baro_counter > baro_counter) {
					baro_updated = true;
					baro_counter = sensor.baro_counter;
					baro_updates++;
				}
				// barometric pressure estimation at start up
				if (!local_flag_baroINITdone && baro_updated) {
					// mean calculation over several measurements
					if (baro_loop_cnt < baro_loop_end) {
						baro_alt0 += sensor.baro_alt_meter;
						baro_loop_cnt++;
					} else {
						baro_alt0 /= (float) (baro_loop_cnt);
						local_flag_baroINITdone = true;
						char str[80];
						sprintf(str,
								"[position_estimator_inav] baro_alt0 = %.2f",
								baro_alt0);
						printf("%s\n", str);
						mavlink_log_info(mavlink_fd, str);
					}
				}
			}
			if (params.use_gps) {
				/* vehicle GPS position */
				if (fds[4].revents & POLLIN) {
					/* new GPS value */
					orb_copy(ORB_ID(vehicle_gps_position), vehicle_gps_position_sub, &gps);
					/* Project gps lat lon (Geographic coordinate system) to plane */
					map_projection_project(((double) (gps.lat)) * 1e-7,
							((double) (gps.lon)) * 1e-7, &(local_pos_gps[0]),
							&(local_pos_gps[1]));
					local_pos_gps[2] = (float) (gps.alt * 1e-3);
					gps_updated = true;
					pos.valid = gps.fix_type >= 3;
					gps_updates++;
				}
			} else {
				pos.valid = true;
			}

		} /* end of poll return value check */

		hrt_abstime t = hrt_absolute_time();
		float dt = (t - last_time) / 1000000.0;
		last_time = t;
		if (att.R_valid) {
			/* transform acceleration vector from UAV frame to NED frame */
			float accel_NED[3];
			for (int i = 0; i < 3; i++) {
				accel_NED[i] = 0.0f;
				for (int j = 0; j < 3; j++) {
					accel_NED[i] += att.R[i][j] * sensor.accelerometer_m_s2[j];
				}
			}
			accel_NED[2] += CONSTANTS_ONE_G;

			/* kalman filter for altitude */
			kalman_filter_inertial_predict(dt, z_est);
			/* prepare vectors for kalman filter correction */
			float z_meas[2];	// position, acceleration
			bool use_z[2] = { false, false };
			if (local_flag_baroINITdone && baro_updated) {
				z_meas[0] = baro_alt0 - sensor.baro_alt_meter;	// Z = -alt
				use_z[0] = true;
			}
			if (accelerometer_updated) {
				z_meas[1] = accel_NED[2];
				use_z[1] = true;
			}
			if (use_z[0] || use_z[1]) {
				/* correction */
				kalman_filter_inertial2_update(z_est, z_meas, params.k_alt, use_z);
			}

			if (params.use_gps) {
				/* kalman filter for position */
				kalman_filter_inertial_predict(dt, x_est);
				kalman_filter_inertial_predict(dt, y_est);
				/* prepare vectors for kalman filter correction */
				float x_meas[3];	// position, velocity, acceleration
				float y_meas[3];	// position, velocity, acceleration
				bool use_xy[3] = { false, false, false };
				if (gps_updated) {
					x_meas[0] = local_pos_gps[0];
					y_meas[0] = local_pos_gps[1];
					x_meas[1] = gps.vel_n_m_s;
					y_meas[1] = gps.vel_e_m_s;
					use_xy[0] = true;
					use_xy[1] = true;
				}
				if (accelerometer_updated) {
					x_meas[2] = accel_NED[0];
					y_meas[2] = accel_NED[1];
					use_xy[2] = true;
				}
				if (use_xy[0] || use_xy[2]) {
					/* correction */
					kalman_filter_inertial3_update(x_est, x_meas, params.k_pos, use_xy);
					kalman_filter_inertial3_update(y_est, y_meas, params.k_pos, use_xy);
				}
			}
		}
		if (verbose_mode) {
			/* print updates rate */
			if (t - updates_counter_start > updates_counter_len) {
				float updates_dt = (t - updates_counter_start) * 0.000001f;
				printf(
						"[position_estimator_inav] updates rate: accelerometer = %.1f/s, baro = %.1f/s, gps = %.1f/s, attitude = %.1f/s\n",
						accelerometer_updates / updates_dt,
						baro_updates / updates_dt,
						gps_updates / updates_dt,
						attitude_updates / updates_dt);
				updates_counter_start = t;
				accelerometer_updates = 0;
				baro_updates = 0;
				gps_updates = 0;
				attitude_updates = 0;
			}
		}
		if (t - pub_last > pub_interval) {
			pub_last = t;
			pos.x = x_est[0];
			pos.vx = x_est[1];
			pos.y = y_est[0];
			pos.vy = y_est[1];
			pos.z = z_est[0];
			pos.vz = z_est[1];
			pos.timestamp = hrt_absolute_time();
			if ((isfinite(pos.x)) && (isfinite(pos.vx))
					&& (isfinite(pos.y))
					&& (isfinite(pos.vy))
					&& (isfinite(pos.z))
					&& (isfinite(pos.vz))) {
				orb_publish(ORB_ID(vehicle_local_position), vehicle_local_position_pub, &pos);
			}
		}
	}

	printf("[position_estimator_inav] exiting.\n");
	mavlink_log_info(mavlink_fd, "[position_estimator_inav] exiting");
	thread_running = false;
	return 0;
}