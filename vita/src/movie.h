/*
 * PixCarsVita - full-screen video playback (Vita only): the game's Bink films converted by
 * tools/vitamovies.py to MP4 (H.264 + AAC), decoded in hardware by SceAvPlayer; frames are
 * bound to a GXM texture as-is (two-plane YUV), audio goes to its own 48 kHz port.
 */
#ifndef PIXCARS_MOVIE_H
#define PIXCARS_MOVIE_H

/* Start a video; returns 0 on success (nothing plays if the file is missing). */
int movie_start(const char *path);
/* Draw the current frame full-screen; returns 1 while the video is playing. */
int movie_draw(void);
void movie_stop(void);

#endif
