#ifndef rpi_shader_H
#define rpi_shader_H

extern unsigned int rpi_shader[];

#define mc_setup_c_q0 (rpi_shader + 0)
#define mc_start (rpi_shader + 0)
#define mc_setup_c_qn (rpi_shader + 2)
#define mc_filter_c_p (rpi_shader + 138)
#define mc_filter_c_b (rpi_shader + 264)
#define mc_sync_q0 (rpi_shader + 452)
#define mc_sync_q1 (rpi_shader + 470)
#define mc_sync_q2 (rpi_shader + 482)
#define mc_sync_q3 (rpi_shader + 494)
#define mc_sync_q4 (rpi_shader + 506)
#define mc_sync_q5 (rpi_shader + 524)
#define mc_sync_q6 (rpi_shader + 536)
#define mc_sync_q7 (rpi_shader + 548)
#define mc_sync_q8 (rpi_shader + 560)
#define mc_sync_q9 (rpi_shader + 578)
#define mc_sync_q10 (rpi_shader + 590)
#define mc_sync_q11 (rpi_shader + 602)
#define mc_exit_c_qn (rpi_shader + 614)
#define mc_exit_y_qn (rpi_shader + 614)
#define mc_exit_c_q0 (rpi_shader + 628)
#define mc_exit_y_q0 (rpi_shader + 628)
#define mc_setup_y_q0 (rpi_shader + 644)
#define mc_setup_y_qn (rpi_shader + 646)
#define mc_filter_y_pxx (rpi_shader + 886)
#define mc_filter_y_bxx (rpi_shader + 1016)
#define mc_filter_y_p00 (rpi_shader + 1148)
#define mc_filter_y_b00 (rpi_shader + 1244)
#define mc_setup_c10_q0 (rpi_shader + 1326)
#define mc_setup_c10_qn (rpi_shader + 1328)
#define mc_filter_c10_p (rpi_shader + 1460)
#define mc_filter_c10_b (rpi_shader + 1586)
#define mc_sync10_q0 (rpi_shader + 1772)
#define mc_sync10_q1 (rpi_shader + 1790)
#define mc_sync10_q2 (rpi_shader + 1802)
#define mc_sync10_q3 (rpi_shader + 1814)
#define mc_sync10_q4 (rpi_shader + 1826)
#define mc_sync10_q5 (rpi_shader + 1844)
#define mc_sync10_q6 (rpi_shader + 1856)
#define mc_sync10_q7 (rpi_shader + 1868)
#define mc_exit_c10_q0 (rpi_shader + 1880)
#define mc_exit_y10_q0 (rpi_shader + 1880)
#define mc_sync10_q10 (rpi_shader + 1880)
#define mc_sync10_q11 (rpi_shader + 1880)
#define mc_sync10_q8 (rpi_shader + 1880)
#define mc_sync10_q9 (rpi_shader + 1880)
#define mc_exit_c10_qn (rpi_shader + 1896)
#define mc_exit_y10_qn (rpi_shader + 1896)
#define mc_setup_y10_q0 (rpi_shader + 1910)
#define mc_setup_y10_qn (rpi_shader + 1912)
#define mc_filter_y10_pxx (rpi_shader + 2162)
#define mc_filter_y10_p00 (rpi_shader + 2292)
#define mc_filter_y10_bxx (rpi_shader + 2390)
#define mc_filter_y10_b00 (rpi_shader + 2522)
#define mc_end (rpi_shader + 2604)

#endif
