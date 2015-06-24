Initially i bough for a very low price, an old USB Skype Phone.

I wanted to make it work on Linux, not just the integrated sound card, but also the keypad.

Sound card was already correctly handled by Linux.
Keypad was handled by cm109 kernel driver but there was a problem for some keys ('volume up', 'volume down', 'playback mute' and 'record mute') that when released continue to repeat.
That's why i got sources of cm109 and made some modifications.
The result is in sub-directory 'cm109.ko'. Compilation is done with script make_nohw.sh.

The second phase was to use the Skype phone to make calls.
As i have a little experience on developping Asterisk channel driver with project bcm63xx-phone, i created channel driver 'chan_alsa_input'.
At first i wanted to make small modifications to Asterisk channel driver chan_alsa to make it handle the keypad, but finally code is quite different because the channel driver can handle several lines and is able to generate tones dialing, busy and invalid.
An example configuration is in sub directory 'configs' and must be put in '/etc/asterisk' directory with the name 'alsa_input.conf'.
The main difference with 'alsa.conf' is the presence of parameters :
- 'event_input_device' to specify a device which generate input events. The device should be those created by cm109 kernel driver to handle the phone keypad.
- 'event_output_device' to specify a device to which you can write input events to make it ring. The device should be those created by cm109 kernel driver, but if for example the buzzer of the Skype phone sounds low, it can be the device of the PC speaker.

The final result is in sub-directory 'chan_alsa_input'. It has been tested against Asterisk 11.

Note that the channel driver can be used without the USB Skype Phone.
The parameters 'snd_capture_device' and 'snd_playback_device' could be the name of any ALSA sound card.
The parameter 'event_input_device' could be the name of a FIFO (see man mkfifo) to which some program with a nice GUI (yet to be developped) would write input events.
And, as already mentionned, the parameter 'event_output_device' could be the name of the PC speaker device (or another FIFO to which some program would read input events and generate ring sound). 


