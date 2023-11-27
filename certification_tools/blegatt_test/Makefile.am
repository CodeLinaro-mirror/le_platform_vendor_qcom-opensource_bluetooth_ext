AM_CPPFLAGS = -O2 \
            -D_GNU_SOURCE \
            -DHAS_NO_BDROID_BUILDCFG \
            -std=c++11

AM_CFLAGS = -Wall \
            -Wundef \
            -Wstrict-prototypes \
            -Wno-trigraphs \
            -std=c++11 \
            -DHAS_NO_BDROID_BUILDCFG


ACLOCAL_AMFLAGS = -I m4

AM_CPPFLAGS += -I${WORKSPACE}/bluetooth/stack/system/bt/stack/include \
    -I${WORKSPACE}/bluetooth/stack/system/bt/include \
    -I${WORKSPACE}/bluetooth/stack/system/bt/internal_include \
    -I${WORKSPACE}/bluetooth/stack/system/bt/stack/l2cap \
    -I${WORKSPACE}/bluetooth/stack/system/bt/utils/include \
    -I${WORKSPACE}/bluetooth/stack/system/bt \
    -I${WORKSPACE}/bluetooth/stack/system/bt/btif/include \
    -I${WORKSPACE}/bluetooth/stack/system/bt/bta/include \
    -I${WORKSPACE}/bluetooth/stack/bluetooth_ext/system_bt_ext/include \
    -I${WORKSPACE}/bluetooth/stack/bluetooth_ext/vhal/include \
    -I${WORKSPACE}/bluetooth/stack/system/bt/types \
    -I${WORKSPACE}/system/core/include \
    -I$(bdroid_C_INCLUDES)



cpp_sources = gatt_test.cpp


if USE_GST
requiredlibs = -lpthread -lm -ldl -lrt -lbtvendorhal -lbttypes -lchrome
else
requiredlibs = -lpthread -lm -ldl -lrt -lbtvendorhal -lbttypes -lchrome
endif


if USE_GLIB

if USE_GST
 gstgattapp_CFLAGS  = $(AM_CFLAGS) -DUSE_GLIB @GLIB_CFLAGS@
 gstgattapp_CPPFLAGS  = $(AM_CPPFLAGS) -DUSE_GLIB @GLIB_CFLAGS@
 gstgattapp_LDFLAGS = $(requiredlibs) @GLIB_LIBS@
 gstgattapp_CPPFLAGS += $(GSTREAMER_CFLAGS) -DUSE_GST
else
 gattapp_CFLAGS  = $(AM_CFLAGS) -DUSE_GLIB @GLIB_CFLAGS@
 gattapp_CPPFLAGS  = $(AM_CPPFLAGS) -DUSE_GLIB @GLIB_CFLAGS@
 gattapp_LDFLAGS = $(requiredlibs) @GLIB_LIBS@
endif

else

if USE_GST
 gstgattapp_CFLAGS = $(AM_CFLAGS)
 gstgattapp_LDFLAGS = $(requiredlibs)
else
 gattapp_CFLAGS = $(AM_CFLAGS)
 gattapp_LDFLAGS = $(requiredlibs)
endif

endif

if USE_GST
gstgattapp_CFLAGS += $(GSTREAMER_CFLAGS)
endif

AM_LDFLAGS = $(LDFLAFGS)

if USE_GST
bin_PROGRAMS = gstgattapp
gstgattapp_CC = @CC@
gstgattapp_SOURCES = $(cpp_sources)
else
bin_PROGRAMS = gattapp
gattapp_CC = @CC@
gattapp_SOURCES = $(cpp_sources)
endif
pkgconfigdir = $(libdir)/pkgconfig
