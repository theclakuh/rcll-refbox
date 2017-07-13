#!/bin/bash

MACHINES=(BS CS1 CS2 RS1 RS2 DS)
TEAMS=(C)
SCREEN=false
if [[ "$TERM" == screen.* ]] ; then
  SCREEN=true
fi

if [[ -n $TMUX ]] || [[ $SCREEN ]]; then
	for t in ${TEAMS[@]}; do
		for m in ${MACHINES[@]}; do
      if [[ -n $TMUX ]] ; then
			  tmux new-window bash -c "./rcll-simulate-mps -m ${t}-${m}; read"
      fi
      if [[ $SCREEN ]]; then
        screen bash -c "./rcll-simulate-mps -m ${t}-${m}; read"
      fi
		done
	done
else
	echo "Must be started in tmux or screen! Exiting."
	exit 1
fi
