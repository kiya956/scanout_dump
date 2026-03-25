dump_fb: dump_fb.c
	gcc -o dump_fb dump_fb.c $(shell pkg-config --cflags --libs libdrm)

clean:
	rm -f dump_fb
