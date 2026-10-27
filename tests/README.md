# Tests

Run the target classification tests with:

```sh
cmake -S . -B build -G Ninja -DENABLE_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

To exercise the picker against a live PipeWire node, start this test source:

```sh
gst-launch-1.0 videotestsrc is-live=true ! 'video/x-raw,format=RGBA,width=320,height=180,framerate=30/1' ! pipewiresink mode=provide 'stream-properties=props,media.class=Video/Source,node.name=obs-pwvideo-test,node.description=OBS-PWVideo-Test'
```

In OBS with the built plugin loaded, create a PipeWire Video source. Confirm
that `OBS-PWVideo-Test` is hidden by default, appears when **Show video devices
and other sources** is enabled, and can be selected. Confirm existing production
streams remain available, and that toggling the option off doesn't disconnect
the selected source. Stop and restart the test source and verify the selection
returns and reconnects. Remove the test source after testing.

These tests use the current user's PipeWire graph and do not start an OBS stream.
