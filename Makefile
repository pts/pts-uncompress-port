uncompress: uncompress.c
	gcc -s -O2 -ansi -pedantic -W -Wall -o uncompress uncompress.c
uncompress.mini: uncompress.c
	minicc -o uncompress uncompress.c  # https://github.com/pts/minilibc686
uncompress.exe:
	owcc -bwin32 -Wl,runtime -Wl,console=3.10 -Os -s -fno-stack-check -march=i386 -W -Wall -o uncompress.exe uncompress.c
