#!/bin/sh

# A simple shell script to help me take the screenshots

screenshot() {
	./wl-games $2 &
	Pid=$!
	printf "%s -- %s: press enter to take screenshot ..." "$2" "$1"
	read _
	grim -g "$(swaymsg -t get_tree | jq -r '.. | select(.pid? and .visible?) | select(.app_id == "wl-games") | .rect | "\(.x),\(.y) \(.width)x\(.height)"')" "$1"
	mogrify -resize 640x480 "$1"
	echo "$1"
	kill $Pid
}


screenshot screenshot.png
for Game in snake sudoku pong tetris car_race breakout
do
	screenshot screenshots/"$Game".png "$Game"
done
