FROM docker.io/alpine:latest AS build
RUN apk add --no-cache make gcc musl-dev argp-standalone
VOLUME /out
WORKDIR /app
COPY . /app
# link with argp-standalone library
ENV LDFLAGS='-largp'
# musl has no program_invocation_name so fake it by using define
ENV CFLAGS='-Dprogram_invocation_name=\"listen-like\" -static'
RUN make \
	&& cp listen-like /out
FROM scratch
COPY --from=build --chmod=755 /app/listen-like /usr/local/bin/listen-like
