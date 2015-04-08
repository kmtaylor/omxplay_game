attract_mode=hulkfists.mp4	    # Stream 0
countdown=countdown.mp4		    # Stream 1
game_screen=battle.mp4		    # Stream 2
winner1=hbwins.mp4		    # Stream 3
winner2=hulkwins.mp4		    # Stream 4

#codec_opts="-c copy"

ffmpeg -i $attract_mode -i $countdown -i $game_screen -i $winner1 -i $winner2 \
	       -map 0 -map 1 -map 2 -map 3 -map 4 $codec_opts media.mp4
