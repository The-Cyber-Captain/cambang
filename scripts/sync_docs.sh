#!/usr/bin/env bash
set -euo pipefail

declare -A TOPICS=(
  ["docs/README.md"]="README.md"
  ["docs/THIRD_PARTY_NOTICES.md"]="THIRD_PARTY_NOTICES.md"
  #["LICENSE"]=""
  ["docs/CONTRIBUTING.md"]="CONTRIBUTING.md"
  #["docs/CHANGELOG.md"]="CHANGELOG.md addons/aide_de_cam/docs/CHANGELOG.md"
)

strip_release_blocks() {
  # Removes:
  # <!-- RELEASE:EXCLUDE:BEGIN --> ... <!-- RELEASE:EXCLUDE:END -->
  perl -0777 -pe 's/<!--\s*RELEASE:EXCLUDE:BEGIN\s*-->.*?<!--\s*RELEASE:EXCLUDE:END\s*-->\n?//sg' "$1"
}

rewrite_docs_asset_paths_for_root() {
  # Avoid duplication screenshots dir. Just re-path.
  # In docs/README.md, author image/link paths as: (screenshots/...)
  # In root README.md, they must be: (docs/screenshots/...)
  # HTML <img ... src=...> (quoted or unquoted, whitespace tolerant, case-insensitive)

  perl -pe '
  s#(\]\()screenshots/#$1docs/screenshots/#g;
  s#(\]\[)screenshots/#$1docs/screenshots/#g;

  s#(<img\b[^>]*\bsrc\s*=\s*)(["'\''"]?)screenshots/#$1$2docs/screenshots/#gi;
' "$1"

}

# 1) Sync fixed-topic docs
for src in "${!TOPICS[@]}"; do
  [[ -f "$src" ]] || { echo "Missing source: $src" >&2; exit 1; }

  for dst in ${TOPICS[$src]}; do
    mkdir -p "$(dirname "$dst")"

    if [[ "$src" == "docs/README.md" && "$dst" == "README.md" ]]; then
      # Root README: keep screenshots, but rewrite asset paths for root visibility.
      rewrite_docs_asset_paths_for_root "$src" > "$dst"
      continue
    fi

 #   if [[ "$src" == "docs/README.md" && "$dst" == addons/*/docs/README.md ]]; then
 #     # Addons README: strip screenshot blocks entirely (no images shipped / no broken paths).
 #     strip_release_blocks "$src" > "$dst"
 #     continue
 #   fi

    cp "$src" "$dst"
  done
done

# 2) Sync schema files (any version)
shopt -s nullglob
schema_sources=(docs/*camera-capabilities*.schema.json docs/*camera-capabilities*.schema.md)
shopt -u nullglob

# if ((${#schema_sources[@]} == 0)); then
#  echo "No schema files found under docs/ matching *camera-capabilities*.schema.(json|md)" >&2
#  exit 1
# fi

# for src in "${schema_sources[@]}"; do
#  [[ -f "$src" ]] || continue
#  base="$(basename "$src")"

#  case "$base" in
#    *.schema.json)
#      dsts=("addons/<addon>/doc_classes/$base")
#      ;;
#    *.schema.md)
#      dsts=("addons/<addon>/docs/$base")
#      ;;
#    *)
#      continue
#      ;;
#  esac

#  for dst in "${dsts[@]}"; do
#    mkdir -p "$(dirname "$dst")"
#    cp "$src" "$dst"
#  done
# done

echo "Synced canonical docs:"
for src in "${!TOPICS[@]}"; do
  echo "  $src -> ${TOPICS[$src]}"
done

# echo "Synced schema files:"
# for src in "${schema_sources[@]}"; do
#  echo "  $src -> addons/<addon>/(doc_classes|docs)/$(basename "$src")"
# done
