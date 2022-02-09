#!/bin/sh

docker run -it --rm --init -v $PWD:/source \
	node:16-alpine3.14 /source/driftsync.js $*
