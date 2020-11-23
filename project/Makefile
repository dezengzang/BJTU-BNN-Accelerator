#set source and target for host
HOST_EXE  = run.exe
HOST_SRCS = ./host/main.cpp ../common/ocl_util.cpp ../common/timer.cpp
HOST_OBJS = $(HOST_SRCS:%.cpp=%.o)
HOST_INCS = ../common
HOST_DEBUG =

ABS_COMMON_REPO = $(shell readlink -f $(HOST_INCS))

#if you are loading images from OpenCV interfaces please set to 1
USE_OPENCV = 0

#set source and target for device
#select the host archecture  x86|aarch32|aarch64
PLATFORM = x86
# PLATFORM = aarch64
# PLATFORM = aarch32

#choose the BSP you would like to use
DEVICE = xilinx_u50_gen3x16_xdma_201920_3
# DEVICE := xilinx_zcu102_base_202010_1
# DEVICE := xilinx_zc706_base_202010_1

#optional flows are hw|hw_emu|sw_emu|profile|hw_sim
#hw      -> generate hw for deployment
#hw_emu  -> generate waveform for SDAccel
#sw_emu  -> perform software emulation
#hw_sim  -> co-sim with Modelsim for Intel OpenCL SDK
#profile -> insert performance counters in fpga for dynamic profiling
FLOW        = hw

#set the Linux related tools
EDGE_COMMON_SW := /opt/petalinux/2020.1
# EDGE_COMMON_SW := /opt/petalinux/2020.1_zc706

ifeq ($(PLATFORM), aarch64)
SYSROOT := $(EDGE_COMMON_SW)/sysroots/aarch64-xilinx-linux
SD_IMAGE_FILE := $(EDGE_COMMON_SW)/Image
#CXX := $(XILINX_VITIS)/gnu/aarch64/lin/aarch64-linux/bin/aarch64-linux-gnu-g++
else ifeq ($(PLATFORM), aarch32)
SYSROOT := $(EDGE_COMMON_SW)/sysroots/cortexa9t2hf-neon-xilinx-linux-gnueabi/
SD_IMAGE_FILE := $(EDGE_COMMON_SW)/uImage
#CXX := $(XILINX_VITIS)/gnu/aarch32/lin/gcc-arm-linux-gnueabi/bin/arm-linux-gnueabihf-g++
endif


#select which kernel file is used
KERNEL_SRCS = ./device/conv_pipe_xilinx.cl
KERNEL_NAME = conv
KERNEL_DEFS =
KERNEL_INCS = 
KERNEL_DEBUG = 1

TEMP_DIR = ./tmp.$(FLOW)
BUILD_DIR = ./build.$(FLOW)
EMCONFIG_DIR = .
KERNEL_OBJS = $(TEMP_DIR)/$(KERNEL_NAME).xo
PACKAGE_OUT = ./sd_card

#set the device binary name
DEV_EXE = $(KERNEL_NAME).xclbin

#host compiler options
CROSS-COMPILE = arm-linux-gnueabihf-
ifeq ($(PLATFORM),x86)
CXX = g++
ifeq ($(USE_OPENCV),1)
#add your OpenCV PATH here
#OCV_INCLUDES = -I/usr/local/include/
#OCV_LIBDIRS = -L/usr/local/lib 
#for opencv_2.x
#OCV_LIBS =  -lopencv_core -lopencv_imgproc -lopencv_highgui -lopencv_ml
#for opencv_3.x
#OCV_LIBS =  -lopencv_core -lopencv_imgcodecs -lopencv_imgproc -lopencv_highgui -lopencv_ml
endif
else ifeq ($(PLATFORM),aarch32)
CXX := $(XILINX_VITIS)/gnu/aarch32/lin/gcc-arm-linux-gnueabi/bin/arm-linux-gnueabihf-g++
ifeq ($(USE_OPENCV),1)
#add your cross compile OpenCV PATH here
#OCV_INCLUDES = -I/usr/local/opencv-arm/include/
#OCV_LIBDIRS = -L/usr/local/opencv-arm/lib 
#OCV_LIBS =  -lopencv_core -lopencv_imgproc -lopencv_highgui -lopencv_ml
endif
else ifeq ($(PLATFORM),aarch64)
CXX := $(XILINX_VITIS)/gnu/aarch64/lin/aarch64-linux/bin/aarch64-linux-gnu-g++
endif

#select whether use OpenCV or not
ifeq ($(USE_OPENCV),1)
CXXFLAGS = -g -Wall -std=c++11 -DUSE_OPENCV -DXILINX -DCL_TARGET_OPENCL_VERSION=120
else
CXXFLAGS = -Wall -O0 -g -std=c++11 -fmessage-length=0 -DXILINX -DCL_TARGET_OPENCL_VERSION=120
endif

# Definition of include file locations
ifneq ($(PLATFORM), x86)
    xrt_path = $(SYSROOT)/usr
    OPENCL_INCLUDE = $(xrt_path)/include
else
    xrt_path = $(XILINX_XRT)
    OPENCL_INCLUDE = $(xrt_path)/include
endif

COMP_CONFIG = -I$(OPENCL_INCLUDE) -I$(XILINX_VIVADO)/include -I$(HOST_INCS) -DFPGA_DEVICE
LINK_CONFIG = -L$(xrt_path)/lib -lOpenCL -lpthread -lrt -lstdc++ 

ifneq ($(PLATFORM), x86)
LINK_CONFIG += --sysroot=$(SYSROOT)
endif


#set opencl compiler
OCC = v++

#opencl compiler options
ifeq ($(FLOW),sw_emu)
OCCFLAGS = -t $(FLOW) --platform $(DEVICE) --save-temps
else ifeq ($(FLOW),hw)
OCCFLAGS = -t $(FLOW) --platform $(DEVICE) --save-temps --kernel_frequency 300
else ifeq ($(FLOW),profile)
OCCFLAGS = -t $(FLOW) --platform $(DEVICE) --save-temps  --profile_kernel data:all:all:all
else ifeq ($(FLOW),hw_sim)
OCCFLAGS = -t $(FLOW) --platform $(DEVICE) --save-temps
endif

.PHONY: all
all: host fpga

.PHONY: host
host: $(HOST_EXE)

.PHONY: fpga
fpga: $(BUILD_DIR)/$(DEV_EXE)

gen_run_app:
	rm -rf run_app.sh
	@echo 'export LD_LIBRARY_PATH=/mnt:/tmp:$(LD_LIBRARY_PATH)' >> run_app.sh
	@echo 'export XILINX_XRT=/usr' >> run_app.sh
ifeq ($(FLOW),$(filter $(FLOW),sw_emu hw_emu))
	@echo 'export XILINX_VITIS=/mnt' >> run_app.sh
	@echo 'export XCL_EMULATION_MODE=$(FLOW)' >> run_app.sh
endif
	@echo  './$(HOST_EXE) $(KERNEL_NAME).xclbin' >> run_app.sh
	@echo  'return_code=$$?' >> run_app.sh
	@echo  'if [ $$return_code -ne 0 ]; then' >> run_app.sh
	@echo  'echo "ERROR: host run failed, RC=$$return_code"' >> run_app.sh
	@echo  'fi' >> run_app.sh
	@echo  'echo "INFO: host run completed."' >> run_app.sh

image: gen_run_app
	rm -rf $(PACKAGE_OUT)
	$(OCC) -t $(FLOW) --platform $(DEVICE) -p $(BUILD_DIR)/$(KERNEL_NAME).xclbin --package.out_dir $(PACKAGE_OUT) --package.rootfs $(EDGE_COMMON_SW)/rootfs.ext4 --package.sd_file $(SD_IMAGE_FILE) --package.sd_file xrt.ini --package.sd_file run_app.sh --package.sd_file $(HOST_EXE) -o $(KERNEL_NAME).xclbin

$(HOST_EXE): $(HOST_OBJS)
	$(CXX) $(OCV_LIBDIRS) $(OCV_INCLUDES) $(HOST_OBJS) -o $@ $(LINK_CONFIG) $(OCV_LIBS)

%.o: %.cpp
	$(CXX) $(OCV_LIBDIRS) $(OCV_INCLUDES) $(CXXFLAGS) $(COMP_CONFIG) $(OCV_LIBS) -c $< -o $@ 

$(KERNEL_OBJS): $(KERNEL_SRCS)
	mkdir -p $(TEMP_DIR)
	$(OCC) $(OCCFLAGS) --temp_dir $(TEMP_DIR) -c -I'$(<D)' -o'$@' '$<'

$(BUILD_DIR)/$(DEV_EXE): $(KERNEL_OBJS)
	mkdir -p $(BUILD_DIR)
	$(OCC) $(OCCFLAGS) --temp_dir $(BUILD_DIR)  -l $(LDCLFLAGS) -o'$@' $(+)
	cp $(BUILD_DIR)/$(DEV_EXE) ./$(DEV_EXE)

.PHONY: run
run:
	./run.exe $(KERNEL_NAME).xclbin

.PHONY: log
log:
	./run.exe $(KERNEL_NAME).xclbin | tee >run.log

.PHONY: emu
emu:
ifeq ($(PLATFORM), x86)
	emconfigutil --platform $(DEVICE) --od $(EMCONFIG_DIR)
	XCL_EMULATION_MODE=$(FLOW) ./run.exe $(KERNEL_NAME).xclbin
else
	$(ABS_COMMON_REPO)/utility/run_emulation.pl "$(PACKAGE_OUT)/launch_$(FLOW).sh | tee embedded_run.log" "./run_app.sh $(FLOW)" "TEST PASSED" "7"
endif


.PHONY: clean
clean:
	rm -rf $(HOST_EXE) *sw_emu* *hw_emu* .run* $(HOST_INCS)/*.o host/*.o run_app.sh
	rm -rf profile_* TempConfig system_estimate.xtxt *.rpt *.csv *.package_summary
	rm -rf host/*.ll *v++* .Xil emconfig.json dltmp* xmltmp* *.log *.jou *.wcfg *.wdb

cleanall: clean
	rm -rf build* sd_card* tmp* .ipcache
	rm -rf _x* *xclbin.run_summary qemu-memory-_* emulation/ _vimage/ pl* start_simulation.sh *.xclbin
