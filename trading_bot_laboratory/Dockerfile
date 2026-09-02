# ---------------------------------------------------------
# ETAPA 1: Compilador de C++ (Hands)
# ---------------------------------------------------------
FROM gcc:12-bookworm AS cpp-builder

RUN apt-get update && apt-get install -y libcurl4-openssl-dev nlohmann-json3-dev
WORKDIR /build

# Copiem el codi des de les subcarpetes del host a l'entorn de compilació
COPY hands/alpaca_bot_refactored/src/ ./src/
COPY hands/alpaca_bot_refactored/include/ ./include/
COPY Makefile ./

RUN make hands

# ---------------------------------------------------------
# ETAPA 2: Compilador d'OCaml (Brain)
# ---------------------------------------------------------
FROM ocaml/opam:debian-12-ocaml-5.0 AS ocaml-builder

WORKDIR /build

# Copiem els fitxers .ml i assignem l'usuari opam per evitar problemes de permisos
COPY --chown=opam:opam brain/brain_refactored/*.ml ./
COPY --chown=opam:opam Makefile ./

RUN eval $(opam env) && make brain

# ---------------------------------------------------------
# ETAPA 3: Imatge Final (Execució)
# ---------------------------------------------------------
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y libcurl4 ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /app

# Extraiem només els binaris compilats
COPY --from=cpp-builder /build/hands .
COPY --from=ocaml-builder /build/brain .

RUN echo "" > data.csv

CMD ["./hands"]
