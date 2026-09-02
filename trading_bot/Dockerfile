# ---------------------------------------------------------
# ETAPA 1: Compilador de C++ (Hands)
# ---------------------------------------------------------
FROM gcc:12-bookworm AS cpp-builder

RUN apt-get update && apt-get install -y cmake libcurl4-openssl-dev nlohmann-json3-dev
WORKDIR /build

# Copiem el codi C++
COPY hands/alpaca_bot_refactored/CMakeLists.txt ./
COPY hands/alpaca_bot_refactored/src/ ./src/
COPY hands/alpaca_bot_refactored/include/ ./include/

# Compilem amb CMake i reanomenem l'executable a 'hands'
RUN cmake . && make && mv alpaca_bot hands

# ---------------------------------------------------------
# ETAPA 2: Compilador d'OCaml (Brain)
# ---------------------------------------------------------
FROM ocaml/opam:debian-12-ocaml-5.0 AS ocaml-builder

WORKDIR /build

# Copiem la carpeta d'OCaml
COPY --chown=opam:opam brain/brain_refactored/ ./

# Compilem (el Makefile intern ja genera 'brain')
RUN eval $(opam env) && make

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