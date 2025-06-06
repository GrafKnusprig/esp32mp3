# flac player rebuild

## prerequesites

- remove esp8266audio library
- remove all dependencies in code/ start from scratch and keep only the pin definitions
- use library miniflac: https://github.com/jprjr/miniflac
- rewrite main.cpp to use miniflac and only play the supported filetypes

## buttons

- the vol up button should be renamed to "BTN_NXT" a short press skips to the next song, a long press skips to the next folder (only if available)
- the vol down button should be renamed to "BTN_PRV" a short press starts the song from the start, a second short press plays the previous song, a long press skips to the previous folder (only if available)
- the mode button switches between play all, shuffle all, play folder, shuffle folder
- play all plays every song one after another in order, starting with the first folder. when the folder ends, it plays the first song of the next folder and so on.
- shuffle all plays all songs from all folders in a true random manner. pla previous and play next song both play a random song here.
- play folder plays all songs of a folder in order. when the folder ends, it starts with the first song of that same folder again.
- shuffle folder plays all songs of one folder in a random manner. but only songs of that folder.

## led

- the led indicates the modes
- colors: play all is green, shuffle all is red, play folder is blue, shuffle folder is yellow
- when the mode is changed, the led pulses in the color of the mode. a 3 second pulse with ramp up and then down again. after that the led is off again to safe battery.

## file handling

- come up with an idea to implement everything we need to realize the folder playing
- minimize sd card writes
- minimize ram usage

## bookmark

- save the current mode in a bookmark file
- save the current playing song in the bookmark file
- after reboot, the bookmark file should be loaded and the player should continue with the saved mode and play the saved song
- we don't have to remember the exact position of the song, just start the song from the beginning. Because writing the current position results in many sd card writes