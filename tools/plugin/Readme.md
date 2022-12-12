##ALSA Plugin

The SOF ALSA plugin allows SOF topologies to be run on the host. The plugin
is still WIP with many rough edges that need refined before production
deployment, however the plugin is usable today as a rapid development
framework for SOF infrastructure and processing.

#Features
 * aplay & arecord usage working today
 * alsamixer & amixer usage working today
 * topology binaries are parsed by sof-pipe and modules are loaded as
   SO shared libraries. 
 * pipelines run as individual userspace threads 
 * pipelines can be pinned to efficency cores
 * pipelines can use realtime priority.
 * alsa sink and alsa source modules available.
 * pipelines can block (non blocking and mmap todo)

#License
Code is a mixture of LGPL and BSD 3c.

#Usage
Please build as cmake project, then 

```
sudo make install <ALSA prefix>
sudo hack-install.sh
```

Some hackery is needed currently to install and create soft links
for the processing modules.

Code can then be run by starting sof-pipe with your desired topology

```
 ./sof-pipe -T /lib/firmware/intel/sof-tplg/sof-bdw-nocodec.tplg
```

At this point the sof-pipe daemon is waiting for IPC. Audio applications can now invoke sof-pipe processing via

```
aplay -Dsof:sof-bdw-nocodec.tplg,1 -f dat some48kHz.wav
```
This renders audio to the sof-pipe daemon using the bdw-nocodec topology
on pipeline 1. Likewise

```
arecord -Dsof:sof-bdw-nocodec.tplg,2 -f dat test.wav
```
Will record audio using teh bdw-nocodec topology and pipeline 2.

Mixer settings can be adjusted for bdw-nocodec by

```
alsamixer -Dsof:sof-bdw-nocodec.tplg
```

#Work in Progress
The code is full of ```TODO``` staments that need to be resolved. This
also include the ```hack-install.sh``` install script for module
installation. YMMV.
