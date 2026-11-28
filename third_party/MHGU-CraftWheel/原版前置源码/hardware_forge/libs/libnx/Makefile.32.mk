all:
	$(MAKE) -C nx/ -f Makefile.32.mk

install:
	$(MAKE) -C nx/ install -f Makefile.32.mk

clean:
	$(MAKE) -C nx/ clean -f Makefile.32.mk
 