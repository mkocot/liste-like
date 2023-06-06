APP_VERSION ?= 0.0.0

LOCAL_CFLAGS := -DAPP_VERSION=\"$(APP_VERSION)\" \
		-std=gnu17 -Wall -Werror -Wno-unused-variable \
		-g -Os

LOCAL_CFLAGS +=	$(CFLAGS)

app: main.c
	$(CC) $(LOCAL_CFLAGS) main.c -o app
test: test.c
	$(CC) test.c -lsystemd -o test
