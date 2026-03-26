#!/usr/bin/env bash
set -euo pipefail

# Clone benchmark repositories for MCP vs Explorer quality comparison.
# Uses shallow clones (--depth 1) to minimize disk usage.
# Shared repos are cloned once and symlinked for secondary languages.

BENCH_DIR="${1:-/tmp/bench}"

clone() {
    local lang="$1" repo="$2" subdir="${3:-}"
    local dest="$BENCH_DIR/$lang"
    if [ -d "$dest" ]; then
        echo "SKIP: $lang (exists)"
        return
    fi
    echo "CLONE: $lang <- $repo"
    git clone --depth 1 --quiet "https://github.com/$repo.git" "$dest"
    echo "  OK: $(du -sh "$dest" | cut -f1)"
}

symlink() {
    local lang="$1" source_lang="$2"
    local dest="$BENCH_DIR/$lang"
    if [ -d "$dest" ] || [ -L "$dest" ]; then
        echo "SKIP: $lang (exists)"
        return
    fi
    echo "LINK: $lang -> $source_lang"
    ln -s "$BENCH_DIR/$source_lang" "$dest"
}

mkdir -p "$BENCH_DIR"

# Programming languages — Tier 1 (44 languages)
clone go          "go-chi/chi"
clone python      "httpie/cli"
clone javascript  "expressjs/express"
clone typescript  "trpc/trpc"
clone tsx         "shadcn-ui/ui"
clone java        "spring-projects/spring-petclinic"
clone kotlin      "JetBrains/Exposed"
clone scala       "playframework/play-samples"
clone rust        "meilisearch/meilisearch"
clone c           "redis/redis"
clone cpp         "google/leveldb"
clone csharp      "ardalis/CleanArchitecture"
clone php         "koel/koel"
clone ruby        "sinatra/sinatra"
clone lua         "awesomeWM/awesome"
clone bash        "bash-it/bash-it"
clone zig         "tigerbeetle/tigerbeetle"
clone haskell     "jgm/pandoc"
clone ocaml       "ocaml/dune"
clone elixir      "plausible/analytics"
clone erlang      "ninenines/cowboy"
clone objc        "AFNetworking/AFNetworking"
clone swift       "Alamofire/Alamofire"
clone dart        "felangel/bloc"
clone perl        "mojolicious/mojo"
clone groovy      "spockframework/spock"
clone r           "tidyverse/dplyr"
clone clojure     "clojure/clojure"
clone fsharp      "giraffe-fsharp/Giraffe"
clone julia       "SciML/DifferentialEquations.jl"
clone vimscript   "SpaceVim/SpaceVim"
clone nix         "NixOS/nixpkgs"
clone commonlisp  "lem-project/lem"
clone elm         "elm/compiler"
clone fortran     "cp2k/cp2k"
clone cobol       "OCamlPro/gnucobol"
clone verilog     "YosysHQ/yosys"
clone emacslisp   "emacs-mirror/emacs"
clone matlab      "acristoffers/tree-sitter-matlab"
clone lean        "leanprover-community/mathlib4"
clone form        "vermaseren/form"
clone wolfram     "WolframResearch/WolframLanguageForJupyter"

# Helper languages — Tier 2 (22 languages)
clone yaml        "kubernetes/examples"
clone hcl         "terraform-aws-modules/terraform-aws-eks"
clone scss        "twbs/bootstrap"
clone dockerfile  "docker-library/official-images"
clone cmake       "Kitware/CMake"
clone protobuf    "googleapis/googleapis"
clone graphql     "graphql/graphql-spec"
clone vue         "vuejs/vue"
clone svelte      "sveltejs/svelte"
clone meson       "mesonbuild/meson"

# Shared repos (symlinked — language uses same repo as primary)
symlink html      javascript    # Express views contain HTML
symlink css       tsx           # shadcn-ui styles
symlink toml      rust          # meilisearch Cargo.toml + config
symlink sql       java          # spring-petclinic SQL schemas
clone cuda        "NVIDIA/cuda-samples"
symlink json      typescript    # trpc JSON configs
symlink xml       java          # spring-petclinic XML configs
symlink markdown  python        # httpie docs
symlink makefile  c             # redis Makefile
clone glsl        "repalash/Open-Shaders"
symlink ini       python        # httpie .cfg/.ini files
symlink magma     lean          # .m files — disambiguated via content markers
symlink kubernetes yaml         # YAML subtype — Deployment/Service manifests
symlink kustomize yaml          # YAML subtype — kustomization.yaml

echo ""
echo "=== Clone complete ==="
ls -1 "$BENCH_DIR/" | wc -l | xargs printf "%s repos ready in $BENCH_DIR\n"
