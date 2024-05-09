#pragma once
#include <gst/gst.h>

#include "jitsi/conference.hpp"
#include "jitsi/jingle-handler/jingle.hpp"
#include "jitsi/websocket.hpp"

extern "C" {
G_BEGIN_DECLS
#define GST_TYPE_JITSIBIN \
    (gst_jitsibin_get_type())
#define GST_JITSIBIN(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_JITSIBIN, GstJitsiBin))
#define GST_JITSIBIN_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_JITSIBIN, GstJitsiBinClass))
#define GST_IS_JITSIBIN(obj) \
    (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_JITSIBIN))
#define GST_IS_JITSIBIN_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_JITSIBIN))

struct GstJitsiBin {
    GstBin  bin;
    GstPad* sink;
    GstPad* src;

    std::unique_ptr<JingleHandler>                   jingle_handler;
    std::unique_ptr<conference::ConferenceCallbacks> conference_callbacks;
    std::unique_ptr<conference::Conference>          conference;
    ws::Connection*                                  ws_conn;
};

struct GstJitsiBinClass {
    GstBinClass parent_class;
};

GType gst_jitsibin_get_type(void);

G_END_DECLS
}
