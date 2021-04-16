#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <pthread.h>
#include <stdio.h>

#include <pthread.h>
#include "input_gstreamer.h"

#define INPUT_PLUGIN_NAME "OV5640 GStreamer grabber"

static globals *pglobal;

struct my_context {
    int id;
    globals *pglobal;
    pthread_t threadID;
    GstAppSink *appsink;
};

static GstElement *pipeline = NULL;
static GstElement *sink = NULL;

void *cam_thread(void *arg);

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
    char *dev = "/dev/video0";
    struct my_context *pctx = calloc(1, sizeof(struct my_context));

    pglobal = param->global;
    pglobal->in[id].context = pctx;
    pctx->id = id;
    pctx->pglobal = param->global;

    param->argv[0] = INPUT_PLUGIN_NAME;

    fprintf(stderr, "%s: Here we are\n", __func__);


    gst_init (0, NULL);
    fprintf(stderr, "%s: gst_init()\n", __func__);

    pipeline = gst_pipeline_new ("macro-pipeline");
    fprintf(stderr, "%s: pipe = %p\n", __func__, pipeline);

    GstElement *source = gst_element_factory_make("imxv4l2videosrc", "macrocam");
    g_print("source = %p\n", source);
    g_object_set (source, "device", "/dev/video0", NULL);
    g_object_set (source, "imx-capture-mode", 5, NULL);
    g_object_set (source, "fps-n", 30, NULL);

    GstElement *mjpeg_enc = gst_element_factory_make("imxvpuenc_mjpeg", "mjpeg");

    if (!mjpeg_enc) {
        fprintf(stderr, "Cannot create imxvpumjpeg_enc!\n");
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

int input_run(int id)
{
    fprintf(stderr, "%s: Here we are\n", __func__);

    input *in = &pglobal->in[id];
    struct my_context *pctx = (struct my_context *) in->context;

    in->buf = malloc(1048576);

    g_print("1111 HEAR YE HEAR YE sink is GstAppSink? %d\n",
        GST_IS_APP_SINK(pctx->appsink));
    pthread_create(&(pctx->threadID), NULL, cam_thread, in);
    pthread_detach(pctx->threadID);
    g_print("2222 HEAR YE HEAR YE sink is GstAppSink? %d\n",
        GST_IS_APP_SINK(pctx->appsink));
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
    g_print("HEAR YE HEAR YE sink is GstAppSink? %d\n",
        GST_IS_APP_SINK(pcontext->appsink));

    while (!pglobal->stop) {
        //g_print("HEAR YE HEAR YE sink is GstAppSink? %d\n", GST_IS_APP_SINK(pcontext->appsink));
        GstSample *frame = gst_app_sink_pull_sample(pcontext->appsink);
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
}

int input_cmd(int plugin, unsigned int control_id, unsigned int typecode, int value)
{
    fprintf(stderr, "%s: Here we are\n", __func__);
    return 0;
}
