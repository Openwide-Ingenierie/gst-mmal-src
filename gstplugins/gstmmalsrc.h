/* Smile MMALSRC GStreamer element
 * Copyright (C) 2017 Alexandra Hospital <hospital.alex@gmail.com>
 * Copyright (C) 2017 Fabien Dutuit <fabien.dutuit@smile.fr>
 *
 * Camera acquisition on Raspberry Pi, shutter, ISO and exposure controls
 * and sink video/x-raw in GStreamer pipeline.
 * This element gets frames from camera using MMAL API.
 */

#ifndef _GST_MMALSRC_H_
#define _GST_MMALSRC_H_

#include <gst/base/gstpushsrc.h>

#include "interface/mmal/mmal.h"
#include "interface/mmal/mmal_logging.h"
#include "interface/mmal/mmal_buffer.h"
#include "interface/mmal/util/mmal_util.h"
#include "interface/mmal/util/mmal_util_params.h"
#include "interface/mmal/util/mmal_default_components.h"
#include "interface/mmal/util/mmal_connection.h"


G_BEGIN_DECLS

#define GST_TYPE_MMALSRC   (gst_mmalsrc_get_type())
#define GST_MMALSRC(obj)   (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_MMALSRC,GstMMALSrc))
#define GST_MMALSRC_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_MMALSRC,GstMMALSrcClass))
#define GST_IS_MMALSRC(obj)   (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_MMALSRC))
#define GST_IS_MMALSRC_CLASS(obj)   (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_MMALSRC))

/* Frame resolution */
#define MMALSRC_DEFAULT_WIDTH 1280
#define MMALSRC_DEFAULT_HEIGHT 720

/* Shutter activation */
#define MMALSRC_DEFAULT_SHUTTER_ACTIVATION "on"
/* Shutter period */
#define MMALSRC_DEFAULT_SHUTTER_PERIOD 10000

/* ISO */
#define MMALSRC_DEFAULT_ISO 400

/* Exposure */
#define MMALSRC_EXPOSURE_OFF "off"
#define MMALSRC_EXPOSURE_ON "on"
#define MMALSRC_DEFAULT_EXPOSURE MMALSRC_EXPOSURE_OFF

/* Number of requested buffers, need at least 2 buffers */
#define MMALSRC_FRMBUF_COUNT 6

/* Video format */
#define MMALSRC_DEFAULT_FORMAT "RGBA"

/* Framerate */
#define MMALSRC_DEFAULT_FRAMERATE_NUM 30
#define MMALSRC_DEFAULT_FRAMERATE_DEN 1

#define MMALSRC_PAR_NUM 1
#define MMALSRC_PAR_DEN 1

/* Standard port setting for the camera component */
#define MMAL_CAMERA_PREVIEW_PORT 0
#define MMAL_CAMERA_VIDEO_PORT 1
#define MMAL_CAMERA_CAPTURE_PORT 2

typedef struct _GstMMALSrc GstMMALSrc;
typedef struct _GstMMALSrcClass GstMMALSrcClass;


struct _GstMMALSrc
{
    GstPushSrc element;

    /* Plugin properties */
    gchar* shutter_activation; /* either overwrite shutter_period or not */
    guint shutter_period;      /* shutter period in microseconds */
    guint iso;                 /* ISO sensitivity value */
    gchar* exposure;           /* camera exposure mechanism on/off */

    /* Plugin variables */
    guint first_port_config;
    guint width;
    guint height;
    MMAL_RATIONAL_T framerate;
    MMAL_RATIONAL_T par;
    MMAL_FOURCC_T encoding;
    //const gchar * pixel_format;

    /* MMAL camera structures */
    MMAL_COMPONENT_T *camera_component;
    MMAL_POOL_T *cam_pool; // image memory buffers
    MMAL_PORT_T *cam_port; // output port
    MMAL_QUEUE_T *queue_video_frames; // pointer queue to image buffers

    // VideoCore events
    VCOS_EVENT_FLAGS_T events;

    gboolean unlock;

};

struct _GstMMALSrcClass
{
    GstPushSrcClass parent_class;
};

GType gst_mmalsrc_get_type (void);

G_END_DECLS

#endif /* _GST_MMALSRC_H_ */
