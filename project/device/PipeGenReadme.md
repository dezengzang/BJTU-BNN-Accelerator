# 各个PIPE_GEN说明
## pipe_gen

    为结构体中的各个字节创建各自的pipe

## pipe_gen_v2

    为每个结构体创建一个pipe，把结构体中的字节串行依次送入pipe

## pipe_gen_v3

    使用char16数据类型设计设计pipe，使用char16作为临时变量temp，将结构体中的数据依次存入temp，最后将temp送入pipe，最大并行度为16*1=16，比较死，不行

## pipe_gen_v4

    使用int16数据类型设计设计pipe，使用int16作为临时变量temp，将结构体中的数据依次存入temp，最后将temp送入pipe，最大并行度为16*4=64，        

## pipe_gen_v5
    直接将结构体送入pipe，但是需要结构体的大小和声明的pipe类型相符，示例如下。使用了int16声明了pipe，size_of(int16)=64，那么结构体的大小也必须是64

# 综合比较上面几个版本，v4和v5可能比较好用
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