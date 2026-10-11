#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Ask y/n questions one at a time, print a summary: tools/ask.sh "question" ...
res=()
for q in "$@"; do
	while :; do
		read -r -p "$q y/n + Enter: " a
		[ "$a" = y ] || [ "$a" = n ] && break
	done
	res+=("$([ "$a" = y ] && echo ok || echo BAD): $q")
done
printf '\nsummary:\n'; printf '  %s\n' "${res[@]}"
