all:
	$(MAKE) -C nx/ -f Makefile.32_min.mk

install:
	$(MAKE) -C nx/ install -f Makefile.32_min.mk

clean:
	$(MAKE) -C nx/ clean -f Makefile.32_min.mk
 