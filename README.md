# lintel


# loadvid test

1. Run:
   `CUDA_VISIBLE_DEVICES= python3 -m loadvid_test --filename <video-filename> --width <width> --height <height>`

   Pass criteria: decoded frames from the video should show up without
   distortion, decoding each clip in < 500ms.
