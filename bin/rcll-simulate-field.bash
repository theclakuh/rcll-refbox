#!/bin/bash

MACHINES=(BS CS1 CS2 RS1 RS2 DS)
TEAMS=(C)

if [[ -n $TMUX ]]; then
	for t in ${TEAMS[@]}; do
		for m in ${MACHINES[@]}; do
			tmux new-window bash -c "./rcll-simulate-mps -m ${t}-${m}; read"
		done
	done
else
	echo "Must be started in tmux! Exiting."
	exit 1
fi
