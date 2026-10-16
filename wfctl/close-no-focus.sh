#!/usr/bin/env bash
# Zavrie všetky toplevel okná okrem aktívneho (focused)

set -euo pipefail

# Aktívne okno
focused_json=$(wfctl window-rules/get-focused-view)
focused_id=$(echo "$focused_json" | jq -r '.info.id // empty')

if [[ -z "$focused_id" || "$focused_id" == "null" ]]; then
    echo "Žiadne aktívne okno" >&2
    exit 1
fi

echo "Ponechávam aktívne okno ID: $focused_id"

# Všetky views
views_json=$(wfctl window-rules/list-views)

# Zavri všetky toplevel okrem focused
echo "$views_json" | jq -c '.[] | select(.type == "toplevel" and .id != '"$focused_id"')' | while read -r view; do
    id=$(echo "$view" | jq -r '.id')
    title=$(echo "$view" | jq -r '.title // "—"')
    app=$(echo "$view" | jq -r '."app-id" // "—"')
    echo "Zatváram ID $id ($app: $title)"
    wfctl window-rules/close-view id="$id"
done

echo "Hotovo."