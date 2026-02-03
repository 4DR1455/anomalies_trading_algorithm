# ---------------------------------------------------------
# ETAPA 1: Compilador de C++ (Hands)
# ---------------------------------------------------------
FROM gcc:12-bookworm AS cpp-builder

# Instal·lem les dependències
RUN apt-get update && apt-get install -y libcurl4-openssl-dev nlohmann-json3-dev

WORKDIR /build

# Copiem el codi C++
COPY hands_api.cc Makefile ./
#COPY test_api.cc Makefile ./

# Compilem
RUN make hands


# ---------------------------------------------------------
# ETAPA 2: Compilador d'OCaml (Brain) - ARREGLAT
# ---------------------------------------------------------
FROM ocaml/opam:debian-12-ocaml-5.0 AS ocaml-builder

WORKDIR /build

# --- FIX: Afegim --chown=opam:opam ---
# Això assegura que l'usuari 'opam' sigui el propietari dels fitxers
# i pugui escriure a la carpeta per generar els compilats.
COPY --chown=opam:opam brain7_2.ml Makefile ./

# Compilem (ara tindrà permisos)
RUN eval $(opam env) && make brain

# ---------------------------------------------------------
# ETAPA 3: Imatge Final (Execució)
# ---------------------------------------------------------
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y \
    libcurl4 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

#COPY --from=cpp-builder /build/test_api .
COPY --from=cpp-builder /build/hands .
COPY --from=ocaml-builder /build/brain .

RUN echo "" > data.csv

CMD ["./hands"]