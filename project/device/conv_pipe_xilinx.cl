/*
 * ------------------------------------------------------
 *
 *   PipeCNN: An OpenCL-Based FPGA Accelerator for CNNs
 *
 * ------------------------------------------------------
 * Filename:
 *   - conv_pipe.cl
 *
 * Author(s):
 *   - Dong Wang, wangdong@m.bjtu.edu.cn
 *
 * History:
 *   - v1.3 Win-Buffer-Based Implementation
 * ------------------------------------
 *
 *   Copyright (C) 2019, Institute of Information Science,
 *   Beijing Jiaotong University. All rights reserved.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 *
 */

#define USE_ROM

// The following macros are used for debug
//#define DEBUG_MEMRD
//#define DEBUG_CONV
//#define DEBUG_POOL
//#define DEBUG_MEMWR

#include "hw_param.cl"
#include "pipe.cl"
// #include "rtl_lib.h"
// #include <ap_int.h>

// Define the precision of the data-path
typedef char DPTYPE;
typedef int  MACTYPE;

#ifdef LAYER_ELT
typedef short ELWTYPE;
#endif

// Vectorized data type
typedef struct {
   DPTYPE data[VEC_SIZE];
} lane_data;

// Combined vec-data type from multiple lane
typedef struct {
   lane_data lane[LANE_NUM];
} channel_vec;

// Combined scalar data type from multiple lane
typedef struct {
   DPTYPE lane[LANE_NUM];
} channel_scal;
#ifdef RESNET
typedef struct {
   float lane[LANE_NUM];
} channel_scal_float;
#endif

// For debug uses
// void decode_disp(char a){
// 	for(int i=0; i<8; i++){
// 		printf("%d",(a&0x80)>>7);
// 		a = a<<1;
// 	}
// }

// void decode_disp_16(short a){
// 	for(int i=0; i<16; i++){
// 		printf("%d",(char)((a&0x8000)>>15));
// 		a = a<<1;
// 	}
// }

// void decode_disp_32(int a){
// 	for(int i=0; i<32; i++){
// 		printf("%d",(char)((a&0x80000000)>>31));
// 		a = a<<1;
// 	}
// }

// parallel MAC units including (VEC_SIZE-1) multipliers
__attribute__((always_inline))
MACTYPE mac(lane_data input, lane_data weights)
{
	MACTYPE output = MASK_MULT & CZERO;

	__attribute__((opencl_unroll_hint))
	for(int i=0; i<VEC_SIZE; i++){
		output += input.data[i]*weights.data[i];
	}
	return output;
}

// parallel XNOR and bit count unit
constant signed char HW[256] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4, //Hamming Weight
								1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 
								1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 
								2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 
								1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 
								2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 
								2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 
								3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 
								1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 
								2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 
								2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 
								3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 
								2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 
								3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 
								3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 
								4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8};

// for scale hard tanh
constant signed char ACT_ALPHA[]   = {96};
constant signed char ACT_ALPHA_FL[]= { 6};
constant signed char ACT_BETA[]    = {-8}; //beta and the input share the same fl
constant signed char ACT_OUT_FL[]  = { 6};
constant short ACT_N_THRE[]       = {-128};
constant short ACT_P_THRE[]       = { 128};

__attribute__((always_inline))
MACTYPE xnor_bitcount(lane_data input, lane_data weights)
{
	MACTYPE output = MASK_MULT & CZERO;

	__attribute__((opencl_unroll_hint))
	for(int i=0; i<VEC_SIZE; i++){
		output += HW[(unsigned char)~(input.data[i] ^ weights.data[i])];
	}
	output = (output<<1) - 8*VEC_SIZE;
	return output;
}

__attribute__((always_inline))
DPTYPE pool_max(DPTYPE a_in, DPTYPE b_in)
{
	DPTYPE max_value;

	if(a_in >= b_in)
		max_value = a_in;
	else
		max_value = b_in;

	return max_value;

}

// Fetch Data from Global Memory
__kernel
__attribute__((reqd_work_group_size(1,1,1)))
void memRead(
			// Params Ports
			uchar  binary,
			uchar  data_dim1,
			uchar  data_dim2,
			ushort data_dim1xdim2,
			uchar  weight_dim1,
			uchar  weight_dim2,
			ushort weight_dim3,
			ushort weight_dim4_div_lane, // avoid generating divider
			uchar  weight_dim1x2,
			uint   weight_dim1x2x3,
			uchar  conv_x,
			//uchar  conv_y,           // not used in this version
			uchar  stride,
			uchar  padding,
			uchar  split,
			uchar  group_num_x,
			uchar  group_num_y,
			uchar  group_rem_size_x,
			//uchar  group_rem_size_y, // not used in this version
			uint   group_rem_size_xyz,
			uchar  win_size_x,
			uchar  win_size_y,
			uint   win_size_xyz,
			// Data Ports
			__global lane_data    *restrict bottom,
			__global channel_vec  *restrict weights,
			__global volatile channel_scal *restrict bias,
			__global volatile channel_scal *restrict scale        )

{


	// Input Data, Weights and Bias
	lane_data     data_vec;
	channel_vec   data_ch_vec;
	channel_vec   weight_ch_vec;
	channel_scal  bias_ch_in;
	channel_scal  scale_ch_in;
	ushort        data_offset = 0; // assuming the 1st layer is not in split

	// virtual loop counters
	ushort gp_num_x, gp_num_y, out_idx_z;
	ushort gp_num_x_winbuf, gp_num_y_winbuf, out_idx_z_winbuf;
	uchar  output_idx_dim1, output_idx_dim2;
	ushort output_idx_dim3;
	uchar  win_itm_x, win_itm_y;
	ushort win_itm_z;

	uchar  gp_item_idx_x;

	ushort feature_idx_dim1, feature_idx_dim2;
	ushort feature_idx_dim3;

	uint   item_loop_bound;

	uchar  flag; // ping-pong flag

	uchar  load_weight_flag = 1;

	// Ping-pong buffer
	__local lane_data    win_buffer[2][WIN_BUF_SIZE]; // working sequence 0->1->0->1 ...
	// Weight buffer
	__local channel_vec  weight_buffer[WEIGHT_BUF_SIZE];
	// whether the data are valid for bin_conv, if the input is generated by padding then it is invalid.
	__local bool         bin_valid;
	__local bool         bin_valid_buffer[2][160];

	// Initialize the winbuf with the data in the first iteration of the group looping (as gp_num_x_winbuf=0, gp_num_y_winbuf=0)
	for(unsigned short win_itm_z=0; win_itm_z<weight_dim3/VEC_SIZE; win_itm_z++){
		for(unsigned char  win_itm_y=0; win_itm_y<win_size_y; win_itm_y++){
			for(unsigned char  win_itm_x=0; win_itm_x<win_size_x; win_itm_x++){

			feature_idx_dim1 = win_itm_x;
			feature_idx_dim2 = win_itm_y;
			feature_idx_dim3 = win_itm_z;

			// fetch feature map for the current group and caching in win buffer
			if((feature_idx_dim1>=padding && feature_idx_dim1<data_dim1+padding) && (feature_idx_dim2>=padding && feature_idx_dim2<data_dim2+padding)){

				data_vec = bottom[data_offset*data_dim1xdim2 + feature_idx_dim3*data_dim1xdim2 + (feature_idx_dim2-padding)*data_dim1 + (feature_idx_dim1-padding)];
				bin_valid = 0x01;
				
			}
			else{ // for padding (feature_idx<padding or data_dim+padding<=feature_idx<data_dim+2*padding)
				// or invalid work-item in the last group set feature map to zeros (feature_idx>=data_dim+2*padding)
				__attribute__((opencl_unroll_hint))
				for(unsigned char vv=0; vv<VEC_SIZE; vv++){
					data_vec.data[vv] = CZERO;
				}
				bin_valid = 0x00;
			}

			// start from using buffer[0]
			win_buffer[0][win_itm_z*win_size_y*win_size_x + win_itm_y*win_size_x + win_itm_x] = data_vec;
			bin_valid_buffer[0][win_itm_y*win_size_x + win_itm_x] = bin_valid;
			}
		}
	}

	// reset group virtual loop counters for winbuf loading operations
	// the gp loop counter for winbuf starts one iteration earlier than global group virtual loop counter
	// in this iteration, the winbuf is pre-initialized as previous loops shows
	if(group_num_x==1 && group_num_y==1){
		gp_num_x_winbuf = 0; // there is only one group for FC mode when batch=1
		gp_num_y_winbuf = 0;}
	else if(group_num_x==1){
		gp_num_x_winbuf = 0; // special case for convolution layers with weight_dim1/2=1, such as resnet50
		gp_num_y_winbuf = 1;}
	else{
		gp_num_x_winbuf = 1; // loop start from the second group for normal convolution layers
		gp_num_y_winbuf = 0;}

	out_idx_z_winbuf = 0;

	// reset global group virtual loop counters
	gp_num_x = 0;
	gp_num_y = 0;
	out_idx_z = 0;

	Group:for(unsigned int out_idx_xyz=0; out_idx_xyz<(weight_dim4_div_lane*group_num_y*group_num_x); out_idx_xyz++){
	// The following group loops are flattened as the upper loop to improve pipeline efficiency
	//for(unsigned short out_idx_z=0; out_idx_z<weight_dim4_div_lane; out_idx_z++){

		// special case when split==1, the output feature maps depend on only half the input feature maps
		if(split==0)
			data_offset = 0;
		else if(out_idx_z_winbuf<(weight_dim4_div_lane>>1)) // the lower half of the output feature maps depend on the lower half of the input
			data_offset = 0;
		else
			data_offset = weight_dim3/VEC_SIZE;	// the upper half of the output feature maps depend on the upper half of the input

		//for(unsigned short gp_num_y=0; gp_num_y<group_num_y; gp_num_y++){
			//for(unsigned short gp_num_x=0; gp_num_x<group_num_x+1; gp_num_x++){ // add one more extra iteration for ping-pong buffering operations

				flag = out_idx_xyz & 0x01; //ping-pong flag

				// reset output loop counters
				output_idx_dim1 = 0;
				output_idx_dim2 = 0;
				output_idx_dim3 = 0;
				// reset in-group item counters
				gp_item_idx_x = 0;

				// reset input winbuffer loop counters
				win_itm_x = 0;
				win_itm_y = 0;
				win_itm_z = 0;


				if(gp_num_x==group_num_x-1) // last group in each row
					// ensuring that both winbuf load loop and output loop are finished, i.e., use a larger value as the loop bound
					item_loop_bound = win_size_x>=group_rem_size_x?(win_size_xyz/VEC_SIZE):(group_rem_size_xyz/VEC_SIZE);
				else{
					if(stride>=weight_dim1 || stride>=weight_dim2) // special case convolution layers with stride>weight_dim1/2, such as resnet50
						item_loop_bound = win_size_xyz/VEC_SIZE;
					else
						item_loop_bound = (weight_dim1x2x3*CONV_GP_SIZE_Y*CONV_GP_SIZE_X/VEC_SIZE);
				}

				__attribute__((xcl_pipeline_loop(1)))
				for(unsigned int  win_itm_xyz=0; win_itm_xyz<item_loop_bound; win_itm_xyz++){
				//// The following loops are flattened as the upper loop to improve pipeline efficiency
				//for(unsigned short win_itm_z=0; win_itm_z<weight_dim3/VEC_SIZE; win_itm_z++){
				//	for(unsigned char  win_itm_y=0; win_itm_y<weight_dim2*CONV_GP_SIZE_Y; win_itm_y++){
				//		for(unsigned char  win_itm_x=0; win_itm_x<weight_dim1*CONV_GP_SIZE_X; win_itm_x++){

							// Winbuffer loading operations
							if(win_itm_z<weight_dim3/VEC_SIZE){

								feature_idx_dim1 = win_itm_x+gp_num_x_winbuf*CONV_GP_SIZE_X*stride;
								feature_idx_dim2 = win_itm_y+gp_num_y_winbuf*CONV_GP_SIZE_Y*stride;
								feature_idx_dim3 = win_itm_z;

								// fetch feature map for the current group and caching in win buffer
								if((feature_idx_dim1>=padding && feature_idx_dim1<data_dim1+padding) && (feature_idx_dim2>=padding && feature_idx_dim2<data_dim2+padding)){
									data_vec = bottom[data_offset*data_dim1xdim2 + feature_idx_dim3*data_dim1xdim2 + (feature_idx_dim2-padding)*data_dim1 + (feature_idx_dim1-padding)];
									bin_valid = 0x01;
								}
								else{ // for padding (feature_idx<padding or data_dim+padding<=feature_idx<data_dim+2*padding)
									// or invalid work-item in the last group set feature map to zeros (feature_idx>=data_dim+2*padding)
									__attribute__((opencl_unroll_hint))
									for(unsigned char vv=0; vv<VEC_SIZE; vv++){
										data_vec.data[vv] = CZERO;
									}
									bin_valid = 0x00;
								}

								win_buffer[(~flag)&0x01][win_itm_z*win_size_y*win_size_x + win_itm_y*win_size_x + win_itm_x] = data_vec;
								bin_valid_buffer[(~flag)&0x01][win_itm_y*win_size_x + win_itm_x] = bin_valid;

								// used as loop counters
								if((win_itm_y==win_size_y-1) && (win_itm_x==win_size_x-1)){
									win_itm_y = 0;
									win_itm_z++;
								}
								else if(win_itm_x==win_size_x-1)
									win_itm_y++;

								if(win_itm_x==win_size_x-1)
									win_itm_x = 0;
								else
									win_itm_x++;

							}

							// Load weight into weight buffer
							if(load_weight_flag==1){
								weight_ch_vec = weights[out_idx_z*weight_dim1x2x3/VEC_SIZE + output_idx_dim3*weight_dim1x2 + output_idx_dim2*weight_dim1 + output_idx_dim1];
								weight_buffer[output_idx_dim3*weight_dim2*weight_dim1 + output_idx_dim2*weight_dim1 + output_idx_dim1] = weight_ch_vec;
							}

							// Only output data for valid convolution work-items
							// In this version, grouping is only performed in row (x) direction
							if(gp_num_x*CONV_GP_SIZE_X+gp_item_idx_x<conv_x){

								if(output_idx_dim1==0 && output_idx_dim2==0 && output_idx_dim3==0){
									bias_ch_in = bias[out_idx_z];
									scale_ch_in= scale[out_idx_z];

									if(binary==0){
                                        bias_ch_write_pipe_block(bias_ch_in);
									}else{
                                        bias_ch_bin_ch_write_pipe_block(bias_ch_in);
                                        scale_ch_bin_ch_write_pipe_block(scale_ch_in);
									}
									//#ifdef DEBUG_MEMRD
									//printf("work-item x=%d, y=%d, z=%d, channel =0, write bias=%d\n", output_idx_dim1, output_idx_dim2, output_idx_dim3, bias_ch_in.lane[0]);
									//#endif
								}

								// data
								data_vec = win_buffer[flag][output_idx_dim3*win_size_y*win_size_x + output_idx_dim2*win_size_x + (output_idx_dim1+gp_item_idx_x*stride)];
								__attribute__((opencl_unroll_hint))
								for(unsigned char ll=0; ll<LANE_NUM; ll++){
									data_ch_vec.lane[ll] = data_vec;
								}
								if(binary==0){
                                    data_write_pipe_block(data_ch_vec);
								}
								else{
                                    data_bin_write_pipe_block(data_ch_vec);
								}


								// weight and bias fetcher
								weight_ch_vec = weight_buffer[output_idx_dim3*weight_dim2*weight_dim1 + output_idx_dim2*weight_dim1 + output_idx_dim1];
								//weight_ch_vec = weights[out_idx_z*weight_dim1x2x3/VEC_SIZE + output_idx_dim3*weight_dim1x2 + output_idx_dim2*weight_dim1 + output_idx_dim1];
								if(binary==0){
                                    weight_write_pipe_block(weight_ch_vec);
								}
								else{
									if(bin_valid_buffer[flag][output_idx_dim2*win_size_x + (output_idx_dim1+gp_item_idx_x*stride)]==0){
										__attribute__((opencl_unroll_hint))
										for(unsigned char lll = 0; lll < LANE_NUM; lll++){
											__attribute__((opencl_unroll_hint))
											for(unsigned char vvv = 0; vvv < VEC_SIZE; vvv++){
												weight_ch_vec.lane[lll].data[vvv] = 0x0f;
											}
										}
									}
                                    weight_bin_write_pipe_block(weight_ch_vec);
								}
								#ifdef DEBUG_MEMRD
								//if(gp_num_x==group_num_x-1 && gp_num_y==0 && out_idx_z==0){
									//printf("work-item x=%d, y=%d, z=%d, offset=%d, write data in channel 0=%f\n", output_idx_dim1, output_idx_dim2, output_idx_dim3, data_offset, (float)data_ch_vec.lane[0].data[0]);
									printf("work-item x=%d, y=%d, z=%d, write weight in channel 0=%f\n", output_idx_dim1, output_idx_dim2, output_idx_dim3, (float)weight_ch_vec.lane[0].data[0]);
								//}
								#endif

								// used as output loop counters
								if((output_idx_dim3==weight_dim3/VEC_SIZE-1) && (output_idx_dim2==weight_dim2-1) && (output_idx_dim1==weight_dim1-1)){
									output_idx_dim3 = 0;
									gp_item_idx_x++;
									load_weight_flag = 0;
								}
								else if((output_idx_dim2==weight_dim2-1)&& (output_idx_dim1==weight_dim1-1))
									output_idx_dim3++;

								if((output_idx_dim2==weight_dim2-1) && (output_idx_dim1==weight_dim1-1))
									output_idx_dim2 = 0;
								else if(output_idx_dim1==weight_dim1-1)
									output_idx_dim2++;

								if(output_idx_dim1==weight_dim1-1)
									output_idx_dim1 = 0;
								else
									output_idx_dim1++;

							}

				}

				//		}// end of win_itm_z
				//	}// end of win_itm_y
				//}// end of win_itm_x

		// used as virtual group loop counters for winbuf loading operations
		if((out_idx_z_winbuf==weight_dim4_div_lane-1) && (gp_num_y_winbuf==group_num_y-1) && (gp_num_x_winbuf==group_num_x-1))
			out_idx_z_winbuf = 0;
		else if((gp_num_y_winbuf==group_num_y-1) && (gp_num_x_winbuf==group_num_x-1))
			out_idx_z_winbuf++;

		if((gp_num_y_winbuf==group_num_y-1) && (gp_num_x_winbuf==group_num_x-1))
			gp_num_y_winbuf = 0;
		else if(gp_num_x_winbuf==group_num_x-1)
			gp_num_y_winbuf++;

		if(gp_num_x_winbuf==group_num_x-1)
			gp_num_x_winbuf = 0;
		else
			gp_num_x_winbuf++;

		// used as virtual group loop counters
		if((out_idx_z==weight_dim4_div_lane-1) && (gp_num_y==group_num_y-1) && (gp_num_x==group_num_x-1))
			out_idx_z = 0;
		else if((gp_num_y==group_num_y-1) && (gp_num_x==group_num_x-1)){
			out_idx_z++;
			load_weight_flag = 1;
		}

		if((gp_num_y==group_num_y-1) && (gp_num_x==group_num_x-1))
			gp_num_y = 0;
		else if(gp_num_x==group_num_x-1)
			gp_num_y++;

		if(gp_num_x==group_num_x-1)
			gp_num_x = 0;
		else
			gp_num_x++;


	//			}// end of gp_num_x
	//		}// end of gp_num_y
	//}// end of out_idx_z
	}

	//printf("Kernel 0 lanched !!!\n");
}


__kernel
//__attribute__((task))
__attribute__((reqd_work_group_size(1,1,1)))
void coreConv(
			// Params Ports
			uint  output_num,
			uint  conv_loop_cnt,
			uint  contol, //[0]-> relu  [1]->bypass pooling,for ResNet [0]->bn.[1]->wr(fc)
			char  frac_w,
			char  frac_din,
			char  frac_dout
			)
{
	channel_vec mac_data;
 	channel_vec mac_weight;
	channel_scal bias_ch_out;
	channel_scal conv_ch_in;
	DPTYPE  bias[LANE_NUM];
	MACTYPE conv_out[LANE_NUM];
	MACTYPE lane_accum[LANE_NUM];
	MACTYPE accum_piped[LANE_NUM][PIPE_DEPTH];
	MACTYPE conv_sign_exten[LANE_NUM];
	MACTYPE conv_with_rnd_bit[LANE_NUM];

	short conv_sum_bias[LANE_NUM];
	DPTYPE  conv_final[LANE_NUM];
#ifdef HTANH
	DPTYPE  act_beta_added[LANE_NUM];
	short   act_beta_added_buf[LANE_NUM];
	short   act_alpha_muled[LANE_NUM];
	short   act_alpha_muled_buf[LANE_NUM];
	short   act_alpha_muled_sign_exten[LANE_NUM];
	DPTYPE  act_output[LANE_NUM];
#endif
	int vec_sum = 0;

	// each iteration generates one output
	for(unsigned int k=0; k<output_num; k++){

        bias_ch_read_pipe_block(bias_ch_out);

		__attribute__((opencl_unroll_hint))
		for(unsigned char ll=0; ll<LANE_NUM; ll++){

			conv_out[ll] = CZERO;
			bias[ll] = bias_ch_out.lane[ll]; // pass to reg, avoid compile error

			// initialize the deep pipelined registers which store PIPE_DEPTH copys of partial results
			__attribute__((opencl_unroll_hint))
			for(unsigned int p=0; p<PIPE_DEPTH; p++){
				accum_piped[ll][p] = MASK_ACCUM & CZERO;
			}
		}

		__attribute__((xcl_pipeline_loop(1)))
		for(int j=0; j<conv_loop_cnt; j++){

			// load data and weights for each lane
            data_read_pipe_block(mac_data);
            weight_read_pipe_block(mac_weight);

			// add results from all lanes
			// accumulate with the last copy
			__attribute__((opencl_unroll_hint))
			for(unsigned char ll=0; ll<LANE_NUM; ll++){

				lane_accum[ll] = (MASK_ACCUM & accum_piped[ll][PIPE_DEPTH-1]) + (MASK_MULT & mac(mac_data.lane[ll], mac_weight.lane[ll]));

				// Shift the pipelined registers backwards
				__attribute__((opencl_unroll_hint))
				for(unsigned int p=PIPE_DEPTH-1; p>0; p-- ){
					accum_piped[ll][p] = MASK_ACCUM & accum_piped[ll][p-1];
				}

				// update the first copy
				accum_piped[ll][0] = MASK_ACCUM & lane_accum[ll];

				#ifdef DEBUG_CONV
				//if(ll==0 && k==0){
				//	printf("dot_cnt=%d data=%f weight=%f (loop=%d, lane= %d, vec=0)\n", k, (float)mac_data.lane[ll].data[0], (float)mac_weight.lane[ll].data[0], j, ll);
				//}
				#endif
			}
		}// end of conv loop

		__attribute__((opencl_unroll_hint))
		for(unsigned char ll=0; ll<LANE_NUM; ll++){

			// accumulate all the partial results
			__attribute__((opencl_unroll_hint))
			for(unsigned i=0; i<PIPE_DEPTH; i++){
				conv_out[ll] += accum_piped[ll][i];
			}
			// round and truncate the results to the output precision
			// note: ((frac_w+frac_din)-frac_dout)) should be checked by host to be a positive number
			if(conv_out[ll]>=0)
				conv_sign_exten[ll] = 0x00;
			else
				conv_sign_exten[ll] = ~(0xFFFFFFFF>>(frac_w+frac_din-frac_dout-1)); // ">>" is logic shift, then perform sign extension manually

			 // First, perform sign extension and the 1st-step rounding before sum with bias
			conv_with_rnd_bit[ll] = (conv_sign_exten[ll] | (conv_out[ll]>>(frac_w+frac_din-frac_dout-1))) + 0x01;
			// Second, deal with Overflow and Underflow cases and the 2nd rounding after sum with bias
			if(conv_with_rnd_bit[ll]>=256)
				conv_sum_bias[ll] = ((short)bias[ll]<<1) + 254;
			else if(conv_with_rnd_bit[ll]<-256)
				conv_sum_bias[ll] = ((short)bias[ll]<<1) - 256;
			else
			    // clear 1st-step rounding bit by using MASK9B
				// then sum with bias and perform 2nd-step rounding
				// note: (frac_w-frac_dout-1) should be checked by host to be a positive number
				// conv_sum_bias[ll] = (MASK9B & conv_with_rnd_bit[ll])+(bias[ll]>>(frac_w-frac_dout-1))+0x01; //bias and the weight share the  same fl.
				conv_sum_bias[ll] = ( conv_with_rnd_bit[ll]) + ((short)bias[ll]<<1); //bias and the output share the same fl.
			// final truncation
			if(conv_sum_bias[ll]>=254){
				conv_final[ll] = 127;
			}				
			else if(conv_sum_bias[ll]<-256){
				conv_final[ll] = -128;
			}
			else{
				conv_final[ll] = MASK8B & (conv_sum_bias[ll]>>0x01);  // remove the last rounding bit
			}
			// Activation function
			if((contol&0x01)==0x01){
#ifndef HTANH //Normal activation e.g. Relu
				// Relu operation
				if((conv_final[ll]&MASKSIGN)==MASKSIGN) // MSB is sign bit
					conv_ch_in.lane[ll] = 0;
				else
					conv_ch_in.lane[ll] = conv_final[ll];
#else
				//////////////////////////// Scale Hard Tanh Begin //////////////////////////////
				// y = clip( alpha(x+beta) )
				act_beta_added_buf[ll] = conv_final[ll] + (signed char)ACT_BETA[0]; //conv_final[ll] share the same fl with BETA.
				if(act_beta_added_buf[ll]>=127)
					act_beta_added[ll] = 127;
				else if(act_beta_added_buf[ll]<-128)
					act_beta_added[ll] = -128;
				else
					act_beta_added[ll] = MASK8B & act_beta_added_buf[ll];
				
				act_alpha_muled_buf[ll] = act_beta_added[ll] * (signed char)ACT_ALPHA[0];

				if(act_alpha_muled_buf[ll]>=0)
					act_alpha_muled_sign_exten[ll] = 0x00;
				else
					act_alpha_muled_sign_exten[ll] = ~(0xFFFF>>(frac_dout+ACT_ALPHA_FL[0]-ACT_OUT_FL[0]-1)); // ">>" is logic shift, then perform sign extension manually

				act_alpha_muled[ll] = (act_alpha_muled_sign_exten[ll] | (act_alpha_muled_buf[ll]>>(frac_dout+ACT_ALPHA_FL[0]-ACT_OUT_FL[0]-1))) + 0x01;

				if(act_alpha_muled[ll] < (short)ACT_N_THRE[0])
					act_output[ll] = 0xff & (ACT_N_THRE[0]>>1);
				else if(act_alpha_muled[ll] > (short)ACT_P_THRE[0])
					act_output[ll] = 0xff & (ACT_P_THRE[0]>>1);
				else
					act_output[ll] = 0xff & (act_alpha_muled[ll]>>1);
				
				conv_ch_in.lane[ll]= act_output[ll];
				//////////////////////////// Scale Hard Tanh Finish ////////////////////////////
			}
			else
				conv_ch_in.lane[ll] = conv_final[ll];

			#ifdef DEBUG_CONV
			if(ll==0 && k==0)
				printf("dot_cnt=%d sum=%f rnd=%f sum_bias=%f final=%f (bias=%f)\n\n", k, (float)conv_out[ll], (float)conv_with_rnd_bit[ll], (float)conv_sum_bias[ll], (float)conv_final[ll], (float)bias[ll]);
			#endif
#endif
		}
        conv_ch_write_pipe_block(conv_ch_in);

	}// end of output_num loop
	//printf("Kernel coreConv lanched !!!\n");
}

__kernel
//__attribute__((task))
__attribute__((reqd_work_group_size(1,1,1)))
void coreConvBin(
			// Params Ports
			uint  output_num,
			uint  conv_loop_cnt,
			uint  contol, //[0]-> relu  [1]->bypass pooling,for ResNet [0]->bn.[1]->wr(fc)
			char  frac_scale,
			char  frac_din,
			char  frac_dout
			)
{
	channel_vec mac_data;
 	channel_vec mac_weight;
	channel_scal scale_ch_out;
	channel_scal bias_ch_out;
	channel_scal conv_ch_in;
	DPTYPE  scale[LANE_NUM];
	DPTYPE  bias[LANE_NUM];
	MACTYPE conv_out[LANE_NUM];
	MACTYPE lane_accum[LANE_NUM];
	MACTYPE accum_piped[LANE_NUM][PIPE_DEPTH];
	MACTYPE conv_sign_exten[LANE_NUM];
	MACTYPE conv_with_rnd_bit[LANE_NUM];
	MACTYPE conv_sum_bias[LANE_NUM];
	DPTYPE  conv_final[LANE_NUM];

	// each iteration generates one output
	for(unsigned int k=0; k<output_num; k++){

        bias_ch_bin_ch_read_pipe_block(bias_ch_out);
        scale_ch_bin_ch_read_pipe_block(scale_ch_out);

		__attribute__((opencl_unroll_hint))
		for(unsigned char ll=0; ll<LANE_NUM; ll++){

			conv_out[ll] = CZERO;
			scale[ll]= scale_ch_out.lane[ll];// scaling factor of the weight.
			bias[ll] = bias_ch_out.lane[ll]; // pass to reg, avoid compile error
			// initialize the deep pipelined registers which store PIPE_DEPTH copys of partial results
			__attribute__((opencl_unroll_hint))
			for(unsigned int p=0; p<PIPE_DEPTH; p++){
				accum_piped[ll][p] = MASK_ACCUM & CZERO;
			}
		}

		__attribute__((xcl_pipeline_loop(1)))
		for(int j=0; j<conv_loop_cnt; j++){

			// load data and weights for each lane
            data_bin_read_pipe_block(mac_data);
            weight_bin_read_pipe_block(mac_weight);
			// add results from all lanes
			// accumulate with the last copy
			__attribute__((opencl_unroll_hint))
			for(unsigned char ll=0; ll<LANE_NUM; ll++){

				lane_accum[ll] = (MASK_ACCUM & accum_piped[ll][PIPE_DEPTH-1]) + (MASK_MULT & xnor_bitcount(mac_data.lane[ll], mac_weight.lane[ll]));

				// Shift the pipelined registers backwards
				__attribute__((opencl_unroll_hint))
				for(unsigned int p=PIPE_DEPTH-1; p>0; p-- ){
					accum_piped[ll][p] = MASK_ACCUM & accum_piped[ll][p-1];
				}

				// update the first copy
				accum_piped[ll][0] = MASK_ACCUM & lane_accum[ll];

				#ifdef DEBUG_CONV
				//if(ll==0 && k==0){
				//	printf("dot_cnt=%d data=%f weight=%f (loop=%d, lane= %d, vec=0)\n", k, (float)mac_data.lane[ll].data[0], (float)mac_weight.lane[ll].data[0], j, ll);
				//}
				#endif
			}
		}// end of conv loop

		__attribute__((opencl_unroll_hint))
		for(unsigned char ll=0; ll<LANE_NUM; ll++){

			// accumulate all the partial results
			__attribute__((opencl_unroll_hint))
			for(unsigned i=0; i<PIPE_DEPTH; i++){
				conv_out[ll] += accum_piped[ll][i];
			}

			// round and truncate the results to the output precision
			// note: ((frac_w+frac_din)-frac_dout)) should be checked by host to be a positive number
			conv_out[ll] = conv_out[ll] * scale[ll];
			if(conv_out[ll]>=0)
				conv_sign_exten[ll] = 0x00;
			else
				conv_sign_exten[ll] = ~(0xFFFFFFFF>>(frac_scale-frac_dout-1)); // ">>" is logic shift, then perform sign extension manually

			 // First, perform sign extension and the 1st-step rounding before sum with bias
			conv_with_rnd_bit[ll] = (conv_sign_exten[ll] | (conv_out[ll]>>(frac_scale-frac_dout-1))) + 0x01;

			// Second, deal with Overflow and Underflow cases and the 2nd rounding after sum with bias
			if(conv_with_rnd_bit[ll]>=256)
				conv_sum_bias[ll] = MASK9B & 0xFF; //=255
			else if(conv_with_rnd_bit[ll]<-256)
				conv_sum_bias[ll] = MASK9B & 0x100; //=-256
			else
			    // clear 1st-step rounding bit by using MASK9B
				// then sum with bias and perform 2nd-step rounding
				// note: (frac_w-frac_dout-1) should be checked by host to be a positive number
				// conv_sum_bias[ll] = (MASK9B & conv_with_rnd_bit[ll])+(bias[ll]>>(frac_w-frac_dout-1))+0x01; //bias and the weight share the  same fl.
				conv_sum_bias[ll] = (MASK9B & conv_with_rnd_bit[ll])+(bias[ll]<<1)+0x01; //bias and the output share the same fl.

			// final truncation
			conv_final[ll] = MASK8B & (conv_sum_bias[ll]>>0x01);  // remove the last rounding bit

			conv_ch_in.lane[ll] = conv_final[ll];

		}
        conv_ch_bin_ch_write_pipe_block(conv_ch_in);
	}// end of output_num loop
	//printf("Kernel coreConv lanched !!!\n");
}

__kernel
//__attribute__((task))
__attribute__((reqd_work_group_size(1,1,1)))
void maxPool(
		// Params Ports
		uchar    conv_x,
		ushort   conv_xy,
		uchar    pool_dim1,
		ushort   pool_dim3,
		ushort   pool_dim1x2,
		uchar    pool_size,
		uchar    pool_stride,
		uchar    padd_offset,
		ushort   pool_times, //pool_group*pool_y
		ushort   pool_group, //the number of pooling z dimension vectorized packet
		ushort   pool_y_bound, //pooling bound per pool_y(item_loop_bound*(pool_win_num_x+1))
		ushort   item_loop_bound, // maximum of load_data_bound and write_back_bound
		ushort   load_data_bound, // pooling window buffer load data bound
		ushort   write_back_bound,// pooling window buffer write back result to global memory bound
		uchar    pool_win_num_x, //the number of pool window buffer
		uchar    win_size_x, // pool window buffer size of x dimension
		__global volatile channel_scal * restrict bottom,
		__global DPTYPE * restrict top,
		//binary
		__global DPTYPE * restrict top_bin
		)
{
	bool  pool_sync=0; // Receive channel synchronization signal
	uint  base_addr; // basic address of global memory
	uchar flag; // ping-pong buffer flag

	// the counter of pooling hierarchy
	ushort  pool_group_cnt; // pooling z dimension vectorized packet counter
	uchar   pool_y_cnt; // pooling y dimension counter
	ushort  item_loop_cnt; // the counter of item_loop_bound
	ushort  pool_win_cnt; // the counter of pool_win_num_x(ping-pong +1)
	// the counter of pool window buffer
	uchar   win_item_x; // x dimension
	uchar   win_item_y; // y dimension
	uchar   win_item_s; // pool stride in pool window buffer
	uchar   win_item_cnt; // for win_item_s
	// the counter of write back
	uchar   gp_final_cnt; // pooling result counter in window buffer
	uchar   lane_cnt;
	ushort  global_z;
	ushort  global_index_z_group;
	uchar   global_index_z_item;
	// the register of pooling
	DPTYPE  shift_reg[LANE_NUM][POOL_MAX_SIZE]; // cache from global memory
	DPTYPE  temp_reg0[LANE_NUM];
	DPTYPE  temp_reg1[LANE_NUM];
	DPTYPE  temp_reg2[LANE_NUM];
	DPTYPE  temp_reg[LANE_NUM];
	DPTYPE  temp_max[LANE_NUM];

	DPTYPE  row_reg0[LANE_NUM][POOL_GP_SIZE_X] __attribute__((xcl_array_partition(complete, 1))); // pooling reslut in the first line
	DPTYPE  row_reg1[LANE_NUM][POOL_GP_SIZE_X] __attribute__((xcl_array_partition(complete, 1))); // pooling reslut in the max(second line , first line)
	DPTYPE  pool_final[2][POOL_GP_SIZE_X][LANE_NUM]; // final pooling reslut

	// binary
	uint    top_addr_bin;
	ushort  global_index_z_group_bin;
	uchar   global_index_z_item_bin;
	__local unsigned char buffer_bin[LANE_NUM/8];

	__attribute__((opencl_unroll_hint))
	for(unsigned char i=0; i<LANE_NUM/8; i++) {
		buffer_bin[i] = 0;
	}

	// init hierarchy counters
	pool_y_cnt = 0;
	pool_group_cnt = 0;
	//#pragma ivdep array(pool_final)
	for(ushort i = 0; i < pool_times; i++){
        pool_sync_ch_read_pipe_block(pool_sync);
		// mem_fence(CLK_CHANNEL_MEM_FENCE);

		// init counters
		pool_win_cnt = 0;
		item_loop_cnt = 0;
		win_item_x = 0;
		win_item_y = 0;
		win_item_s = 0;
		win_item_cnt = 0;
		gp_final_cnt = 0;
		lane_cnt = 0;

		__attribute__((xcl_pipeline_loop(1)))
		for(ushort k=0; k<pool_y_bound; k++){
			flag = pool_win_cnt & 0x01;
			base_addr = pool_group_cnt*conv_xy + pool_stride*conv_x*pool_y_cnt + pool_stride*pool_win_cnt*POOL_GP_SIZE_X;

			// load data from global memory to shift registers and pool (0--pool_win_num_x-1)
			if((pool_win_cnt < pool_win_num_x)&&(item_loop_cnt < load_data_bound)){

				if(win_item_x > pool_size-1){
					win_item_cnt++;
				}
				__attribute__((opencl_unroll_hint))
				for(uchar ll=0; ll<LANE_NUM; ll++){
					if( (pool_win_cnt*POOL_GP_SIZE_X*pool_stride+win_item_x) < conv_x){
						// load global memory to shift register
						shift_reg[ll][0] = bottom[base_addr+win_item_y*conv_x+win_item_x].lane[ll];

						// fetch the data from shift register to pool
						if((win_item_x == pool_size-1) || (win_item_cnt == pool_stride)){
							temp_reg0[ll] = shift_reg[ll][0];
							temp_reg1[ll] = shift_reg[ll][1];
							temp_reg2[ll] = shift_reg[ll][2];
						}

						else{
							temp_reg0[ll] = CZERO;
							temp_reg1[ll] = CZERO;
							temp_reg2[ll] = CZERO;
						}
						// pooling for pool size equal 3
						if(pool_size == 3){
							temp_reg[ll] = pool_max(temp_reg0[ll],temp_reg1[ll]);
							temp_max[ll] = pool_max(temp_reg2[ll],temp_reg[ll]);
							switch(win_item_y){
								case 0: row_reg0[ll][win_item_s] = temp_max[ll]; break;
								case 1: row_reg1[ll][win_item_s] = pool_max(temp_max[ll],row_reg0[ll][win_item_s]); break;
								case 2: pool_final[flag][win_item_s][ll] = pool_max(temp_max[ll],row_reg1[ll][win_item_s]); break;
							}
						}
						// pooling for pool size equal 2
						else{
							temp_max[ll] = pool_max(temp_reg1[ll],temp_reg0[ll]);
							switch(win_item_y){
								case 0: row_reg0[ll][win_item_s] = temp_max[ll]; break;
								case 1: pool_final[flag][win_item_s][ll] = pool_max(temp_max[ll],row_reg0[ll][win_item_s]); break;
							}
						}

						// shift register
						__attribute__((opencl_unroll_hint))
						for(uchar p=POOL_MAX_SIZE-1; p>0; p--){
							shift_reg[ll][p] = shift_reg[ll][p-1];
						}
					}
				}


				if((win_item_x == pool_size-1)||(win_item_cnt == pool_stride)){
					win_item_s++;
				}

				if(win_item_cnt == pool_stride){
					win_item_cnt = 0;
				}

				if((win_item_y == pool_size-1) && (win_item_x == win_size_x-1))
					win_item_y = 0;
				else if(win_item_x == win_size_x-1)
					win_item_y++;
				if(win_item_x == win_size_x-1)
					win_item_x = 0;
				else
					win_item_x++;

				if(win_item_x == 0)
					win_item_s = 0;

			}

			// write back result to global memoey
			// perform vectorization in dim3 (global_z) by combining multiple DPTYPE data into lane_data type
			if((pool_win_cnt > 0) && (item_loop_cnt < write_back_bound)){
				if(((pool_win_cnt-1)*POOL_GP_SIZE_X+gp_final_cnt) < pool_dim1){
					global_z = pool_group_cnt*LANE_NUM+lane_cnt;
					global_index_z_group = (global_z-padd_offset) / VEC_SIZE;
					global_index_z_item =  (global_z-padd_offset) % VEC_SIZE;
					if((global_z-padd_offset)<pool_dim3 && global_z>=padd_offset){
						top[global_index_z_group*pool_dim1x2*VEC_SIZE+pool_y_cnt*pool_dim1*VEC_SIZE+((pool_win_cnt-1)*POOL_GP_SIZE_X+gp_final_cnt)*VEC_SIZE+global_index_z_item] = pool_final[!flag][gp_final_cnt][lane_cnt];
						
						// binary packing: merge 8 bits into one byte.
						// WARNING: The padd_offset MUST EQUALS TO ZERO!
						buffer_bin[lane_cnt/8] =  (buffer_bin[lane_cnt/8]<<1) | (0x01 ^ ( (pool_final[!flag][gp_final_cnt][lane_cnt] & 0x80)>>7 ));
						if(lane_cnt%8==7){
							global_index_z_group_bin = (global_z/8) / VEC_SIZE;
							global_index_z_item_bin  = (global_z/8) % VEC_SIZE;
							top_addr_bin = global_index_z_group_bin*pool_dim1x2*VEC_SIZE + pool_y_cnt*pool_dim1*VEC_SIZE + ((pool_win_cnt-1)*POOL_GP_SIZE_X+gp_final_cnt)*VEC_SIZE + global_index_z_item_bin;
							top_bin[top_addr_bin] = buffer_bin[lane_cnt/8];
						}
					}
				}

				if((gp_final_cnt == POOL_GP_SIZE_X-1) && (lane_cnt == LANE_NUM-1))
					gp_final_cnt = 0;
				else if(lane_cnt == LANE_NUM-1)
					gp_final_cnt++;
				if(lane_cnt == LANE_NUM-1)
					lane_cnt = 0;
				else
					lane_cnt++;
			}

			if((pool_win_cnt == pool_win_num_x) && (item_loop_cnt == item_loop_bound-1))
				pool_win_cnt = 0;
			else if(item_loop_cnt == item_loop_bound-1)
				pool_win_cnt++;
			if(item_loop_cnt == item_loop_bound-1)
				item_loop_cnt = 0;
			else
				item_loop_cnt++;
		}

		if((pool_group_cnt == pool_group-1) && (pool_y_cnt == pool_dim1-1))
			pool_group_cnt = 0;
		else if(pool_y_cnt == pool_dim1-1)
			pool_group_cnt++;
		if(pool_y_cnt == pool_dim1-1)
			pool_y_cnt = 0;
		else
			pool_y_cnt++;
	}
}

__kernel
// __attribute__((task))
__attribute__((reqd_work_group_size(1,1,1)))
void memWrite(
				// Params Ports
				uchar  binary, //data are sent by the conv_bin kenel.
				uchar  out_dim1,
				uchar  out_dim2,
				ushort out_dim3,
				ushort out_dim1xbatch, // out_dim1 x sqrt(batch_size)
				uint   out_dim1x2xbatch, // out_dim1 x out_dim2 x batch_size
				uchar  batch_indx_dim1,
				uchar  batch_indx_dim2,
#if defined(RESNET) || defined(TEST)
				uchar  bypass,
				uchar  pool_pad,	  //RESNET need pad,set to 1,other CNN set 0
#endif
				uchar  padd_offset,
				uchar  pool_on,
				uchar  pool_size,
				uchar  pool_stride,
				// uchar  top_bin_ctrl, //whether to package and store the binary feature map.
				// Data Ports
				__global DPTYPE *restrict top,
				__global DPTYPE *restrict top_bin
)
{
    uchar  index_z_item; // max value 256
    ushort index_z_group;// max value 4096
	uint   top_addr;
	bool   pool_on_signal=1;
    channel_scal output;
    __local DPTYPE buffer[LANE_NUM];
	uint   base_addr;

	uchar  vec_gp = 0;//which vec in one lane
	ushort loop_x = 0;
	ushort loop_y = 0;
	ushort loop_z = 0;//which lane

	__attribute__((xcl_pipeline_loop(1)))
    for(unsigned int i=0; i<(out_dim1*out_dim2*out_dim3/VEC_SIZE); i++) {
		if(vec_gp == 0){
			if((pool_on == 1) && ((loop_y >= out_dim2-pool_pad) || ((loop_x >= out_dim1-pool_pad)))){
				__attribute__((opencl_unroll_hint))
				for(uchar ll=0; ll<LANE_NUM; ll++){
					output.lane[ll]=-128;//-128
				}
			}
			else{
				if(binary==0){
                    conv_ch_read_pipe_block(output);
				}
				else{
                    conv_ch_bin_ch_read_pipe_block(output);
				}
			}
			// store the vectorized output into local buffer
			__attribute__((opencl_unroll_hint))
			for(uchar ll=0; ll<LANE_NUM; ll++){
				buffer[ll]=output.lane[ll];
			}
		}
		// fetch data from local buffer and write back to DDR
		// perform vectorization in dim3 (global_z) by combining multiple DPTYPE data into lane_data type
		if(pool_on != 1)//padding offset must be ZERO!
			base_addr = (loop_z*LANE_NUM+vec_gp*VEC_SIZE)/VEC_SIZE*out_dim1x2xbatch*VEC_SIZE + (loop_y+batch_indx_dim2*out_dim2)*out_dim1xbatch*VEC_SIZE + (loop_x+batch_indx_dim1*out_dim1)*VEC_SIZE;
		else
			base_addr = loop_z*out_dim1x2xbatch*LANE_NUM +(loop_y+batch_indx_dim2*out_dim2)*out_dim1xbatch*LANE_NUM + (loop_x+batch_indx_dim1*out_dim1)*LANE_NUM + vec_gp*VEC_SIZE;
		
		__attribute__((opencl_unroll_hint))
		for(uchar vv=0; vv<VEC_SIZE; vv++){
			top[base_addr+vv] = buffer[vec_gp*VEC_SIZE + vv];
		}

		if(pool_on == 1 && vec_gp == (LANE_NUM/VEC_SIZE-1)){
			if((loop_x==out_dim1-1)&&(loop_y > 0)&&((loop_y-pool_size+1)%2 == 0)){//%2, for 2 is the pooling stride
				pool_sync_ch_write_pipe_block(pool_on_signal);
			}
		}

		if((loop_z==out_dim3/LANE_NUM-1) && (loop_y==out_dim2-1) && (loop_x==out_dim1-1) && (vec_gp==(LANE_NUM/VEC_SIZE-1)))
			loop_z = 0;
		else if((loop_y==out_dim2-1) && (loop_x==out_dim1-1) && (vec_gp==(LANE_NUM/VEC_SIZE-1)))
			loop_z++;

		if((loop_y==out_dim2-1) && (loop_x==out_dim1-1) && (vec_gp==(LANE_NUM/VEC_SIZE-1)))
			loop_y = 0;
		else if((loop_x==out_dim1-1) && (vec_gp==(LANE_NUM/VEC_SIZE-1)))
			loop_y++;

		if((loop_x==out_dim1-1) && (vec_gp==(LANE_NUM/VEC_SIZE-1)))
			loop_x = 0;
		else if(vec_gp==(LANE_NUM/VEC_SIZE-1))
			loop_x++;

		if(vec_gp==(LANE_NUM/VEC_SIZE-1))
			vec_gp = 0;
		else
			vec_gp++;
			
    }

}

#ifdef LAYER_ELT
__kernel
//__attribute__((task))
__attribute__((reqd_work_group_size(1,1,1)))
void eltwise(
			char  act_ctrl,
			uint  input_num,//dim1*dim2*dim3/VEC_SIZE
			uchar pool_on,//only avgpool used
			//uchar pool_size,  // by now, only pooling size is 7
			uchar conv_x,
			uchar conv_y,
			// uint  convz_div_vec,
			uint  conv_xy,
			uchar stride,     // stride of stride pool
			// float divisor,	  //1/pool_size^2
			char  in1_frac,	  //elt in1 frac
			char  in2_frac,   //elt in2 frac
			char  out_frac,   //elt out frac
			// scale hardtanh
			char  act_alpha,
			char  act_alpha_fl,
			char  act_beta,
			char  act_out_fl,
			short act_n_thre,
			short act_p_thre,
			// float out_conver2char,
			__global lane_data *restrict bottom_1,
			__global lane_data *restrict bottom_2,
			__global lane_data *restrict top,
			__global volatile DPTYPE *restrict top_bin
			)
{
	lane_data data_out;
	//uchar pool_size_num;  //pool_size^2
	//pool_size_num=pool_size*pool_size;
	uchar conv_itm_x=0;
	uint  conv_itm_xyz=0;
	uint  xyz_offset=0;
	uint  xy_offset=0;
	uint  ptr=0;
	uint  outnum=0;//if have avgpool the out ptr
	//avg pool
	int   out;
	int   sumAvg[VEC_SIZE];
	int   avgPoolSum[VEC_SIZE];
	int   avgPoolBuf[VEC_SIZE][ELT_PIPE_DEPTH];

	// fixpoint eltwise
	char  n_bits[VEC_SIZE];
	short bottom_1_round[VEC_SIZE];
	short bottom_1_alined[VEC_SIZE];
	short bottom_1_sign_extent[VEC_SIZE];
	char  elt_sum[VEC_SIZE];
	short elt_sum_tmp[VEC_SIZE];
	short elt_mul_tmp[VEC_SIZE];
	char  act_final[VEC_SIZE];
	// fixpoint avgpool
	int  divisor = 84;
	#define DIVISOR_FL 12;

	// binary packing
	unsigned char  fix_bin_x_idx = 0;
	unsigned char  fix_bin_y_idx = 0;
	unsigned short fix_gpz_idx = 0;
	unsigned short bin_base_idxz = 0;
	
	unsigned short fix_xy_idx  = 0;
	int     top_bin_ptr = 0;
	uchar bin_buffer0 = 0;
	uchar bin_buffer0_tmp[8];
	uchar bin_buffer1 = 0;
	char  bit_sel = 0;
	int		gp_tmp = 0;

#ifdef HTANH
	DPTYPE  act_beta_added[VEC_SIZE];
	short act_beta_added_buf[VEC_SIZE];
	short act_alpha_muled[VEC_SIZE];
	short act_alpha_muled_buf[VEC_SIZE];
	short   act_alpha_muled_sign_exten[VEC_SIZE];
	DPTYPE  act_output[VEC_SIZE];
#endif
	//init avg_pool buffer.
	__attribute__((opencl_unroll_hint))
	for(unsigned char vv=0; vv<VEC_SIZE; vv++){
		avgPoolSum[vv]=0;
		__attribute__((opencl_unroll_hint))
		for(unsigned char pp=0; pp<ELT_PIPE_DEPTH; pp++){
			avgPoolBuf[vv][pp] = 0;
		}
	}

	__attribute__((xcl_pipeline_loop(1)))
	for(unsigned int j=0;j<input_num;j++){
		//load binary feature map VEC_SIZE==8
		bin_base_idxz = fix_gpz_idx*VEC_SIZE/8; // base index_z of top bin
		top_bin_ptr = (bin_base_idxz/VEC_SIZE)*conv_xy*VEC_SIZE + fix_bin_y_idx*conv_x*VEC_SIZE + fix_bin_x_idx*VEC_SIZE + bin_base_idxz%VEC_SIZE;// the address of top bin
		//load binary feature map, VEC_SIZE==12 or VEC_SIZE==16
		// TODO
		gp_tmp = fix_gpz_idx*VEC_SIZE;
		__attribute__((opencl_unroll_hint))
		for(unsigned char vv=0; vv<VEC_SIZE; vv++){
			// aline the fl, in the final version, only one of bottom1 or bottom2 needs to be alined,
			// for one of the bottoms is alined when doing the convolution. out_frac is the desire fl.
			if(in1_frac <= out_frac){
				bottom_1_alined[vv] = ((short)bottom_1[j].data[vv]) << (out_frac-in1_frac);
			}
			else{
				n_bits[vv] = in1_frac - out_frac;
				bottom_1_round[vv] = ((short)bottom_1[j].data[vv]) + (0x01<<(n_bits[vv]-1));
				if(bottom_1_round[vv] >= 0)
					bottom_1_sign_extent[vv] = 0;
				else
					bottom_1_sign_extent[vv] = ~(0xffff >> n_bits[vv]);
				bottom_1_alined[vv] = bottom_1_sign_extent[vv] | bottom_1_round[vv]>>n_bits[vv];
			}

			elt_sum_tmp[vv] = bottom_1_alined[vv] + (short)bottom_2[j].data[vv];

			if(elt_sum_tmp[vv]<-128){
				elt_sum_tmp[vv] = -128;
			}
			if(elt_sum_tmp[vv]>127)
				elt_sum_tmp[vv] = 127;

			// activation function selection
			if(act_ctrl==0)
				data_out.data[vv] = elt_sum_tmp[vv];
			else{
				//Scale Hard Tanh Begin; y = clip( alpha(x+beta) )
				act_beta_added_buf[vv] = elt_sum_tmp[vv] + (signed char)act_beta; //conv_final[ll] share the same fl with BETA.
				if(act_beta_added_buf[vv]>=127)
					act_beta_added[vv] = 127;
				else if(act_beta_added_buf[vv]<-128)
					act_beta_added[vv] = -128;
				else
					act_beta_added[vv] = MASK8B & act_beta_added_buf[vv];
				
				act_alpha_muled_buf[vv] = act_beta_added[vv] * (signed char)act_alpha;

				if(act_alpha_muled_buf[vv]>=0)
					act_alpha_muled_sign_exten[vv] = 0x00;
				else
					act_alpha_muled_sign_exten[vv] = ~(0xFFFF>>(out_frac+act_alpha_fl-act_out_fl-1)); // ">>" is logic shift, then perform sign extension manually

				act_alpha_muled[vv] = (act_alpha_muled_sign_exten[vv] | (act_alpha_muled_buf[vv]>>(out_frac+act_alpha_fl-act_out_fl-1))) + 0x01;

				if(act_alpha_muled[vv] < (short)(0xffff & act_n_thre))
					act_output[vv] = 0xff & (act_n_thre>>1);
				else if(act_alpha_muled[vv] > (short)(0xffff & act_p_thre))
					act_output[vv] = 0xff & (act_p_thre>>1);
				else
					act_output[vv] = 0xff & (act_alpha_muled[vv]>>1);
				// Scale Hard Tanh Finish
				data_out.data[vv] = act_output[vv];//Round towards zero
			}

			if(pool_on==3){
				sumAvg[vv] = data_out.data[vv] + avgPoolBuf[vv][ELT_PIPE_DEPTH-1];
				__attribute__((opencl_unroll_hint))
				for(uchar p=ELT_PIPE_DEPTH-1; p>0; p-- ){
					avgPoolBuf[vv][p] = avgPoolBuf[vv][p-1];
				}
				avgPoolBuf[vv][0] = sumAvg[vv];
			}

			// Binary Packing Begin
			bit_sel = 7 - ((gp_tmp + vv) & 0x07); //which bit of the byte to store.
			bin_buffer0_tmp[vv] = (unsigned char)((0x80^(data_out.data[vv]&0x80))>>(7-bit_sel));

		}
		bin_buffer0 = 0;
		__attribute__((opencl_unroll_hint))
		for(int ii=0; ii < 8; ii++)
			bin_buffer0 += bin_buffer0_tmp[ii];

		
		top_bin[top_bin_ptr] = bin_buffer0;
		
		//Pooling config, including avg_pool and stride_pool
		if(pool_on==0){
			top[j]=data_out;
		}
		else if(pool_on==2){//stride pool
			conv_itm_xyz = xyz_offset + xy_offset + conv_itm_x;
			if(conv_itm_xyz==j){
				top[outnum] = data_out;
				outnum++;
				conv_itm_x = conv_itm_x + stride;
				if(conv_itm_x>=conv_x){
					conv_itm_x = 0;
					xy_offset += stride * conv_x;
					if(xy_offset>=conv_xy){
						xy_offset = 0;
						xyz_offset += conv_xy;
					}
				}
			}
		}
		else if(pool_on==3){//avgpool
		//TODO: in the later version, the avgpool will be changed to fixpoint version.
			ptr++;
			if(ptr==AVGPOOL_SIZE)
			{
				ptr=0;
				__attribute__((opencl_unroll_hint))
				for(unsigned char vv=0; vv<VEC_SIZE; vv++){
					__attribute__((opencl_unroll_hint))
					for(unsigned i=0; i<ELT_PIPE_DEPTH; i++){
						avgPoolSum[vv] += avgPoolBuf[vv][i];
						avgPoolBuf[vv][i] = 0;
					}
					out = avgPoolSum[vv] * divisor + 0x800;//2048:2^(12-1)
					if(out>=0){
						out = out>>12;
					}
					else{
						out = 0xfff00000 | (out>>12);
					}
					//overflow,because of relu no <0 value here
					if(out>127)
						out=127;
					if(out<-128)
						out=-128;
					data_out.data[vv]=0xff & out;//	Round towards zero
					avgPoolSum[vv]=0;
				}
				top[outnum]=data_out;
				outnum++;
			}
		}// end of pooling

		if((fix_bin_y_idx==conv_y-1) && (fix_bin_x_idx==conv_x-1)){
			fix_gpz_idx++;
			fix_bin_y_idx = 0;
		}
		else if(fix_bin_x_idx==conv_x-1)
			fix_bin_y_idx++;

		if(fix_bin_x_idx==conv_x-1)
			fix_bin_x_idx = 0;
		else
			fix_bin_x_idx++;

	}
	//printf("Kernel eltwise lanched !!!\n");
}
#endif
