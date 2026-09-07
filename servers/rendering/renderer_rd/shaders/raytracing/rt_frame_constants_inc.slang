#ifndef RT_FRAME_CONSTANTS_INC_SLANG
#define RT_FRAME_CONSTANTS_INC_SLANG

struct RTFrameConstants {
	float4 camera_to_rt[3];
	float4 previous_camera_to_rt[3];
};

float4x4 rt_camera_to_rt(RTFrameConstants frame, bool previous_frame) {
	return previous_frame ?
			float4x4(frame.previous_camera_to_rt[0], frame.previous_camera_to_rt[1], frame.previous_camera_to_rt[2], float4(0.0, 0.0, 0.0, 1.0)) :
			float4x4(frame.camera_to_rt[0], frame.camera_to_rt[1], frame.camera_to_rt[2], float4(0.0, 0.0, 0.0, 1.0));
}

#endif
