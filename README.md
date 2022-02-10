# DRIFTsync
A lightweight and dependency free time synchronisation library with
implementations for various languages and frameworks.

The architecture consists of a server, acting as the reference clock, and
clients that synchronize to this reference clock.

The main output of the system is a common global time among all of the clients.
The actual value of the synchronized timestamp is arbitrary, but it runs in real
time and can be used to measure the relative passage of time.

Drift between the local and server clock is estimated and integrated to improve
the stability of the timestamps in the face of varying network conditions like
jitter and variable latency. These estimations operate on a sliding window and
adapt to fluctuations in the round trip time as well as local and remote clock
skew.

The accuracy when synchronizing to a remote server over a wired internet
connection with reasonably stable network conditions can be expected to be
within tens to a few hundreds of microseconds. Operating over a less stable
wireless connection is expected to still reach accuracies in the hundereds of
microseconds up to a few milliseconds.

This means that the expected jitter in the global timestamp due to network
conditions and clock rate differences between client and server are kept within
these bounds. The current min, max and average jump of the timestamp value due
to synchronization can be queried with the accuracy method of the API when
measureAccuracy is enabled.

## Available Clients
The client side is implemented for the following platforms:

* Python 3 using only the standard library
* Node.js >= 12 using only standard library
* C99 with POSIX networking and threading with a function interface
* Kotlin/JVM using only standard library
* C# targetting .NET Standard 2.0

## Time Scale / Units
The synchronization operates in microseconds and all internal functions use this
time scale.

To reduce the need for converting the large microsecond values in user code, all
API functions that take or return time operate on a scale that can be configured
by the user. This scale can be set on construction and adjusted on the fly with
the scale property. Constants are provided for the most common scales:

```
SCALE_US = microseconds, native
SCALE_MS = milliseconds
SCALE_S = seconds
```

Exception: The sync interval argument to the constructor and the timeout of the
accuracy data getter are in the native unit of the sleep and synchronization
primitives of the respective language.

* Python: seconds possibly with fractional part
* JavaScript: milliseconds
* C: microseconds
* Kotlin: milliseconds
* C#: milliseconds

## Setting Up a Server
The recommended way of running the server is by using a Docker container. A
docker-compose file is provided that includes a setup for the server that can
be run like this:

```
docker-compose up -d server
```

It will build the needed container image and binary from source on first start.

The server is implemented as a portable C application targeting POSIX network
APIs. It can therefore also be built and ran locally by using the Makefile
provided in the server directory.

For non production use a public DRIFTsync server is provided at driftsync.org on
the default port 4318.

## API
All clients provide the same API and follow the same conventions, argument order
and default values. This documentation shows the arguments in abstract form,
listing default values where available.

Note for the C implementation: The C API emulates a class by providing functions
and operating on a struct. The functions follow the naming scheme
`DRIFTsync_{methodName}`. The `DRIFTsync` struct is returned by the
`DRIFTsync_create` function and needs to be supplied as the first argument to
all API calls.

### Constructor / Create function
```
constructor(server="localhost", port=4318, scale=SCALE_US, interval=5s,
measureAccuracy=false)
```

Creates the sync object and sets it up to synchronize to the provided server on
the given port. The constructor automatically spawns and starts threads to
produce synchronization requests and handle responses.

The public APIs will operate with time values at the scale given in the scale
argument and synchronization requests will be sent at the specified interval.

The measureAccuracy argument allows to enable or disable tracking of timestamp
accuracy at each sync response. It defaults to off due to the slight memory and
performance overhead it introduces. When measureAccuracy is disabled, calls to
the accuracy function will always return 0 for all values and time out when
instructed to wait. Accuracy measurements can be enabled and disabled on the fly
with the public measureAccuracy property.

Note for the JavaScript implementation: The arguments should be provided as an
object with keys of the same name. This allows similar behaviour to named
arguments.

### localTime
```
localTime()
```

Returns a high resolution local timestamp with an implementation defined time
base.

The timestamp can be used for relative timekeeping in the local application. It
is different across all clients participating in a system.

### globalTime
```
globalTime()
```

Returns a synchronized global timestamp that is the same across all clients that
synchronize with the same server.

The returned value is 0 in the initial phase right after startup when no
synchronization responses have yet been received.

### suggestPlaybackRate
```
suggestPlaybackRate(globalStartTime, playbackPosition)
```

Returns a suggested playback rate based on a global start time reference and
current playback position. The returned rate aims to keep the local playback in
sync with the global time by speeding up or slowing down to compensate for drift
between the systems.

The globalStartTime argument should be a timestamp acquired using the globalTime
method close to the point in time when playback has started, i.e. the playback
position was 0.

The playbackPosition argument should be the current playback position.

### medianRoundTripTime
```
medianRoundTripTime()
```

Returns the median round trip time of the synchronization requests. This value
can be used to to gauge the latency between the client and server.

During synchronization, responses with a round trip time difference of more than
10 milliseconds to this value will be rejected as they cannot provide a stable
enough timestamp offset.

### accuracy
```
accuracy(wait=false, reset=false, timeout=15s)
```

Returns a three tuple, struct or object with the fields:

```
min, average, max
```

The numbers represent the min, average and max jump of the global timestamp
measured at the worst point in time, i.e. the delta of the global timestamp
just before and just after integrating a new synchronization response. They are
in the selected time scale.

Note that measureAccuracy has to be enabled for measurements to be recorded.
When it is disabled, this method returns immediately with with a valid structure
that has all values set to 0.

The measurement operates on a 10 entry sliding window and therefore represents a
sampling period of 10 * interval. This window can be cleared by setting the
reset argument to true.

The wait argument instructs the method to block and wait for the next accuracy
measurement to come in or time out after the amount of time specified in the
timeout argument. This can be used for continuously monitoring the
synchronization accuracy of a system and is what the demo code does.

### scale
This property determines the scale the input and output timestamps of the public
API shall have. It can be set at runtime and the change immediately takes
effect.

### measureAccuracy
Sets whether or not to keep track of timestamp accuracy on synchronization
responses. Can be enabled and disabled at runtime. Note that after enabling, the
values have to accumulate in the sliding window before becoming meaningful.

### offset
This read only property holds the current average time offset between the local
client and the server.

### clockRate
This read only property holds the clock rate difference between the local client
and the server.

### statistics
This read only property holds lifetime statistics of synchronization requests
and responses. It is a three tuple, struct or object with the following fields:

```
sentRequests, receivedSamples, rejectedSamples
```

The sentRequests counter increments whenever a synchronization request is sent
from the client to the server.

The receivedSamples counter increments whenever a synchronization response is
received, irrespective of whether or not is is eventually integrated.

The rejectedSamples counter is incremented whenever a synchronization response
is disregarded because of a round trip time that strays from the median round
trip time by more than 10 milliseconds.

The packet loss can be calculated by subtracting receivedSamples from
sentRequests. Note that this may temporarily lead to a value > 0 while a request
is in flight.
