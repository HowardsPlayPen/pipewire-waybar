# Hyprland widget for interacting with Pipewire devices

I have multiple audio sinks connected (several via HDMI, optical output, bluetooth and usb) and routing sound to the right one is a pain. 
I want a a c++ pipewire client that listens to pipewire events, has a UI for waybar and allows me to choose which sink combination I want.

## Implementation

C++ using CMake
No Boost libraries at all

Waybar widget (described later)


## Features

There should be a pipewire client listening to pipewire events continuously

A config file will define 
* a list of sinks - eg "user friendly name" -> device id
* ability to define a virtual sink - i.e. "user friendly name" -> [array of devices]

The client will have the ability to change ALL sound "input devices" to point to a chosen output device (i.e. one of the list defined in the config). This might be a dbus message that can be contacted from the waybar control - which would then connect all of the sound devices to the specified output sink.

Some of the devices in the list might be transient - i.e. bluetooth devices turned on or off

As new tools start to play sound then the pipewire client should check that they are pointing to the right / chosen sound sink and connect them if necessary.

## Waybar control

In the waybar menu it should display the currently "chosen" sound device (eg query the above pipewire client to obtain it)

If the user hovers (or clicks?) over the control it should show a list of available sinks and let the user choose one - upon choosing a sink the pipewire client can be informed to change all sound to the chosen sink.

When the pipewire client detects a new device connected (from the list in the config) then it should trigger a brief message to the user - eg flashing the widget, then ensure this device is available in the list of sinks that the user can choose.

It would be good to signify the difference between device types - eg virtual sink vs hdmi vs bluetooth as a "nice to have"