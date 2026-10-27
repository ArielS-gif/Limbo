#!/usr/bin/env bash
# One mission on this PC (Git Bash), for trying the mission types: starts one copy of OT6.5 (coop1, via coop-run.sh
# solo), opens the zone with /Game/World/MissionGens/MissionGen_<generator> (`playmission`), waits until the mission is
# loaded and turns on god mode; then prints the mission's objectives.
#   tools/mission-run.sh LaunchTheRocket [Forest|Grassland|Industrial|Suburban|Urban]
set -u
here="$(cd "$(dirname "$0")" && pwd)"
out="$here/../out"
[ $# -ge 1 ] || { echo "usage: $0 <generator> [zone]"; exit 1; }
bash "$here/coop-run.sh" solo | tail -1 || exit 1
sleep 3
printf 'playmission %s\n' "$*" > "$out/command.txt"
for _ in $(seq 1 180); do grep -q -E "objectives: [0-9]+ mission|playmission <generator>" "$out/gameserver.log" && break; sleep 1; done
grep -E "lobby: (playmission|.*'s mission is)|game: LoadMission|objectives: [0-9]+ mission" "$out/gameserver.log" | cut -c16-200
sleep 3
printf 'cheat God\n' > "$out/command.txt"
sleep 2
printf 'objectives\n' > "$out/command.txt"
sleep 2
grep -A12 "> objectives" "$out/gameserver.log" | tail -12 | grep -E "^\[.{12}\]   " | cut -c16-200
