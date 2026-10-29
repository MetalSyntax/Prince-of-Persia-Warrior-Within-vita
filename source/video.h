/**
 * @file video.h
 * @brief Cutscene video playback subsystem using FFmpeg software decoding.
 */
#ifndef POP_VIDEO_H
#define POP_VIDEO_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the video playback subsystem and mutexes.
 */
void video_init(void);

/**
 * @brief Releases video player textures and buffers.
 */
void video_shutdown(void);

/**
 * @brief Plays a video file (M4V/MP4 with MPEG-4 or H.264 video and AAC audio).
 * @param name File name or path to the video cutscene.
 * @return 1 on success, 0 on failure.
 */
int video_play(const char *name);

// H.264 (avc1) via SceAvcdec hardware: vitasdk's libavcodec ships no h264 decoder. Takes a full path.
int video_play_avc(const char *path);

#ifdef __cplusplus
}
#endif

#endif // POP_VIDEO_H
