#!/usr/bin/env sh
# Publish the canonical agent skills (tools/agents/skills/) as Claude Code
# project skills (.claude/skills/), so the same content is usable both by the
# independent agent tooling AND by Claude Code (/<name>, Skill tool).
#
# Canonical source of truth: tools/agents/skills/<name>/
# Generated copy (do not hand-edit): .claude/skills/<name>/
#
# Claude Code requires the SKILL.md frontmatter `name:` to match its directory
# name, so this rewrites `name:` in each published copy to the dir basename.
# Re-run after editing anything under tools/agents/skills/.
set -eu

repo_root=$(cd "$(dirname "$0")/.." && pwd)
src="$repo_root/tools/agents/skills"
dst="$repo_root/.claude/skills"

[ -d "$src" ] || { echo "no $src"; exit 0; }
mkdir -p "$dst"

for d in "$src"/*/; do
    [ -f "$d/SKILL.md" ] || continue
    name=$(basename "$d")
    rm -rf "$dst/$name"
    cp -r "$d" "$dst/$name"
    sed -i "s/^name:.*/name: $name/" "$dst/$name/SKILL.md"
    echo "published skill: $name"
done
