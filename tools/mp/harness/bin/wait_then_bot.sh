#!/bin/bash
# Start an mp_bot only once another console has printed a line (a MARK, not a fixed time): the harness's bots have no start trigger.
#   wait_then_bot.sh <label of the console to watch> <ERE with '.' for every space> <mp_bot arguments...>
# $HARNESS_RUN_DIR is <pair>/<this bot's label> (run_pair.sh), so the watched console's log is <pair>/<label>/console.log.
# Gives up (and starts the bot anyway, so the rule that needs it fails visibly) after 600 s.
label=$1; rx=$2; shift 2
log="$HARNESS_RUN_DIR/../$label/console.log"
n=0
until grep -qE -- "$rx" "$log" 2>/dev/null; do
  sleep 0.2; n=$((n + 1)); [ $n -gt 3000 ] && break
done
echo "wait_then_bot: mark seen ($rx in $label), starting the bot"
exec /tmp/harness_core_tests/mp_bot "$@"
