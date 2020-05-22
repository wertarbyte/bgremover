#ifndef DEBUG_H
#define DEBUG_H

enum DebugFlags {
    DebugFlagShowOutputFrame = 1 << 0,      // show_output_frame
    DebugFlagShowModelInputFrame = 1 << 1,  // show_model_input_frame
    DebugFlagShowModelOutput = 1 << 2,      // show_model_output
};

extern int debug_flags;  // main.cc

#endif  // DEBUG_H
