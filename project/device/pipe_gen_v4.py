
import sys
import numpy as np
import pdb

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print ('Usage: python [lane_num] [vec_size]')
        exit(1)
    lane_num = int(sys.argv[1])
    vec_size = int(sys.argv[2])

    code_str = '#ifndef _PIPE_H\n'+'#define _PIPE_H\n\n'
    # channel_vec intN
    intN = int(2**np.ceil(np.log2(lane_num*vec_size/4)))
    pipe_depth = 32
    code_str += 'pipe int{} data_ch __attribute__((xcl_reqd_pipe_depth({})));\n'.format(intN,pipe_depth) +\
                'pipe int{} weight_ch __attribute__((xcl_reqd_pipe_depth({})));\n'.format(intN,pipe_depth) +\
                'pipe int{} data_bin_ch __attribute__((xcl_reqd_pipe_depth({})));\n'.format(intN,pipe_depth) +\
                'pipe int{} weight_bin_ch __attribute__((xcl_reqd_pipe_depth({})));\n'.format(intN,pipe_depth)

    list_name = ['data', 'weight', 'data_bin', 'weight_bin']
    for list_item in list_name:
        #write pipe
        code_str += '#define {}_write_pipe_block(input_data)  '.format(list_item) +'{' +\
                    'int{} temp_{} = (int{})(0); char* temp_char_{} = &temp_{};\\\n'.format(intN,list_item,intN,list_item,list_item)
        for i in range(lane_num*vec_size):
            code_str += '                                           temp_char_{}[{}] = input_data.lane[{}].data[{}]; \\\n'.format(list_item,i, i/vec_size, i%vec_size)
        code_str += '                                           write_pipe_block({}_ch, &temp_{});'.format(list_item,list_item) +'}\n\n'
        
        #read pipe
        code_str += '#define {}_read_pipe_block(input_data)  '.format(list_item) +'{' +\
                    'int{} temp_{} = (int{})(0); char* temp_char_{} = &temp_{};\\\n'.format(intN,list_item,intN,list_item,list_item)
        code_str += '                                           read_pipe_block({}_ch, &temp_{});\\\n'.format(list_item,list_item)
        for i in range(lane_num*vec_size):
            if (i < (lane_num*vec_size-1)):
                code_str += '                                           input_data.lane[{}].data[{}] = temp_char_{}[{}]; \\\n'.format(i/vec_size, i%vec_size,list_item,i)
            else:
                code_str += '                                           input_data.lane[{}].data[{}] = temp_char_{}[{}];'.format(i/vec_size, i%vec_size,list_item,i) +'}\n\n'

    # channel_scal intN
    intN = int(2**np.ceil(np.log2(lane_num/4)))
    pipe_depth = 32
    code_str += 'pipe int{} bias_ch __attribute__((xcl_reqd_pipe_depth({})));\n'.format(intN,pipe_depth) +\
                'pipe int{} conv_ch __attribute__((xcl_reqd_pipe_depth({})));\n'.format(intN,pipe_depth) +\
                'pipe int{} bias_ch_bin_ch __attribute__((xcl_reqd_pipe_depth({})));\n'.format(intN,pipe_depth) +\
                'pipe int{} conv_ch_bin_ch __attribute__((xcl_reqd_pipe_depth({})));\n'.format(intN,pipe_depth) +\
                'pipe int{} scale_ch_bin_ch __attribute__((xcl_reqd_pipe_depth({})));\n'.format(intN,pipe_depth)

    list_name = ['bias_ch','conv_ch','bias_ch_bin_ch','conv_ch_bin_ch','scale_ch_bin_ch']
    for list_item in list_name:
        #write pipe
        code_str += '#define {}_write_pipe_block(input_data)  '.format(list_item) +'{' +\
                    'int{} temp_{} = (int{})(0); char* temp_char_{} = &temp_{};\\\n'.format(intN,list_item,intN,list_item,list_item)
        for i in range(lane_num):
            code_str += '                                           temp_char_{}[{}] = input_data.lane[{}]; \\\n'.format(list_item,i,i)
        code_str += '                                           write_pipe_block({}, &temp_{});'.format(list_item,list_item) +'}\n\n'

        code_str += '#define {}_read_pipe_block(input_data)  '.format(list_item) +'{' +\
                    'int{} temp_{} = (int{})(0); char* temp_char_{} = &temp_{};\\\n'.format(intN,list_item,intN,list_item,list_item)
        code_str += '                                           read_pipe_block({}, &temp_{});\\\n'.format(list_item,list_item)
        for i in range(lane_num):
            if i < lane_num-1:
                code_str += '                                           input_data.lane[{}] = temp_char_{}[{}]; \\\n'.format(i,list_item,i)
            else:
                code_str += '                                           input_data.lane[{}] = temp_char_{}[{}];'.format(i,list_item,i) +'}\n\n'

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
