#!/bin/sh

docker run -it --rm --init -v $PWD:/source \
	mcr.microsoft.com/dotnet/sdk:3.1-alpine3.14 \
	dotnet run -p /source -- $*
