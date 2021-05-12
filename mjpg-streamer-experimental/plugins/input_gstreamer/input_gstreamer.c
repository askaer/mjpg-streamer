#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <pthread.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <time.h>
#include <pthread.h>
#include <getopt.h>
#include <stdint.h>
#include "input_gstreamer.h"

#include "../../utils.h"

#define INPUT_PLUGIN_NAME "OV5640 GStreamer grabber"

static globals *pglobal;

struct my_context {
    int id;
    globals *pglobal;
    pthread_t threadID;
    GstAppSink *appsink;
    int video_dev;
    int active_flag;
};

struct ov5640_mode {
    const int mode_nr;
    const char *string;
    const int width, height;
};

static const struct ov5640_mode camera_modes[] = {
    { 0, "VGA",   640,  480  },
    { 1, "QVGA",  320,  240  },
    { 2, "NTSC",  720,  480  },
    { 3, "PAL",   720,  576  },
    { 4, "HD",    1280, 720  },
    { 5, "FHD",   1920, 1080 },
    { 6, "QSXGA", 2592, 1944 },
    { 7, "QCIF",  176,  144  },
    { 8, "XGA",   1024, 768  },
};
/*
enum ov5640_mode {
        ov5640_mode_MIN = 0,
        ov5640_mode_VGA_640_480 = 0,
        ov5640_mode_QVGA_320_240 = 1,
        ov5640_mode_NTSC_720_480 = 2,
        ov5640_mode_PAL_720_576 = 3,
        ov5640_mode_720P_1280_720 = 4,
        ov5640_mode_1080P_1920_1080 = 5,
        ov5640_mode_QSXGA_2592_1944 = 6,
        ov5640_mode_QCIF_176_144 = 7,
        ov5640_mode_XGA_1024_768 = 8,
        ov5640_mode_MAX = 8,
        ov5640_mode_INIT = 0xff,
};
*/
static GstElement *pipeline = NULL;
static GstElement *sink = NULL;

void *cam_thread(void *arg);
void *cam_thread2(void *arg);

void help(void)
{
    fprintf(stderr, " ---------------------------------------------------------------\n" \
    " Help for input plugin..: "INPUT_PLUGIN_NAME"\n" \
    " ---------------------------------------------------------------\n" \
    " The following parameters can be passed to this plugin:\n\n" \
    " [-d | --device ].......: video device to open (your camera)\n" \
    " [-r | --resolution ]...: resolution\n" \
    " [-f | fps ]............: frame rate (15 or 30)\n" \
    " [-s | swjpeg ].........: use sw jpeg encoding\n" \
    "                          ");
}

const struct ov5640_mode *find_camera_mode(const char *optarg)
{
    int i, width, height;
    const struct ov5640_mode *p = &camera_modes[0];
    const struct ov5640_mode *end = &camera_modes[LENGTH_OF(camera_modes)];

    for (p = &camera_modes[0]; p != end; p++) {
        if (strcmp(p->string, optarg) == 0) {
            return p;
        }
    }

    if (sscanf(optarg, "%dx%d", &width, &height) == 2) {
        for (p = &camera_modes[0]; p != end; p++) {
            if (width == p->width && height == p->height)
                return p;
        }
    }

    fprintf(stderr, "Invalid resolution or height/width '%s' specified!\n", optarg);

    return NULL;
}

typedef struct  {
    GstPad *preview_pad;
    GstPad *snapshot_pad;
    GstPad *source_pad;
    int to_snapshot;
} switch_pad_info_t;

static switch_pad_info_t switch_pad_info;

static pthread_mutex_t snapshot_mtx = PTHREAD_MUTEX_INITIALIZER;
static int snapshot_count = 0;
typedef enum {
    stateIdle,
    statePreview,
    statePreviewToSnap,
    stateSnap,
    stateSnapToPreview
} pipeline_state_t;

static pipeline_state_t pipeline_state = stateIdle;

/*
static GstPadProbeReturn snapshot_rewire_callback(GstPad *pad, GstPadProbeInfo *info, gpointer data);
static void install_snapshot_rewire_callback()
{
    gst_pad_add_probe(switch_pad_info.source_pad, GST_PAD_PROBE_TYPE_BLOCK,
        snapshot_rewire_callback, NULL, NULL);
}

static void do_rewire()
{
    if (pipeline_state == stateSnap) {
        gst_pad_unlink(switch_pad_info.source_pad, switch_pad_info.preview_pad);
        gst_pad_link(switch_pad_info.source_pad, switch_pad_info.snapshot_pad);
        g_print("linked to snapshot\n");
    }
    else {
        gst_pad_unlink(switch_pad_info.source_pad, switch_pad_info.snapshot_pad);
        gst_pad_link(switch_pad_info.source_pad, switch_pad_info.preview_pad);
        g_print("linked to preview\n");
    }
}
*/
static GstPadProbeReturn snapshot_data_probe(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
    int a = 0;
    switch (pipeline_state) {
    case statePreview:
        g_print("%s: preview\n", __func__);
        break;

    case statePreviewToSnap:
        g_print("%s: preview->snap\n", __func__);

        snapshot_count = 1;
        gst_pad_unlink(switch_pad_info.source_pad, switch_pad_info.preview_pad);
        gst_pad_link(switch_pad_info.source_pad, switch_pad_info.snapshot_pad);
        g_print("linked to snapshot\n");
        pipeline_state = stateSnap;
        break;

    case stateSnap:
        g_print("%s: snapshot\n", __func__);
        break;

    case stateSnapToPreview:
        g_print("%s: snapshot -> preview\n", __func__);
        gst_pad_unlink(switch_pad_info.source_pad, switch_pad_info.snapshot_pad);
        gst_pad_link(switch_pad_info.source_pad, switch_pad_info.preview_pad);
        g_print("linked to preview\n");
        pipeline_state = statePreview;
        break;

    case stateIdle:
        g_print("%s: idle\n", __func__);
        break;

    default:
        break;
    }
    /*
    if (pipeline_state == stateSnap) {
        snapshot_count = 1;
        //install_snapshot_rewire_callback();
        do_rewire();
        a = 1;
    }

    if (a)
        g_print("   rewire_probe installed\n");
    */
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn snapshot_gate_callback(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
    GstPadProbeReturn retval = GST_PAD_PROBE_DROP;
    g_print("%s\n", __func__);

    if (pipeline_state == stateSnap) {
        pipeline_state = stateSnapToPreview;
        snapshot_count = 0;

        // Pass sample to sink
        retval =  GST_PAD_PROBE_OK;
    }
    return retval;
}

static GstPadProbeReturn pad_debug(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
    GstElement *e = gst_pad_get_parent_element(pad);
    g_print("%s: %s:%s\n", __func__, GST_ELEMENT_NAME(e), GST_PAD_NAME(pad));
}
/*
GstPadProbeReturn snapshot_rewire_callback(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
    g_print("%s\n", __func__);
    if (pipeline_state == stateSnap) {
        gst_pad_unlink(switch_pad_info.source_pad, switch_pad_info.preview_pad);
        gst_pad_link(switch_pad_info.source_pad, switch_pad_info.snapshot_pad);
        g_print("linked to snapshot\n");
    }
    else {
        gst_pad_unlink(switch_pad_info.source_pad, switch_pad_info.snapshot_pad);
        gst_pad_link(switch_pad_info.source_pad, switch_pad_info.preview_pad);
        g_print("linked to preview\n");
    }

    return GST_PAD_PROBE_REMOVE;
}
*/
static void handle_frame(GstSample *sample, input *in)
{
    struct my_context *pctx = (struct my_context *) in->context;
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstMapInfo info;

    pthread_mutex_lock(&pglobal->in[pctx->id].db);

    if (gst_buffer_map(buffer, &info, GST_MAP_READ)) {
        memcpy(in[pctx->id].buf, info.data, info.size);
        in[pctx->id].size = info.size;
        gst_buffer_unmap(buffer, &info);
    }
    else {
        g_printerr("Cannot map GstBuffer!\n");
    }

    pthread_cond_broadcast(&in[pctx->id].db_update);
    pthread_mutex_unlock(&in[pctx->id].db);
}

int input_init(input_parameter* param, int id)
{
    int fps = 30, sw_jpeg = 0;
    const struct ov5640_mode *camera_mode = NULL;
    char *dev = "/dev/video0";
    struct my_context *pctx = calloc(1, sizeof(struct my_context));

    pglobal = param->global;
    pglobal->in[id].context = pctx;
    pctx->id = id;
    pctx->pglobal = param->global;

    param->argv[0] = INPUT_PLUGIN_NAME;

    fprintf(stderr, "%s: Here we are\n", __func__);

    // Handle parameters
    reset_getopt();
    while (1) {
        int option_index = 0, c = 0;
        static struct option long_options[] = {
            {"h", no_argument, 0, 0},
            {"help", no_argument, 0, 0},
            {"d", required_argument, 0, 0},
            {"device", required_argument, 0, 0},
            {"f", required_argument, 0, 0},
            {"fps", required_argument, 0, 0},
            {"s", no_argument, 0, 0},
            {"swjpeg", no_argument, 0, 0},
            {"r", required_argument, 0, 0},
            {"resolution", required_argument, 0, 0},
            {0, 0, 0, 0}
        };

        /* parsing all parameters according to the list above is sufficent */
        c = getopt_long_only(param->argc, param->argv, "", long_options, &option_index);

        /* no more options to parse */
        if(c == -1) break;

        /* unrecognized option */
        if(c == '?') {
            help();
            return 1;
        }

        /* dispatch the given options */
        switch(option_index) {
        /* h, help */
        case 0:
        case 1:
            DBG("case 0,1\n");
            help();
            return 1;
            break;

        /* d, device */
        case 2:
        case 3:
            DBG("case 2,3\n");
            dev = realpath(optarg, NULL);
            sscanf(dev, "/dev/video%d", &pctx->video_dev);
            printf("video_dev == %d\n", pctx->video_dev);
            break;

        /* f, fps */
        case 4:
        case 5:
            DBG("case 4,5\n");
            fps = atoi(optarg);
            if (fps != 15 && fps != 30) {
                fprintf(stderr, "The driver only support 15 or 30 fps!\n");
                return 1;
            }
            break;

        // SW jpeg encoding
        case 6:
        case 7:
            DBG("case 6, 7\n");
            sw_jpeg = 1;
            printf("SW jpeg encoding\n");
            break;

        case 8:
        case 9:
            DBG("case 8, 9\n");
            camera_mode = find_camera_mode(optarg);
            break;
        }
    }

    g_print("Input device is %s\n", dev);
    if (camera_mode == NULL) {
        g_print("Choosing default resolution\n");
        camera_mode = &camera_modes[5];
    }
    g_print("Camera mode %d (%d x %d)\n", camera_mode->mode_nr,
        camera_mode->width, camera_mode->height);

    gst_init (0, NULL);
    fprintf(stderr, "%s: gst_init()\n", __func__);

    pipeline = gst_pipeline_new ("macro-pipeline");
    fprintf(stderr, "%s: pipe = %p\n", __func__, pipeline);

    GstElement *source = gst_element_factory_make("imxv4l2videosrc", "macrocam");
    g_print("source = %p\n", source);
    g_object_set (source, "device", dev, NULL);
    g_object_set (source, "imx-capture-mode", camera_mode->mode_nr, NULL);
    g_object_set (source, "fps-n", fps, NULL);

    GstElement *mjpeg_enc = NULL;

    if (sw_jpeg)
        mjpeg_enc = gst_element_factory_make("jpegenc", "mjpeg");
    else
        mjpeg_enc = gst_element_factory_make("imxvpuenc_mjpeg", "mjpeg");

    if (!mjpeg_enc) {
        fprintf(stderr, "Cannot create %smjpeg_enc!\n",
            (sw_jpeg ? "sw-" : ""));
        return -1;
    }
    g_print("mjpeg_enc = %p\n", mjpeg_enc);

    GstElement *sink = gst_element_factory_make ("appsink", "appsink");
    if (!sink) {
        g_printerr("sink = %p\n", sink);
        return -1;
    }

    g_print("sink is GstAppSink? %d\n", GST_IS_APP_SINK(sink));
    g_print("sink = %p\n", sink);

    pctx->appsink = (GstAppSink *) sink;

    GstElement *identity = gst_element_factory_make("identity", "identity");

    GstElement *imageenc = gst_element_factory_make("jpegenc", "imageenc");
    //g_object_set(imageenc, "snapshot", TRUE, NULL);

    GstElement *videoscale = gst_element_factory_make("videoscale", "videoscale");
    GstElement *capsfilter = gst_element_factory_make("capsfilter", "scalecaps");
    GstCaps *caps = gst_caps_new_simple("video/x-raw",
        "width", G_TYPE_INT, 640,
        "height", G_TYPE_INT, 480, NULL);
    g_object_set(capsfilter, "caps", caps, NULL);
    gst_caps_unref(caps);

    GstElement *multifilesink = gst_element_factory_make("multifilesink", "imagesink");
    g_object_set(multifilesink, "async", FALSE, NULL);
    g_object_set(multifilesink, "location", "%05d.jpg", NULL);

    gst_bin_add_many (GST_BIN (pipeline), source, identity, videoscale,
        capsfilter, imageenc, multifilesink, mjpeg_enc, sink, NULL);

    gst_element_link(source, identity);
    gst_element_link(identity, videoscale);
    gst_element_link(videoscale, capsfilter);
    gst_element_link(capsfilter, mjpeg_enc);
    gst_element_link(mjpeg_enc, sink);

    gst_element_link(imageenc, multifilesink);
    GstPad *imageenc_src_pad = gst_element_get_static_pad(imageenc, "src");

    switch_pad_info.source_pad = gst_element_get_static_pad(identity, "src");
    switch_pad_info.preview_pad = gst_element_get_static_pad(videoscale, "sink");
    switch_pad_info.snapshot_pad = gst_element_get_static_pad(imageenc, "sink");

    gst_pad_add_probe(switch_pad_info.source_pad, GST_PAD_PROBE_TYPE_BUFFER,
        snapshot_data_probe, NULL, NULL);

    gst_pad_add_probe(imageenc_src_pad, GST_PAD_PROBE_TYPE_BUFFER,
        snapshot_gate_callback, NULL, NULL);

    GstPad *sink_pad = gst_element_get_static_pad(multifilesink, "sink");
    GstPad *appsink_pad = gst_element_get_static_pad(sink, "sink");

    gst_pad_add_probe(sink_pad, GST_PAD_PROBE_TYPE_BUFFER, pad_debug, NULL, NULL);
    gst_pad_add_probe(appsink_pad, GST_PAD_PROBE_TYPE_BUFFER, pad_debug, NULL, NULL);

    pthread_mutex_init(&snapshot_mtx, NULL);
    return 0;
}

int input_stop(int id)
{
    fprintf(stderr, "%s: Here we are\n", __func__);

    GstStateChangeReturn ret;

    ret = gst_element_set_state (pipeline, GST_STATE_PAUSED);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr ("Unable to set the pipeline to the PAUSED state.\n");
        gst_object_unref (pipeline);
        return -1;
    }

    input * in = &pglobal->in[id];
    struct my_context *pctx = (struct my_context *) in->context;

    DBG("will cancel camera thread #%02d\n", id);
    pthread_cancel(pctx->threadID);

    return 0;
}

int kickoff(input *in)
{
    //input *in = &pglobal->in[id];
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    double ns = ts.tv_sec * 1e9 + ts.tv_nsec;
    g_printerr("kickoff at %.3f us\n", ns / 1000.0);
    struct my_context *pctx = (struct my_context *) in->context;

    pthread_create(&(pctx->threadID), NULL, cam_thread2, in);
    pthread_detach(pctx->threadID);
}

int input_run(int id)
{
    fprintf(stderr, "%s: Here we are\n", __func__);

    input *in = &pglobal->in[id];
    struct my_context *pctx = (struct my_context *) in->context;

    in->buf = malloc(1048576);

    g_print("run(): input is %p\n", &in);
    /*
    pthread_create(&(pctx->threadID), NULL, cam_thread, in);
    pthread_detach(pctx->threadID);
    */

    kickoff(in);
    return 0;
}

void cam_cleanup(void *arg)
{
    input *in = (input *) arg;
    struct my_context *pctx = (struct my_context *) in->context;

    IPRINT("cleaning up resources allocated by input thread\n");

    free(in->buf);
    in->buf = NULL;
    in->size = 0;
}

void *cam_thread(void *arg)
{
    input *in = (input *) arg;
    struct my_context *pcontext = (struct my_context *) in->context;
    int first = 1;
    pthread_cleanup_push(cam_cleanup, NULL);

    GstStateChangeReturn ret;

    ret = gst_element_set_state (pipeline, GST_STATE_PLAYING);

    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr ("Unable to set the pipeline to the PLAYING state.\n");
        gst_object_unref (pipeline);
        goto out;
    }

    GstBus *bus;
    GstMessage *msg;

    bus = gst_element_get_bus (pipeline);

    while (!pglobal->stop) {
        //g_print("HEAR YE HEAR YE sink is GstAppSink? %d\n", GST_IS_APP_SINK(pcontext->appsink));
        GstSample *frame = gst_app_sink_pull_sample(pcontext->appsink);

        if (first) {
            first = 0;
            struct timespec t;
            clock_gettime(CLOCK_MONOTONIC_RAW, &t);
            double ns = t.tv_sec * 1e9 + t.tv_nsec;
            g_printerr("First frame at %.3f us\n", ns / 1000.0);
        }

        //g_print("Got frame %p\n", frame);
        handle_frame(frame, in);
        gst_sample_unref(frame);
#if 0
        if (msg != NULL) {
            GError *err;
            gchar *debug_info;
            msg = gst_bus_timed_pop_filtered (bus, GST_CLOCK_TIME_NONE,
                (GstMessageType) (GST_MESSAGE_ERROR | GST_MESSAGE_EOS));

            switch (GST_MESSAGE_TYPE (msg)) {
            case GST_MESSAGE_ERROR:
                gst_message_parse_error (msg, &err, &debug_info);
                g_printerr ("Error received from element %s: %s\n",
                    GST_OBJECT_NAME (msg->src), err->message);
                g_printerr ("Debugging information: %s\n",
                    debug_info ? debug_info : "none");
                g_clear_error (&err);
                g_free (debug_info);
                break;

            case GST_MESSAGE_EOS:
                g_print ("End-Of-Stream reached.\n");
                break;

            default:
                /* We should not reach here because we only asked for ERRORs and EOS */
                g_printerr ("Unexpected message received.\n");
                break;
            }
            gst_message_unref(msg);
        }
#endif
    }

out:
    gst_object_unref (bus);
    gst_element_set_state (pipeline, GST_STATE_NULL);
    gst_object_unref (pipeline);

    pthread_cleanup_pop(1);
    g_print("Exiting cam_thread\n");
}

void *cam_thread2(void *arg)
{
    int cam_running = 0;
    input *in = (input *) arg;
    struct my_context *pcontext = (struct my_context *) in->context;
    int first = 1;
    pthread_cleanup_push(cam_cleanup, NULL);

    GstBus *bus;
    GstMessage *msg;

    bus = gst_element_get_bus (pipeline);

    GstStateChangeReturn ret;
    g_print("starting cam_thread2\n");
    while (!pglobal->stop) {
        switch (cam_running) {
        case 0:
            if (pcontext->active_flag) {
                g_print("ACTIVATING CAMERA\n");
                ret = gst_element_set_state (pipeline, GST_STATE_PLAYING);
                if (ret != GST_STATE_CHANGE_FAILURE) {
                    cam_running = 1;
                    first = 1;
                    g_print("Camera started!\n");
                }
                else {
                    g_print("ret = %d\n", ret);
                }
            }
            else {
                usleep(10000L);
            }
            break;

        case 1:
            if (!pcontext->active_flag) {
                g_print("UNACTIVATE\n");
                ret = gst_element_set_state (pipeline, GST_STATE_PAUSED);
                if (ret != GST_STATE_CHANGE_FAILURE) {
                    cam_running = 0;
                    g_print("Camera stopped!\n");
                }
                else {
                    g_print("ret = %d\n", ret);
                }
            }
            break;

        default:
            break;
        }

        if (cam_running) {
            GstSample *frame = gst_app_sink_pull_sample(pcontext->appsink);

            if (first) {
                first = 0;
                struct timespec t;
                clock_gettime(CLOCK_MONOTONIC_RAW, &t);
                double ns = t.tv_sec * 1e9 + t.tv_nsec;
                g_printerr("First frame at %.3f us\n", ns / 1000.0);
            }

            handle_frame(frame, in);
            gst_sample_unref(frame);

        }
    }

out:
    gst_object_unref (bus);
    gst_element_set_state (pipeline, GST_STATE_NULL);
    gst_object_unref (pipeline);

    pthread_cleanup_pop(1);
    g_print("Exiting cam_thread\n");
}


/*
 struct v4l2_control ctrl;
    if(on)
        ctrl.id = V4L2_CID_AUTO_FOCUS_START;
    else
        ctrl.id = V4L2_CID_AUTO_FOCUS_STOP;
    ctrl.value = 0;
    if(ioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0)
        qWarning() << "IOCTL for " << ctrl.id << " failed";
    close(fd);

    struct v4l2_control ctrl;
    if(on)
        ctrl.id = V4L2_CID_FOCUS_AUTO;
    else
        ctrl.id = V4L2_CID_AUTO_FOCUS_STOP;
    ctrl.value = 0;
    if(ioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0)
        qWarning() << "IOCTL for " << ctrl.id << " failed";
    close(fd);

case V4L2_CID_FOCUS_ABSOLUTE:
                // Custom implementation that sets the ROI
                retval = ov5640_af_set_region((vc->value >> 16),
                                              (vc->value & 0xffff));
                break;
*/

static int open_device(input *in)
{
    int retval = 0;
    char devfile[128] = {0};
    struct my_context *pcontext = (struct my_context *) in->context;
    struct timespec t1, t2;

    sprintf(devfile, "/dev/video%d", pcontext->video_dev);

    clock_gettime(CLOCK_MONOTONIC_RAW, &t1);

    int fd = open(devfile, O_RDWR);
    if (fd < 0) {
        g_printerr("Cannot open video device %s: %d\n", devfile, errno);
        return -1;
    }
    clock_gettime(CLOCK_MONOTONIC_RAW, &t2);

    int64_t ns = (t2.tv_sec * 1000000000L + t2.tv_nsec) - (t1.tv_sec * 1000000000L + t1.tv_nsec);
    g_print("Open device took %.2f us\n", ns / 1000.0);

    return fd;
}

static int get_autofocus_status(int fd)
{
    struct v4l2_control ctrl;
    ctrl.id = V4L2_CID_AUTO_FOCUS_STATUS;
    ctrl.value = 0;
    if (ioctl(fd, VIDIOC_G_CTRL, &ctrl) < 0) {
            perror("V4L2_CID_AUTO_FOCUS_STATUS");
            return -1;
    }
    return ctrl.value;
}

static int lock_autofocus(int fd)
{
    struct v4l2_control ctrl;
    ctrl.id = V4L2_CID_3A_LOCK;
    ctrl.value = V4L2_LOCK_FOCUS;
    if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0) {
            perror("V4L2_CID_3A_LOCK");
            return -1;
    }
    return 0;
}

static int release_focus(int fd)
{
    struct v4l2_control ctrl;
    ctrl.id = V4L2_CID_AUTO_FOCUS_STOP;
    ctrl.value = 0;
    if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0) {
            perror("V4L2_CID_AUTO_FOCUS_STOP");
            return -1;
    }
    return 0;
}

static int start_autofocus_single(int fd)
{
    struct v4l2_control ctrl;
    ctrl.id = V4L2_CID_AUTO_FOCUS_START;
    ctrl.value = 0;
    printf("fd = %d, VIDIOC_S_CTRL, ctrl.id = %x, ctrl.value = %d\n",
            fd, ctrl.id, ctrl.value);
    if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0) {
            perror("V4L2_CID_AUTO_FOCUS_START");
            return -1;
    }
    return 0;
}

static int stop_autofocus(int fd)
{
    struct v4l2_control ctrl;
    ctrl.id = V4L2_CID_AUTO_FOCUS_STOP;
    ctrl.value = 0;
    printf("fd = %d, VIDIOC_S_CTRL, ctrl.id = %x, ctrl.value = %d\n",
            fd, ctrl.id, ctrl.value);
    if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0) {
            perror("V4L2_CID_AUTO_FOCUS_STOP");
            return -1;
    }
    return 0;
}

static int start_autofocus_cont(int fd)
{
    struct v4l2_control ctrl;
    ctrl.id = V4L2_CID_FOCUS_AUTO;
    ctrl.value = 0;
    printf("fd = %d, VIDIOC_S_CTRL, ctrl.id = %x, ctrl.value = %d\n",
            fd, ctrl.id, ctrl.value);
    if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0) {
            perror("V4L2_CID_FOCUS_AUTO");
            return -1;
    }
    return 0;
}

static int set_autofocus_region(int fd, uint16_t x, uint16_t y)
{
    struct v4l2_control ctrl;
    ctrl.id = V4L2_CID_FOCUS_ABSOLUTE;
    ctrl.value = (((uint32_t) x) << 16) | y;
    printf("fd = %d, VIDIOC_S_CTRL, ctrl.id = %x, ctrl.value = %d, 0x%08x\n",
            fd, ctrl.id, ctrl.value, ctrl.value);
    if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0) {
            perror("V4L2_CID_FOCUS_ABSOLUTE");
            return -1;
    }
    return 0;
}

int input_cmd(int plugin, unsigned int control_id, unsigned int typecode, int value)
{
    int res = 0, retval = 0;
    struct v4l2_control vc;
    int fd;

    input in = pglobal->in[plugin];

    input *iin = &pglobal->in[plugin];
    struct my_context *pctx = (struct my_context *) iin->context;

    fprintf(stderr, "%s: Here we are\n", __func__);
    printf("%s: called with: plugin = %d, control_id = %u, typecode = %u, value = %d\n",
        __func__, plugin, control_id, typecode, value);

    switch (control_id) {
    case 1: // Start stream
        g_print("input is %p\n", iin);
        //kickoff(iin);
        pipeline_state = statePreview;
        pctx->active_flag = 1;
        break;

    case 2: // Stop stream
        g_print("Stop stream\n");
        pipeline_state = stateIdle;
        //pglobal->stop = 1;
        pctx->active_flag = 0;
        break;

    case 3: // Run single autofocus
        g_print("opening device...\n");
        fd = open_device(&in);
        if (fd < 0) {
            g_printerr("Cannot open device\n");
            retval = -1;
            break;
        }

        if (start_autofocus_single(fd) < 0) {
            retval = -1;
        }

        close(fd);
        g_print("result = %d\n", res);
        break;

    case 4: // Run continuous autofocus
        g_print("opening device...\n");
        fd = open_device(&in);
        if (fd < 0) {
            g_printerr("Cannot open device\n");
            retval = -1;
            break;
        }

        if (start_autofocus_cont(fd) < 0) {
            retval = -1;
        }

        close(fd);
        g_print("result = %d\n", res);
        break;

    case 5: // Set autofocus region
        {
            uint16_t x = ((uint32_t) value) >> 16;
            uint16_t y = ((uint32_t) value) & 0xffff;

            fd = open_device(&in);
            if (fd < 0) {
                g_printerr("Cannot open device\n");
                retval = -1;
                break;
            }

            if (set_autofocus_region(fd, x, y) < 0) {
                retval = -1;
            }
        }
        break;

    case 6: // Stop autofocus
        g_print("opening device...\n");
        fd = open_device(&in);
        if (fd < 0) {
            g_printerr("Cannot open device\n");
            retval = -1;
            break;
        }

        if (stop_autofocus(fd) < 0) {
            retval = -1;
        }

        close(fd);
        g_print("result = %d\n", res);
        break;

    case 7: // snap
        g_print("snapshot command\n");
        pipeline_state = statePreviewToSnap;
        g_print("set: pipeline state -> PreviewToSnap\n");

    default:
        break;
    }

    return retval;
}
