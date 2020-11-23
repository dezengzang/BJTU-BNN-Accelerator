import sys
import numpy as np
import pdb

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print ('Usage: python [lane_num] [vec_size]')
        exit(1)
    lane_num = int(sys.argv[1])
    vec_size = int(sys.argv[2])
    code_str = '#ifndef _PIPE_H\n'+'#define _PIPE_H\n'
    # channel_vec
    pipe_depth = int(2**np.ceil(np.log2(32*lane_num*vec_size)))
    code_str += 'pipe char data_ch __attribute__((xcl_reqd_pipe_depth({})));\n'.format(pipe_depth) +\
                'pipe char weight_ch __attribute__((xcl_reqd_pipe_depth({})));\n'.format(pipe_depth)

    # channel_scal 
    pipe_depth = int(2**np.ceil(np.log2(32*lane_num)))
    code_str += 'pipe char bias_ch __attribute__((xcl_reqd_pipe_depth({})));\n'.format(pipe_depth) +\
                'pipe char conv_ch __attribute__((xcl_reqd_pipe_depth({})));\n'.format(pipe_depth) +\
                'pipe char batchNorm_ch __attribute__((xcl_reqd_pipe_depth({})));\n'.format(pipe_depth) +\
                'pipe char bypass_bn_ch __attribute__((xcl_reqd_pipe_depth({})));\n'.format(pipe_depth)

    # channel_vec
    list_name = ['data','weight']
    for list_item in list_name:
        code_str += '#define {}_write_pipe_block(input_data)  '.format(list_item) +'{' +\
                    'char temp[{}];\\\n'.format(lane_num*vec_size)
        count = 0
        for i in range(0,lane_num):
            for j in range(0,vec_size):
                count = count + 1
                if (count <= (lane_num*vec_size-1)):
                    code_str += '                                           temp[{}] = input_data.lane[{}].data[{}]; \\\n'.format(count-1, i, j) +\
                                '                                           write_pipe_block({}_ch, &temp[{}]);\\\n'.format(list_item,count-1)
                else:
                    code_str += '                                           temp[{}] = input_data.lane[{}].data[{}]; \\\n'.format(count-1, i, j) +\
                                '                                           write_pipe_block({}_ch, &temp[{}]);'.format(list_item,count-1) +'}\n\n'
        
        code_str += '#define {}_read_pipe_block(input_data)  '.format(list_item) +'{' +\
                    'char temp[{}];\\\n'.format(lane_num*vec_size)
        count = 0
        for i in range(0,lane_num):
            for j in range(0,vec_size):
                count = count + 1
                if (count <= (lane_num*vec_size-1)):
                    code_str += '                                           read_pipe_block({}_ch, &temp[{}]);\\\n'.format(list_item,count-1) +\
                                '                                           input_data.lane[{}].data[{}] = temp[{}]; \\\n'.format(i,j,count-1)
                else:
                    code_str += '                                           read_pipe_block({}_ch, &temp[{}]);\\\n'.format(list_item,count-1) +\
                                '                                           input_data.lane[{}].data[{}] = temp[{}];'.format(i,j,count-1) +'}\n\n'
        
        
    # channel_scal
    list_name = ['bias_ch','conv_ch','batchNorm_ch','bypass_bn_ch']
    for list_item in list_name:
        code_str += '#define {}_write_pipe_block(input_data)  '.format(list_item) +'{' +\
                    'char temp[{}];\\\n'.format(lane_num)
        count=0
        for i in range(0,lane_num):
            count = count + 1
            if (count <= (lane_num-1)):
                code_str += '                                           temp[{}] = input_data.lane[{}]; \\\n'.format(count-1,i) +\
                            '                                           write_pipe_block({}, &temp[{}]);\\\n'.format(list_item,count-1)
            else:
                code_str += '                                           temp[{}] = input_data.lane[{}]; \\\n'.format(count-1,i) +\
                            '                                           write_pipe_block({}, &temp[{}]);'.format(list_item,count-1) +'}\n\n'
        code_str += '#define {}_read_pipe_block(input_data)  '.format(list_item) +'{' +\
                    'char temp[{}];\\\n'.format(lane_num)
        count=0
        for i in range(0,lane_num):
            count = count + 1
            if (count <= (lane_num-1)):
                code_str += '                                           read_pipe_block({}, &temp[{}]);\\\n'.format(list_item,count-1) +\
                            '                                           input_data.lane[{}] = temp[{}]; \\\n'.format(i,count-1)
    
            else:
                code_str += '                                           read_pipe_block({}, &temp[{}]);\\\n'.format(list_item,count-1) +\
                            '                                           input_data.lane[{}] = temp[{}];'.format(i,count-1) +'}\n\n'
    
    code_str += 'pipe bool pool_sync_ch __attribute__((xcl_reqd_pipe_depth(32)));\n' +\
            '#define pool_sync_ch_write_pipe_block(input_data) { bool temp; \\\n' +\
            '                                                    temp = input_data; \\\n' +\
            '                                                    write_pipe_block(pool_sync_ch, &temp); }\n' +\
            '#define pool_sync_ch_read_pipe_block(input_data)  { bool temp; \\\n' +\
            '                                                    read_pipe_block (pool_sync_ch, &temp);\\\n' +\
            '                                                    input_data = temp;}\n\n'
    
    code_str += '#endif\n'
    fd = open('pipe.cl', 'w')
    fd.write(code_str)
    fd.close()
