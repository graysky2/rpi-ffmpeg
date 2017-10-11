/*
 * HEVC video decoder
 *
 * Copyright (C) 2012 - 2013 Guillaume Martres
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVCODEC_HEVC_H
#define AVCODEC_HEVC_H

// define RPI to split the CABAC/prediction/transform into separate stages
#ifndef RPI

  #define RPI_INTER          0
  #define RPI_TSTATS         0
  #define RPI_HEVC_SAND      0

#else

  #include "rpi_qpu.h"
  #define RPI_INTER          1          // 0 use ARM for UV inter-pred, 1 use QPU

  // By passing jobs to a worker thread we hope to be able to catch up during slow frames
  // This has no effect unless RPI_WORKER is defined
  // N.B. The extra thread count is effectively RPI_MAX_JOBS - 1 as
  // RPI_MAX_JOBS defines the number of worker parameter sets and we must have one
  // free for the foreground to fill in.
  #define RPI_MAX_JOBS 2

  // Define RPI_DEBLOCK_VPU to perform deblocking on the VPUs
  // As it stands there is something mildy broken in VPU deblock - looks mostly OK
  // but reliably fails some conformance tests (e.g. DBLK_A/B/C_)
  // With VPU luma & chroma pred it is much the same speed to deblock on the ARM
//  #define RPI_DEBLOCK_VPU

  #define RPI_VPU_DEBLOCK_CACHED 1

  #if HAVE_NEON
  #define RPI_HEVC_SAND      1
  #else
  // Sand bust on Pi1 currently - reasons unknown
  #define RPI_HEVC_SAND      0
  #endif


  #define RPI_QPU_EMU_Y      0
  #define RPI_QPU_EMU_C      0

  #define RPI_TSTATS 0
#endif

#include "libavutil/buffer.h"
#include "libavutil/md5.h"

#include "avcodec.h"
#include "bswapdsp.h"
#include "cabac.h"
#include "get_bits.h"
#include "hevcpred.h"
#include "h2645_parse.h"
#include "hevcdsp.h"
#include "internal.h"
#include "thread.h"
#include "videodsp.h"

#define MAX_DPB_SIZE 16 // A.4.1
#define MAX_REFS 16

#define MAX_NB_THREADS 16
#define SHIFT_CTB_WPP 2

/**
 * 7.4.2.1
 */
#define MAX_SUB_LAYERS 7
#define MAX_VPS_COUNT 16
#define MAX_SPS_COUNT 32
#define MAX_PPS_COUNT 256
#define MAX_SHORT_TERM_RPS_COUNT 64
#define MAX_CU_SIZE 128

//TODO: check if this is really the maximum
#define MAX_TRANSFORM_DEPTH 5

#define MAX_TB_SIZE 32
#define MAX_LOG2_CTB_SIZE 6
#define MAX_QP 51
#define DEFAULT_INTRA_TC_OFFSET 2

#define HEVC_CONTEXTS 199

#define MRG_MAX_NUM_CANDS     5

#define L0 0
#define L1 1

#define EPEL_EXTRA_BEFORE 1
#define EPEL_EXTRA_AFTER  2
#define EPEL_EXTRA        3
#define QPEL_EXTRA_BEFORE 3
#define QPEL_EXTRA_AFTER  4
#define QPEL_EXTRA        7

#define EDGE_EMU_BUFFER_STRIDE 80

/**
 * Value of the luma sample at position (x, y) in the 2D array tab.
 */
#define SAMPLE(tab, x, y) ((tab)[(y) * s->sps->width + (x)])
#define SAMPLE_CTB(tab, x, y) ((tab)[(y) * min_cb_width + (x)])

#define IS_IDR(s) ((s)->nal_unit_type == NAL_IDR_W_RADL || (s)->nal_unit_type == NAL_IDR_N_LP)
#define IS_BLA(s) ((s)->nal_unit_type == NAL_BLA_W_RADL || (s)->nal_unit_type == NAL_BLA_W_LP || \
                   (s)->nal_unit_type == NAL_BLA_N_LP)
#define IS_IRAP(s) ((s)->nal_unit_type >= 16 && (s)->nal_unit_type <= 23)

/**
 * Table 7-3: NAL unit type codes
 */
enum NALUnitType {
    NAL_TRAIL_N    = 0,
    NAL_TRAIL_R    = 1,
    NAL_TSA_N      = 2,
    NAL_TSA_R      = 3,
    NAL_STSA_N     = 4,
    NAL_STSA_R     = 5,
    NAL_RADL_N     = 6,
    NAL_RADL_R     = 7,
    NAL_RASL_N     = 8,
    NAL_RASL_R     = 9,
    NAL_BLA_W_LP   = 16,
    NAL_BLA_W_RADL = 17,
    NAL_BLA_N_LP   = 18,
    NAL_IDR_W_RADL = 19,
    NAL_IDR_N_LP   = 20,
    NAL_CRA_NUT    = 21,
    NAL_VPS        = 32,
    NAL_SPS        = 33,
    NAL_PPS        = 34,
    NAL_AUD        = 35,
    NAL_EOS_NUT    = 36,
    NAL_EOB_NUT    = 37,
    NAL_FD_NUT     = 38,
    NAL_SEI_PREFIX = 39,
    NAL_SEI_SUFFIX = 40,
};

enum RPSType {
    ST_CURR_BEF = 0,
    ST_CURR_AFT,
    ST_FOLL,
    LT_CURR,
    LT_FOLL,
    NB_RPS_TYPE,
};

enum SliceType {
    B_SLICE = 0,
    P_SLICE = 1,
    I_SLICE = 2,
};

enum SyntaxElement {
    SAO_MERGE_FLAG = 0,
    SAO_TYPE_IDX,
    SAO_EO_CLASS,
    SAO_BAND_POSITION,
    SAO_OFFSET_ABS,
    SAO_OFFSET_SIGN,
    END_OF_SLICE_FLAG,
    SPLIT_CODING_UNIT_FLAG,
    CU_TRANSQUANT_BYPASS_FLAG,
    SKIP_FLAG,
    CU_QP_DELTA,
    PRED_MODE_FLAG,
    PART_MODE,
    PCM_FLAG,
    PREV_INTRA_LUMA_PRED_FLAG,
    MPM_IDX,
    REM_INTRA_LUMA_PRED_MODE,
    INTRA_CHROMA_PRED_MODE,
    MERGE_FLAG,
    MERGE_IDX,
    INTER_PRED_IDC,
    REF_IDX_L0,
    REF_IDX_L1,
    ABS_MVD_GREATER0_FLAG,
    ABS_MVD_GREATER1_FLAG,
    ABS_MVD_MINUS2,
    MVD_SIGN_FLAG,
    MVP_LX_FLAG,
    NO_RESIDUAL_DATA_FLAG,
    SPLIT_TRANSFORM_FLAG,
    CBF_LUMA,
    CBF_CB_CR,
    TRANSFORM_SKIP_FLAG,
    EXPLICIT_RDPCM_FLAG,
    EXPLICIT_RDPCM_DIR_FLAG,
    LAST_SIGNIFICANT_COEFF_X_PREFIX,
    LAST_SIGNIFICANT_COEFF_Y_PREFIX,
    LAST_SIGNIFICANT_COEFF_X_SUFFIX,
    LAST_SIGNIFICANT_COEFF_Y_SUFFIX,
    SIGNIFICANT_COEFF_GROUP_FLAG,
    SIGNIFICANT_COEFF_FLAG,
    COEFF_ABS_LEVEL_GREATER1_FLAG,
    COEFF_ABS_LEVEL_GREATER2_FLAG,
    COEFF_ABS_LEVEL_REMAINING,
    COEFF_SIGN_FLAG,
    LOG2_RES_SCALE_ABS,
    RES_SCALE_SIGN_FLAG,
    CU_CHROMA_QP_OFFSET_FLAG,
    CU_CHROMA_QP_OFFSET_IDX,
};

enum PartMode {
    PART_2Nx2N = 0,
    PART_2NxN  = 1,
    PART_Nx2N  = 2,
    PART_NxN   = 3,
    PART_2NxnU = 4,
    PART_2NxnD = 5,
    PART_nLx2N = 6,
    PART_nRx2N = 7,
};

enum PredMode {
    MODE_INTER = 0,
    MODE_INTRA,
    MODE_SKIP,
};

enum InterPredIdc {
    PRED_L0 = 0,
    PRED_L1,
    PRED_BI,
};

enum PredFlag {
    PF_INTRA = 0,
    PF_L0,
    PF_L1,
    PF_BI,
};

enum IntraPredMode {
    INTRA_PLANAR = 0,
    INTRA_DC,
    INTRA_ANGULAR_2,
    INTRA_ANGULAR_3,
    INTRA_ANGULAR_4,
    INTRA_ANGULAR_5,
    INTRA_ANGULAR_6,
    INTRA_ANGULAR_7,
    INTRA_ANGULAR_8,
    INTRA_ANGULAR_9,
    INTRA_ANGULAR_10,
    INTRA_ANGULAR_11,
    INTRA_ANGULAR_12,
    INTRA_ANGULAR_13,
    INTRA_ANGULAR_14,
    INTRA_ANGULAR_15,
    INTRA_ANGULAR_16,
    INTRA_ANGULAR_17,
    INTRA_ANGULAR_18,
    INTRA_ANGULAR_19,
    INTRA_ANGULAR_20,
    INTRA_ANGULAR_21,
    INTRA_ANGULAR_22,
    INTRA_ANGULAR_23,
    INTRA_ANGULAR_24,
    INTRA_ANGULAR_25,
    INTRA_ANGULAR_26,
    INTRA_ANGULAR_27,
    INTRA_ANGULAR_28,
    INTRA_ANGULAR_29,
    INTRA_ANGULAR_30,
    INTRA_ANGULAR_31,
    INTRA_ANGULAR_32,
    INTRA_ANGULAR_33,
    INTRA_ANGULAR_34,
};

enum SAOType {
    SAO_NOT_APPLIED = 0,
    SAO_BAND,
    SAO_EDGE,
    SAO_APPLIED
};

enum SAOEOClass {
    SAO_EO_HORIZ = 0,
    SAO_EO_VERT,
    SAO_EO_135D,
    SAO_EO_45D,
};

enum ScanType {
    SCAN_DIAG = 0,
    SCAN_HORIZ,
    SCAN_VERT,
};

typedef struct ShortTermRPS {
    unsigned int num_negative_pics;
    int num_delta_pocs;
    int rps_idx_num_delta_pocs;
    int32_t delta_poc[32];
    uint8_t used[32];
} ShortTermRPS;

typedef struct LongTermRPS {
    int     poc[32];
    uint8_t used[32];
    uint8_t nb_refs;
} LongTermRPS;

typedef struct RefPicList {
    struct HEVCFrame *ref[MAX_REFS];
    int list[MAX_REFS];
    int isLongTerm[MAX_REFS];
    int nb_refs;
} RefPicList;

typedef struct RefPicListTab {
    RefPicList refPicList[2];
} RefPicListTab;

typedef struct HEVCWindow {
    unsigned int left_offset;
    unsigned int right_offset;
    unsigned int top_offset;
    unsigned int bottom_offset;
} HEVCWindow;

typedef struct VUI {
    AVRational sar;

    int overscan_info_present_flag;
    int overscan_appropriate_flag;

    int video_signal_type_present_flag;
    int video_format;
    int video_full_range_flag;
    int colour_description_present_flag;
    uint8_t colour_primaries;
    uint8_t transfer_characteristic;
    uint8_t matrix_coeffs;

    int chroma_loc_info_present_flag;
    int chroma_sample_loc_type_top_field;
    int chroma_sample_loc_type_bottom_field;
    int neutra_chroma_indication_flag;

    int field_seq_flag;
    int frame_field_info_present_flag;

    int default_display_window_flag;
    HEVCWindow def_disp_win;

    int vui_timing_info_present_flag;
    uint32_t vui_num_units_in_tick;
    uint32_t vui_time_scale;
    int vui_poc_proportional_to_timing_flag;
    int vui_num_ticks_poc_diff_one_minus1;
    int vui_hrd_parameters_present_flag;

    int bitstream_restriction_flag;
    int tiles_fixed_structure_flag;
    int motion_vectors_over_pic_boundaries_flag;
    int restricted_ref_pic_lists_flag;
    int min_spatial_segmentation_idc;
    int max_bytes_per_pic_denom;
    int max_bits_per_min_cu_denom;
    int log2_max_mv_length_horizontal;
    int log2_max_mv_length_vertical;
} VUI;

typedef struct PTLCommon {
    uint8_t profile_space;
    uint8_t tier_flag;
    uint8_t profile_idc;
    uint8_t profile_compatibility_flag[32];
    uint8_t level_idc;
    uint8_t progressive_source_flag;
    uint8_t interlaced_source_flag;
    uint8_t non_packed_constraint_flag;
    uint8_t frame_only_constraint_flag;
} PTLCommon;

typedef struct PTL {
    PTLCommon general_ptl;
    PTLCommon sub_layer_ptl[MAX_SUB_LAYERS];

    uint8_t sub_layer_profile_present_flag[MAX_SUB_LAYERS];
    uint8_t sub_layer_level_present_flag[MAX_SUB_LAYERS];
} PTL;

typedef struct HEVCVPS {
    uint8_t vps_temporal_id_nesting_flag;
    int vps_max_layers;
    int vps_max_sub_layers; ///< vps_max_temporal_layers_minus1 + 1

    PTL ptl;
    int vps_sub_layer_ordering_info_present_flag;
    unsigned int vps_max_dec_pic_buffering[MAX_SUB_LAYERS];
    unsigned int vps_num_reorder_pics[MAX_SUB_LAYERS];
    unsigned int vps_max_latency_increase[MAX_SUB_LAYERS];
    int vps_max_layer_id;
    int vps_num_layer_sets; ///< vps_num_layer_sets_minus1 + 1
    uint8_t vps_timing_info_present_flag;
    uint32_t vps_num_units_in_tick;
    uint32_t vps_time_scale;
    uint8_t vps_poc_proportional_to_timing_flag;
    int vps_num_ticks_poc_diff_one; ///< vps_num_ticks_poc_diff_one_minus1 + 1
    int vps_num_hrd_parameters;
} HEVCVPS;

typedef struct ScalingList {
    /* This is a little wasteful, since sizeID 0 only needs 8 coeffs,
     * and size ID 3 only has 2 arrays, not 6. */
    uint8_t sl[4][6][64];
    uint8_t sl_dc[2][6];
} ScalingList;

typedef struct HEVCSPS {
    unsigned vps_id;
    int chroma_format_idc;
    uint8_t separate_colour_plane_flag;

    ///< output (i.e. cropped) values
    int output_width, output_height;
    HEVCWindow output_window;

    HEVCWindow pic_conf_win;

    int bit_depth;
    int pixel_shift;
    enum AVPixelFormat pix_fmt;

    unsigned int log2_max_poc_lsb;
    int pcm_enabled_flag;

    int max_sub_layers;
    struct {
        int max_dec_pic_buffering;
        int num_reorder_pics;
        int max_latency_increase;
    } temporal_layer[MAX_SUB_LAYERS];

    VUI vui;
    PTL ptl;

    uint8_t scaling_list_enable_flag;
    ScalingList scaling_list;

    unsigned int nb_st_rps;
    ShortTermRPS st_rps[MAX_SHORT_TERM_RPS_COUNT];

    uint8_t amp_enabled_flag;
    uint8_t sao_enabled;

    uint8_t long_term_ref_pics_present_flag;
    uint16_t lt_ref_pic_poc_lsb_sps[32];
    uint8_t used_by_curr_pic_lt_sps_flag[32];
    uint8_t num_long_term_ref_pics_sps;

    struct {
        uint8_t bit_depth;
        uint8_t bit_depth_chroma;
        unsigned int log2_min_pcm_cb_size;
        unsigned int log2_max_pcm_cb_size;
        uint8_t loop_filter_disable_flag;
    } pcm;
    uint8_t sps_temporal_mvp_enabled_flag;
    uint8_t sps_strong_intra_smoothing_enable_flag;

    unsigned int log2_min_cb_size;
    unsigned int log2_diff_max_min_coding_block_size;
    unsigned int log2_min_tb_size;
    unsigned int log2_max_trafo_size;
    unsigned int log2_ctb_size;
    unsigned int log2_min_pu_size;

    int max_transform_hierarchy_depth_inter;
    int max_transform_hierarchy_depth_intra;

    int transform_skip_rotation_enabled_flag;
    int transform_skip_context_enabled_flag;
    int implicit_rdpcm_enabled_flag;
    int explicit_rdpcm_enabled_flag;
    int intra_smoothing_disabled_flag;
    int high_precision_offsets_enabled_flag;
    int persistent_rice_adaptation_enabled_flag;

    ///< coded frame dimension in various units
    int width;
    int height;
    int ctb_width;
    int ctb_height;
    int ctb_size;
    int min_cb_width;
    int min_cb_height;
    int min_tb_width;
    int min_tb_height;
    int min_pu_width;
    int min_pu_height;
    int tb_mask;

    int hshift[3];
    int vshift[3];

    int qp_bd_offset;
} HEVCSPS;

typedef struct HEVCPPS {
    unsigned int sps_id; ///< seq_parameter_set_id

    uint8_t sign_data_hiding_flag;

    uint8_t cabac_init_present_flag;

    int num_ref_idx_l0_default_active; ///< num_ref_idx_l0_default_active_minus1 + 1
    int num_ref_idx_l1_default_active; ///< num_ref_idx_l1_default_active_minus1 + 1
    int pic_init_qp_minus26;

    uint8_t constrained_intra_pred_flag;
    uint8_t transform_skip_enabled_flag;

    uint8_t cu_qp_delta_enabled_flag;
    int diff_cu_qp_delta_depth;

    int cb_qp_offset;
    int cr_qp_offset;
    uint8_t pic_slice_level_chroma_qp_offsets_present_flag;
    uint8_t weighted_pred_flag;
    uint8_t weighted_bipred_flag;
    uint8_t output_flag_present_flag;
    uint8_t transquant_bypass_enable_flag;

    uint8_t dependent_slice_segments_enabled_flag;
    uint8_t tiles_enabled_flag;
    uint8_t entropy_coding_sync_enabled_flag;

    int num_tile_columns;   ///< num_tile_columns_minus1 + 1
    int num_tile_rows;      ///< num_tile_rows_minus1 + 1
    uint8_t uniform_spacing_flag;
    uint8_t loop_filter_across_tiles_enabled_flag;

    uint8_t seq_loop_filter_across_slices_enabled_flag;

    uint8_t deblocking_filter_control_present_flag;
    uint8_t deblocking_filter_override_enabled_flag;
    uint8_t disable_dbf;
    int beta_offset;    ///< beta_offset_div2 * 2
    int tc_offset;      ///< tc_offset_div2 * 2

    uint8_t scaling_list_data_present_flag;
    ScalingList scaling_list;

    uint8_t lists_modification_present_flag;
    int log2_parallel_merge_level; ///< log2_parallel_merge_level_minus2 + 2
    int num_extra_slice_header_bits;
    uint8_t slice_header_extension_present_flag;
    uint8_t log2_max_transform_skip_block_size;
    uint8_t cross_component_prediction_enabled_flag;
    uint8_t chroma_qp_offset_list_enabled_flag;
    uint8_t diff_cu_chroma_qp_offset_depth;
    uint8_t chroma_qp_offset_list_len_minus1;
    int8_t  cb_qp_offset_list[6];
    int8_t  cr_qp_offset_list[6];
    uint8_t log2_sao_offset_scale_luma;
    uint8_t log2_sao_offset_scale_chroma;

    // Inferred parameters
    unsigned int *column_width;  ///< ColumnWidth
    unsigned int *row_height;    ///< RowHeight
    unsigned int *col_bd;        ///< ColBd
    unsigned int *row_bd;        ///< RowBd
    int *col_idxX;

    int *ctb_addr_rs_to_ts; ///< CtbAddrRSToTS
    int *ctb_addr_ts_to_rs; ///< CtbAddrTSToRS
    int *tile_id;           ///< TileId
    int *tile_pos_rs;       ///< TilePosRS
    int *min_tb_addr_zs;    ///< MinTbAddrZS
    int *min_tb_addr_zs_tab;///< MinTbAddrZS
} HEVCPPS;

typedef struct HEVCParamSets {
    AVBufferRef *vps_list[MAX_VPS_COUNT];
    AVBufferRef *sps_list[MAX_SPS_COUNT];
    AVBufferRef *pps_list[MAX_PPS_COUNT];

    /* currently active parameter sets */
    const HEVCVPS *vps;
    const HEVCSPS *sps;
    const HEVCPPS *pps;
} HEVCParamSets;

typedef struct SliceHeader {
    unsigned int pps_id;

    ///< address (in raster order) of the first block in the current slice segment
    unsigned int   slice_segment_addr;
    ///< address (in raster order) of the first block in the current slice
    unsigned int   slice_addr;

    enum SliceType slice_type;

    int pic_order_cnt_lsb;

    uint8_t first_slice_in_pic_flag;
    uint8_t dependent_slice_segment_flag;
    uint8_t pic_output_flag;
    uint8_t colour_plane_id;

    ///< RPS coded in the slice header itself is stored here
    int short_term_ref_pic_set_sps_flag;
    int short_term_ref_pic_set_size;
    ShortTermRPS slice_rps;
    const ShortTermRPS *short_term_rps;
    int long_term_ref_pic_set_size;
    LongTermRPS long_term_rps;
    unsigned int list_entry_lx[2][32];

    uint8_t rpl_modification_flag[2];
    uint8_t no_output_of_prior_pics_flag;
    uint8_t slice_temporal_mvp_enabled_flag;

    unsigned int nb_refs[2];

    uint8_t slice_sample_adaptive_offset_flag[3];
    uint8_t mvd_l1_zero_flag;

    uint8_t cabac_init_flag;
    uint8_t disable_deblocking_filter_flag; ///< slice_header_disable_deblocking_filter_flag
    uint8_t slice_loop_filter_across_slices_enabled_flag;
    uint8_t collocated_list;

    unsigned int collocated_ref_idx;

    int slice_qp_delta;
    int slice_cb_qp_offset;
    int slice_cr_qp_offset;

    uint8_t cu_chroma_qp_offset_enabled_flag;

    int beta_offset;    ///< beta_offset_div2 * 2
    int tc_offset;      ///< tc_offset_div2 * 2

    unsigned int max_num_merge_cand; ///< 5 - 5_minus_max_num_merge_cand

    unsigned *entry_point_offset;
    int * offset;
    int * size;
    int num_entry_point_offsets;

    int8_t slice_qp;

    uint8_t luma_log2_weight_denom;
    int16_t chroma_log2_weight_denom;

    int16_t luma_weight_l0[16];
    int16_t chroma_weight_l0[16][2];
    int16_t chroma_weight_l1[16][2];
    int16_t luma_weight_l1[16];

    int16_t luma_offset_l0[16];
    int16_t chroma_offset_l0[16][2];

    int16_t luma_offset_l1[16];
    int16_t chroma_offset_l1[16][2];

    int slice_ctb_addr_rs;
} SliceHeader;

typedef struct CodingUnit {
    int x;
    int y;

    enum PredMode pred_mode;    ///< PredMode
    enum PartMode part_mode;    ///< PartMode

    // Inferred parameters
    uint8_t intra_split_flag;   ///< IntraSplitFlag
    uint8_t max_trafo_depth;    ///< MaxTrafoDepth
    uint8_t cu_transquant_bypass_flag;
} CodingUnit;

#if 0
typedef struct Mv {
    int16_t x;  ///< horizontal component of motion vector
    int16_t y;  ///< vertical component of motion vector
} Mv;

typedef struct MvField {
    DECLARE_ALIGNED(4, Mv, mv)[2];
    int8_t ref_idx[2];
    int8_t pred_flag;
} MvField;
#endif

typedef struct NeighbourAvailable {
    int cand_bottom_left;
    int cand_left;
    int cand_up;
    int cand_up_left;
    int cand_up_right;
    int cand_up_right_sap;
} NeighbourAvailable;

typedef struct PredictionUnit {
    int mpm_idx;
    int rem_intra_luma_pred_mode;
    uint8_t intra_pred_mode[4];
    Mv mvd;
    uint8_t merge_flag;
    uint8_t intra_pred_mode_c[4];
    uint8_t chroma_mode_c[4];
} PredictionUnit;

typedef struct TransformUnit {
    int cu_qp_delta;

    int res_scale_val;

    // Inferred parameters;
    int intra_pred_mode;
    int intra_pred_mode_c;
    int chroma_mode_c;
    uint8_t is_cu_qp_delta_coded;
    uint8_t is_cu_chroma_qp_offset_coded;
    int8_t  cu_qp_offset_cb;
    int8_t  cu_qp_offset_cr;
    uint8_t cross_pf;
} TransformUnit;

typedef struct DBParams {
    int beta_offset;
    int tc_offset;
} DBParams;

#define HEVC_FRAME_FLAG_OUTPUT    (1 << 0)
#define HEVC_FRAME_FLAG_SHORT_REF (1 << 1)
#define HEVC_FRAME_FLAG_LONG_REF  (1 << 2)
#define HEVC_FRAME_FLAG_BUMPING   (1 << 3)

typedef struct HEVCFrame {
    AVFrame *frame;
    ThreadFrame tf;
    MvField *tab_mvf;
    RefPicList *refPicList;
    RefPicListTab **rpl_tab;
    int ctb_count;
    int poc;
    struct HEVCFrame *collocated_ref;

    HEVCWindow window;

    AVBufferRef *tab_mvf_buf;
    AVBufferRef *rpl_tab_buf;
    AVBufferRef *rpl_buf;

    AVBufferRef *hwaccel_priv_buf;
    void *hwaccel_picture_private;

    /**
     * A sequence counter, so that old frames are output first
     * after a POC reset
     */
    uint16_t sequence;

    /**
     * A combination of HEVC_FRAME_FLAG_*
     */
    uint8_t flags;

    // Entry no in DPB - can be used as a small unique
    // frame identifier (within the current thread)
    uint8_t dpb_no;
} HEVCFrame;

#ifdef RPI
typedef struct HEVCLocalContextIntra {
    TransformUnit tu;
    NeighbourAvailable na;
} HEVCLocalContextIntra;
#endif

typedef struct HEVCLocalContext {
    TransformUnit tu;  // Moved to start to match HEVCLocalContextIntra (yuk!)
    NeighbourAvailable na;

    uint8_t cabac_state[HEVC_CONTEXTS];

    uint8_t stat_coeff[4];

    uint8_t first_qp_group;

    GetBitContext gb;
    CABACContext cc;

    int8_t qp_y;
    int8_t curr_qp_y;

    int qPy_pred;

    uint8_t ctb_left_flag;
    uint8_t ctb_up_flag;
    uint8_t ctb_up_right_flag;
    uint8_t ctb_up_left_flag;
    int     end_of_tiles_x;
    int     end_of_tiles_y;
    /* +7 is for subpixel interpolation, *2 for high bit depths */
    DECLARE_ALIGNED(32, uint8_t, edge_emu_buffer)[(MAX_PB_SIZE + 7) * EDGE_EMU_BUFFER_STRIDE * 2];
    /* The extended size between the new edge emu buffer is abused by SAO */
    DECLARE_ALIGNED(32, uint8_t, edge_emu_buffer2)[(MAX_PB_SIZE + 7) * EDGE_EMU_BUFFER_STRIDE * 2];
    DECLARE_ALIGNED(32, int16_t, tmp [MAX_PB_SIZE * MAX_PB_SIZE]);

    int ct_depth;
    CodingUnit cu;
    PredictionUnit pu;

#define BOUNDARY_LEFT_SLICE     (1 << 0)
#define BOUNDARY_LEFT_TILE      (1 << 1)
#define BOUNDARY_UPPER_SLICE    (1 << 2)
#define BOUNDARY_UPPER_TILE     (1 << 3)
    /* properties of the boundary of the current CTB for the purposes
     * of the deblocking filter */
    int boundary_flags;
} HEVCLocalContext;

#ifdef RPI

// The processing is done in chunks
// Increasing RPI_NUM_CHUNKS will reduce time spent activating QPUs and cache flushing,
// but allocate more memory and increase the latency before data in the next frame can be processed
#define RPI_NUM_CHUNKS 4
#define RPI_CHUNK_SIZE 12
#define RPI_ROUND_TO_LINES 0

// RPI_MAX_WIDTH is maximum width in pixels supported by the accelerated code
#define RPI_MAX_WIDTH (RPI_NUM_CHUNKS*64*RPI_CHUNK_SIZE)

// Worst case is for 4:4:4 4x4 blocks with 64 high coding tree blocks, so 16 MV cmds per 4 pixels across for each colour plane, * 2 for bi
#define RPI_MAX_MV_CMDS_Y   (2*16*1*(RPI_MAX_WIDTH/4))
#define RPI_MAX_MV_CMDS_C   (2*16*2*(RPI_MAX_WIDTH/4))
// Each block can have an intra prediction and a transform_add command
#define RPI_MAX_PRED_CMDS (2*16*3*(RPI_MAX_WIDTH/4))
// Worst case is 16x16 CTUs
#define RPI_MAX_DEBLOCK_CMDS (RPI_MAX_WIDTH*4/16)

#define RPI_CMD_LUMA_UNI 0
#define RPI_CMD_CHROMA_UNI 1
#define RPI_CMD_LUMA_BI 2
#define RPI_CMD_CHROMA_BI 3
#define RPI_CMD_V_BI 4

// Command for inter prediction
typedef struct HEVCMvCmd {
    uint8_t cmd;
    uint8_t block_w;
    uint8_t block_h;
    int8_t ref_idx[2];
    uint16_t dststride;
    uint16_t srcstride;
    uint16_t srcstride1;
    int16_t weight;
    int16_t offset;
    int16_t x_off;
    int16_t y_off;
    uint8_t *src;
    uint8_t *src1;
    uint8_t *dst;
    Mv mv;
    Mv mv1;
} HEVCMvCmd;


// Command for intra prediction and transform_add of predictions to coefficients
enum rpi_pred_cmd_e
{
    RPI_PRED_ADD_RESIDUAL,
    RPI_PRED_ADD_RESIDUAL_U, // = RPI_PRED_TRANSFORM_ADD + c_idx
    RPI_PRED_ADD_RESIDUAL_V, // = RPI_PRED_TRANSFORM_ADD + c_idx
    RPI_PRED_ADD_RESIDUAL_C, // Merged U+V
    RPI_PRED_ADD_DC,
    RPI_PRED_ADD_DC_U,       // Both U & V are effectively C
    RPI_PRED_ADD_DC_V,
    RPI_PRED_INTRA,
    RPI_PRED_I_PCM,
    RPI_PRED_CMD_MAX
};

typedef struct HEVCPredCmd {
    uint8_t type;
    uint8_t size;  // log2 "size" used by all variants
    uint8_t na;    // i_pred - but left here as they pack well
    uint8_t c_idx; // i_pred
    union {
        struct {  // TRANSFORM_ADD
            uint8_t * dst;
            const int16_t * buf;
            uint16_t stride;  // Should be good enough for all pic fmts we use
            int16_t dc;
        } ta;
        struct {
            uint8_t * dst;
            uint32_t stride;
            int dc;
        } dc;
        struct {  // INTRA
            uint16_t x;
            uint16_t y;
            enum IntraPredMode mode;
        } i_pred;
        struct {  // I_PCM
            uint16_t x;
            uint16_t y;
            const void * src;
            uint32_t src_len;
        } i_pcm;
    };
} HEVCPredCmd;

#endif

#ifdef RPI
#include <semaphore.h>

union qpu_mc_pred_cmd_s;
struct qpu_mc_pred_y_p_s;
struct qpu_mc_src_s;

typedef struct HEVCRpiInterPredQ
{
    union qpu_mc_pred_cmd_u *qpu_mc_base;
    union qpu_mc_pred_cmd_u *qpu_mc_curr;
    struct qpu_mc_src_s *last_l0;
    struct qpu_mc_src_s *last_l1;
    unsigned int load;
    uint32_t code_setup;
    uint32_t code_sync;
    uint32_t code_exit;
} HEVCRpiInterPredQ;

typedef struct HEVCRpiInterPredEnv
{
    HEVCRpiInterPredQ * q;
    unsigned int n;        // Number of Qs
    unsigned int n_grp;    // Number of Q in a group
    unsigned int curr;     // Current Q number (0..n-1)
    int used;              // 0 if nothing in any Q, 1 otherwise
    int used_grp;          // 0 if nothing in any Q in the current group
    unsigned int max_fill;
    unsigned int min_gap;
    GPU_MEM_PTR_T gptr;
} HEVCRpiInterPredEnv;

typedef struct HEVCRpiIntraPredEnv {
    unsigned int n;        // Number of commands
    HEVCPredCmd * cmds;
} HEVCRpiIntraPredEnv;

typedef struct HEVCRpiCeoffEnv {
    unsigned int n;
    uint16_t * buf;
} HEVCRpiCoeffEnv;

typedef struct HEVCRpiCeoffsEnv {
    HEVCRpiCoeffEnv s[4];
    GPU_MEM_PTR_T gptr;
    void * mptr;
} HEVCRpiCoeffsEnv;

typedef struct HEVCRpiDeblkBlk {
    uint16_t x_ctb;
    uint16_t y_ctb;
} HEVCRpiDeblkBlk;

typedef struct HEVCRpiDeblkEnv {
    unsigned int n;
    HEVCRpiDeblkBlk * blks;
} HEVCRpiDeblkEnv;

typedef struct HEVCRPiFrameProgressWait {
    int req;
    struct HEVCRPiFrameProgressWait * next;
    sem_t sem;
} HEVCRPiFrameProgressWait;

typedef struct HEVCRPiFrameProgressState {
    struct HEVCRPiFrameProgressWait * first;
    struct HEVCRPiFrameProgressWait * last;
    pthread_mutex_t lock;
} HEVCRPiFrameProgressState;

typedef struct HEVCRpiJob {
    volatile int terminate;
    int pending;
    sem_t sem_in;       // set by main
    sem_t sem_out;      // set by worker
    HEVCRpiInterPredEnv chroma_ip;
    HEVCRpiInterPredEnv luma_ip;
    int16_t progress[32];  // index by dpb_no
    HEVCRpiIntraPredEnv intra;
    HEVCRpiCoeffsEnv coeffs;
    HEVCRpiDeblkEnv deblk;
    HEVCRPiFrameProgressWait progress_wait;
} HEVCRpiJob;

#if RPI_TSTATS
typedef struct HEVCRpiStats {
    int y_pred1_y8_merge;
    int y_pred1_xy;
    int y_pred1_x0;
    int y_pred1_y0;
    int y_pred1_x0y0;
    int y_pred1_wle8;
    int y_pred1_wgt8;
    int y_pred1_hle16;
    int y_pred1_hgt16;
    int y_pred2_xy;
    int y_pred2_x0;
    int y_pred2_y0;
    int y_pred2_x0y0;
    int y_pred2_hle16;
    int y_pred2_hgt16;
} HEVCRpiStats;
#endif

#endif

typedef struct HEVCContext {
    const AVClass *c;  // needed by private avoptions
    AVCodecContext *avctx;

    struct HEVCContext  *sList[MAX_NB_THREADS];

    HEVCLocalContext    *HEVClcList[MAX_NB_THREADS];
    HEVCLocalContext    *HEVClc;

    uint8_t             threads_type;
    uint8_t             threads_number;

    int                 width;
    int                 height;

    int used_for_ref;  // rpi
#ifdef RPI
    int enable_rpi;
    unsigned int pass0_job; // Pass0 does coefficient decode
    unsigned int pass1_job; // Pass1 does pixel processing
    int ctu_count; // Number of CTUs done in pass0 so far
    int max_ctu_count; // Number of CTUs when we trigger a round of processing

    HEVCRpiJob * jb0;
    HEVCRpiJob * jb1;
    HEVCRpiJob jobs[RPI_MAX_JOBS];
#if RPI_TSTATS
    HEVCRpiStats tstats;
#endif
#if RPI_INTER
    struct qpu_mc_pred_y_p_s * last_y8_p;
    struct qpu_mc_src_s * last_y8_l1;

    // Function pointers
#if RPI_QPU_EMU_Y || RPI_QPU_EMU_C
    const uint8_t * qpu_dummy_frame_emu;
#endif
#if !RPI_QPU_EMU_Y || !RPI_QPU_EMU_C
    uint32_t qpu_dummy_frame_qpu;  // Not a frame - just a bit of memory
#endif
    HEVCRpiQpu qpu;
#endif

    pthread_t worker_thread;

#ifdef RPI_DEBLOCK_VPU
#define RPI_DEBLOCK_VPU_Q_COUNT 2
    int enable_rpi_deblock;

    int uv_setup_width;
    int uv_setup_height;
    int setup_width; // Number of 16x16 blocks across the image
    int setup_height; // Number of 16x16 blocks down the image

    struct dblk_vpu_q_s
    {
        GPU_MEM_PTR_T deblock_vpu_gmem;

        uint8_t (*y_setup_arm)[2][2][2][4];
        uint8_t (*y_setup_vc)[2][2][2][4];

        uint8_t (*uv_setup_arm)[2][2][2][4];  // Half of this is unused [][][1][], but easier for the VPU as it allows us to store with zeros and addresses are aligned
        uint8_t (*uv_setup_vc)[2][2][2][4];

        int (*vpu_cmds_arm)[6]; // r0-r5 for each command
        int vpu_cmds_vc;

        vpu_qpu_wait_h cmd_id;
    } dvq_ents[RPI_DEBLOCK_VPU_Q_COUNT];

    struct dblk_vpu_q_s * dvq;
    unsigned int dvq_n;

#endif
    HEVCLocalContextIntra HEVClcIntra;
    HEVCRPiFrameProgressState progress_states[2];
#endif

    uint8_t *cabac_state;

    /** 1 if the independent slice segment header was successfully parsed */
    uint8_t slice_initialized;

    AVFrame *frame;
    AVFrame *output_frame;
    uint8_t *sao_pixel_buffer_h[3];
    uint8_t *sao_pixel_buffer_v[3];

    HEVCParamSets ps;

    AVBufferPool *tab_mvf_pool;
    AVBufferPool *rpl_tab_pool;

    ///< candidate references for the current frame
    RefPicList rps[5];

    SliceHeader sh;
    SAOParams *sao;
    DBParams *deblock;
    enum NALUnitType nal_unit_type;
    int temporal_id;  ///< temporal_id_plus1 - 1
    HEVCFrame *ref;
    HEVCFrame DPB[32];
    int poc;
    int pocTid0;
    int slice_idx; ///< number of the slice being currently decoded
    int eos;       ///< current packet contains an EOS/EOB NAL
    int last_eos;  ///< last packet contains an EOS/EOB NAL
    int max_ra;
    int bs_width;
    int bs_height;

    int is_decoded;
    int no_rasl_output_flag;

    HEVCPredContext hpc;
    HEVCDSPContext hevcdsp;
    VideoDSPContext vdsp;
    BswapDSPContext bdsp;
    int8_t *qp_y_tab;
    uint8_t *horizontal_bs;
    uint8_t *vertical_bs;

    int32_t *tab_slice_address;

    //  CU
    uint8_t *skip_flag;
    uint8_t *tab_ct_depth;
    // PU
    uint8_t *tab_ipm;

    uint8_t *cbf_luma; // cbf_luma of colocated TU
    uint8_t *is_pcm;

    // CTB-level flags affecting loop filter operation
    uint8_t *filter_slice_edges;

    /** used on BE to byteswap the lines for checksumming */
    uint8_t *checksum_buf;
    int      checksum_buf_size;

    /**
     * Sequence counters for decoded and output frames, so that old
     * frames are output first after a POC reset
     */
    uint16_t seq_decode;
    uint16_t seq_output;

    int enable_parallel_tiles;
    int wpp_err;

    const uint8_t *data;

    H2645Packet pkt;
    // type of the first VCL NAL of the current frame
    enum NALUnitType first_nal_type;

    // for checking the frame checksums
    struct AVMD5 *md5_ctx;
    uint8_t       md5[3][16];
    uint8_t is_md5;

    uint8_t context_initialized;
    uint8_t is_nalff;       ///< this flag is != 0 if bitstream is encapsulated
                            ///< as a format defined in 14496-15
    int apply_defdispwin;

    int active_seq_parameter_set_id;

    int nal_length_size;    ///< Number of bytes used for nal length (1, 2 or 4)
    int nuh_layer_id;

    /** frame packing arrangement variables */
    int sei_frame_packing_present;
    int frame_packing_arrangement_type;
    int content_interpretation_type;
    int quincunx_subsampling;

    /** display orientation */
    int sei_display_orientation_present;
    int sei_anticlockwise_rotation;
    int sei_hflip, sei_vflip;

    int picture_struct;

    uint8_t* a53_caption;
    int a53_caption_size;

    /** mastering display */
    int sei_mastering_display_info_present;
    uint16_t display_primaries[3][2];
    uint16_t white_point[2];
    uint32_t max_mastering_luminance;
    uint32_t min_mastering_luminance;

} HEVCContext;

int ff_hevc_decode_short_term_rps(GetBitContext *gb, AVCodecContext *avctx,
                                  ShortTermRPS *rps, const HEVCSPS *sps, int is_slice_header);

/**
 * Parse the SPS from the bitstream into the provided HEVCSPS struct.
 *
 * @param sps_id the SPS id will be written here
 * @param apply_defdispwin if set 1, the default display window from the VUI
 *                         will be applied to the video dimensions
 * @param vps_list if non-NULL, this function will validate that the SPS refers
 *                 to an existing VPS
 */
int ff_hevc_parse_sps(HEVCSPS *sps, GetBitContext *gb, unsigned int *sps_id,
                      int apply_defdispwin, AVBufferRef **vps_list, AVCodecContext *avctx);

int ff_hevc_decode_nal_vps(GetBitContext *gb, AVCodecContext *avctx,
                           HEVCParamSets *ps);
int ff_hevc_decode_nal_sps(GetBitContext *gb, AVCodecContext *avctx,
                           HEVCParamSets *ps, int apply_defdispwin);
int ff_hevc_decode_nal_pps(GetBitContext *gb, AVCodecContext *avctx,
                           HEVCParamSets *ps);
int ff_hevc_decode_nal_sei(HEVCContext *s);

/**
 * Mark all frames in DPB as unused for reference.
 */
void ff_hevc_clear_refs(HEVCContext *s);

/**
 * Drop all frames currently in DPB.
 */
void ff_hevc_flush_dpb(HEVCContext *s);

/**
 * Compute POC of the current frame and return it.
 */
int ff_hevc_compute_poc(HEVCContext *s, int poc_lsb);

RefPicList *ff_hevc_get_ref_list(HEVCContext *s, HEVCFrame *frame,
                                 int x0, int y0);

/**
 * Construct the reference picture sets for the current frame.
 */
int ff_hevc_frame_rps(HEVCContext *s);

/**
 * Construct the reference picture list(s) for the current slice.
 */
int ff_hevc_slice_rpl(HEVCContext *s);

void ff_hevc_save_states(HEVCContext *s, int ctb_addr_ts);
void ff_hevc_cabac_init(HEVCContext *s, int ctb_addr_ts);
int ff_hevc_sao_merge_flag_decode(HEVCContext *s);
int ff_hevc_sao_type_idx_decode(HEVCContext *s);
int ff_hevc_sao_band_position_decode(HEVCContext *s);
int ff_hevc_sao_offset_abs_decode(HEVCContext *s);
int ff_hevc_sao_offset_sign_decode(HEVCContext *s);
int ff_hevc_sao_eo_class_decode(HEVCContext *s);
int ff_hevc_end_of_slice_flag_decode(HEVCContext *s);
int ff_hevc_cu_transquant_bypass_flag_decode(HEVCContext *s);
int ff_hevc_skip_flag_decode(HEVCContext *s, int x0, int y0,
                             int x_cb, int y_cb);
int ff_hevc_pred_mode_decode(HEVCContext *s);
int ff_hevc_split_coding_unit_flag_decode(HEVCContext *s, int ct_depth,
                                          int x0, int y0);
int ff_hevc_part_mode_decode(HEVCContext *s, int log2_cb_size);
int ff_hevc_pcm_flag_decode(HEVCContext *s);
int ff_hevc_prev_intra_luma_pred_flag_decode(HEVCContext *s);
int ff_hevc_mpm_idx_decode(HEVCContext *s);
int ff_hevc_rem_intra_luma_pred_mode_decode(HEVCContext *s);
int ff_hevc_intra_chroma_pred_mode_decode(HEVCContext *s);
int ff_hevc_merge_idx_decode(HEVCContext *s);
int ff_hevc_merge_flag_decode(HEVCContext *s);
int ff_hevc_inter_pred_idc_decode(HEVCContext *s, int nPbW, int nPbH);
int ff_hevc_ref_idx_lx_decode(HEVCContext *s, int num_ref_idx_lx);
int ff_hevc_mvp_lx_flag_decode(HEVCContext *s);
int ff_hevc_no_residual_syntax_flag_decode(HEVCContext *s);
int ff_hevc_split_transform_flag_decode(HEVCContext *s, int log2_trafo_size);
int ff_hevc_cbf_cb_cr_decode(HEVCContext *s, int trafo_depth);
int ff_hevc_cbf_luma_decode(HEVCContext *s, int trafo_depth);
int ff_hevc_log2_res_scale_abs(HEVCContext *s, int idx);
int ff_hevc_res_scale_sign_flag(HEVCContext *s, int idx);

/**
 * Get the number of candidate references for the current frame.
 */
int ff_hevc_frame_nb_refs(HEVCContext *s);

int ff_hevc_set_new_ref(HEVCContext *s, AVFrame **frame, int poc);

/**
 * Find next frame in output order and put a reference to it in frame.
 * @return 1 if a frame was output, 0 otherwise
 */
int ff_hevc_output_frame(HEVCContext *s, AVFrame *frame, int flush);

void ff_hevc_bump_frame(HEVCContext *s);

void ff_hevc_unref_frame(HEVCContext *s, HEVCFrame *frame, int flags);

void ff_hevc_set_neighbour_available(HEVCContext *s, int x0, int y0,
                                     int nPbW, int nPbH);
void ff_hevc_luma_mv_merge_mode(HEVCContext *s, int x0, int y0,
                                int nPbW, int nPbH, int log2_cb_size,
                                int part_idx, int merge_idx, MvField *mv);
void ff_hevc_luma_mv_mvp_mode(HEVCContext *s, int x0, int y0,
                              int nPbW, int nPbH, int log2_cb_size,
                              int part_idx, int merge_idx,
                              MvField *mv, int mvp_lx_flag, int LX);
void ff_hevc_set_qPy(HEVCContext *s, int xBase, int yBase,
                     int log2_cb_size);
void ff_hevc_deblocking_boundary_strengths(HEVCContext *s, int x0, int y0,
                                           int log2_trafo_size);
int ff_hevc_cu_qp_delta_sign_flag(HEVCContext *s);
int ff_hevc_cu_qp_delta_abs(HEVCContext *s);
int ff_hevc_cu_chroma_qp_offset_flag(HEVCContext *s);
int ff_hevc_cu_chroma_qp_offset_idx(HEVCContext *s);
void ff_hevc_hls_filter(HEVCContext *s, int x, int y, int ctb_size);
void ff_hevc_hls_filters(HEVCContext *s, int x_ctb, int y_ctb, int ctb_size);
void ff_hevc_hls_residual_coding(HEVCContext *s, int x0, int y0,
                                 int log2_trafo_size, enum ScanType scan_idx,
                                 int c_idx);

void ff_hevc_hls_mvd_coding(HEVCContext *s, int x0, int y0, int log2_cb_size);


int ff_hevc_encode_nal_vps(HEVCVPS *vps, unsigned int id,
                           uint8_t *buf, int buf_size);
#if RPI_INTER
extern void rpi_flush_ref_frame_progress(HEVCContext * const s, ThreadFrame * const f, const unsigned int n);
#endif


/**
 * Reset SEI values that are stored on the Context.
 * e.g. Caption data that was extracted during NAL
 * parsing.
 *
 * @param s HEVCContext.
 */
void ff_hevc_reset_sei(HEVCContext *s);

extern const uint8_t ff_hevc_qpel_extra_before[4];
extern const uint8_t ff_hevc_qpel_extra_after[4];
extern const uint8_t ff_hevc_qpel_extra[4];

extern const uint8_t ff_hevc_diag_scan4x4_x[16];
extern const uint8_t ff_hevc_diag_scan4x4_y[16];
extern const uint8_t ff_hevc_diag_scan8x8_x[64];
extern const uint8_t ff_hevc_diag_scan8x8_y[64];

#ifdef RPI
int16_t * rpi_alloc_coeff_buf(HEVCContext * const s, const int buf_no, const int n);

// arm/hevc_misc_neon.S
// Neon coeff zap fn
#if HAVE_NEON
extern void rpi_zap_coeff_vals_neon(int16_t * dst, unsigned int l2ts_m2);
#endif

void ff_hevc_rpi_progress_wait_field(HEVCContext * const s, HEVCRpiJob * const jb,
                                     const HEVCFrame * const ref, const int val, const int field);

void ff_hevc_rpi_progress_signal_field(HEVCContext * const s, const int val, const int field);

// All of these expect that s->threads_type == FF_THREAD_FRAME

static inline void ff_hevc_progress_wait_mv(HEVCContext * const s, HEVCRpiJob * const jb,
                                     const HEVCFrame * const ref, const int y)
{
    if (s->enable_rpi)
        ff_hevc_rpi_progress_wait_field(s, jb, ref, y, 1);
    else
        ff_thread_await_progress((ThreadFrame*)&ref->tf, y, 0);
}

static inline void ff_hevc_progress_signal_mv(HEVCContext * const s, const int y)
{
    if (s->enable_rpi && s->used_for_ref)
        ff_hevc_rpi_progress_signal_field(s, y, 1);
}

static inline void ff_hevc_progress_wait_recon(HEVCContext * const s, HEVCRpiJob * const jb,
                                     const HEVCFrame * const ref, const int y)
{
    if (s->enable_rpi)
        ff_hevc_rpi_progress_wait_field(s, jb, ref, y, 0);
    else
        ff_thread_await_progress((ThreadFrame*)&ref->tf, y, 0);
}

static inline void ff_hevc_progress_signal_recon(HEVCContext * const s, const int y)
{
    if (s->used_for_ref)
    {
        if (s->enable_rpi)
            ff_hevc_rpi_progress_signal_field(s, y, 0);
        else
            ff_thread_report_progress(&s->ref->tf, y, 0);
    }
}

static inline void ff_hevc_progress_signal_all_done(HEVCContext * const s)
{
    if (s->enable_rpi)
    {
        ff_hevc_rpi_progress_signal_field(s, INT_MAX, 0);
        ff_hevc_rpi_progress_signal_field(s, INT_MAX, 1);
    }
    else
        ff_thread_report_progress(&s->ref->tf, INT_MAX, 0);
}

#else

// Use #define as that allows us to discard "jb" which won't exist in non-RPI world
#define ff_hevc_progress_wait_mv(s, jb, ref, y) ff_thread_await_progress((ThreadFrame *)&ref->tf, y, 0)
#define ff_hevc_progress_wait_recon(s, jb, ref, y) ff_thread_await_progress((ThreadFrame *)&ref->tf, y, 0)
#define ff_hevc_progress_signal_mv(s, y)
#define ff_hevc_progress_signal_recon(s, y) ff_thread_report_progress(&s->ref->tf, y, 0)
#define ff_hevc_progress_signal_all_done(s) ff_thread_report_progress(&s->ref->tf, INT_MAX, 0)

#endif

// Set all done - signal nothing (used in missing refs)
// Works for both rpi & non-rpi
static inline void ff_hevc_progress_set_all_done(HEVCFrame * const ref)
{
    if (ref->tf.progress != NULL)
    {
        int * const p = (int *)&ref->tf.progress->data;
        p[0] = INT_MAX;
        p[1] = INT_MAX;
    }
}

#endif /* AVCODEC_HEVC_H */
