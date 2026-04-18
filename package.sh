#!/bin/bash
set -e

VERSION=$(grep -oP '(?<=#define VTREE_VERSION ")[^"]+' main.c)
FOLDER="vTree Gold"
ARCHIVE="vTree Gold ${VERSION}"

echo "Packaging vTree Gold v${VERSION}..."

rm -rf "$FOLDER"
mkdir -p "$FOLDER"

cp vtree config.ini theme.ini "$FOLDER/"
cp -r res fonts lang "$FOLDER/"

zip -r "${ARCHIVE}.zip" "$FOLDER"
mv "${ARCHIVE}.zip" "${ARCHIVE}.muxapp"
rm -rf "$FOLDER"

echo "Done: ${ARCHIVE}.muxapp"
