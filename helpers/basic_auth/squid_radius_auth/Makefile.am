#
#  Makefile for the Squid LDAP authentication helper
#
#  $Id$
#
#  Uncomment and customize the following to suit your needs:
#

libexec_PROGRAMS 	= squid_radius_auth
man_MANS 		= squid_radius_auth.8
EXTRA_DIST		= squid_radius_auth.8
squid_radius_auth_SOURCES = \
			squid_rad_auth.c \
			radius.h \
			util.c util.h

LDADD			= -L$(top_builddir)/lib -lmiscutil $(SSLLIB) $(XTRA_LIBS)
INCLUDES		= -I$(top_srcdir)/include
