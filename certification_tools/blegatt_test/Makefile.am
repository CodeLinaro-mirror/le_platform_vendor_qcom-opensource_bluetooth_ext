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

AM_CPPFLAGS += -I${STAGING_INCDIR}/bluetooth/stack/include \
    -I${STAGING_INCDIR}/bluetooth/include \
    -I${STAGING_INCDIR}/bluetooth/internal_include \
    -I${STAGING_INCDIR}/bluetooth/stack/l2cap \
    -I${STAGING_INCDIR}/bluetooth/utils/include \
    -I${STAGING_INCDIR}/bluetooth \
    -I${STAGING_INCDIR}/bluetooth/btif/include \
    -I${STAGING_INCDIR}/bluetooth/bta/include \
    -I${STAGING_INCDIR}/bluetooth/types \
    -I${BT_SOURCE}/system_bt_ext/include \
    -I${BT_SOURCE}/vhal/include \
    -I$(bdroid_C_INCLUDES)



cpp_sources = gatt_test.cpp


if USE_GST
requiredlibs = -lpthread -lm -ldl -lrt -lbtvendorhal -lbttypes -lchrome -lbsd -Wl,--no-as-needed,-lcutils,--as-needed
else
requiredlibs = -lpthread -lm -ldl -lrt -lbtvendorhal -lbttypes -lchrome -lbsd -Wl,--no-as-needed,-lcutils,--as-needed
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
