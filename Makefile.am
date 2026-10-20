ACLOCAL_AMFLAGS = -I m4

AM_CPPFLAGS = \
	-I$(top_srcdir)/src \
	-DSYSCONFDIR=\"$(sysconfdir)\" \
	-DGVS_STORAGEDIR=\"$(gvs_storagedir)\"
AM_CFLAGS = $(GVS_CFLAGS) $(WARN_CFLAGS)

# ---------------------------------------------------------------------
# Shared core (voicegram XML + storage), used by daemon, tool and tests
# ---------------------------------------------------------------------
noinst_LIBRARIES = libgvs.a
libgvs_a_SOURCES = \
	src/gvs-voicegram.c src/gvs-voicegram.h \
	src/gvs-store.c src/gvs-store.h

# ---------------------------------------------------------------------
# Programs
# ---------------------------------------------------------------------
bin_PROGRAMS = gnome-voice-server gnome-voice-send

gnome_voice_server_SOURCES = \
	src/gvs-main.c \
	src/gvs-server.c src/gvs-server.h
gnome_voice_server_LDADD = libgvs.a $(GVS_LIBS)

gnome_voice_send_SOURCES = src/gnome-voice-send.c
gnome_voice_send_LDADD = libgvs.a $(GVS_LIBS)

# ---------------------------------------------------------------------
# Data
# ---------------------------------------------------------------------
dist_man_MANS = doc/gnome-voice-server.1 doc/gnome-voice-send.1

configdir = $(sysconfdir)/gnome-voice-server
dist_config_DATA = data/gnome-voice-server.conf

if HAVE_SYSTEMD
systemdsystemunit_DATA = data/gnome-voice-server.service
endif

data/gnome-voice-server.service: data/gnome-voice-server.service.in Makefile
	$(AM_V_GEN)$(MKDIR_P) data && \
	sed -e 's|@bindir[@]|$(bindir)|g' \
	    -e 's|@sysconfdir[@]|$(sysconfdir)|g' \
	    -e 's|@gvs_storagedir[@]|$(gvs_storagedir)|g' \
	    $(srcdir)/data/gnome-voice-server.service.in > $@

CLEANFILES = data/gnome-voice-server.service

# ---------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------
check_PROGRAMS = tests/test-voicegram
tests_test_voicegram_SOURCES = tests/test-voicegram.c
tests_test_voicegram_LDADD = libgvs.a $(GVS_LIBS)

TESTS = tests/test-voicegram tests/test-roundtrip.sh
TESTS_ENVIRONMENT = \
	GVS_SERVER=$(abs_top_builddir)/gnome-voice-server \
	GVS_SEND=$(abs_top_builddir)/gnome-voice-send \
	GVS_SRCDIR=$(abs_top_srcdir)

EXTRA_DIST = \
	autogen.sh \
	README.md \
	PROTOCOL.md \
	data/gnome-voice-server.service.in \
	gnome-voice-server.spec.in \
	tests/test-roundtrip.sh \
	tests/data/GNOME.voice
