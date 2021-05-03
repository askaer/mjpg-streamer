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
    " [-f | fps ]............: frame rate (15 or 30)\n" \
    " [-s | swjpeg ].........: use sw jpeg encoding\n" \
    "                          ");
}

static GstFlowReturn new_frame_callback(GstElement *sink, input *in)
{
    GstSample *frame;
    struct my_context *pctx = (struct my_context *) in->context;
    static int count = 0;

    g_signal_emit_by_name(sink, "pull-sample", &frame);
    if (frame) {
        //g_print("New frame = %p\n", frame);

        GstBuffer *buffer = gst_sample_get_buffer(frame);
        //g_print("GstBuffer buffer = %p\n", buffer);

        GstMapInfo info;
        if (count++ % 10 == 0) {
            g_print("cam: mutex = %p, condvar = %p\n", &pglobal->in[pctx->id].db,
                &pglobal->in[pctx->id].db_update);
        }
        pthread_mutex_lock(&pglobal->in[pctx->id].db);

        /*
        if (gst_buffer_map(buffer, &info, GST_MAP_READ)) {
            g_print("Copying %d bytes to %p...", info.size, in[pctx->id].buf);

            memcpy(in[pctx->id].buf, info.data, info.size);
            in[pctx->id].size = info.size;
            gst_buffer_unmap(buffer, &info);
            g_print(" Copied\n");
        }
        else {
            g_printerr("Cannot map GstBuffer!\n");
        }
        */

        //pthread_cond_broadcast(&in[pctx->id].db_update);
        pthread_mutex_unlock(&in[pctx->id].db);

        gst_sample_unref(frame);
        return GST_FLOW_OK;
    }
    g_printerr("No frame\n");
    return GST_FLOW_ERROR;
}

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
    int fps = 30, capture_mode = 5, sw_jpeg = 0;
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
        }
    }

    g_print("Input device is %s\n", dev);

    gst_init (0, NULL);
    fprintf(stderr, "%s: gst_init()\n", __func__);

    pipeline = gst_pipeline_new ("macro-pipeline");
    fprintf(stderr, "%s: pipe = %p\n", __func__, pipeline);

    GstElement *source = gst_element_factory_make("imxv4l2videosrc", "macrocam");
    g_print("source = %p\n", source);
    g_object_set (source, "device", dev, NULL);
    g_object_set (source, "imx-capture-mode", capture_mode, NULL);
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

    gst_bin_add_many (GST_BIN (pipeline), source, mjpeg_enc, sink, NULL);


    if (!gst_element_link(source, mjpeg_enc)) {
        g_printerr("Cannot link source to mjpeg_enc\n");
        gst_object_unref(pipeline);
        return -1;
    }

    if (!gst_element_link(mjpeg_enc, sink)) {
        g_printerr("Cannot link mjpeg_enc to sink\n");
        gst_object_unref(pipeline);
        return -1;
    }

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

int input_cmd(int plugin, unsigned int control_id, unsigned int typecode, int value)
{
    int res = 0;
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
        pctx->active_flag = 1;
        break;

    case 2: // Stop stream
        g_print("Stop stream\n");
        //pglobal->stop = 1;
        pctx->active_flag = 0;

        break;

    case 3: // Run autofocus
        g_print("opening device...\n");
        fd = open_device(&in);
        if (fd < 0) {
            g_printerr("Cannot open device\n");
            return -1;
        }

        if (start_autofocus_single(fd) < 0) {
            goto bye;
        }

bye:

        close(fd);
        g_print("result = %d\n", res);
        break;

    default:
        break;
    }

    return 0;
}
