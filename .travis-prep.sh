#!/bin/sh

echo "@set UPDATED "`date +"%d %B %Y"`"" > doc/version.texi
echo "@set UPDATED-MONTH "`date +"%d %B"`"" >> doc/version.texi
echo "@set EDITION ${MY_PV}" >> doc/version.texi
echo "@set VERSION ${MY_PV}" >> doc/version.texi

autoreconf -f -i
automake
./configure $@
