//////////////////////////////////////////
//
// OpenCL host program template for multiple
// FPGA boards.
//
// Created by dongwang@2016.01.10
//
/////////////////////////////////////////
#define CL_TARGET_OPENCL_VERSION 120

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <iostream>
#include <iomanip>
#include <fstream>

#include <CL/opencl.h>

// user defined library
#include "ocl_util.h"
#include "timer.h"

// CNN network configuration file
#include "../device/hw_param.cl"
#include "layer_config.h"

#ifdef USE_OPENCV
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/core/core.hpp>
using namespace cv;
#endif

using namespace std;
using namespace ocl_util;

typedef  signed char  DTYPE;
typedef  float FTYPE;

#ifdef XILINX
//#define USE_SDX_1DDR  // reserved for v7-690T device, DO NOT USE
//#define USE_SDX_4DDR  // reserved for v7-690T device, DO NOT USE
#endif

//----------- Design Parameters --------------//
// select what platform is used
#ifdef XILINX
const char *vendor_name = "Xilinx";
#else
//----------- SDK version <= 19.1 -----------//
// const char *vendor_name = "Intel";
//----------- SDK version >= 19.3 ----------//
#if defined(SW_EMU)
const char *vendor_name = "Intel(R) FPGA Emulation Platform for OpenCL(TM)";
#else
const char *vendor_name = "Intel(R) FPGA SDK for OpenCL(TM)";
#endif
#endif
#define DEVICE_TYPE CL_DEVICE_TYPE_ACCELERATOR

// SW System parameters
#define DMA_ALIGNMENT   64
#define MAX_LAYER_NUM   54
#define MAX_BATCH_SIZE  16

#define IN_BUF_SIZE    256*256*128  // Note: the buffer size should be large enough to hold all temperary results
#define OUT_BUF_SIZE   256*256*128
#define POOL_BUF_SIZE  256*256*128
#define FC_BUF_SIZE    32768*MAX_BATCH_SIZE

#define MEAN_DATA_WIDTH   256
#define MEAN_DATA_HEIGHT  256
#define MEAN_DATA_CHANNEl 3
#define PICTURE_NUM 8000
#define MAX_PIC_NUM 50000
const char *mean_data_file_path    = "./data/imagenet/mean_data.dat";
const char *synset_word_file_path  = "./data/imagenet/synset_words.txt";
const char *LabelPath              = "./data/imagenet/val.txt";
char  picture_file_path_head[100]  = "/home/dwang/Work/imagenet/ilsvrc2012/ILSVRC2012_img_val/ILSVRC2012_val_";
char  picture_file_path[100];
int   label[MAX_PIC_NUM]={0};
char  label_buf[MAX_PIC_NUM][1024]={0};
char  synset_buf[1000][1024]={0};
DTYPE searchTop[1024];
float accuracy1=0;
float accuracy5=0;

#ifdef TEST
// TEST
// Original problem size
// File size is in num of DTYPE numbers
#define IMAGE_FILE_SIZE		(224*224*3)
#define WEIGHTS_FILE_SIZE	11697024
#define SCALES_FILE_SIZE	3840
#define LAYER_NUM         	21 //21 
#define CONV_NUM         	20 
#define LAYER_START             1  //start form 1
#define LAYER_END               21 
const char *weight_file_path = "./data/IRModel/weights.dat";
const char *scales_file_path = "./data/IRModel/scales.dat";
const char *input_file_path  = "./data/IRModel/image.dat";
const char *dump_file_path   = "./result_dump.txt";
const char *fc1024_bias_path = "./data/IRModel/fc1024bias.dat";
const char *ref_file_path    = "./data/IRModel/fc1024_bias_summed.dat";

#endif
char fc1024_bias[1024];

#ifdef TEST
	#define MAXPOOL_PAD 1
#else
	#define MAXPOOL_PAD 0
#endif

int act_ptr = 1;


// Configuration file instructions
enum config_item{
layer_type, // "0" -> conv, "1" -> fc, "2" -> binary conv

data_w, data_h, data_n, weight_w, weight_h, weight_n, weight_m, bias_size, //memRd Parameters

memrd_src, //"0"-> data_buf  "1"-> output_buf  "2"->"fc_1_buffer"  "3"->"fc_2_buffer"  "4"->"pool_buffer"  "5"->"eltwise_buf"(resnet)  "6"->"output_buf_bin"

conv_x, conv_y, conv_z, conv_stride, conv_padding, conv_split, conv_relu, //Conv Parameters

pool_on, pool_x, pool_y, pool_z, pool_size, pool_stride, // Pooling Parameters

lrn_on,// lrn on/off control

memwr_dst//"0"-> data_buf  "1"-> output_buf  "2"->"fc_1_buffer"  "3"->"fc_2_buffer"  ("4"->"pool_buffer")  "5"->"eltwise_buf"  "6"->"output_buf_bin"

};

enum input_item{

image_w, image_h, image_n, // original image size

batch_size

};

enum output_item{

output_w, output_h, output_n

};

enum precision_item{

frac_w, frac_din, frac_dout

};
#ifdef LAYER_ELT
enum eltwise_item{
eltwise_on, eltwise_in_1, eltwise_in_2, eltwise_out, eltwise_act
};
enum addition_precision_item{
bn_dout,elt_din1,elt_din2,elt_dout
};
#endif


// Define the kernel names used
const char *knl_name_memRd = "memRead";
const char *knl_name_conv  = "coreConv";
const char *knl_name_Pool  = "maxPool";
const char *knl_name_memWr = "memWrite";
const char *knl_name_lrn   = "lrn";
#ifdef LAYER_ELT
const char *knl_name_elt   = "eltwise";
#endif
// binary
const char *knl_name_conv_bin  = "coreConvBin";

//------------ Global Functions & Variables ------------//
cl_uint num_devices = 0;
cl_platform_id platform_id = NULL;
cl_context context = NULL;
cl_program program = NULL;
scoped_array<cl_device_id> device;
scoped_array<cl_kernel> knl_memRd;
scoped_array<cl_kernel> knl_conv;
scoped_array<cl_kernel> knl_memWr;
scoped_array<cl_kernel> knl_pool;
scoped_array<cl_command_queue> que_memRd;
scoped_array<cl_command_queue> que_conv;
scoped_array<cl_command_queue> que_memWr;
scoped_array<cl_command_queue> que_pool;
scoped_array<cl_command_queue> que_lrn;
scoped_array<cl_mem> data_buf;
scoped_array<cl_mem> output_buf;
scoped_array<cl_mem> pool_buf;
scoped_array<cl_mem> weights_buf;
scoped_array<cl_mem> bias_buf;
scoped_array<cl_mem> fc_1_buf;
scoped_array<cl_mem> fc_2_buf;

#ifdef LAYER_ELT
scoped_array<cl_kernel> knl_elt;
scoped_array<cl_mem>    eltwise_buf;
#endif

//binary output
scoped_array<cl_kernel> knl_conv_bin;
scoped_array<cl_command_queue> que_conv_bin;
scoped_array<cl_mem> output_buf_bin;
scoped_array<cl_mem> pool_buf_bin;
scoped_array<cl_mem> scale_buf_bin;

#ifdef XILINX
	scoped_array<cl_event> write_event(num_devices);
#else
//DO NOTHING
#endif

DTYPE *weights;
DTYPE *image;
DTYPE *data_init;
DTYPE *weight_conv[MAX_LAYER_NUM];
DTYPE *bias_conv[MAX_LAYER_NUM];
DTYPE *output;
DTYPE *output_one_item;
DTYPE *output_reorder;
DTYPE *golden_ref;

// binary data
DTYPE *scales;
DTYPE *scale_conv_bin[MAX_LAYER_NUM];
DTYPE *output_bin;
DTYPE *output_reorder_bin;

unsigned layer_config_original[LAYER_NUM][NUM_CONFIG_ITEM];

#ifdef USE_OPENCV
int  load_picture(DTYPE *image);
void getAccuracy(DTYPE *output_reorder,int num);
void labelNum();
void numtochar(int num,char *end);
#endif

void loadImageToBuffer(int num);
int  prepare();
void readDataBack();
void verifyResult(int num);
void dumpResult();
void reorderWeights(DTYPE *weights, DTYPE *weight_buf, unsigned dim1, unsigned dim2, unsigned dim3, unsigned dim4, unsigned dim3_original, unsigned dim4_original, unsigned offset, unsigned padding_offset, unsigned vecSize, unsigned laneNum);
void reorderBias(DTYPE *dataIn, DTYPE *bias, unsigned offset, unsigned padding_offset, unsigned dim4, unsigned dim4_original, unsigned laneNum);
void reorderOutput(DTYPE *output, DTYPE *output_reorder, unsigned dim1, unsigned dim2, unsigned dim3);
void extractOutput(DTYPE *output, DTYPE *output_one_item, unsigned item_num, unsigned batch_size, unsigned dim1, unsigned dim2, unsigned dim3);
void softmax(DTYPE *output_reorder , DTYPE *output);
int getProb(DTYPE *output);
void cleanup();

//binary
void reorderBinaryOutput(DTYPE *output_bin, DTYPE *output_reorder, unsigned dim1, unsigned dim2, unsigned dim3);
void reorderBinaryWeights(DTYPE *weights, DTYPE *weight_buf, unsigned dim1, unsigned dim2, unsigned dim3, unsigned dim4, unsigned dim3_original, unsigned dim4_original, unsigned offset, unsigned padding_offset, unsigned vecSize, unsigned laneNum);

int main(int argc, char** argv)
{
	cl_int status;
    unsigned int device_ptr;

	unsigned int conv_output_num;
	unsigned int conv_loop_cnt;
	unsigned int conv_control;
	unsigned short conv_xy;
	unsigned short pool_dim1x2;
	unsigned char batch_size_in_dim;
	unsigned char batch_indx_dim1;
	unsigned char batch_indx_dim2;

	unsigned int weight_buf_size;

#ifndef MEM_WT_TASK
	size_t knl_memWr_global_size[3];
	size_t knl_memWr_local_size[3];
#endif

	Timer t;  // Timer used for performance measurement
	float time;

	if (argc != 2){
	printf("Error: wrong commad format, usage:\n");
	printf("%s <binaryfile>\n", argv[0]);
	return EXIT_FAILURE;
	}


	printf("***************************************************\n");
	printf("PipeCNN: An OpenCL-Based FPGA Accelerator for CNNs \n");
	printf("***************************************************\n");

	// Connect to the desired platform
	platform_id = findPlatform(vendor_name);
	if(platform_id == NULL) {
		printf("ERROR: Unable to find the desired OpenCL platform.\n");
		return false;
	}

    // Query the available OpenCL device
    device.reset(getDevices(platform_id, DEVICE_TYPE, &num_devices));
    printf("\nPlatform: %s\n", getPlatformName(platform_id).c_str());
    printf("Totally %d device(s) are found\n", num_devices);
#if defined(SW_EMU)
    device_ptr = 0; // only use one device, select the proper idx
#else
    device_ptr = 0; // only use one device, select the proper idx,0:de5a_net;1:a10_ref
#endif
    num_devices = 1; // reset the num of device to 1
    //for(unsigned device_ptr = 0; device_ptr < num_devices; ++device_ptr) {
        printf("  Using Device %d: %s\n", device_ptr, getDeviceName(device[device_ptr]).c_str());
        displayDeviceInfo(device[device_ptr]);
    //}


	// Create the context.
    context = clCreateContext(NULL, num_devices, &device[device_ptr], NULL, NULL, &status);
	checkError(status, "Failed to create context");

	// Create Program Objects
	char *kernel_file_name=argv[1];

	#ifdef XILINX
		char *fpga_bin;
		size_t fpga_bin_size;
		fpga_bin_size = load_file_to_memory(kernel_file_name, &fpga_bin);
		program = clCreateProgramWithBinary(context, 1, &device[device_ptr], &fpga_bin_size, (const unsigned char **) &fpga_bin, NULL, &status);
	#else
		// Create the program for all device. All devices execute the same kernel.
		program = createProgramFromFile(context, (const char *) kernel_file_name, &device[device_ptr], num_devices);
	#endif

	checkError(status, "Failed to create context");
	
	// Create per-device objects.
	que_memRd.reset(num_devices);
	que_conv.reset(num_devices);
	que_memWr.reset(num_devices);
	que_pool.reset(num_devices);
	que_lrn.reset(num_devices);
	knl_memRd.reset(num_devices);
	knl_conv.reset(num_devices);
	knl_memWr.reset(num_devices);
	knl_pool.reset(num_devices);
	// For each layer a group of buffers are created to store the weights and bias
	weights_buf.reset(num_devices*LAYER_NUM);
	bias_buf.reset(num_devices*LAYER_NUM);
	// Two buffers (data and output) are used as ping-pong buffers for conv layers
	data_buf.reset(num_devices*MAX_BATCH_SIZE);
	output_buf.reset(num_devices*MAX_BATCH_SIZE);
	pool_buf.reset(num_devices*MAX_BATCH_SIZE);
	// Two buffers are used as ping-pong buffers for fc layers
	fc_1_buf.reset(num_devices);
	fc_2_buf.reset(num_devices);

#ifdef LAYER_ELT
knl_elt.reset(num_devices);
eltwise_buf.reset(num_devices*MAX_BATCH_SIZE);
#endif


// binary 
	que_conv_bin.reset(num_devices);
	knl_conv_bin.reset(num_devices);
	output_buf_bin.reset(num_devices*MAX_BATCH_SIZE);
	pool_buf_bin.reset(num_devices*MAX_BATCH_SIZE);
	scale_buf_bin.reset(num_devices*LAYER_NUM);

	// Prepare compute data
	status = prepare();
	if(status == 1){
		printf("Allocate memory for data and weights failed !!!\n");
		return false;
	}

	// Create qeues, kernels and mem objs
	for(unsigned i = 0; i < num_devices; ++i) {
		// Command queue
		que_memRd[i] = clCreateCommandQueue(context, device[device_ptr], CL_QUEUE_PROFILING_ENABLE, &status);
		checkError(status, "Failed to create command queue 0");
		que_conv[i] = clCreateCommandQueue(context, device[device_ptr], CL_QUEUE_PROFILING_ENABLE, &status);
		checkError(status, "Failed to create command queue 1");
		que_memWr[i] = clCreateCommandQueue(context, device[device_ptr], CL_QUEUE_PROFILING_ENABLE, &status);
		checkError(status, "Failed to create command queue 2");
		que_pool[i] = clCreateCommandQueue(context, device[device_ptr], CL_QUEUE_PROFILING_ENABLE, &status);
		checkError(status, "Failed to create command queue 3");

		// Kernel
		knl_memRd[i] = clCreateKernel(program, knl_name_memRd, &status);
		checkError(status, "Failed to create memRd kernel");

		knl_conv[i] = clCreateKernel(program, knl_name_conv, &status);
		checkError(status, "Failed to create conv kernel");

		knl_pool[i] = clCreateKernel(program, knl_name_Pool, &status);
		checkError(status, "Failed to create pooling kernel");

		knl_memWr[i] = clCreateKernel(program, knl_name_memWr, &status);
		checkError(status, "Failed to create memWr kernel");

		// Binary
		que_conv_bin[i] = clCreateCommandQueue(context, device[device_ptr], CL_QUEUE_PROFILING_ENABLE, &status);
		checkError(status, "Failed to create command queue 1");
		knl_conv_bin[i] = clCreateKernel(program, knl_name_conv_bin, &status);
		checkError(status, "Failed to create conv kernel");

#ifdef LAYER_ELT
		knl_elt[i] = clCreateKernel(program, knl_name_elt, &status);
		checkError(status, "Failed to create elt kernel");
#endif

		// Mems
		// Create weight and bias buffers for each layer
		for(unsigned j = 0; j < LAYER_NUM; ++j){
			
			if(layer_config[j][layer_type]==2){
				// binary convolution
				weight_buf_size = (layer_config[j][weight_w]*layer_config[j][weight_h]*layer_config[j][weight_n]/8*layer_config[j][weight_m]);
			}else{
				weight_buf_size = (layer_config[j][weight_w]*layer_config[j][weight_h]*layer_config[j][weight_n]*layer_config[j][weight_m]);
			}

			// Weights buffers for each layer
			weights_buf[i*LAYER_NUM+j] = clCreateBuffer(context, CL_MEM_READ_ONLY, weight_buf_size* sizeof(DTYPE), NULL, &status);
			checkError(status, "Failed to create buffer for weights in layer");

			// Bias buffers for each layer
			bias_buf[i*LAYER_NUM+j] = clCreateBuffer(context, CL_MEM_READ_ONLY, layer_config[j][bias_size] * sizeof(DTYPE), NULL, &status);
			checkError(status, "Failed to create buffer for bias in layer");

			//binary scaling factor for each layer
			scale_buf_bin[i*LAYER_NUM+j] = clCreateBuffer(context, CL_MEM_READ_ONLY, layer_config[j][bias_size] * sizeof(DTYPE), NULL, &status);
			checkError(status, "Failed to create buffer for bias in layer");

			// Initializing all weights buffers, blocking write is used
			status = clEnqueueWriteBuffer(que_memRd[i], weights_buf[i*LAYER_NUM+j], CL_TRUE, 0, weight_buf_size*sizeof(DTYPE), weight_conv[j], 0, NULL, NULL);
			checkError(status, "Failed to transfer weight");

			status = clEnqueueWriteBuffer(que_memRd[i], bias_buf[i*LAYER_NUM+j], CL_TRUE, 0, layer_config[j][bias_size] * sizeof(DTYPE), bias_conv[j], 0, NULL, NULL);
			checkError(status, "Failed to transfer bias");

			status = clEnqueueWriteBuffer(que_memRd[i], scale_buf_bin[i*LAYER_NUM+j], CL_TRUE, 0, layer_config[j][bias_size] * sizeof(DTYPE), scale_conv_bin[j], 0, NULL, NULL);
			checkError(status, "Failed to transfer scale");
		}
		
		// Wait for all data are send to FPGA
		clFinish(que_memRd[0]);

		// Create data buffers for each batch item
		for(unsigned j = 0; j < input_config[batch_size]; ++j){
			// Input data buffers
			data_buf[i*input_config[batch_size]+j] = clCreateBuffer(context, CL_MEM_READ_WRITE, IN_BUF_SIZE * sizeof(DTYPE), NULL, &status);
			checkError(status, "Failed to create buffer for data in layer");

			// Output results buffers
			output_buf[i*input_config[batch_size]+j] = clCreateBuffer(context, CL_MEM_READ_WRITE, OUT_BUF_SIZE * sizeof(DTYPE), NULL, &status);
			checkError(status, "Failed to create buffer for output");
			
			// pool_buffer
			pool_buf[i*input_config[batch_size]+j] = clCreateBuffer(context, CL_MEM_READ_WRITE, POOL_BUF_SIZE * sizeof(DTYPE), NULL, &status);
			checkError(status, "Failed to create buffer for pool_buffer");
#ifdef LAYER_ELT
			eltwise_buf[i*input_config[batch_size]+j] = clCreateBuffer(context, CL_MEM_READ_WRITE, OUT_BUF_SIZE * sizeof(DTYPE), NULL, &status);
			checkError(status, "Failed to create buffer for eltwise_buf");
#endif

			// Binary Output results buffers
			output_buf_bin[i*input_config[batch_size]+j] = clCreateBuffer(context, CL_MEM_READ_WRITE, OUT_BUF_SIZE/8 * sizeof(DTYPE), NULL, &status);
			checkError(status, "Failed to create buffer for binary output");
			// pool_buffer
			pool_buf_bin[i*input_config[batch_size]+j] = clCreateBuffer(context, CL_MEM_READ_WRITE, POOL_BUF_SIZE/8 * sizeof(DTYPE), NULL, &status);
			checkError(status, "Failed to create buffer for pool_buffer");


			//// Load image data into buffers
			//status = clEnqueueWriteBuffer(que_memRd[i], data_buf[i*input_config[batch_size]+j], CL_TRUE,
			//	0, (layer_config[0][data_w]*layer_config[0][data_h]*layer_config[0][data_n]) * sizeof(DTYPE), data_init, 0, NULL, NULL);
			//checkError(status, "Failed to transfer input image");
		}
		// Allocate fc buffers
		fc_1_buf[i] = clCreateBuffer(context,  CL_MEM_READ_WRITE, FC_BUF_SIZE * sizeof(DTYPE), NULL, &status);
		checkError(status, "Failed to create buffer for data in fc layer");

		fc_2_buf[i] = clCreateBuffer(context,  CL_MEM_READ_WRITE, FC_BUF_SIZE * sizeof(DTYPE), NULL, &status);
		checkError(status, "Failed to create buffer for data in fc layer");
	}

	// Execute the kernel
	scoped_array<cl_event> memRd_event(num_devices);
	scoped_array<cl_event> conv_event(num_devices);
	scoped_array<cl_event> pool_event(num_devices);
	scoped_array<cl_event> memWr_event(num_devices);
	scoped_array<cl_event> lrn_event(num_devices);
#ifdef LAYER_ELT
	scoped_array<cl_event> elt_event(num_devices);
#endif

	// Recorde the excution time of each operation for each layer
#ifndef USE_OPENCV
	cl_ulong memWr_time[LAYER_NUM];
	cl_ulong conv_time[LAYER_NUM];
	cl_ulong pool_time[LAYER_NUM];
	cl_ulong memRd_time[LAYER_NUM];
	cl_ulong lrn_time[LAYER_NUM];
#ifdef LAYER_ELT
	cl_ulong elt_time[LAYER_NUM];
#endif
#endif

	unsigned       iter_num;
	unsigned short out_dim1xbatch;
	unsigned int   out_dim1x2xbatch;
	//unsigned int   write_bound;
	unsigned char  padding_offset;
	unsigned char  pool_conv_x;
	unsigned char  pool_conv_y;
	unsigned short pool_group;
	unsigned char  pool_win_num_x;
	unsigned char  pool_win_size_x;
	unsigned short pool_item_loop_bound;
	unsigned short pool_load_data_bound;
	unsigned short pool_write_back_bound;
	unsigned short pool_y_bound;
	unsigned short pool_times;
	unsigned char  conv_group_num_dim1, conv_group_num_dim2;
	unsigned char  conv_win_size_dim1, conv_win_size_dim2;
	unsigned int   conv_win_size_dim1x2x3;
	unsigned char  conv_group_rem_dim1, conv_group_rem_dim2;
	unsigned int   conv_group_rem_dim1x2x3;
	unsigned short data_dim1x2;
	unsigned char  weight_dim1x2;
	unsigned int   weight_dim1x2x3;
	unsigned short weight_dim4_div_LaneNum;
	unsigned int   pic_num =1;
	
	//Binary
	unsigned char  binary = 0;
	unsigned short  weight_dim3 = 0;

	// Kernel excutions main loops
	for(unsigned i = 0; i < num_devices; ++i) {


#ifdef USE_OPENCV
		// Run PipeCNN for multiple input pictures
		for (pic_num = 1; pic_num <=PICTURE_NUM; ++pic_num){
#endif
		loadImageToBuffer(pic_num);

		// Recorde the start time
		t.start();

		// Each iteration excutes one layer convolution
		// MemRd -> Conv(Relu) -> (MaxPool) -> MemWr -> (Lrn)
		for(unsigned char j = LAYER_START-1; j < LAYER_END; ++j){

#ifndef USE_OPENCV
			memWr_time[j] =0;
			conv_time[j]  =0;
			pool_time[j]  =0;
			memRd_time[j] =0;
			lrn_time[j]   =0;
#ifdef LAYER_ELT
			elt_time[j]	  =0;
#endif
#endif

			if(j<CONV_NUM)
				iter_num = input_config[batch_size]; // for conv layers, process by batch_size time
			else
				iter_num = 1; // for FC layers, process only one time

			// Each iteration process one item in batch
			for(unsigned k = 0; k < iter_num; ++k){
			// Set Arguments
			// Set knl_memRd arguments.
			unsigned argi = 0;

			// Convolution tasks (conv_x,conv_y) are divided into multiple groups
			// each group process CONV_GP_SIZE_X*CONV_GP_SIZE_Y convolutions in parallel
			conv_group_num_dim1   = ceil((float)layer_config[j][conv_x]/CONV_GP_SIZE_X);
			conv_group_num_dim2   = ceil((float)layer_config[j][conv_y]/CONV_GP_SIZE_Y);
			if(layer_config[j][conv_x]==1){ // when only one group for FC layer
				conv_win_size_dim1  = layer_config[j][weight_w];
				conv_group_rem_dim1   = layer_config[j][weight_w];
			}
			else{
				conv_win_size_dim1  = layer_config[j][weight_w]+(CONV_GP_SIZE_X-1)*layer_config[j][conv_stride];
				// actual number of input pixels need to be read in the last group according to the number of the remaining valid conv items in the last group
				// the remaining valid conv items is layer_config[j][conv_x]%CONV_GP_SIZE_X
				if(layer_config[j][conv_x]%CONV_GP_SIZE_X==0)
					conv_group_rem_dim1   = CONV_GP_SIZE_X*layer_config[j][weight_w];
				else
					conv_group_rem_dim1   = layer_config[j][conv_x]%CONV_GP_SIZE_X*layer_config[j][weight_w];
			}
			// In this version, grouping is not performed in the column (y) direction, i.e., CONV_GP_SIZE_Y=1
			conv_win_size_dim2    = layer_config[j][weight_h];
			conv_group_rem_dim2   = layer_config[j][weight_h];

			weight_dim4_div_LaneNum = layer_config[j][weight_m]/LANE_NUM;
			data_dim1x2 = layer_config[j][data_w]*layer_config[j][data_h];
			weight_dim1x2 = layer_config[j][weight_w]*layer_config[j][weight_h];
			
			//binary
			if(layer_config[j][layer_type]==2){
				binary = 1;
				conv_win_size_dim1x2x3  = conv_win_size_dim1*conv_win_size_dim2*layer_config[j][weight_n]/8;
				conv_group_rem_dim1x2x3 = conv_group_rem_dim1*conv_group_rem_dim2*layer_config[j][weight_n]/8;
				weight_dim1x2x3 = layer_config[j][weight_w]*layer_config[j][weight_h]*layer_config[j][weight_n]/8;
				weight_dim3 = layer_config[j][weight_n]/8;
			}else{
				binary = 0;
				conv_win_size_dim1x2x3  = conv_win_size_dim1*conv_win_size_dim2*layer_config[j][weight_n];
				conv_group_rem_dim1x2x3 = conv_group_rem_dim1*conv_group_rem_dim2*layer_config[j][weight_n];
				weight_dim1x2x3 = layer_config[j][weight_w]*layer_config[j][weight_h]*layer_config[j][weight_n];
				weight_dim3 = layer_config[j][weight_n];
			}

			status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_uchar), &binary);
			checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);

			status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_uchar), &layer_config[j][data_w]);
			checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);

			status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_uchar), &layer_config[j][data_h]);
			checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);

			status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_ushort), &data_dim1x2);
			checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);

			status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_uchar), &layer_config[j][weight_w]);
			checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);

			status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_uchar), &layer_config[j][weight_h]);
			checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);

			status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_ushort), &weight_dim3);
			checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);

			status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_ushort), &weight_dim4_div_LaneNum);
			checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);

			status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_uchar), &weight_dim1x2);
			checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);

			status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_uint),  &weight_dim1x2x3);
			checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);

			status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_uchar), &layer_config[j][conv_x]);
			checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);

			//status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_uchar), &layer_config[j][conv_y]);
			//checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);

			status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_uchar), &layer_config[j][conv_stride]);
			checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);

			status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_uchar), &layer_config[j][conv_padding]);
			checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);

			status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_uchar), &layer_config[j][conv_split]);
			checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);

			status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_uchar), &conv_group_num_dim1);
			checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);

			status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_uchar), &conv_group_num_dim2);
			checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);

			status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_uchar), &conv_group_rem_dim1);
			checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);

			//status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_uchar), &conv_group_rem_dim2);
			//checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);

			status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_uint), &conv_group_rem_dim1x2x3);
			checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);

			status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_uchar), &conv_win_size_dim1);
			checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);

			status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_uchar), &conv_win_size_dim2);
			checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);

			status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_uint), &conv_win_size_dim1x2x3);
			checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);
			// Select the kernel input mem object source
			// data_buf -> conv1 -> output_buf -> lrn1 -> data_buf -> conv2 -> output_buf -> lrn2 -> data_buf
			// -> conv3 -> output_buf -> conv4 -> output_buf -> ...
			if(layer_config[j][memrd_src]==0){
				status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_mem), &data_buf[i*input_config[batch_size]+k]);
				checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);
			}
			else if(layer_config[j][memrd_src]==1)
			{
				status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_mem), &output_buf[i*input_config[batch_size]+k]);
				checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);
			}
			else if(layer_config[j][memrd_src]==2)
			{
				status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_mem), &fc_1_buf[i]);
				checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);
			}
			else if(layer_config[j][memrd_src]==3)
			{
				status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_mem), &fc_2_buf[i]);
				checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);
			}
			else if(layer_config[j][memrd_src]==4)
			{
				status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_mem), &pool_buf[i*input_config[batch_size]+k]);
				checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);
			}
			#ifdef LAYER_ELT
			else if(layer_config[j][memrd_src]==5)
			{
				status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_mem), &eltwise_buf[i*input_config[batch_size]+k]);
				checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);
			}
			#endif
			// binary
			else if(layer_config[j][memrd_src]==6)
			{
				status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_mem), &output_buf_bin[i*input_config[batch_size]+k]);
				checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);
			}
			else if(layer_config[j][memrd_src]==7)
			{
				status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_mem), &pool_buf_bin[i*input_config[batch_size]+k]);
				checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);
			}


			status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_mem), &weights_buf[i*LAYER_NUM+j]);
			checkError(status, "Failed to set argument %d kernel memRd", argi - 1);

			status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_mem), &bias_buf[i*LAYER_NUM+j]);
			checkError(status, "Failed to set argument %d kernel memRd", argi - 1);

			status = clSetKernelArg(knl_memRd[i], argi++, sizeof(cl_mem), &scale_buf_bin[i*LAYER_NUM+j]);
			checkError(status, "Failed to set argument %d kernel memRd", argi - 1);

			//  Set FIX POINT knl_conv arguments.
			if(layer_config[j][layer_type]!=2){
				argi = 0;

				conv_loop_cnt = layer_config[j][weight_w]*layer_config[j][weight_h]*layer_config[j][weight_n]/VEC_SIZE;
				conv_output_num = layer_config[j][conv_x]*layer_config[j][conv_y]*layer_config[j][weight_m]/LANE_NUM; // new weight_m is divisible by LANE_NUM
				conv_control = (layer_config[j][conv_relu]&0x01)|(((~layer_config[j][pool_on])&0x01)<<1);

				status = clSetKernelArg(knl_conv[i], argi++, sizeof(cl_uint), &conv_output_num);
				checkError(status, "Failed to set argument %d of kernel conv", argi - 1);

				status = clSetKernelArg(knl_conv[i], argi++, sizeof(cl_uint), &conv_loop_cnt);
				checkError(status, "Failed to set argument %d of kernel conv", argi - 1);

				status = clSetKernelArg(knl_conv[i], argi++, sizeof(cl_uint), &conv_control);
				checkError(status, "Failed to set argument %d of kernel conv", argi - 1);

				status = clSetKernelArg(knl_conv[i], argi++, sizeof(cl_char), &precision_config[j][frac_w]);
				checkError(status, "Failed to set argument %d of kernel conv", argi - 1);

				status = clSetKernelArg(knl_conv[i], argi++, sizeof(cl_char), &precision_config[j][frac_din]);
				checkError(status, "Failed to set argument %d of kernel conv", argi - 1);

				status = clSetKernelArg(knl_conv[i], argi++, sizeof(cl_char), &precision_config[j][frac_dout]);
				checkError(status, "Failed to set argument %d of kernel conv", argi - 1);
			}

			//  Binary: Set knl_conv_bin arguments.
			if(layer_config[j][layer_type]==2){
				argi = 0;

				conv_loop_cnt = layer_config[j][weight_w]*layer_config[j][weight_h]*layer_config[j][weight_n]/VEC_SIZE/8;
				conv_output_num = layer_config[j][conv_x]*layer_config[j][conv_y]*layer_config[j][weight_m]/LANE_NUM; // new weight_m is divisible by LANE_NUM
				
				status = clSetKernelArg(knl_conv_bin[i], argi++, sizeof(cl_uint), &conv_output_num);
				checkError(status, "Failed to set argument %d of kernel conv", argi - 1);

				status = clSetKernelArg(knl_conv_bin[i], argi++, sizeof(cl_uint), &conv_loop_cnt);
				checkError(status, "Failed to set argument %d of kernel conv", argi - 1);

				status = clSetKernelArg(knl_conv_bin[i], argi++, sizeof(cl_uint), &conv_control);
				checkError(status, "Failed to set argument %d of kernel conv", argi - 1);

				status = clSetKernelArg(knl_conv_bin[i], argi++, sizeof(cl_char), &precision_config[j][frac_w]);// frac_scale
				checkError(status, "Failed to set argument %d of kernel conv", argi - 1);

				status = clSetKernelArg(knl_conv_bin[i], argi++, sizeof(cl_char), &precision_config[j][frac_din]);
				checkError(status, "Failed to set argument %d of kernel conv", argi - 1);

				status = clSetKernelArg(knl_conv_bin[i], argi++, sizeof(cl_char), &precision_config[j][frac_dout]);
				checkError(status, "Failed to set argument %d of kernel conv", argi - 1);
			}

			//  Set knl_pool arguments.
			if(layer_config[j][pool_on]==1){
				argi = 0;
				pool_conv_x = layer_config[j][conv_x] + MAXPOOL_PAD;
				pool_conv_y = layer_config[j][conv_y] + MAXPOOL_PAD;
				pool_dim1x2 = layer_config[j][pool_x]*layer_config[j][pool_y];
				conv_xy = pool_conv_x*pool_conv_y;
				padding_offset = (layer_config[j][weight_m]-layer_config_original[j][weight_m])/2;
				pool_group = layer_config[j][weight_m] / LANE_NUM;
				pool_win_num_x   = ceil ((float)layer_config[j][pool_x] / POOL_GP_SIZE_X);
				pool_win_size_x =  layer_config[j][pool_size]+(POOL_GP_SIZE_X-1)*layer_config[j][pool_stride];
				pool_load_data_bound = pool_win_size_x*layer_config[j][pool_size];
				pool_write_back_bound = POOL_GP_SIZE_X*LANE_NUM;
				pool_item_loop_bound = pool_load_data_bound > pool_write_back_bound ? pool_load_data_bound : pool_write_back_bound;
				pool_y_bound = (pool_win_num_x+1)*pool_item_loop_bound;
				pool_times = pool_group*layer_config[j][pool_y];
				status = clSetKernelArg(knl_pool[i], argi++, sizeof(cl_uchar), &pool_conv_x); //conv_x
				checkError(status, "Failed to set argument %d of kernel pool", argi - 1);

				status = clSetKernelArg(knl_pool[i], argi++, sizeof(cl_ushort), &conv_xy); //conv_xy
				checkError(status, "Failed to set argument %d of kernel pool", argi - 1);

				status = clSetKernelArg(knl_pool[i], argi++, sizeof(cl_uchar), &layer_config[j][pool_x]); // pool_dim1
				checkError(status, "Failed to set argument %d of kernel pool", argi - 1);

				status = clSetKernelArg(knl_pool[i], argi++, sizeof(cl_ushort), &layer_config[j][pool_z]); // pool_dim3
				checkError(status, "Failed to set argument %d of kernel pool", argi - 1);

				status = clSetKernelArg(knl_pool[i], argi++, sizeof(cl_ushort), &pool_dim1x2); // pool_dim1x2
				checkError(status, "Failed to set argument %d of kernel pool", argi - 1);

				status = clSetKernelArg(knl_pool[i], argi++, sizeof(cl_uchar), &layer_config[j][pool_size]); //pool_size
				checkError(status, "Failed to set argument %d of kernel pool", argi - 1);

				status = clSetKernelArg(knl_pool[i], argi++, sizeof(cl_uchar), &layer_config[j][pool_stride]); //pool_stride
				checkError(status, "Failed to set argument %d of kernel pool", argi - 1);

				status = clSetKernelArg(knl_pool[i], argi++, sizeof(cl_uchar), &padding_offset); //padding_offset
				checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);

				status = clSetKernelArg(knl_pool[i], argi++, sizeof(cl_ushort), &pool_times); //pool_times
				checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);

				status = clSetKernelArg(knl_pool[i], argi++, sizeof(cl_ushort), &pool_group); //pool_group
				checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);

				status = clSetKernelArg(knl_pool[i], argi++, sizeof(cl_ushort), &pool_y_bound); //pool_y_bound
				checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);

				status = clSetKernelArg(knl_pool[i], argi++, sizeof(cl_ushort), &pool_item_loop_bound); //pool_item_loop_bound
				checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);

				status = clSetKernelArg(knl_pool[i], argi++, sizeof(cl_ushort), &pool_load_data_bound); //pool_load_data_bound
				checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);

				status = clSetKernelArg(knl_pool[i], argi++, sizeof(cl_ushort), &pool_write_back_bound); //pool_write_back_bound
				checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);

				status = clSetKernelArg(knl_pool[i], argi++, sizeof(cl_uchar), &pool_win_num_x); //pool_win_num_x
				checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);

				status = clSetKernelArg(knl_pool[i], argi++, sizeof(cl_uchar), &pool_win_size_x); //pool_win_size_x
				checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);

				if(layer_config[j][memwr_dst]==0){
					status = clSetKernelArg(knl_pool[i], argi++, sizeof(cl_mem), &data_buf[i*input_config[batch_size]+k]);
					checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);
				}
				else if(layer_config[j][memwr_dst]==1)
				{
					status = clSetKernelArg(knl_pool[i], argi++, sizeof(cl_mem), &output_buf[i*input_config[batch_size]+k]);
					checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);
				}
				else if(layer_config[j][memwr_dst]==2)
				{
					status = clSetKernelArg(knl_pool[i], argi++, sizeof(cl_mem), &fc_1_buf[i]);
					checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);
				}
				else // 3
				{
					status = clSetKernelArg(knl_pool[i], argi++, sizeof(cl_mem), &fc_2_buf[i]);
					checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);
				}

				status = clSetKernelArg(knl_pool[i], argi++, sizeof(cl_mem), &pool_buf[i*input_config[batch_size]+k]);//top
				checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);

				status = clSetKernelArg(knl_pool[i], argi++, sizeof(cl_mem), &pool_buf_bin[i*input_config[batch_size]+k]);//top_bin
				checkError(status, "Failed to set argument %d of kernel memRd", argi - 1);

			}

			//  Set knl_memWr arguments.
			argi = 0;
			unsigned char batch_size_in_dim_log;
			unsigned char mask = 0xff;
			unsigned char memWr_dim1, memWr_dim2;
			unsigned short memWr_dim3;

			if(layer_config[j][pool_on] == 1){ // for ResNet pool padding
				memWr_dim1 = layer_config[j][conv_x]+MAXPOOL_PAD;
				memWr_dim2 = layer_config[j][conv_y]+MAXPOOL_PAD;
		    }
		    else{
				memWr_dim1 = layer_config[j][conv_x];
				memWr_dim2 = layer_config[j][conv_y];
			}
			memWr_dim3 = layer_config[j][conv_z];

			if(layer_config[j][layer_type]==2){// if the input of the memwr is from bin conv
				binary = 1;
			}else{
				binary = 0;
			}
			status = clSetKernelArg(knl_memWr[i], argi++, sizeof(cl_uchar), &binary);
			checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);

			status = clSetKernelArg(knl_memWr[i], argi++, sizeof(cl_uchar), &memWr_dim1);
			checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);

			status = clSetKernelArg(knl_memWr[i], argi++, sizeof(cl_uchar), &memWr_dim2);
			checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);

			status = clSetKernelArg(knl_memWr[i], argi++, sizeof(cl_ushort), &memWr_dim3); // pool_z equals original weight_m
			checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);

			if(j==(CONV_NUM-1)){ // For last Conv Layer, combine all batch data into one fc buffer
				if(input_config[batch_size]==1){
					batch_size_in_dim = 1;
					batch_indx_dim1 = 0;
					batch_indx_dim2 = 0;
				}
				else{
					batch_size_in_dim = log(input_config[batch_size])/log(2);
					batch_size_in_dim_log = log(batch_size_in_dim)/log(2);
					batch_indx_dim1 = k&(~((mask>>batch_size_in_dim_log)<<batch_size_in_dim_log));
					batch_indx_dim2 = k>>batch_size_in_dim_log;
					//printf("k=%d (%d, %d)\n", k, batch_size_in_dim, batch_size_in_dim_log);
					//printf("batch_indx_dim1=%d\n", batch_indx_dim1);
					//printf("batch_indx_dim2=%d\n", batch_indx_dim2);
				}
			}
			else{ // Normal WR Operations
				batch_size_in_dim = 1;
				batch_indx_dim1 = 0;
				batch_indx_dim2 = 0;
			}

			out_dim1xbatch = memWr_dim1*batch_size_in_dim;
			out_dim1x2xbatch = memWr_dim1*memWr_dim2*batch_size_in_dim*batch_size_in_dim;
			padding_offset = (layer_config[j][weight_m]-layer_config_original[j][weight_m])/2;
#if defined(TEST)
			unsigned char bn_bypass = layer_config[j][layer_type];
			unsigned char poolPad=MAXPOOL_PAD;
#endif
			//write_bound = memWr_dim1*memWr_dim2*layer_config[j][weight_m];

			status = clSetKernelArg(knl_memWr[i], argi++, sizeof(cl_ushort), &out_dim1xbatch);
			checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);

			status = clSetKernelArg(knl_memWr[i], argi++, sizeof(cl_uint), &out_dim1x2xbatch);
			checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);

			status = clSetKernelArg(knl_memWr[i], argi++, sizeof(cl_uchar), &batch_indx_dim1);
			checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);

			status = clSetKernelArg(knl_memWr[i], argi++, sizeof(cl_uchar), &batch_indx_dim2);
			checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);
#if defined(TEST)
			status = clSetKernelArg(knl_memWr[i], argi++, sizeof(cl_uchar), &bn_bypass);
			checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);

			status = clSetKernelArg(knl_memWr[i], argi++, sizeof(cl_uchar), &poolPad);
			checkError(status, "Failed to set argument %d of kernel pool", argi - 1);
#endif
			//status = clSetKernelArg(knl_memWr[i], argi++, sizeof(cl_uint), &write_bound);
			//checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);

			status = clSetKernelArg(knl_memWr[i], argi++, sizeof(cl_uchar), &padding_offset);
			checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);

			status = clSetKernelArg(knl_memWr[i], argi++, sizeof(cl_uchar), &layer_config[j][pool_on]);
			checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);

			status = clSetKernelArg(knl_memWr[i], argi++, sizeof(cl_uchar), &layer_config[j][pool_size]);
			checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);

			status = clSetKernelArg(knl_memWr[i], argi++, sizeof(cl_uchar), &layer_config[j][pool_stride]);
			checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);

			// Select the kernel output mem object source
			if(layer_config[j][memwr_dst]==0){
				status = clSetKernelArg(knl_memWr[i], argi++, sizeof(cl_mem), &data_buf[i*input_config[batch_size]+k]);
				checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);
			}
			else if(layer_config[j][memwr_dst]==1)
			{
				status = clSetKernelArg(knl_memWr[i], argi++, sizeof(cl_mem), &output_buf[i*input_config[batch_size]+k]);
				checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);
			}
			else if(layer_config[j][memwr_dst]==2)
			{
				status = clSetKernelArg(knl_memWr[i], argi++, sizeof(cl_mem), &fc_1_buf[i]);
				checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);
			}
			else if(layer_config[j][memwr_dst]==3)// 3
			{
				status = clSetKernelArg(knl_memWr[i], argi++, sizeof(cl_mem), &fc_2_buf[i]);
				checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);
			}
#ifdef LAYER_ELT
			else // 5
			{
				status = clSetKernelArg(knl_memWr[i], argi++, sizeof(cl_mem), &eltwise_buf[i*input_config[batch_size]+k]);
				checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);
			}
#endif
			// // binary output feature map
			status = clSetKernelArg(knl_memWr[i], argi++, sizeof(cl_mem), &output_buf_bin[i*input_config[batch_size]+k]);
			checkError(status, "Failed to set argument %d of kernel memWr", argi - 1);

#ifdef LAYER_ELT
			//Set knl_elt arguments
			if(eltwise_config[j][eltwise_on])
			{
				argi = 0;
				unsigned int  elt_input_num;
				unsigned char elt_pool_on;
				unsigned int  conv_xy;
				float divisor;	  //1/pool_size^2
				elt_pool_on=layer_config[j][pool_on];
				if(elt_pool_on==3){//avgPool
					divisor=1.0/(layer_config[j][pool_size]*layer_config[j][pool_size]);
				}
				else{
					divisor=0;
				}
				elt_input_num=layer_config[j][conv_x]*layer_config[j][conv_y]*layer_config[j][conv_z]/VEC_SIZE;
				conv_xy=layer_config[j][conv_x]*layer_config[j][conv_y];

				status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_uchar), &eltwise_config[j][eltwise_act]);
				checkError(status, "Failed to set argument %d of kernel elt", argi - 1);

				status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_uint), &elt_input_num);
				checkError(status, "Failed to set argument %d of kernel elt", argi - 1);

				status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_uchar), &elt_pool_on);
				checkError(status, "Failed to set argument %d of kernel elt", argi - 1);

				// status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_uchar), &avgpoolSize);
				// checkError(status, "Failed to set argument %d of kernel elt", argi - 1);

				status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_uchar), &layer_config[j][conv_x]);
				checkError(status, "Failed to set argument %d of kernel elt", argi - 1);

				status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_uchar), &layer_config[j][conv_y]);
				checkError(status, "Failed to set argument %d of kernel elt", argi - 1);

				status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_uint), &conv_xy);
				checkError(status, "Failed to set argument %d of kernel elt", argi - 1);
				// status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_uchar), &layer_config[j][conv_y]);
				// checkError(status, "Failed to set argument %d of kernel elt", argi - 1);

				status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_uchar), &layer_config[j][pool_stride]);
				checkError(status, "Failed to set argument %d of kernel elt", argi - 1);

				// status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_float), &divisor);
				// checkError(status, "Failed to set argument %d of kernel elt", argi - 1);

				status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_uchar), &addition_precision_config[j][elt_din1]);
				checkError(status, "Failed to set argument %d of kernel elt", argi - 1);

				status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_uchar), &addition_precision_config[j][elt_din2]);
				checkError(status, "Failed to set argument %d of kernel elt", argi - 1);
				
				status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_uchar), &addition_precision_config[j][elt_dout]);
				checkError(status, "Failed to set argument %d of kernel elt", argi - 1);

				// scale hard tanh
				status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_uchar), &ACT_ALPHA[act_ptr]);
				checkError(status, "Failed to set argument %d of kernel elt", argi - 1);

				status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_uchar), &ACT_ALPHA_FL[act_ptr]);
				checkError(status, "Failed to set argument %d of kernel elt", argi - 1);

				status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_uchar), &ACT_BETA[act_ptr]);
				checkError(status, "Failed to set argument %d of kernel elt", argi - 1);

				status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_uchar), &ACT_OUT_FL[act_ptr]);
				checkError(status, "Failed to set argument %d of kernel elt", argi - 1);

				status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_short), &ACT_N_THRE[act_ptr]);
				checkError(status, "Failed to set argument %d of kernel elt", argi - 1);
				
				status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_short), &ACT_P_THRE[act_ptr++]);
				checkError(status, "Failed to set argument %d of kernel elt", argi - 1);

				// bottom 1
				if(eltwise_config[j][eltwise_in_1]==0)
					status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_mem), &data_buf[i*input_config[batch_size]+k]);
				else if(eltwise_config[j][eltwise_in_1]==1)
					status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_mem), &output_buf[i*input_config[batch_size]+k]);
				else if(eltwise_config[j][eltwise_in_1]==4)
					status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_mem), &pool_buf[i*input_config[batch_size]+k]);
				else //eltwise_config[j][eltwise_in_1]==5)
					status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_mem), &eltwise_buf[i*input_config[batch_size]+k]);
				checkError(status, "Failed to set argument %d of kernel elt", argi - 1);

				// bottom 2
				if(eltwise_config[j][eltwise_in_2]==0)
					status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_mem), &data_buf[i*input_config[batch_size]+k]);
				else if(eltwise_config[j][eltwise_in_2]==1)
					status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_mem), &output_buf[i*input_config[batch_size]+k]);
				else if(eltwise_config[j][eltwise_in_2]==4)
					status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_mem), &pool_buf[i*input_config[batch_size]+k]);
				else //eltwise_config[j][eltwise_in_2]==5
					status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_mem), &eltwise_buf[i*input_config[batch_size]+k]);
				checkError(status, "Failed to set argument %d of kernel elt", argi - 1);

				// top
				if(eltwise_config[j][eltwise_out]==0)
					status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_mem), &data_buf[i*input_config[batch_size]+k]);
				else if(eltwise_config[j][eltwise_out]==1)
					status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_mem), &output_buf[i*input_config[batch_size]+k]);
				else if(eltwise_config[j][eltwise_out]==5)
					status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_mem), &eltwise_buf[i*input_config[batch_size]+k]);
				else //(eltwise_config[j][eltwise_out]==2)
					status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_mem), &fc_1_buf[i]);
				checkError(status, "Failed to set argument %d of kernel elt", argi - 1);

				// top_bin
				status = clSetKernelArg(knl_elt[i], argi++, sizeof(cl_mem), &output_buf_bin[i*input_config[batch_size]+k]);
				checkError(status, "Failed to set argument %d of kernel elt", argi - 1);
			}
#endif

			// Excutes Kernel
			//
			if(k == 0&&pic_num==1)
				printf("\nExecuting Layer %d:\n", j+1);

			// kernel memRd
			if(k == 0&&pic_num==1)
				printf("\nLaunching single work-item kernel winbuffer\n");

			status = clEnqueueTask(que_memRd[i], knl_memRd[i], 0, NULL, &memRd_event[i]);
			checkError(status, "Failed to launch kernel memRD kernel");

			// kernel conv
			if(layer_config[j][layer_type]==2){
				//binary
				if(k == 0&&pic_num==1)
					printf("\nLaunching single work-item kernel Binary Conv\n");
				status = clEnqueueTask(que_conv_bin[i], knl_conv_bin[i], 0, NULL, &conv_event[i]);
				checkError(status, "Failed to launch kernel conv kernel");
			}else{
				if(k == 0&&pic_num==1)
					printf("\nLaunching single work-item kernel Conv\n");
				status = clEnqueueTask(que_conv[i], knl_conv[i], 0, NULL, &conv_event[i]);
				checkError(status, "Failed to launch kernel conv kernel");
			}

			// // kernel memWr
#ifdef MEM_WT_TASK
			if(k == 0&&pic_num==1)
				printf("\nLaunching single work-item kernel MemWr\n");
			status = clEnqueueTask(que_memWr[i], knl_memWr[i], 0, NULL, &memWr_event[i]);
			checkError(status, "Failed to launch kernel memWr");
#else
			knl_memWr_global_size[0] = memWr_dim1;
			knl_memWr_global_size[1] = memWr_dim2;
			knl_memWr_global_size[2] = layer_config[j][weight_m]; // pool_z equals original weight_m, new weight_m is divisible by LANE_NUM
			knl_memWr_local_size[0] = 1;
			knl_memWr_local_size[1] = 1;
			knl_memWr_local_size[2] = LANE_NUM;
			if(k == 0&&pic_num==1)
				printf("\nLaunching kernel MemWr with local size: %d, %d, %d  (global size: %d, %d, %d)\n",
									(int)knl_memWr_local_size[0], (int)knl_memWr_local_size[1], (int)knl_memWr_local_size[2],
									(int)knl_memWr_global_size[0], (int)knl_memWr_global_size[1], (int)knl_memWr_global_size[2]);
			status = clEnqueueNDRangeKernel(que_memWr[i], knl_memWr[i], 3, NULL, knl_memWr_global_size, knl_memWr_local_size, 0, NULL, &memWr_event[i]);
#endif

			if(layer_config[j][pool_on]==1){
				// kernel pool
				status = clEnqueueTask(que_pool[i], knl_pool[i], 0, NULL, &pool_event[i]);
				//status = clEnqueueNDRangeKernel(que_pool[i], knl_pool[i], 3, NULL, knl_maxPool_global_size, knl_maxPool_local_size, 0, NULL, &pool_event[i]);
				checkError(status, "Failed to launch kernel pooling");
				if(k == 0&&pic_num==1)
					printf("\nLaunching single work-item kernel Pool\n");
					//printf("\nLaunching kernel pool with local size: %d, %d, %d  (global size: %d, %d, %d)\n", (int)knl_maxPool_local_size[0], (int)knl_maxPool_local_size[1], (int)knl_maxPool_local_size[2], (int)knl_maxPool_global_size[0], (int)knl_maxPool_global_size[1], (int)knl_maxPool_global_size[2]);
			}

#ifdef LAYER_ELT
			if(eltwise_config[j][eltwise_on]){
				//enqueue elt kernel
				if(k == 0&&pic_num==1)
				printf("\nLaunching single work-item kernel eltwise\n");

				status = clEnqueueTask(que_memWr[i], knl_elt[i], 0, NULL, &elt_event[i]);
				checkError(status, "Failed to launch kernel eltwise kernel");
			}
#endif
			// Wait for all kernel to finish
			if(layer_config[j][lrn_on]){
				status = clWaitForEvents(num_devices, lrn_event);
				checkError(status, "Failed to finish lrn event");
			}
#ifdef LAYER_ELT
				else if(eltwise_config[j][eltwise_on]){
					//wait for elt kernel done
					status = clWaitForEvents(num_devices, elt_event);
					checkError(status, "Failed to finish elt event");
				}
#endif
			else if(layer_config[j][pool_on]==1){
				status = clWaitForEvents(num_devices, pool_event);
				checkError(status, "Failed to finish memWR event");
			}
			else{
				status = clWaitForEvents(num_devices, memWr_event);
				checkError(status, "Failed to finish memWR event");
			}

#ifndef USE_OPENCV
			// Profile mode, get excution time for each kernel
			memRd_time[j] += getKernelStartEndTime(memRd_event[i]);
			conv_time[j]  += getKernelStartEndTime(conv_event[i]);
			if(layer_config[j][pool_on]==1)
				pool_time[j] += getKernelStartEndTime(pool_event[i]);
			memWr_time[j] += getKernelStartEndTime(memWr_event[i]);
			if(layer_config[j][lrn_on])
				lrn_time[j] += getKernelStartEndTime(lrn_event[i]);

#ifdef LAYER_ELT
			if(eltwise_config[j][eltwise_on]){
				elt_time[j] += getKernelStartEndTime(elt_event[i]);
			}
#endif
#endif

			// Must release event object to avoid performance degeneration !!!
			clReleaseEvent(memRd_event[i]);
			checkError(status, "Failed to release memRD event object");
			clReleaseEvent(conv_event[i]);
			checkError(status, "Failed to release Conv event object");
			clReleaseEvent(memWr_event[i]);
			checkError(status, "Failed to release memWR event object");
			if(layer_config[j][pool_on]==1){
				status = clReleaseEvent(pool_event[i]);
				checkError(status, "Failed to release pool event object");
			}
			if(layer_config[j][lrn_on]){
				status = clReleaseEvent(lrn_event[i]);
				checkError(status, "Failed to release lrn event object");
			}

#ifdef LAYER_ELT
			if(eltwise_config[j][eltwise_on]){
				clReleaseEvent(elt_event[i]);
				checkError(status, "Failed to release elt event object");
			}
#endif
			}// end of batch iteration


		}// end of layer iteration

		t.stop();
		time = t.get_time_s();
#ifdef USE_OPENCV
		printf("Done! Inference time is %fs \n", time);
#endif
		readDataBack();
		verifyResult(pic_num);

#ifdef USE_OPENCV
			getAccuracy(output_reorder,pic_num);
			printf("\n\n");
		}//end of picture iteration
#endif

	}// end of board iteration


	//Recorde the end time
	printf("\nPipeCNN exited !!!\n\n");


#ifdef USE_OPENCV
	printf("Total number of %d pictures have been processed.\n\n", PICTURE_NUM);

	accuracy1=accuracy1/(PICTURE_NUM);
	accuracy5=accuracy5/(PICTURE_NUM);
	printf("Final Top-1 accuracy = %5.3f\n", accuracy1);
	printf("Final Top-5 accuracy = %5.3f\n\n", accuracy5);
#else
	printf("\n-------------------\n");
	printf("\nPerformance Summary\n\n");
	float kernel_time = 0.0f;
	float batch_float = float(input_config[batch_size]);
	// bandwidth for each device
	printf("Kernel runtime summary:\n");
	for(unsigned j = LAYER_START-1; j < LAYER_END; ++j) {
		printf("  Layer-%d:\n", j+1);
		printf("    MemRd: %0.3f ms\n", double(memRd_time[j])/batch_float * 1e-6);
		printf("    Conv : %0.3f ms\n", double(conv_time[j])/batch_float * 1e-6);
		printf("    Pool : %0.3f ms\n", double(pool_time[j])/batch_float * 1e-6);
		printf("    MemWr: %0.3f ms\n", double(memWr_time[j])/batch_float * 1e-6);
#ifdef LAYER_ELT
		printf("    Elt  : %0.3f ms\n", double(elt_time[j])/batch_float * 1e-6);
#else
		printf("    Lrn  : %0.3f ms\n", double(lrn_time[j])/batch_float * 1e-6);
#endif
		kernel_time += conv_time[j];
	}
	printf("\nTotal kernel runtime %0.3f ms \n", double(kernel_time) * 1e-6);
	printf("Batch size = %d, average process time per batch: %0.3f ms \n\n", input_config[batch_size], double(kernel_time/batch_float) * 1e-6);
 	printf("Total runtime: %fs \n\n", time);
#endif
	// Release resource
	cleanup();

	return EXIT_SUCCESS;
}

void readDataBack()
{
	unsigned int read_buf_size;
	//binary
	unsigned int read_buf_size_bin;
	cl_int status;
	scoped_array<cl_event> finish_event(num_devices);
	// Read back the results from the device to verify the output
	// Note：only device0 is used here
	if(num_devices!=1)
		printf("Warnning: only the result from device0 will be verified!!!\n\n");

	// Select whith item you would like to compare with the golden ref
	// Item num start from 0
	unsigned batch_item_num = 0;
	if(batch_item_num>(input_config[batch_size]-1)){
		printf("Error: wrong configuration，can't verify the item since it is layer than batch size !!!\n\n");
	}
	// TODO: verify results
	if(LAYER_NUM<CONV_NUM){ // verify conv results
		read_buf_size = output_config[output_w]*output_config[output_h]*output_config[output_n];

	}
	else // verify the last conv and all fc results
		read_buf_size = output_config[output_w]*output_config[output_h]*output_config[output_n]*input_config[batch_size];
	read_buf_size_bin = read_buf_size/8;

	// For the last conv layer and all fc layers, read result from one of the fc buffers
	if((layer_config[LAYER_NUM-1][lrn_on] == 0) && (layer_config[LAYER_NUM-1][pool_on] != 1)){
		if(layer_config[LAYER_NUM-1][memwr_dst] == 0){
			printf("\nCopyed all batched results from data buffers.\n");
			status = clEnqueueReadBuffer(que_memWr[0], data_buf[0], CL_FALSE,          // read from device0
				0, sizeof(DTYPE) * read_buf_size, (void *)output, 0, NULL, &finish_event[0]);
			checkError(status, "Failed to set transfer output data");
		}
		else if(layer_config[LAYER_NUM-1][memwr_dst] == 1){
			printf("\nCopyed all batched results from output buffers.\n");
			status = clEnqueueReadBuffer(que_memWr[0], output_buf[0], CL_FALSE,          // read from device0
				0, sizeof(DTYPE) * read_buf_size, (void *)output, 0, NULL, &finish_event[0]);
			checkError(status, "Failed to set transfer output data");
		}
		else if(layer_config[LAYER_NUM-1][memwr_dst] == 2){
		printf("\nCopyed all batched results from fc_1 buffers.\n");
		status = clEnqueueReadBuffer(que_memWr[0], fc_1_buf[0], CL_FALSE,          // read from device0
			0, sizeof(DTYPE) * read_buf_size, (void *)output, 0, NULL, &finish_event[0]);
		checkError(status, "Failed to set transfer output data");
		}
		else if(layer_config[LAYER_NUM-1][memwr_dst] == 3){
		printf("\nCopyed all batched results from fc_2 buffers.\n");
		status = clEnqueueReadBuffer(que_memWr[0], fc_2_buf[0], CL_FALSE,          // read from device0
			0, sizeof(DTYPE) * read_buf_size, (void *)output, 0, NULL, &finish_event[0]);
		checkError(status, "Failed to set transfer output data");
		}
	}
	else if((layer_config[LAYER_NUM-1][lrn_on] == 0) && (layer_config[LAYER_NUM-1][pool_on] == 1)){
		printf("\nCopyed all batched results from pool buffers.\n");
		status = clEnqueueReadBuffer(que_pool[0], pool_buf[0], CL_FALSE,          // read from device0
			0, sizeof(DTYPE) * read_buf_size, (void *)output, 0, NULL, &finish_event[0]);
		checkError(status, "Failed to set transfer output data");
		//binary
		status = clEnqueueReadBuffer(que_pool[0], pool_buf_bin[0], CL_FALSE,      // read from device0
			0, sizeof(DTYPE) * read_buf_size_bin, (void *)output_bin, 0, NULL, &finish_event[0]);
		checkError(status, "Failed to set transfer output data");
	}
	else{
		printf("\nCopyed all batched results from data buffers.\n");
		status = clEnqueueReadBuffer(que_lrn[0], data_buf[0], CL_FALSE,          // read from device0
			0, sizeof(DTYPE) * read_buf_size, (void *)output, 0, NULL, &finish_event[0]);
		checkError(status, "Failed to set transfer output data");
	}

#ifdef LAYER_ELT
	if(eltwise_config[LAYER_NUM-1][eltwise_on]){
		if(eltwise_config[LAYER_NUM-1][eltwise_out]==0){
			printf("\nCopyed one results from NO.%d data buffers(eltwise).\n", batch_item_num);
			status = clEnqueueReadBuffer(que_memWr[0], data_buf[batch_item_num], CL_FALSE,           // read from device0
				0, sizeof(DTYPE) * read_buf_size, (void *)output, 0, NULL, &finish_event[0]);
			checkError(status, "Failed to set transfer output data");
		}
		else if(eltwise_config[LAYER_NUM-1][eltwise_out]==1){
			printf("\nCopyed one results from NO.%d output buffers(eltwise).\n", batch_item_num);
			status = clEnqueueReadBuffer(que_memWr[0], output_buf[batch_item_num], CL_FALSE,           // read from device0
				0, sizeof(DTYPE) * read_buf_size, (void *)output, 0, NULL, &finish_event[0]);
			checkError(status, "Failed to set transfer output data");
		}
		else if(eltwise_config[LAYER_NUM-1][eltwise_out]== 2){
			printf("\nCopyed all batched results from fc_1 buffers(eltwise).\n");
			status = clEnqueueReadBuffer(que_memWr[0], fc_1_buf[0], CL_FALSE,          // read from device0
				0, sizeof(DTYPE) * read_buf_size, (void *)output, 0, NULL, &finish_event[0]);
			checkError(status, "Failed to set transfer output data");
		}
		else if(eltwise_config[LAYER_NUM-1][eltwise_out]== 5){
			printf("\nCopyed one results from NO.%d eltwise buffers(eltwise).\n", batch_item_num);
			status = clEnqueueReadBuffer(que_memWr[0], eltwise_buf[batch_item_num], CL_FALSE,          // read from device0
				0, sizeof(DTYPE) * read_buf_size, (void *)output, 0, NULL, &finish_event[0]);
			checkError(status, "Failed to set transfer output data");
		}		
	}
	else if(layer_config[LAYER_NUM-1][memwr_dst] == 5){
		printf("\nCopyed one results from NO.%d eltwise buffers.\n", batch_item_num);
		status = clEnqueueReadBuffer(que_memWr[0], eltwise_buf[batch_item_num], CL_FALSE,          // read from device0
			0, sizeof(DTYPE) * read_buf_size, (void *)output, 0, NULL, &finish_event[0]);
		checkError(status, "Failed to set transfer output data");
	}
#endif	

	// binary output
	printf("\nCopyed all batched results from output_bin buffers.\n");
	status = clEnqueueReadBuffer(que_memWr[0], output_buf_bin[0], CL_FALSE,          // read from device0
		0, sizeof(DTYPE) * read_buf_size_bin, (void *)output_bin, 0, NULL, &finish_event[0]);
	checkError(status, "Failed to set transfer output data");

	// Wait for reads to finish
	clWaitForEvents(1, &finish_event[0]);
	clReleaseEvent(finish_event[0]);
	checkError(status, "Failed to release finish event object");

	if(LAYER_NUM>=CONV_NUM){  //Select with batch item you would like to verify from the last conv and all fc output
		printf("Selected item = %d from the combined batch results in fc buffers\n", batch_item_num);
		// Extract one results from all batch arranged results stored in fc buffers
		extractOutput(output, output_one_item, batch_item_num, input_config[batch_size], output_config[output_w], output_config[output_h], output_config[output_n]);
	}
	else{
		if(layer_config[LAYER_NUM-1][pool_on])
			// Copy results from one of the output/data buffers
			extractOutput(output, output_one_item, 0, 1, layer_config[LAYER_NUM-1][pool_x], layer_config[LAYER_NUM-1][pool_y], layer_config[LAYER_NUM-1][pool_z]);
		else
			extractOutput(output, output_one_item, 0, 1, layer_config[LAYER_NUM-1][conv_x], layer_config[LAYER_NUM-1][conv_y], layer_config[LAYER_NUM-1][conv_z]);
	}

	// Reorder one item of the batch results into scalar format
	reorderOutput(output_one_item, output_reorder, output_config[output_w], output_config[output_h], output_config[output_n]);
	// reorderBinaryOutput(output_bin, output_reorder, output_config[output_w], output_config[output_h], output_config[output_n]);
	
	// fc1024 sumbias
	if(LAYER_NUM==21){
		int sum_tmp = 0;
		for(unsigned int iii=0; iii<output_config[output_n]; iii++){
			sum_tmp = (signed char)output_reorder[iii] + (signed char)fc1024_bias[iii];
			if(sum_tmp>127)
				output_reorder[iii] = 127;
			else if(sum_tmp<-128)
				output_reorder[iii] = -128;
			else
				output_reorder[iii] = sum_tmp;
		}
	}

}

void verifyResult(int num)
{

#ifdef USE_OPENCV
	int max_label;
	char * substr;
	softmax(output_reorder, output_one_item);
	max_label = getProb(output_one_item);
	// Show the picture
	substr = &synset_buf[max_label][10];

	Mat img = imread(picture_file_path);
	putText(img,substr,Point(20, 50), CV_FONT_HERSHEY_SIMPLEX, 0.8, Scalar(255, 255, 0), 2, 8);
	if(max_label == label[num-1]){
		putText(img,"True",Point(20, 80), CV_FONT_HERSHEY_SIMPLEX, 0.8, Scalar(0, 255, 0), 2, 8);
	} else {
		putText(img,"False",Point(20, 80), CV_FONT_HERSHEY_SIMPLEX, 0.8, Scalar(0, 0, 255), 2, 8);
		printf("False: True_label = %d Inferred_label = %d\n\n", label[num], max_label);
	}
	imshow( "PipeCNN", img);
	cvMoveWindow("PipeCNN",0,0);//set the window's position
	waitKey(600);
#else
	// Validate the results
	printf("\nStart verifying results ...\n");
	unsigned int err_num;
	float std_err;  // standard errors
	unsigned int batch_item_size;
	// Compare each results with the golden reference data
	batch_item_size = output_config[output_w]*output_config[output_h]*output_config[output_n];
	err_num = 0;
	for (unsigned int j = 0; j < batch_item_size; j++){
		std_err = abs(output_reorder[j] - golden_ref[j]);
		if(std_err > 0){
			err_num++;
			if(err_num<10)
				printf("Item=%d is wrong (result=%f, golden_ref=%f)\n", j, (float)output_reorder[j], (float)golden_ref[j]);
		}
	}
	if(err_num>0)
		printf("Totally %d Wrong Results\n", err_num);
	else{
		printf("\nCheck Pass !!!\n");
		softmax(output_reorder, output_one_item);
		getProb(output_one_item);
	}
	// Dump results and golden_ref for debugging
	dumpResult();
#endif
}

void loadImageToBuffer(int num)
{
	cl_int status;
	ifstream bin_file_r;

	// Image
#ifdef USE_OPENCV
	// load image from picture files
	// get the correct paths for each pictures
	char end[14]="00000000.JPEG";//endof the char[] '\0'
	char head[100];
	numtochar(num,end);
	memset(picture_file_path,0x00,sizeof(char)*100);
	strcpy(head,picture_file_path_head);
	strcpy(picture_file_path,strcat(head,end));

	if(load_picture(image)==1)
		printf("Error: loading image data from real pictures failed !!!\n");
#else
	unsigned file_size;
	// load image from binary files
	bin_file_r.open(input_file_path, ios::in | ios::binary);

    if(bin_file_r.is_open())
    {
		//Get file size
		bin_file_r.seekg(0, bin_file_r.end);
		file_size = bin_file_r.tellg();
		bin_file_r.seekg(0, bin_file_r.beg);

		bin_file_r.read((char *)image, sizeof(DTYPE)*IMAGE_FILE_SIZE);
		printf("\n%d bytes image data read from binary files\n", file_size);
		if(IMAGE_FILE_SIZE!=(file_size/(sizeof(DTYPE))))
			printf("Warning: image file size does not match user configuration !!!\n");
		bin_file_r.close();
	}
	else
		printf("Image file does not exits !!!\n");
#endif

	// Vectorize the input image by a factor of VEC_SIZE
	for(unsigned n = 0; n<layer_config[LAYER_START-1][data_n]/VEC_SIZE; n++){
		for(unsigned i = 0; i<layer_config[LAYER_START-1][data_h]; i++){
			for(unsigned j = 0; j<layer_config[LAYER_START-1][data_w]; j++){
				for(unsigned k = 0; k<VEC_SIZE; k++){
					if((n*VEC_SIZE+k)<layer_config_original[0][data_n]){ //  when layer_config[LAYER_START-1][data_n] > layer_config_original[0][data_n], only copy valid pixels
						data_init[n*VEC_SIZE*layer_config[LAYER_START-1][data_h]*layer_config[LAYER_START-1][data_w] + i*layer_config[LAYER_START-1][data_w]*VEC_SIZE + j*VEC_SIZE + k]
							= (DTYPE) image[(n*VEC_SIZE+k)*layer_config[LAYER_START-1][data_h]*layer_config[LAYER_START-1][data_w] + i*layer_config[LAYER_START-1][data_w] + j];
					}
				}
			}
		}
	}

	for(unsigned i = 0; i < num_devices; ++i) {
		// Create data buffers for each batch item
		for(unsigned j = 0; j < input_config[batch_size]; ++j){
			// Load image data into buffers
			if(layer_config[LAYER_START-1][memrd_src] == 0){
				status = clEnqueueWriteBuffer(que_memRd[i], data_buf[i*input_config[batch_size]+j], CL_TRUE, 0, (layer_config[LAYER_START-1][data_w]*layer_config[LAYER_START-1][data_h]*layer_config[LAYER_START-1][data_n]) * sizeof(DTYPE), data_init, 0, NULL, NULL);
				checkError(status, "Failed to transfer input image");
			}
		}
	}
}

// Read all input data and golden ref data
int prepare()
{

	// Load Image data, CNN net weights and golden_results
    ifstream bin_file_r;
    unsigned file_size;
	unsigned weight_size;
	unsigned output_size;
	unsigned godref_size;
	int ptr = 0;  // original weight and bias offset for each layer
	//binary
	int ptr_scale_bin = 0;

	unsigned char  conv_win_size_dim1, conv_win_size_dim2;

	unsigned padding_offset[LAYER_NUM];

	// Parameter initialization and safty check
	for(unsigned ll=0; ll<LAYER_NUM; ll++){

		// First, backup the original layer configurations
		for(unsigned ii=0; ii<NUM_CONFIG_ITEM; ii++){
			layer_config_original[ll][ii]=layer_config[ll][ii];
		}

		// Second, perform padding on dim4, when it is not divisible by LANE_NUM
		if(layer_config[ll][weight_m]%LANE_NUM != 0){
			printf("\nWarnning: layer-%d requires padding zero-value feature maps for give param LANE_NUM=%d\n", ll+1, LANE_NUM);
			// change the num of output featuremaps to new value that is divisible by LANE_NUM
			// Note: the num of input feature maps of next layer remains the same
			layer_config[ll][weight_m] = ceil((float)layer_config[ll][weight_m]/LANE_NUM)*LANE_NUM;
			layer_config[ll][bias_size] = layer_config[ll][weight_m];
			printf("      original num of feature maps is %d, new value is %d\n", layer_config_original[ll][weight_m], layer_config[ll][weight_m]);

			// padding of weight on dim4 is needed
			padding_offset[ll] = layer_config[ll][weight_m] - layer_config_original[ll][weight_m];
			// check if evenly padding on two sides is possible
			if(((layer_config[ll][weight_m]/LANE_NUM)%2!=0) & (layer_config[ll][conv_split]==1)){
				printf("Error: could not perform padding for split mode, weight_m/LANE_NUM must be divisible by 2 !!!\n\n");
				return 1;
			}
			else{ // padding zeros evenly on two sides of dim4
				padding_offset[ll] = padding_offset[ll]/2;
				printf("      padding_offset=%d (layer=%d)\n\n", padding_offset[ll], ll+1);
			}

		}
		else{
			padding_offset[ll] = 0;
		}

		// Check parameters
		if(ll==0){ // check parameters for layer-1
			if(input_config[image_w] != layer_config_original[ll][data_w] ||  input_config[image_h] != layer_config_original[ll][data_h]
				|| input_config[image_n] != layer_config_original[ll][data_n] || input_config[image_n] != layer_config_original[ll][weight_n]){
					printf("Error: incorrect layer configuration for layer-%d !!!\n", ll+1);
					//return 1;
				}

			if((layer_config_original[ll][weight_n]!=input_config[image_n])){
				printf("\nError: incorrect layer configuration for layer-%d !!!\n", ll+1);
				//return 1;
			}

		}
		else{ // other layers

			// Currently weight_n must be divisible by VEC_SIZE (for first layer, padding is performed when weight_n is not divisible by VEC_SIZE)
			if((layer_config[ll][weight_n]%VEC_SIZE)!=0){
				printf("\nError: incorrect setting of parameter VEC_SIZE !!!\n");
				return 1;
			}
// #ifdef RESNET
// 			if((layer_config_original[ll][data_n]!=layer_config_original[eltwise_layer_previous[ll]-1][conv_z])){
// 				printf("\nError: incorrect setting of convolution input/output size for layer-%d!!!\n", ll+1);
// 				return 1;
// 			}
// #else
// 			if((layer_config_original[ll][data_n]!=layer_config_original[ll-1][conv_z])){
// 				printf("\nError: incorrect setting of convolution input/output size for layer-%d!!!\n", ll+1);
// 				return 1;
// 			}
// #endif
		}
		if((layer_config_original[ll][conv_x]!=(layer_config_original[ll][data_w]-layer_config_original[ll][weight_w]+2*layer_config_original[ll][conv_padding])/layer_config_original[ll][conv_stride]+1)
			|| (layer_config_original[ll][conv_y]!=(layer_config_original[ll][data_h]-layer_config_original[ll][weight_h]+2*layer_config_original[ll][conv_padding])/layer_config_original[ll][conv_stride]+1)
		    || (layer_config_original[ll][conv_z]!=layer_config_original[ll][weight_m])){
			printf("\nError: incorrect setting of convolution output size or filter params for layer-%d!!!\n", ll+1);
			return 1;
		}
		//ResNet maxPool need pad
		if((layer_config_original[ll][pool_on]==1) && ((layer_config_original[ll][pool_x]!=((layer_config_original[ll][conv_x]+MAXPOOL_PAD)-layer_config_original[ll][pool_size])/layer_config_original[ll][pool_stride]+1)
			|| (layer_config_original[ll][pool_y]!=((layer_config_original[ll][conv_y]+MAXPOOL_PAD)-layer_config_original[ll][pool_size])/layer_config_original[ll][pool_stride]+1)
		    || (layer_config_original[ll][pool_z]!=layer_config_original[ll][conv_z]))){
			printf("\nError: incorrect setting of pooling input/output size for layer-%d!!!\n", ll+1);
			return 1;
		}

		// TODO: check buffer size, hw params
		if(layer_config[ll][conv_x]==1){ // when only one group for FC layer
			conv_win_size_dim1  = layer_config[ll][weight_w];
		}
		else{
			conv_win_size_dim1  = layer_config[ll][weight_w]+(CONV_GP_SIZE_X-1)*layer_config[ll][conv_stride];
		}
		conv_win_size_dim2    = layer_config[ll][weight_h];
		// check win_buffer size
		if(conv_win_size_dim1*conv_win_size_dim2*layer_config[ll][weight_n]/VEC_SIZE > WIN_BUF_SIZE){

			printf("Error: required win_buffer size is %d, configured size is %d \n", conv_win_size_dim1*conv_win_size_dim2*layer_config[ll][weight_n]/VEC_SIZE, WIN_BUF_SIZE);
			return 1;
		}
		// check weight_buffer size
		if(layer_config[ll][weight_w]*layer_config[ll][weight_h]*layer_config[ll][weight_n]/VEC_SIZE > WEIGHT_BUF_SIZE){

			printf("Error: required weight_buffer size is %d, configured size is %d \n", layer_config[ll][weight_w]*layer_config[ll][weight_h]*layer_config[ll][weight_n]/VEC_SIZE, WEIGHT_BUF_SIZE);
			return 1;
		}

	}

	// image and weight files
	weights      = (DTYPE *)alignedMalloc(sizeof(DTYPE)*WEIGHTS_FILE_SIZE, DMA_ALIGNMENT);
	image        = (DTYPE *)alignedMalloc(sizeof(DTYPE)*IMAGE_FILE_SIZE, DMA_ALIGNMENT);

	// input data buffers
	// padding the input RGB image with extra number of zeros channels, so that data_n/weight_n is divisible by VEC_SIZE
	layer_config[0][weight_n] = ceil((float)layer_config[0][weight_n]/VEC_SIZE)*VEC_SIZE;
	layer_config[0][data_n] = layer_config[0][weight_n];

	data_init   = (DTYPE *)alignedMalloc(sizeof(DTYPE)*layer_config[0][data_w]*layer_config[0][data_h]*layer_config[0][data_n], DMA_ALIGNMENT);
	memset(data_init, 0, sizeof(DTYPE)*layer_config[0][data_w]*layer_config[0][data_h]*layer_config[0][data_n]);// fill non-RGB dims with 0

	// final results
	if(LAYER_NUM>=CONV_NUM)// For last conv and all fc layers, all batch results are read back
		output_size = output_config[output_w]*output_config[output_h]*output_config[output_n]*input_config[batch_size];
	else // For other conv layers, only one item of
		output_size = output_config[output_w]*output_config[output_h]*output_config[output_n];

	godref_size = output_config[output_w]*output_config[output_h]*output_config[output_n];

	output          = (DTYPE *)alignedMalloc(sizeof(DTYPE)*output_size, DMA_ALIGNMENT); // vectorized results
	output_one_item = (DTYPE *)alignedMalloc(sizeof(DTYPE)*godref_size, DMA_ALIGNMENT); // one item extracted from batch results
	golden_ref      = (DTYPE *)alignedMalloc(sizeof(DTYPE)*godref_size, DMA_ALIGNMENT);
    output_reorder  = (DTYPE *)alignedMalloc(sizeof(DTYPE)*godref_size, DMA_ALIGNMENT); // reordered results for verifying
	//binary
	scales			= (DTYPE *)alignedMalloc(sizeof(DTYPE)*SCALES_FILE_SIZE, DMA_ALIGNMENT);
	output_bin      = (DTYPE *)alignedMalloc(sizeof(DTYPE)*output_size/8, DMA_ALIGNMENT); // vectorized results

	if(weights == NULL || image == NULL || golden_ref == NULL || data_init == NULL || output == NULL || output_one_item == NULL || output_reorder == NULL)
	{
		printf("Not enough memory !!!");
		alignedFree(weights);
		alignedFree(image);
		alignedFree(data_init);
		alignedFree(golden_ref);
		alignedFree(output_one_item);
		alignedFree(output);
		alignedFree(output_reorder);

		return 1;
	}

	// weights and bias	buffers
	for(int j=0; j<LAYER_NUM; j++){
		if(layer_config[j][layer_type]==2){
			// binary convolution
			weight_size = (layer_config[j][weight_w]*layer_config[j][weight_h]*layer_config[j][weight_n]/8*layer_config[j][weight_m]);
		}else{
			weight_size = (layer_config[j][weight_w]*layer_config[j][weight_h]*layer_config[j][weight_n]*layer_config[j][weight_m]);
		}
		weight_conv[j] = (DTYPE *)alignedMalloc(sizeof(DTYPE)*weight_size, DMA_ALIGNMENT);
		memset(weight_conv[j], 0, sizeof(DTYPE)*weight_size);             // reset all value (include padding value) to zero

		// Binary scale
		scale_conv_bin[j]   = (DTYPE *)alignedMalloc(sizeof(DTYPE)*layer_config[j][weight_m], DMA_ALIGNMENT);
		memset(scale_conv_bin[j], 0, sizeof(DTYPE)*layer_config[j][weight_m]);// reset all value (include padding value) to zero
		
		bias_conv[j]   = (DTYPE *)alignedMalloc(sizeof(DTYPE)*layer_config[j][bias_size], DMA_ALIGNMENT);
		memset(bias_conv[j], 0, sizeof(DTYPE)*layer_config[j][bias_size]);// reset all value (include padding value) to zero

		if(weight_conv[j] == NULL || bias_conv[j] == NULL )
		{
			printf("Not enough memory !!!");
			for(int i=0; i<=j; i++){
				alignedFree(weight_conv[i]);
				alignedFree(bias_conv[i]);
			}
			return 1;
		}
	}

	// Weights
	bin_file_r.open(weight_file_path, ios::in | ios::binary);

	if(bin_file_r.is_open())
	{
		//Get file size
		bin_file_r.seekg(0, bin_file_r.end);
		file_size = bin_file_r.tellg();
		bin_file_r.seekg(0, bin_file_r.beg);

		bin_file_r.read((char *)weights, sizeof(DTYPE)*WEIGHTS_FILE_SIZE);
		printf("\n%d total weights read \n", file_size/((int)sizeof(DTYPE)));
		if(WEIGHTS_FILE_SIZE!=(file_size/(sizeof(DTYPE))))
			printf("Warning: weight file size does not match user configuration !!!\n");
		bin_file_r.close();
	}
	else
		printf("Weights file does not exits !!!\n");

	// Synset_words
     int nn=0;
	 FILE *fp=fopen(synset_word_file_path,"r");
	 if(!fp)
	 {
		 printf("Synset word file does not exits !!!\n");
		 return 1;
	 }
	 while (!feof(fp)){
		fgets(synset_buf[nn], 1024, fp);
		nn++;
	}
	fclose(fp);

	// Scaling factor
	bin_file_r.open(scales_file_path, ios::in | ios::binary);

	if(bin_file_r.is_open())
	{
		//Get file size
		bin_file_r.seekg(0, bin_file_r.end);
		file_size = bin_file_r.tellg();
		bin_file_r.seekg(0, bin_file_r.beg);

		bin_file_r.read((char *)scales, sizeof(DTYPE)*SCALES_FILE_SIZE);
		printf("\n%d total scales read \n", file_size/((int)sizeof(DTYPE)));
		if(SCALES_FILE_SIZE!=(file_size/(sizeof(DTYPE))))
			printf("Warning: scale file size does not match user configuration !!!\n");
		bin_file_r.close();
	}
	else
		printf("Weights file does not exits !!!\n");

	// fc1024bias
	bin_file_r.open(fc1024_bias_path, ios::in | ios::binary);

	if(bin_file_r.is_open())
	{
		//Get file size
		bin_file_r.seekg(0, bin_file_r.end);
		file_size = bin_file_r.tellg();
		bin_file_r.seekg(0, bin_file_r.beg);

		bin_file_r.read((char *)fc1024_bias, sizeof(DTYPE)*1024);
		printf("\n%d total fc1024_bias read \n", file_size/((int)sizeof(DTYPE)));
		if(1024!=(file_size/(sizeof(DTYPE))))
			printf("Warning: fc1024_bias file size does not match user configuration !!!\n");
		bin_file_r.close();
	}
	else
		printf("fc1024_bias file does not exits !!!\n");

#ifdef USE_OPENCV
	// label
	nn=0;
	fp=fopen(LabelPath,"r");
	if(!fp)
	{
		 printf("Label file does not exits !!!\n");
		 return 1;
	 }
	while (!feof(fp)&&nn<PICTURE_NUM){
		//printf("read%d......\n",nn);
		fgets(label_buf[nn], 1024, fp);
		nn++;
	}
	fclose(fp);
	labelNum();
#endif
	// golden_output
	bin_file_r.open(ref_file_path, ios::in | ios::binary);

	if(bin_file_r.is_open())
	{
		//Get file size
		bin_file_r.seekg(0, bin_file_r.end);
		file_size = bin_file_r.tellg();
		bin_file_r.seekg(0, bin_file_r.beg);

		bin_file_r.read((char *)golden_ref, sizeof(DTYPE)*godref_size);
		printf("%d total output reference read \n\n", file_size/((int)sizeof(DTYPE)));
		if(godref_size!=(file_size/(sizeof(DTYPE))))
			printf("Warning: golden reference file size does not match !!!\n");
		bin_file_r.close();
	}
	else
		printf("Golden file does not exits !!!\n");

	// Layer-1
	reorderWeights(weights, weight_conv[0], layer_config[0][weight_w], layer_config[0][weight_h], layer_config[0][weight_n], layer_config[0][weight_m], layer_config_original[0][weight_n], layer_config_original[0][weight_m], ptr, padding_offset[0], VEC_SIZE, LANE_NUM);
	ptr+=layer_config[0][weight_w]*layer_config[0][weight_h]*layer_config_original[0][weight_n]*layer_config_original[0][weight_m];
	reorderBias(weights, bias_conv[0], ptr, padding_offset[0], layer_config[0][bias_size], layer_config_original[0][bias_size], LANE_NUM);
	ptr+=layer_config_original[0][bias_size];

	// Other layers
	for(unsigned j=1; j<LAYER_NUM; j++){

		if(ptr+layer_config[j][weight_w]*layer_config[j][weight_h]*layer_config_original[j][weight_n]*layer_config_original[j][weight_m]>WEIGHTS_FILE_SIZE)
		{
			printf("Error：exceed weight file size !!!\n");
			return 1;
		}

		// binary
		if(layer_config[j][layer_type]==2){
			reorderBinaryWeights(weights, weight_conv[j], layer_config[j][weight_w], layer_config[j][weight_h], layer_config[j][weight_n], layer_config[j][weight_m], layer_config_original[j][weight_n], layer_config_original[j][weight_m], ptr, padding_offset[j], VEC_SIZE, LANE_NUM);
			ptr+=layer_config[j][weight_w]*layer_config[j][weight_h]*layer_config_original[j][weight_n]*layer_config_original[j][weight_m];
			
			reorderBias(weights, bias_conv[j], ptr, padding_offset[j], layer_config[j][bias_size], layer_config_original[j][bias_size], LANE_NUM);
			ptr+=layer_config_original[j][bias_size];

			reorderBias(scales, scale_conv_bin[j], ptr_scale_bin, padding_offset[j], layer_config[j][bias_size], layer_config_original[j][bias_size], LANE_NUM);
			ptr_scale_bin+=layer_config_original[j][bias_size];
		}else{
			reorderWeights(weights, weight_conv[j], layer_config[j][weight_w], layer_config[j][weight_h], layer_config[j][weight_n], layer_config[j][weight_m], layer_config_original[j][weight_n], layer_config_original[j][weight_m], ptr, padding_offset[j], VEC_SIZE, LANE_NUM);
			ptr+=layer_config[j][weight_w]*layer_config[j][weight_h]*layer_config_original[j][weight_n]*layer_config_original[j][weight_m];
			reorderBias(weights, bias_conv[j], ptr, padding_offset[j], layer_config[j][bias_size], layer_config_original[j][bias_size], LANE_NUM);
			ptr+=layer_config_original[j][bias_size];
		}
	}
	return 0;
}

void reorderWeights(DTYPE *weights, DTYPE *weight_buf, unsigned dim1, unsigned dim2, unsigned dim3, unsigned dim4, unsigned dim3_original, unsigned dim4_original, unsigned offset, unsigned padding_offset, unsigned vecSize, unsigned laneNum){

	DTYPE    *copy_with_padding;

	// First, copy the data into new buffer and padding in dim3/dim4 with zeros if needed
	copy_with_padding  = (DTYPE *)malloc(sizeof(DTYPE)*dim1*dim2*dim3*dim4);
	if(copy_with_padding == NULL)
	{
		printf("Error: not enough memory when padding weight!!!");
		free(copy_with_padding);
	}
	memset(copy_with_padding, 0, sizeof(DTYPE)*dim1*dim2*dim3*dim4);

	for(unsigned m = 0; m<dim4_original; m++){
		for(unsigned n = 0; n<dim3_original; n++){
			for(unsigned i = 0; i<dim2; i++){
				for(unsigned j = 0; j<dim1; j++){
							copy_with_padding[(padding_offset*dim1*dim2*dim3) + m*dim1*dim2*dim3 + n*dim1*dim2 + i*dim1 + j]
													= (DTYPE) weights[offset+m*dim1*dim2*dim3_original + n*dim1*dim2 + i*dim1 + j];
				}
			}
		}
	}

	// Second, perform vectorization in dim3 by VEC_SIZE and at the same time, perform vectorization in dim4 by a factor of LANE_NUM
	for(unsigned m = 0; m<(dim4/laneNum); m++){
		for(unsigned n = 0; n<(dim3/vecSize); n++){
			for(unsigned i = 0; i<dim2; i++){
				for(unsigned j = 0; j<dim1; j++){
					for(unsigned ll = 0; ll<laneNum; ll++){
						for(unsigned k = 0; k<vecSize; k++){
							weight_buf[m*dim1*dim2*dim3*laneNum + n*dim1*dim2*vecSize*laneNum + i*dim1*vecSize*laneNum + j*vecSize*laneNum + ll*vecSize + k]
													= (DTYPE) copy_with_padding[(m*laneNum+ll)*dim3*dim2*dim1 + (n*vecSize+k)*dim1*dim2 + i*dim1 + j];
						}
					}
				}
			}
		}
	}

	// release resource
	free(copy_with_padding);
}

void reorderBinaryWeights(DTYPE *weights, DTYPE *weight_buf, unsigned dim1, unsigned dim2, unsigned dim3, unsigned dim4, unsigned dim3_original, unsigned dim4_original, unsigned offset, unsigned padding_offset, unsigned vecSize, unsigned laneNum){

	DTYPE    *copy_with_padding;

	// First, copy the data into new buffer and padding in dim3/dim4 with zeros if needed
	// Attention: padding is not performed on binary weights

	copy_with_padding  = (DTYPE *)malloc(sizeof(DTYPE)*dim1*dim2*dim3*dim4);
	if(copy_with_padding == NULL)
	{
		printf("Error: not enough memory when padding weight!!!");
		free(copy_with_padding);
	}
	memset(copy_with_padding, 0, sizeof(DTYPE)*dim1*dim2*dim3*dim4);

	for(unsigned m = 0; m<dim4_original; m++){
		for(unsigned n = 0; n<dim3_original; n++){
			for(unsigned i = 0; i<dim2; i++){
				for(unsigned j = 0; j<dim1; j++){
							copy_with_padding[(padding_offset*dim1*dim2*dim3) + m*dim1*dim2*dim3 + n*dim1*dim2 + i*dim1 + j]
													= (DTYPE) weights[offset+m*dim1*dim2*dim3_original + n*dim1*dim2 + i*dim1 + j];
				}
			}
		}
	}

	// Second, perform vectorization in dim3 by VEC_SIZE and at the same time, perform vectorization in dim4 by a factor of LANE_NUM
	unsigned char buffer_bin = 0;
	for(unsigned m = 0; m<dim4/laneNum; m++){
		for(unsigned n = 0; n<dim3/(vecSize*8); n++){
			for(unsigned i = 0; i<dim2; i++){
				for(unsigned j = 0; j<dim1; j++){
					for(unsigned l = 0; l<laneNum; l++){
						for(unsigned k = 0; k<vecSize; k++){
							buffer_bin = 0;
							for(unsigned b = 0; b<8; b++){
								buffer_bin = (buffer_bin<<1) | (DTYPE)copy_with_padding[(m*laneNum+l)*dim3*dim2*dim1 + (n*vecSize*8+k*8+b)*dim1*dim2 + i*dim1 + j];
							}
							weight_buf[m*dim1*dim2*(dim3/8)*laneNum + n*dim1*dim2*vecSize*laneNum + i*dim1*vecSize*laneNum + j*vecSize*laneNum + l*vecSize + k] = buffer_bin;
						}
					}
				}
			}
		}
	}

	// release resource
	free(copy_with_padding);
}

void reorderBias(DTYPE *dataIn, DTYPE *bias, unsigned offset, unsigned padding_offset, unsigned dim4, unsigned dim4_original, unsigned laneNum){

	DTYPE *copy_with_padding;

	// first copy the data into new buffer with zero paddings
	copy_with_padding  = (DTYPE *)malloc(sizeof(DTYPE)*dim4);
	if(copy_with_padding == NULL)
	{
		printf("Not enough memory when reordering bias!!!");
		free(copy_with_padding);
	}
	memset(copy_with_padding, 0, sizeof(DTYPE)*dim4);
	// padding evenly on two sides of weight_m
	memcpy(copy_with_padding+padding_offset, dataIn+offset, sizeof(DTYPE)*dim4_original);
	// second, perform vectorization by factor of LANE_NUM
	for(unsigned m = 0; m<(dim4/laneNum); m++){
		for(unsigned ll = 0; ll<laneNum; ll++){
			bias[m*laneNum + ll] = (DTYPE) copy_with_padding[m*laneNum + ll];
		}
	}
	// release resource
	free(copy_with_padding);
}


// Extract one item from batch results
void extractOutput(DTYPE *output, DTYPE *output_one_item, unsigned item_num, unsigned batch_size, unsigned dim1, unsigned dim2, unsigned dim3){

	unsigned char mask = 0xff;
	unsigned char batch_size_in_dim;
	unsigned char batch_size_in_dim_log;
	unsigned char batch_indx_dim1;
	unsigned char batch_indx_dim2;

	if(batch_size==1){
		batch_size_in_dim = 1;
		batch_indx_dim1 = 0;
		batch_indx_dim2 = 0;
	}
	else{
		batch_size_in_dim = log(batch_size)/log(2);
		batch_size_in_dim_log = log(batch_size_in_dim)/log(2);
		batch_indx_dim1 = item_num&(~((mask>>batch_size_in_dim_log)<<batch_size_in_dim_log));
		batch_indx_dim2 = item_num>>batch_size_in_dim_log;
		printf("Batch Size=%d, verifying NO.%d batch item (indx= %d, %d) ...\n", batch_size, item_num, batch_indx_dim1, batch_indx_dim2);
	}


	for(unsigned k = 0; k<(dim3/VEC_SIZE); k++){
		for(unsigned i = 0; i<dim2; i++){
			for(unsigned j = 0; j<dim1; j++){
				for(unsigned vv = 0; vv<VEC_SIZE; vv++){
					output_one_item[k*dim2*dim1*VEC_SIZE + i*dim1*VEC_SIZE + j*VEC_SIZE + vv]
						= output[k*dim2*dim1*batch_size_in_dim*batch_size_in_dim*VEC_SIZE + (i+batch_indx_dim2*dim2)*batch_size_in_dim*dim1*VEC_SIZE + (j+batch_indx_dim1*dim1)*VEC_SIZE + vv];
				}
			}
		}
	}
}


// Re-ordering the vectorized output into scalar form
void reorderOutput(DTYPE *output, DTYPE *output_reorder, unsigned dim1, unsigned dim2, unsigned dim3){

	for(unsigned k = 0; k<(dim3/VEC_SIZE); k++){
		for(unsigned i = 0; i<dim2; i++){
			for(unsigned j = 0; j<dim1; j++){		
				for(unsigned vv = 0; vv<VEC_SIZE; vv++){
					output_reorder[(k*VEC_SIZE+vv)*dim2*dim1 + i*dim1 + j]
						= output[k*dim2*dim1*VEC_SIZE + i*dim1*VEC_SIZE + j*VEC_SIZE + vv];
				}
			}
		}
	}
}

// Re-ordering the vectorized binary output into scalar form
void reorderBinaryOutput(DTYPE *output_bin, DTYPE *output_reorder, unsigned dim1, unsigned dim2, unsigned dim3){
	unsigned k;
	for( k = 0; k<(dim3/VEC_SIZE/8); k++){
		for(unsigned i = 0; i<dim2; i++){
			for(unsigned j = 0; j<dim1; j++){
				for(unsigned vv = 0; vv<VEC_SIZE; vv++){
					for(unsigned bb=0; bb<8; bb++){
						output_reorder[(k*VEC_SIZE*8+vv*8+bb)*dim2*dim1 + i*dim1 + j]
							= (output_bin[k*dim2*dim1*VEC_SIZE + i*dim1*VEC_SIZE + j*VEC_SIZE + vv] >> (7-bb)) & 0x01;
					}
				}
			}
		}
	}
}

void softmax(DTYPE *output_reorder , DTYPE *output)
{
    unsigned int i;
	float data_max=0.0;
	float data_exp;
	float sum_exp=0.0;
	for(i=0;i<output_config[output_n];i++)
	{
	if(data_max<output_reorder[i])
		data_max=output_reorder[i];
	}
	for(i=0;i<output_config[output_n];i++)
	{
		data_exp=exp((float)output_reorder[i]-data_max);
		sum_exp += data_exp;
	}
	for(i=0;i<output_config[output_n];i++)
	{
		data_exp=exp((float)output_reorder[i]-data_max);
		output[i]=data_exp / sum_exp*100.0;

	}
}

int getProb(DTYPE *output)
{
	int m=0;
	float max=output[0];

	// find the class with the highest score
	for(unsigned int i=0;i<output_config[output_n];i++){
		if(max<output[i]){
			max=output[i];
			m=i;
		}
	}

	// replace the last two ASCII charactor with space
	int ii=strlen(synset_buf[m]);
	synset_buf[m][ii-2]= 32;
	synset_buf[m][ii-1]= 32;

	printf("\nThe inference result is %s (the prob is %5.2f) \n\n", synset_buf[m], max);

	return m;

}

void dumpResult(){

	ofstream result_file;

	result_file.open(dump_file_path, ios::out);

	for(unsigned i=0; i<output_config[output_n]; i++){
		result_file << "z=" << i << endl;
		for(unsigned j=0; j<output_config[output_h]; j++){
			result_file << "x=" << setw(3) << right << j << ": ";
			for(unsigned k=0; k<output_config[output_w]; k++){
					result_file << setw(3) << right << (float)output_reorder[output_config[output_w]*output_config[output_h]*i + output_config[output_w]*j + k] << "(";
					result_file << setw(3) << right << (float)golden_ref[output_config[output_w]*output_config[output_h]*i + output_config[output_w]*j + k] << ") ";
			}
			result_file << endl;
		}
		result_file << endl;
	}
	result_file.close();
}

#ifdef USE_OPENCV
// Load image from files
int load_picture(DTYPE *image){

		float *mean_data;

		printf("\nLoading picture %s .....\n\n", picture_file_path);

		// load ILSVRC2012 database mean data
		mean_data = (float *) malloc(sizeof(float)*MEAN_DATA_WIDTH*MEAN_DATA_HEIGHT*MEAN_DATA_CHANNEl);
		if(mean_data == NULL){
			printf("Error: allocating memory for images failed !!!\n");
			return 1;
		}

		FILE *p_mean_data=fopen(mean_data_file_path,"rb");
		fread(mean_data,sizeof(float),MEAN_DATA_WIDTH*MEAN_DATA_HEIGHT*MEAN_DATA_CHANNEl,p_mean_data);

		// load picture from files
		Mat img = imread(picture_file_path);
		// resize pic to MEAN_DATA_WIDTH*MEAN_DATA_HEIGHT, and substract with mean data
		Mat img1;
		resize(img,img1,Size(MEAN_DATA_WIDTH,MEAN_DATA_HEIGHT));
		img1.convertTo(img1,CV_32FC3);
		Mat mean_mat(MEAN_DATA_WIDTH, MEAN_DATA_HEIGHT, CV_32FC3, mean_data);
		img1 = img1 - mean_mat;
		// resize to the input size of the first layer
		Mat img2;
		resize(img1,img2,Size(layer_config_original[0][data_w],layer_config_original[0][data_h]));
		// convert to 8-bit fixed-point
		img2.convertTo(img2,CV_8SC3);
		// reorder channel sequence from RGB to GBR
		DTYPE * data_ptr = (DTYPE*)img2.data;
		unsigned int w,h,c;
		unsigned int k=0;
		for(h=0;h<layer_config_original[0][data_h];h++){
			for(w=0;w<layer_config_original[0][data_w];w++){
				for (c=0;c<layer_config_original[0][data_n];c++){
					 image[c*layer_config_original[0][data_w]*layer_config_original[0][data_h]+h*layer_config_original[0][data_w]+w]=data_ptr[k];
					 k++;
				}
			}
		}
		fclose(p_mean_data);
		free(mean_data);
		return 0;
}

void labelNum()
{
	int i,num;
	char *p;
	for(i=0;i<PICTURE_NUM;i++)
	{
		num=0;
		p=label_buf[i];
		while(*p!='\0')
		{
			if(*p==' ')
			{
				p++;
				break;
			}
			p++;
		}
		while(*p!='\n')
		{
			num=num*10+(*p-'0');
			p++;
		}
		label[i]=num;
	}
}

void numtochar(int num,char *end)
{
	int counter=0;
	while(num>0)
	{
		end[7-counter]='0'+num%10;
		num/=10;
		counter++;
	}

}

void strcopy(DTYPE *str1,DTYPE *str2)
{
	for(unsigned int i=0;i<output_config[output_n];i++)
	{
		str1[i]=str2[i];
	}
}
//Top-k Accuracy
void getAccuracy(DTYPE *output_reorder,int num)
{
	strcopy(searchTop,output_reorder);
	int predictionTop5[5],max;
	float data_max;
	unsigned int i,k;
	float tmp_accuracy1,tmp_accuracy5;
	num=num-1;//label[] begin from 0
	for(k=0;k<5;k++)
	{
		data_max=searchTop[0];
		max=0;
		for(i=0;i<output_config[output_n];i++)
		{
		  if(data_max<searchTop[i])
		  {
			data_max=searchTop[i];
			max=i;
		  }
		}
		predictionTop5[k]=max;
		searchTop[max]=-1;//min
	}
	//top-k
	for(i=0;i<5;i++)
	{
		if(predictionTop5[i]==label[num])
		{
			accuracy5++;
			if(i==0)
				accuracy1++;
			break;
		}
	}
	tmp_accuracy1=accuracy1/(num+1);
	tmp_accuracy5=accuracy5/(num+1);

	printf("Current Top-1 accuracy = %5.3f\n", tmp_accuracy1);
	printf("Current Top-5 accuracy = %5.3f\n", tmp_accuracy5);

}
#endif

// Release all memory resources here
void cleanup()
{

	// Release the opencl runtime resource allocated
	for(unsigned i = 0; i < num_devices; ++i) {
		if(knl_memRd && knl_memRd[i]) {
			clReleaseKernel(knl_memRd[i]);
		}
		if(knl_conv && knl_conv[i]) {
			clReleaseKernel(knl_conv[i]);
		}
		if(knl_memWr && knl_memWr[i]) {
			clReleaseKernel(knl_memWr[i]);
		}
		if(knl_pool && knl_pool[i]) {
			clReleaseKernel(knl_pool[i]);
		}
		if(que_memRd && que_memRd[i]) {
			clReleaseCommandQueue(que_memRd[i]);
		}
		if(que_conv && que_conv[i]) {
			clReleaseCommandQueue(que_conv[i]);
		}
		if(que_memWr && que_memWr[i]) {
			clReleaseCommandQueue(que_memWr[i]);
		}
		if(que_pool && que_pool[i]) {
			clReleaseCommandQueue(que_pool[i]);
		}
		if(data_buf && data_buf[i]) {
			clReleaseMemObject(data_buf[i]);
		}
		if(pool_buf && pool_buf[i]) {
			clReleaseMemObject(pool_buf[i]);
		}
		if(output_buf && output_buf[i]) {
			clReleaseMemObject(output_buf[i]);
		}
		if(weights_buf && weights_buf[i]) {
			clReleaseMemObject(weights_buf[i]);
		}
		if(bias_buf && bias_buf[i]) {
			clReleaseMemObject(bias_buf[i]);
		}
		if(fc_1_buf && fc_1_buf[i]) {
			clReleaseMemObject(fc_1_buf[i]);
		}
		if(fc_2_buf && fc_2_buf[i]) {
			clReleaseMemObject(fc_2_buf[i]);
		}
		// Binary
		if(knl_conv_bin && knl_conv_bin[i]) {
			clReleaseKernel(knl_conv_bin[i]);
		}
		if(que_conv_bin && que_conv_bin[i]) {
			clReleaseCommandQueue(que_conv_bin[i]);
		}
		if(output_buf_bin && output_buf_bin[i]){
			clReleaseMemObject(output_buf_bin[i]);
		}
		if(pool_buf_bin && pool_buf_bin[i]){
			clReleaseMemObject(pool_buf_bin[i]);
		}
		if(scale_buf_bin && scale_buf_bin[i]){
			clReleaseMemObject(scale_buf_bin[i]);
		}

#ifdef LAYER_ELT
		if(knl_elt && knl_elt[i]) {
			clReleaseKernel(knl_elt[i]);
		}
		if(eltwise_buf && eltwise_buf[i]) {
			clReleaseMemObject(eltwise_buf[i]);
		}
#endif
	}

	if(program) {
		clReleaseProgram(program);
	}
	if(context) {
		clReleaseContext(context);
	}

	// Release the memory resource allocated
	alignedFree(weights);
	alignedFree(image);

	alignedFree(data_init);
	for(int j=0; j<LAYER_NUM; j++){
		alignedFree(weight_conv[j]);
		alignedFree(bias_conv[j]);

	}
	alignedFree(golden_ref);
	alignedFree(output);
	alignedFree(output_reorder);
	alignedFree(output_one_item);

	alignedFree(output_bin);
	alignedFree(output_reorder_bin);
}
