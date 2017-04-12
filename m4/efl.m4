dnl efl.m4
dnl Copyright (C) 2017 Obsidian-Studios, Inc.
dnl Author William L. Thomson Jr.
dnl
dnl This file is part of PINENTRY.
dnl
dnl PINENTRY is free software; you can redistribute it and/or modify
dnl it under the terms of the GNU General Public License as published by
dnl the Free Software Foundation; either version 2 of the License, or
dnl (at your option) any later version.
dnl
dnl PINENTRY is distributed in the hope that it will be useful,
dnl but WITHOUT ANY WARRANTY; without even the implied warranty of
dnl MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
dnl GNU General Public License for more details.
dnl
dnl You should have received a copy of the GNU General Public License
dnl along with this program; if not, write to the Free Software
dnl Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA

dnl Autoconf macro to find EFL
dnl
dnl sets PINENTRY_EFL_LIBS and PINENTRY_EFL_CFLAGS
dnl
dnl if EFL was found have_efl_libs is set to yes


AC_DEFUN([FIND_EFL],
[
  AC_ARG_ENABLE(pinentry-efl,
                AC_HELP_STRING([--disable-pinentry-efl],
                           [Don't use efl even if it is available.]),
                enable_pinentry_efl=$enableval,
                enable_pinentry_efl="yes")
])
