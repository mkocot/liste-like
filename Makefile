APP_VERSION ?= 0.0.0

LOCAL_CFLAGS := -DAPP_VERSION=\"$(APP_VERSION)\" \
		-std=gnu17 -Wall -Werror -Wno-unused-variable \
		-g -Os

LOCAL_CFLAGS +=	$(CFLAGS)
LOCAL_LDFLAGS += $(LDFLAGS)

listen-like: main.c
	$(CC) $(LOCAL_CFLAGS) main.c $(LOCAL_LDFLAGS) -o $@
	strip $@

test: test.c
	$(CC) test.c -lsystemd -o test

docker:
	rm -r $(PWD)/x
	mkdir -p $(PWD)/x
	podman build -v $(PWD)/x:/out -t listen-like-build .
