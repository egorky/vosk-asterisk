MAINTAINERCLEANFILES          = Makefile.in

AM_CPPFLAGS                   = -I$(top_srcdir)/include $(ASTERISK_INCLUDES) $(GRPC_CFLAGS) $(GOOGLE_CLOUD_SPEECH_CFLAGS)
AM_CFLAGS                     = -DAST_MODULE_SELF_SYM="__internal_res_speech_google"

moddir                        = $(ASTERISK_MODDIR)
mod_LTLIBRARIES               = res_speech_google.la

res_speech_google_la_SOURCES = res_speech_google.c
res_speech_google_la_LDFLAGS = -avoid-version -no-undefined -module -shared
res_speech_google_la_LIBADD  = $(GRPC_LIBS) $(GOOGLE_CLOUD_SPEECH_LIBS)

load:
	asterisk -rx "module load res_speech_google.so"

unload:
	asterisk -rx "module unload res_speech_google.so"
