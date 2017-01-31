st - simple terminal
--------------------
st is a simple terminal emulator for X which sucks less.

[![Build Status](https://travis-ci.org/shiva/st.svg?branch=patched)](https://travis-ci.org/shiva/st)
[![Join the chat at https://gitter.im/shiva/st](https://badges.gitter.im/Join%20Chat.svg)](https://gitter.im/shiva/st?utm_source=badge&utm_medium=badge&utm_campaign=pr-badge&utm_content=badge)

Requirements
------------
In order to build st you need the Xlib header files.


Installation
------------
Edit config.mk to match your local setup (st is installed into
the /usr/local namespace by default).

Afterwards enter the following command to build and install st (if
necessary as root):

    make clean install


Running st
----------
If you did not install st with make clean install, you must compile
the st terminfo entry with the following command:

    tic -sx st.info

See the man page for additional details.

Applied patches
---------------
The following patches have been applied, and fixed to co-exist

1. clipboard support:
    - st-clipboard-20160727-308bfbf.diff

2. Scrollback support:
    - st-scrollback-20170104-c63a87c.diff
    - st-scrollback-mouse-20161020-6e79e83.diff

3. Align line vertically:
    - st-vertcenter-20160819-023225e.diff

4. support solarized and switch with F6:
    - st-no_bold_colors-20160727-308bfbf.diff
    - st-solarized-both-20160727-308bfbf.diff

Credits
-------
Based on Aurélien APTEL <aurelien dot aptel at gmail dot com> bt source code.

