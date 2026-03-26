dump_fb: dump_fb.c
	gcc -o dump_fb dump_fb.c \
		-I/usr/include/libdrm -ldrm -lgbm -lEGL -lGLESv2

clean:
	rm -f dump_fb
