# pts-uncompress-port: a port of the BusyBox uncompress applet to C89 and C++98

pts-uncompress-port is a source-level port of the BusyBox uncompress applet
(itself based on compress-4.2) to a standalone command-line tool implemented
standard C (C89, C99) and standard C++ (C++98). It doesn't add or change
functionality.

The port also works with minicc in
[https://github.com/pts/minilibc686](minilibc686), producing an 1398-byte
Linux i386 executable program. The memory use is about 400 KiB.
