/* Smile MMALSRC GStreamer element
 * Copyright (C) 2017 Alexandra Hospital <hospital.alex@gmail.com>
 * Copyright (C) 2017 Fabien Dutuit <fabien.dutuit@smile.fr>
 */
/**
 * SECTION:element-gstmmalsrc
 *
 * Camera acquisition on Raspberry Pi, shutter, ISO and exposure controls
 * and sink video/x-raw in GStreamer pipeline.
 * This element gets frames from camera using MMAL API.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v mmalsrc ! videoconvert ! fbdevsink
 * ]|
 * </refsect2>
 */

#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>
#include <gst/video/video.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/time.h>
#include <string.h>

#include "bcm_host.h"
#include "gstmmalsrc.h"

#include "interface/vcos/vcos.h"

GST_DEBUG_CATEGORY_STATIC(gst_mmalsrc_debug_category);
#define GST_CAT_DEFAULT gst_mmalsrc_debug_category


/******************************************************************
 * Prototypes
 ******************************************************************/

static void gst_mmalsrc_set_property(GObject * object, guint property_id,
		const GValue * value, GParamSpec * pspec);
static void gst_mmalsrc_get_property(GObject * object, guint property_id,
		GValue * value, GParamSpec * pspec);

static gboolean gst_mmalsrc_start(GstBaseSrc * src);
static gboolean gst_mmalsrc_stop(GstBaseSrc * src);
static void gst_mmalsrc_finalize(GObject * object);

static gboolean gst_mmalsrc_unlock(GstBaseSrc * src);
static gboolean gst_mmalsrc_unlock_stop(GstBaseSrc * src);

static GstCaps *gst_mmalsrc_fixate(GstBaseSrc * src, GstCaps * caps);
static gboolean gst_mmalsrc_set_caps(GstBaseSrc * src, GstCaps * caps);
static gboolean gst_mmalsrc_is_seekable(GstBaseSrc * src);
static GstFlowReturn gst_mmalsrc_create(GstPushSrc * psrc,
		GstBuffer ** outbuf);

/******************************************************************
 * Globals/Static/Decl.
 ******************************************************************/
enum {
	PROP_0,
	PROP_SHUTTER_ACTIVATION,
	PROP_SHUTTER_PERIOD,
	PROP_ISO,
	PROP_EXPOSURE
};

#define MMAL_VIDEO_CAPS \
  "video/x-raw, "                 									\
  "format = (string) { I420, RGBA, BGRA, YV12, YVYU, UYVY }, "      \
  "width = (int) [ 1, 1920 ], "     								\
  "height = (int) [ 1, 1080 ], "      								\
  "pixel-aspect-ratio = 1/1, "       								\
  "framerate = (fraction) [ 0/1, 90/1 ]"

static GstStaticPadTemplate gst_mmalsrc_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
		GST_PAD_SRC,
		GST_PAD_ALWAYS,
		GST_STATIC_CAPS (MMAL_VIDEO_CAPS)
);

static VCOS_EVENT_FLAGS_T events;

typedef enum {
	MMAL_CAM_BUFFER_READY = 1 << 0,
	MMAL_CAM_AUTOFOCUS_COMPLETE = 1 << 1,
	MMAL_CAM_ANY_EVENT = 0x7FFFFFFF
} MMAL_CAM_EVENT_T;

G_DEFINE_TYPE_WITH_CODE(GstMMALSrc, gst_mmalsrc, GST_TYPE_PUSH_SRC,
		GST_DEBUG_CATEGORY_INIT (gst_mmalsrc_debug_category, "mmalsrc", 3, "debug category for mmalsrc element"))

/******************************************************************
 ******************************************************************
 ****** Callbacks *************************************************
 *****************************************************************
 ******************************************************************/

/******************************************************************
 * control callback
 ******************************************************************/
static void control_bh_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
	GST_INFO("control_bh_cb %p,%p (cmd=0x%08x)", port, buffer, buffer->cmd);
	if (buffer->cmd == MMAL_EVENT_PARAMETER_CHANGED) {
		MMAL_EVENT_PARAMETER_CHANGED_T *param =
				(MMAL_EVENT_PARAMETER_CHANGED_T *) buffer->data;

		vcos_assert(buffer->length >= sizeof(MMAL_EVENT_PARAMETER_CHANGED_T));
		vcos_assert(buffer->length == param->hdr.size);
		switch (param->hdr.id) {
		case MMAL_PARAMETER_CAMERA_NUM:
			vcos_assert(param->hdr.size == sizeof(MMAL_PARAMETER_UINT32_T));
			{
				MMAL_PARAMETER_UINT32_T *camera_num =
						(MMAL_PARAMETER_UINT32_T *) param;
				GST_INFO("Camera number: %d", camera_num->value);
			}
			break;
		default:
			GST_ERROR("Unexpected changed event for parameter 0x%08x",
					param->hdr.id);
		}
	} else {
		GST_ERROR("Unexpected event, 0x%08x", buffer->cmd);
	}
	mmal_buffer_header_release(buffer);
}

/******************************************************************
 * output port callback
 * put buffer into queue
 ******************************************************************/
static void generic_output_port_cb(MMAL_PORT_T *port,
		MMAL_BUFFER_HEADER_T *buffer) {

	if (buffer->cmd != 0) {
		GST_INFO("%s callback: event %u not supported", port->name,
				buffer->cmd);
		mmal_buffer_header_release(buffer);
	} else {
		MMAL_QUEUE_T *queue = (MMAL_QUEUE_T *) port->userdata;
		GST_INFO("%s callback", port->name);
		mmal_queue_put(queue, buffer);
	}

	vcos_event_flags_set(&events, MMAL_CAM_BUFFER_READY, VCOS_OR);
}

/******************************************************************
 * gst_mmalsrc_class_init
 * Class initialization
 ******************************************************************/
static void gst_mmalsrc_class_init(GstMMALSrcClass * klass) {
	GObjectClass *gobject_class;
	GstBaseSrcClass *base_src_class;
	GstPushSrcClass *push_src_class;

	gobject_class = (GObjectClass *) klass;
	base_src_class = (GstBaseSrcClass *) klass;
	push_src_class = (GstPushSrcClass *) klass;

	gst_element_class_add_pad_template(GST_ELEMENT_CLASS(klass),
			gst_static_pad_template_get(&gst_mmalsrc_src_template));

	gst_element_class_set_static_metadata(GST_ELEMENT_CLASS(klass),
			"MMAL video source", "mmalsrc",
			"Camera acquisition on Raspberry Pi, shutter, ISO and exposure controls"
			" and sink video/x-raw in GStreamer pipeline."
			" This element gets frames from camera using MMAL API.",
			"Alexandra HOSPITAL <alhos@smile.fr>, Fabien DUTUIT <fadut@smile.fr>");

	gobject_class->set_property = gst_mmalsrc_set_property;
	gobject_class->get_property = gst_mmalsrc_get_property;
	gobject_class->finalize = gst_mmalsrc_finalize;

	g_object_class_install_property(gobject_class, PROP_SHUTTER_ACTIVATION,
			g_param_spec_string("shutter-activation", "shutter-activation",
					"send if the shutter period has to be set (on or off)",
					MMALSRC_DEFAULT_SHUTTER_ACTIVATION, G_PARAM_READWRITE));

	g_object_class_install_property(gobject_class, PROP_SHUTTER_PERIOD,
			g_param_spec_uint("shutter-period", "shutter-period",
					"camera shutter in open state; duration in microseconds", 0, 300000,
					MMALSRC_DEFAULT_SHUTTER_PERIOD, G_PARAM_READWRITE));

	g_object_class_install_property(gobject_class, PROP_ISO,
			g_param_spec_uint("ISO", "ISO", "ISO sensitivity", 100, 1600,
			MMALSRC_DEFAULT_ISO, G_PARAM_READWRITE));

	g_object_class_install_property(gobject_class, PROP_EXPOSURE,
			g_param_spec_string("exposure", "exposure", "exposure  (on or off)",
			MMALSRC_DEFAULT_EXPOSURE, G_PARAM_READWRITE));

	base_src_class->fixate = GST_DEBUG_FUNCPTR(gst_mmalsrc_fixate);
	base_src_class->set_caps = GST_DEBUG_FUNCPTR(gst_mmalsrc_set_caps);
	base_src_class->start = GST_DEBUG_FUNCPTR(gst_mmalsrc_start);
	base_src_class->stop = GST_DEBUG_FUNCPTR(gst_mmalsrc_stop);
	base_src_class->is_seekable = GST_DEBUG_FUNCPTR(gst_mmalsrc_is_seekable);
	base_src_class->unlock = GST_DEBUG_FUNCPTR(gst_mmalsrc_unlock);
	base_src_class->unlock_stop = GST_DEBUG_FUNCPTR(gst_mmalsrc_unlock_stop);

	push_src_class->create = GST_DEBUG_FUNCPTR(gst_mmalsrc_create);

}

/******************************************************************
 * gst_mmalsrc_init
 * Init function
 ******************************************************************/
static void gst_mmalsrc_init(GstMMALSrc *mmalsrc) {
	mmalsrc->shutter_activation = g_strdup(MMALSRC_DEFAULT_SHUTTER_ACTIVATION);
	mmalsrc->shutter_period = MMALSRC_DEFAULT_SHUTTER_PERIOD;
	mmalsrc->iso = MMALSRC_DEFAULT_ISO;
	mmalsrc->exposure = g_strdup(MMALSRC_DEFAULT_EXPOSURE);
	mmalsrc->unlock = false;
	gst_base_src_set_format(GST_BASE_SRC(mmalsrc), GST_FORMAT_TIME);
	gst_base_src_set_live(GST_BASE_SRC(mmalsrc), TRUE);
}

/******************************************************************
 * gst_mmalsrc_finalize
 * Release function
 ******************************************************************/
void gst_mmalsrc_finalize(GObject * object) {
	/* Default finalize function */
	G_OBJECT_CLASS (gst_mmalsrc_parent_class)->finalize(object);
}

/******************************************************************
 * gst_mmalsrc_set_property
 * Properties management
 ******************************************************************/
void gst_mmalsrc_set_property(GObject * object, guint property_id,
		const GValue * value, GParamSpec * pspec) {
	GstMMALSrc *mmalsrc = GST_MMALSRC(object);

	switch (property_id) {
	case PROP_SHUTTER_ACTIVATION: {
		const gchar* shutter_activation = g_value_get_string(value);
		g_free(mmalsrc->shutter_activation);
		mmalsrc->shutter_activation = g_strdup(shutter_activation);
		GST_INFO("shutter activation set to %s\n",
				mmalsrc->shutter_activation);
		break;
	}
	case PROP_SHUTTER_PERIOD: {
		mmalsrc->shutter_period = g_value_get_uint(value);
		GST_INFO("shutter period set to %d\n", mmalsrc->shutter_period);
		break;
	}
	case PROP_ISO: {
		mmalsrc->iso = g_value_get_uint(value);
		GST_INFO("ISO value set to %d\n", mmalsrc->iso);
		break;
	}
	case PROP_EXPOSURE: {
		const gchar* exposure = g_value_get_string(value);
		g_free(mmalsrc->exposure);
		mmalsrc->exposure = g_strdup(exposure);
		GST_INFO("exposure set to %s\n", mmalsrc->exposure);
		break;
	}
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
		break;
	}
}

/******************************************************************
 * gst_mmalsrc_get_property
 * Properties management
 ******************************************************************/
void gst_mmalsrc_get_property(GObject * object, guint property_id,
		GValue * value, GParamSpec * pspec) {
	GstMMALSrc *mmalsrc = GST_MMALSRC(object);
	switch (property_id) {
	case PROP_SHUTTER_ACTIVATION:
		g_value_set_string(value, mmalsrc->shutter_activation);
		break;
	case PROP_SHUTTER_PERIOD:
		g_value_set_uint(value, (uint) mmalsrc->shutter_period);
		break;
	case PROP_ISO:
		g_value_set_uint(value, (uint) mmalsrc->iso);
		break;
	case PROP_EXPOSURE:
		g_value_set_string(value, mmalsrc->exposure);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
		break;
	}
}

/******************************************************************
 * gst_mmalsrc_fixate
 *
 * Called during negotiation if capabilities need to be fixed
 *
 ******************************************************************/
static GstCaps *gst_mmalsrc_fixate(GstBaseSrc * src, GstCaps * caps) {
	GstMMALSrc *mmalsrc = GST_MMALSRC(src);
	GstStructure *structure;

	caps = gst_caps_make_writable(caps);
	structure = gst_caps_get_structure(caps, 0);

	gst_structure_fixate_field_nearest_int(structure, "width",
			MMALSRC_DEFAULT_WIDTH);
	gst_structure_fixate_field_nearest_int(structure, "height",
			MMALSRC_DEFAULT_HEIGHT);

	gst_structure_fixate_field_nearest_fraction(structure, "framerate",
			MMALSRC_DEFAULT_FRAMERATE_NUM, MMALSRC_DEFAULT_FRAMERATE_DEN);

	gst_structure_fixate_field_string(structure, "format", MMALSRC_DEFAULT_FORMAT);

	gst_structure_fixate_field_nearest_fraction(structure, "pixel-aspect-ratio",
			MMALSRC_PAR_NUM, MMALSRC_PAR_DEN);

	GST_INFO("fixate returning %" GST_PTR_FORMAT, caps);
	return caps;
}

/******************************************************************
 * gst_mmalsrc_set_caps
 *
 * Notify the subclass of new caps
 *
 ******************************************************************/
static gboolean gst_mmalsrc_set_caps(GstBaseSrc * src, GstCaps * caps) {
	gboolean res = TRUE;

	GstMMALSrc *mmalsrc = GST_MMALSRC(src);
	GstStructure *structure;
	GstVideoInfo info;

	if (!gst_video_info_from_caps(&info, caps))
		return FALSE;

	structure = gst_caps_get_structure(caps, 0);

	if (gst_structure_has_name(structure, "video/x-raw")) {

		mmalsrc->width = info.width;
		mmalsrc->height = info.height;
		mmalsrc->framerate.num = info.fps_n;
		mmalsrc->framerate.den = info.fps_d;
		mmalsrc->par.num = info.par_n;
		mmalsrc->par.den = info.par_d;
		//mmalsrc->pixel_format = info.finfo->name;

		// If encoding has 3 letters
		if (info.finfo->name[3] == '\0') {
			mmalsrc->encoding = MMAL_FOURCC(info.finfo->name[0],
					info.finfo->name[1], info.finfo->name[2], ' ');
		}

		else { // 4 letters
			mmalsrc->encoding = MMAL_FOURCC(info.finfo->name[0],
					info.finfo->name[1], info.finfo->name[2],
					info.finfo->name[3]);
		}
	}
	// else : if we add other encoding later

	GST_INFO("set_caps returning %" GST_PTR_FORMAT, caps);

	return res;
}

/******************************************************************
 * Util. functions
 ******************************************************************/

/*******************************************************************
 * gst_mmalsrc_is_seekable
 *
 * Tell downstream elements that the stream is not seekable
 *
 ******************************************************************/
static gboolean gst_mmalsrc_is_seekable(GstBaseSrc * src) {
	return FALSE;
}

/******************************************************************
 ******************************************************************
 * Core functions
 ******************************************************************
 ******************************************************************/

/*******************************************************************
 * destroy_camera_component
 *
 * Destroy MMAL camera component
 *
 ******************************************************************/
static void destroy_camera_component(GstMMALSrc *mmalsrc) {

	if (mmalsrc) {
		mmal_component_destroy(mmalsrc->camera_component);
		mmalsrc->camera_component = NULL;
		GST_INFO("MMAL camera component destroyed.");
	}
}

/*******************************************************************
 * gst_mmalsrc_start
 *
 * Create the camera component, set the parameters and enable stream.
 * Return TRUE on success.
 *
 ******************************************************************/
static gboolean gst_mmalsrc_start(GstBaseSrc * src) {
	GstMMALSrc *mmalsrc = GST_MMALSRC(src);
	gboolean ret = TRUE;
	MMAL_STATUS_T status;
	MMAL_COMPONENT_T *camera = 0;
	MMAL_ES_FORMAT_T *format;
	uint32_t width, height;

	mmalsrc->first_port_config = 0;

	//Camera parameter
	MMAL_PARAMETER_CHANGE_EVENT_REQUEST_T change_event_request = { {
			MMAL_PARAMETER_CHANGE_EVENT_REQUEST,
			sizeof(MMAL_PARAMETER_CHANGE_EVENT_REQUEST_T) }, 0, 1 };

	MMAL_PARAMETER_BOOLEAN_T camera_capture = { { MMAL_PARAMETER_CAPTURE,
			sizeof(camera_capture) }, 1 };

	MMAL_PARAMETER_INT32_T camera_num = { { MMAL_PARAMETER_CAMERA_NUM,
			sizeof(camera_num) }, 0 };

	MMAL_PARAMETER_UINT32_T camera_iso = { { MMAL_PARAMETER_ISO,
			sizeof(camera_iso) }, mmalsrc->iso };


	MMAL_PARAMETER_EXPOSUREMODE_T camera_exposure;

	//Exposure => on : MMAL_PARAM_EXPOSUREMODE_AUTO; off = MMAL_PARAM_EXPOSUREMODE_OFF
	if (strcmp(mmalsrc->exposure, MMALSRC_EXPOSURE_ON) == 0) { //if "on"
		camera_exposure.hdr.id = MMAL_PARAMETER_EXPOSURE_MODE;
		camera_exposure.hdr.size = sizeof(camera_exposure);
		camera_exposure.value = MMAL_PARAM_EXPOSUREMODE_AUTO;

	} else { //if "off"
		camera_exposure.hdr.id = MMAL_PARAMETER_EXPOSURE_MODE;
		camera_exposure.hdr.size = sizeof(camera_exposure);
		camera_exposure.value = MMAL_PARAM_EXPOSUREMODE_OFF;
	}

	bcm_host_init();

	if (vcos_event_flags_create(&events, "mmalsrc") != VCOS_SUCCESS) {
		GST_ERROR("%s: failed to create event", __func__);
		goto error;
	}

	/************** CREATE CAMERA COMPONENT **************/
	status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA, &camera);

	if (status != MMAL_SUCCESS) {
		GST_ERROR("%s: couldn't create camera", __func__);
		goto error;
	}
	GST_INFO("%s: mmal camera component created", __func__);

	if (!camera->output_num) {
		GST_ERROR("camera doesn't have output ports");
		status = MMAL_EINVAL;
		goto error;
	}

	// Camera port is of type "video port"
	mmalsrc->cam_port = camera->output[MMAL_CAMERA_VIDEO_PORT];



	/************** PARAMETERS**************/
	//- Camera capture
	status = mmal_port_parameter_set(mmalsrc->cam_port, &camera_capture.hdr);
	if (status != MMAL_SUCCESS && status != MMAL_ENOSYS) {
		GST_ERROR("Error no camera capture");
		goto error;
	}

	//- Camera num
	status = mmal_port_parameter_set(camera->control, &camera_num.hdr);

	if (status != MMAL_SUCCESS && status != MMAL_ENOSYS) {
		GST_ERROR("Could not select camera : error %d", status);
		goto error;
	}

	//- Camera exposure
	status = mmal_port_parameter_set(camera->control, &camera_exposure.hdr);

	if (status != MMAL_SUCCESS && status != MMAL_ENOSYS) {
		GST_ERROR("Could not set exposure : error %d", status);
		goto error;
	}

	//- Camera ISO
	status = mmal_port_parameter_set(camera->control, &camera_iso.hdr);

	if (status != MMAL_SUCCESS && status != MMAL_ENOSYS) {
		GST_ERROR("Could not set iso : error %d", status);
		goto error;
	}

	// If shutter activation is on, we set the shutter period (in microseconds)
	if (strcmp(mmalsrc->shutter_activation, "on") == 0) {
		MMAL_PARAMETER_UINT32_T camera_shutter = { {
				MMAL_PARAMETER_SHUTTER_SPEED, sizeof(camera_shutter) },
				mmalsrc->shutter_period };

		//- Camera shutter
		status = mmal_port_parameter_set(camera->control, &camera_shutter.hdr);

		if (status != MMAL_SUCCESS && status != MMAL_ENOSYS) {
			GST_ERROR("Could not set camera shutter : error %d", status);
			goto error;
		}
	}

	/************** ENABLE CONTROL PORT **************/
	status = mmal_port_enable(camera->control, control_bh_cb);

	if (status != MMAL_SUCCESS) {
		GST_ERROR("Unable to enable control port : error %d", status);
		goto error;
	}

	GST_INFO("control port enabled");

	/************** ENABLE CAMERA COMPONENT **************/
	status = mmal_component_enable(camera);
	if (status) {
		GST_ERROR("%s: camera component couldn't be enabled", __func__);
		goto error;
	}

	mmalsrc->camera_component = camera;

	GST_INFO("%s: camera component created", __func__);

	return ret;

error:
	if (camera)
		mmal_component_destroy(camera);

	GST_ERROR("%s: Failed to create camera component", __func__);
	ret = FALSE;
	return ret;
}

/*******************************************************************
 * gst_mmalsrc_stop
 *
 ******************************************************************/
static gboolean gst_mmalsrc_stop(GstBaseSrc * src) {
	GstMMALSrc *mmalsrc = GST_MMALSRC(src);
	gboolean ret = TRUE;

	GST_INFO("stop function");
	vcos_event_flags_delete(&events);
	destroy_camera_component(mmalsrc);

	mmal_queue_destroy(mmalsrc->queue_video_frames);
	mmal_pool_destroy(mmalsrc->cam_pool);

	return ret;
}

/*******************************************************************
 * gst_release_buffer_cb
 *
 * Buffer release when GStreamer is done using it.
 *
 ******************************************************************/
static void gst_release_buffer_cb(gpointer data) {

	MMAL_BUFFER_HEADER_T *d = (MMAL_BUFFER_HEADER_T *) data;
	mmal_buffer_header_release(d);

}

/*******************************************************************
 * gst_mmalsrc_create
 *
 * Give a buffer to GStreamer containing the image.
 * Also sets the port format once when the stream starts.
 *
 ******************************************************************/
static GstFlowReturn gst_mmalsrc_create(GstPushSrc *src, GstBuffer **buf) {
	GstMMALSrc *mmalsrc = GST_MMALSRC(src);
	GstFlowReturn ret;

	MMAL_BUFFER_HEADER_T *buffer_h = NULL;
	MMAL_STATUS_T status = MMAL_SUCCESS;
	VCOS_UNSIGNED set;
	MMAL_ES_FORMAT_T *format;

	/* Not Implemented */
	ret = GST_FLOW_ERROR;

	GST_INFO("===== Enter create function =====");

	/* Barrier */
	if (!mmalsrc->camera_component) {
		GST_ERROR("no camera");
		return ret;
	}

	if (!mmalsrc->first_port_config) {
		/************** CAMERA PORT **************/
		/* Set up the port format */
		format = mmalsrc->cam_port->format;

		format->type = MMAL_ES_TYPE_VIDEO;
		format->encoding = mmalsrc->encoding;
		format->es->video.width = mmalsrc->width;
		format->es->video.height = mmalsrc->height;
		format->es->video.crop.x = 0;
		format->es->video.crop.y = 0;
		format->es->video.crop.width = mmalsrc->width;
		format->es->video.crop.height = mmalsrc->height;
		format->es->video.frame_rate.num = mmalsrc->framerate.num;
		format->es->video.frame_rate.den = mmalsrc->framerate.den;
		format->es->video.par.num = MMALSRC_PAR_NUM;
		format->es->video.par.den = MMALSRC_PAR_DEN;

		status = mmal_port_format_commit(mmalsrc->cam_port);
		if (status != MMAL_SUCCESS) {
			GST_ERROR("camera output port format couldn't be set");
			return ret;
		}

		/* set port size */
		mmalsrc->cam_port->buffer_size =
				mmalsrc->cam_port->buffer_size_recommended;
		mmalsrc->cam_port->buffer_num = MMALSRC_FRMBUF_COUNT;

		if (mmalsrc->cam_port->buffer_size
				< mmalsrc->cam_port->buffer_size_min)
			mmalsrc->cam_port->buffer_size =
					mmalsrc->cam_port->buffer_size_min;

		if (mmalsrc->cam_port->buffer_num
				< mmalsrc->cam_port->buffer_num_min)
			mmalsrc->cam_port->buffer_num =
					mmalsrc->cam_port->buffer_num_min;

		/* Create pool of buffer headers for the output port to consume */
		mmalsrc->cam_pool = mmal_port_pool_create(mmalsrc->cam_port,
				mmalsrc->cam_port->buffer_num,
				mmalsrc->cam_port->buffer_size);

		if (!mmalsrc->cam_pool) {
			GST_ERROR("failed to create pool for %s",
					mmalsrc->cam_port->name);
			return ret;
		}

		/* Display buffer information */
		GST_INFO("%s: buffer size recommended %d", __func__,
				mmalsrc->cam_port->buffer_size_recommended);
		GST_INFO("%s: buffer size: %d", __func__,
				mmalsrc->cam_port->buffer_size);

		GST_INFO("%s: buffer num recommended : %d", __func__,
				mmalsrc->cam_port->buffer_num_recommended);
		GST_INFO("%s: buffer num min : %d", __func__,
				mmalsrc->cam_port->buffer_num_min);
		GST_INFO("%s: buffer num : %d", __func__,
				mmalsrc->cam_port->buffer_num);

		// Create a queue to store our video frames. The callback we will get when
		// a frame has been decoded will put the frame into this queue.

		mmalsrc->queue_video_frames = mmal_queue_create();

		if (!mmalsrc->queue_video_frames) {
			GST_ERROR("failed to create queue video frames");
			return ret;
		}
		mmalsrc->cam_port->userdata =
				(void *) mmalsrc->queue_video_frames;

		/* Enable port with callback */
		status = mmal_port_enable(mmalsrc->cam_port, generic_output_port_cb);
		if (status != MMAL_SUCCESS) {
			GST_ERROR("failed to enable %s", mmalsrc->cam_port->name);
			return ret;
		}
		GST_INFO("camera port enabled with output callback");

		mmalsrc->first_port_config = 1;
	}

	/* Set VideoCore event communication */
	vcos_event_flags_get(&events, MMAL_CAM_ANY_EVENT, VCOS_OR_CONSUME,
			VCOS_TICKS_TO_MS(2), &set);

	// Send empty buffers to the output port of the video to allow the video to start
	// producing frames as soon as it gets input data
	while ((buffer_h = mmal_queue_get(mmalsrc->cam_pool->queue)) != NULL) {
		status = mmal_port_send_buffer(mmalsrc->cam_port, buffer_h);
		if (status != MMAL_SUCCESS) {
			GST_INFO("Error when sending EMPTY buffer to camera port");
		}
	}

	// Waiting for a ready buffer
	buffer_h = mmal_queue_wait(mmalsrc->queue_video_frames);

	// Wrap the buffer in the output GstBuffer
	if (buffer_h) {

		*buf = gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_READONLY,
				buffer_h->data, mmalsrc->cam_port->buffer_size, 0,
				mmalsrc->cam_port->buffer_size, buffer_h,
				(GDestroyNotify) gst_release_buffer_cb);

		if (!buf) {
			GST_ERROR("buffer already used");
			return ret;
		}

		// everything's OK !
		ret = GST_FLOW_OK;
	} else {
		GST_ERROR("No valid buffer !");
	}

	return ret;
}

/*******************************************************************
 * gst_mmalsrc_unlock
 * TODO : use in create function to unlock the element.
 *
 ******************************************************************/
static gboolean gst_mmalsrc_unlock(GstBaseSrc * src) {
	GstMMALSrc *mmalsrc = GST_MMALSRC(src);
	mmalsrc->unlock = true;
	return true;
}

/*******************************************************************
 * gst_mmalsrc_unlock_stop
 *
 *
 ******************************************************************/
static gboolean gst_mmalsrc_unlock_stop(GstBaseSrc * src) {
	GstMMALSrc *mmalsrc = GST_MMALSRC(src);
	mmalsrc->unlock = false;
	return true;
}

/******************************************************************
 * plugin_init
 * Plugin creation
 ******************************************************************/
static gboolean plugin_init(GstPlugin * plugin) {
	return gst_element_register(plugin, "mmalsrc", GST_RANK_MARGINAL,
	GST_TYPE_MMALSRC);
}

#define PACKAGE "gst-smile-plugin"
#define PACKAGE_NAME "GStreamer Smile Plug-in for Raspberry Pi"
#define PACKAGE_ORIGIN "https://github.com/Openwide-Ingenierie/gst-mmal-src.git"
#define PACKAGE_VERSION "1.0"
#define PACKAGE_LICENSE "LGPL"

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, smile-mmalsrc,
		"MMAL plugin library", plugin_init, PACKAGE_VERSION, PACKAGE_LICENSE,
		PACKAGE_NAME, PACKAGE_ORIGIN)
