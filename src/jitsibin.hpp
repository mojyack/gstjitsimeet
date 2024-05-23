#pragma once
#include <gst/gst.h>

extern "C" {
G_BEGIN_DECLS
#define GST_TYPE_JITSIBIN            (gst_jitsibin_get_type())
#define GST_JITSIBIN(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_JITSIBIN, GstJitsiBin))
#define GST_JITSIBIN_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_JITSIBIN, GstJitsiBinClass))
#define GST_JITSIBIN_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS((obj), GST_TYPE_JITSIBIN, GstJitsiBinClass))
#define GST_IS_JITSIBIN(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_JITSIBIN))
#define GST_IS_JITSIBIN_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_JITSIBIN))

struct RealSelf;

struct GstJitsiBin {
    GstBin bin;

    // many fields are not trivially constructible and
    // initializing every field explicitly is pain...
    RealSelf* real_self;
};

struct GstJitsiBinClass {
    GstBinClass parent_class;

    // signals
    guint participant_joined_signal;
    guint participant_left_signal;
    guint mute_state_changed_signal;
};

GType gst_jitsibin_get_type(void);

G_END_DECLS
}
