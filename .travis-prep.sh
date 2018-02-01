#!/bin/sh

"@set UPDATED "`date +"%d %B %Y"`"" > doc/version.texi
"@set UPDATED-MONTH "`date +"%d %B"`"" >> doc/version.texi
"@set EDITION ${MY_PV}" >> doc/version.texi
"@set VERSION ${MY_PV}" >> doc/version.texi

autoreconf -f -i
automake
./configure $@
