FROM alpine:3.14

RUN apk add gcc libc-dev make

COPY . /source

RUN cd /source/server && make ARGS=-static

FROM scratch

COPY --from=0 /source/server/driftsync_server /

ENTRYPOINT ["/driftsync_server"]
