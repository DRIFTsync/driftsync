#!/bin/sh

docker run -it --rm --init -v $PWD:/source zenika/kotlin \
	kotlinc -script /source/driftsync.kts -- $*
