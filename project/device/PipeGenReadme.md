# Description of each PIPE_GEN
## pipe_gen

    Create a separate pipe for each byte in the structure.

## pipe_gen_v2

    Create a pipe for each construct, and feed the bytes from the construct into the pipe serially in sequence.

## pipe_gen_v3

    Using char16 data type design design pipe, using char16 as a temporary variable temp, the data in the structure are saved into temp sequentially, and finally send temp into the pipe, the maximum parallelism is 16 * 1 = 16, more inflexible, not good.

## pipe_gen_v4

    Design the pipe using the int16 data type, use int16 as a temporary variable temp, store the data in the structure in temp, and finally feed temp into the pipe with a maximum parallelism of 16*4=64.

## pipe_gen_v5
    The structure is fed directly into the pipe, but the size of the structure must match the declared pipe type, as shown in the following example. The pipe is declared with int16, size_of(int16) = 64, so the size of the construct must be 64 as well.

# Comparing the above versions, v4 and v5 are probably better.
```C
// vec=4
// lan=16

#ifndef _PIPE_H
#define _PIPE_H

pipe int16 data_ch __attribute__((xcl_reqd_pipe_depth(32)));
pipe int16 weight_ch __attribute__((xcl_reqd_pipe_depth(32)));
#define data_write_pipe_block(input_data)  {write_pipe_block(data_ch, (int16*)(&input_data));}
#define data_read_pipe_block(input_data)   {read_pipe_block(data_ch, (int16*)(&input_data));}
#define weight_write_pipe_block(input_data)  {write_pipe_block(weight_ch, (int16*)(&input_data));}
#define weight_read_pipe_block(input_data)   {read_pipe_block(weight_ch, (int16*)(&input_data));}

pipe int4 bias_ch __attribute__((xcl_reqd_pipe_depth(32)));
pipe int4 conv_ch __attribute__((xcl_reqd_pipe_depth(32)));
pipe int4 batchNorm_ch __attribute__((xcl_reqd_pipe_depth(32)));
pipe int4 bypass_bn_ch __attribute__((xcl_reqd_pipe_depth(32)));
#define bias_ch_write_pipe_block(input_data)  {write_pipe_block(bias_ch, (int4*)(&input_data));}
#define bias_ch_read_pipe_block(input_data)   {read_pipe_block(bias_ch, (int4*)(&input_data));}
#define conv_ch_write_pipe_block(input_data)  {write_pipe_block(conv_ch, (int4*)(&input_data));}
#define conv_ch_read_pipe_block(input_data)   {read_pipe_block(conv_ch, (int4*)(&input_data));}
#define batchNorm_ch_write_pipe_block(input_data)  {write_pipe_block(batchNorm_ch, (int4*)(&input_data));}
#define batchNorm_ch_read_pipe_block(input_data)   {read_pipe_block(batchNorm_ch, (int4*)(&input_data));}
#define bypass_bn_ch_write_pipe_block(input_data)  {write_pipe_block(bypass_bn_ch, (int4*)(&input_data));}
#define bypass_bn_ch_read_pipe_block(input_data)   {read_pipe_block(bypass_bn_ch, (int4*)(&input_data));}

pipe bool pool_sync_ch __attribute__((xcl_reqd_pipe_depth(32)));
#define pool_sync_ch_write_pipe_block(input_data) { bool temp; \
                                                    temp = input_data; \
                                                    write_pipe_block(pool_sync_ch, &temp); }
#define pool_sync_ch_read_pipe_block(input_data)  { bool temp; \
                                                    read_pipe_block (pool_sync_ch, &temp);\
                                                    input_data = temp;}

#endif
```