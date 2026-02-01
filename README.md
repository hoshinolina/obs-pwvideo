# OBS PipeWire Video Source

This source is a fork of the OBS PipeWire video code (used for screen/window capture on Wayland and PipeWire webcam capture), modified to work as a generic video source. It is the video counterpart to the JACK audio source, and serves a similar purpose as the Spout2 and Syphon sources on Windows and macOS, respectively.

It is designed to work with apps that can send video using PipeWire, such as those using [libfunnel](https://github.com/hoshinolina/libfunnel/).

This fork also serves as a testbed for improvements to the OBS PipeWire code, which can hopefully be upstreamed over time.

## Differences with the built-in OBS PipeWire support

* No portals used/required (this still works in Flatpak as the Flatpak already has PipeWire permissions to support the JACK source)
* Improved PipeWire video support (bugfixes, support for double buffering, and lazy mode for pull-based frame pacing)
* Support for selecting arbitrary PipeWire video sources and auto-reconnect without user action (WIP)

## Why not portals?

Portals are a great concept. Unfortunately, xdg-desktop-portal doesn't really support this use case currently, and it would require multiple changes to work well:

* Implementation of [App to App Media Sharing](https://github.com/flatpak/xdg-desktop-portal/discussions/1141)
* A refactor of session resume to be more robust and reliable (the current approach is not appropriate for professional streaming setups)
* Support for a coarse-permissioned "user-managed" mode for power users who wish to manage video stream connections manually (e.g. with qpwgraph)

If and when these features make it into xdg-desktop-portal, then this plugin will be archived and users will be able to transition to a portal-enabled solution, which would be bundled with OBS instead of being a separate plugin.

## Dependencies

This plugin has been tested with PipeWire 1.2.4 or later. The minimum build requirement is 0.3.83.

Preliminary explicit sync support requires PipeWire 1.2.0. Full explicit sync support will require 1.6.0 (not released yet nor implemented).

## Credits

The PipeWire code is based on the upstream OBS source code, which at the time of forking had the following contributors:

* Georges Basile Stavracas Neto
* tytan652
* columbarius
* Kurt Kartaltepe
* Wim Taymans
* PatTheMav
* Ivan Molodetskikh
* WizardCM
* PancakeTAS
* Norihiro Kamae
* Vainock
* Ryan Foster
* Robert Mader
* Lain
* Joshua Ashton
* Jan Beich
* Doğukan Korkmaztürk
* dimtpap
* Hoshino Lina
* Aleix Pol
