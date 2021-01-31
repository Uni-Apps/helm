ifndef CONFIG
  CONFIG=Release
endif

all: binary ttl_generator lv2

binary:
	$(MAKE) -f Makefile.binary CONFIG=$(CONFIG) DEBCXXFLAGS="$(DEBCXXFLAGS)" DEBLDFLAGS="$(DEBLDFLAGS)"

ttl_generator:
	$(MAKE) -f Makefile.ttl_generator CONFIG=$(CONFIG)

lv2: ttl_generator binary
	./lv2_ttl_generator build/helm-synth.so
	mkdir -p helm-synth.lv2
	cp build/helm-synth.so helm-synth.lv2
	mv *.ttl helm-synth.lv2

clean:
	$(MAKE) clean CONFIG=$(CONFIG) -f Makefile.binary
	$(MAKE) clean CONFIG=$(CONFIG) -f Makefile.ttl_generator
	rm -rf build
	rm -rf helm-synth.lv2
