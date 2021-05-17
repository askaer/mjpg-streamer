#ifndef INPUT_GSTREAMER_H_
#define INPUT_GSTREAMER_H_

#include "../../mjpg_streamer.h"
//#include "../../utils.h"

int input_init(input_parameter* param, int id);
int input_stop(int id);
int input_run(int id);
int input_cmd(int plugin, unsigned int control_id, unsigned int typecode, int value);


#endif /* INPUT_GSTREAMER_H_ */