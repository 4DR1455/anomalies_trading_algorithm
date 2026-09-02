# Variables de compilació (Bones pràctiques)
CXX = g++
OCAMLOPT = ocamlopt
CXXFLAGS = -Wall -std=c++17 -pthread -I./include
LDFLAGS = -lcurl

# Llistes de fitxers font
CPP_SOURCES = src/main.cpp src/Utils.cpp src/AlpacaAPI.cpp src/BrainCommunicator.cpp src/OrderManager.cpp
# És vital mantenir aquest ordre a OCaml per les dependències de mòduls
ML_SOURCES = Types.ml Config.ml Strategy.ml Comm.ml Engine.ml Main.ml

# Objectiu principal
all: hands brain data

# Compilació del C++ (Gestor / Comunicacions)
hands: $(CPP_SOURCES)
	$(CXX) $(CXXFLAGS) -o hands $(CPP_SOURCES) $(LDFLAGS)

# Compilació de l'OCaml (Cervell / Lògica)
brain: $(ML_SOURCES)
	$(OCAMLOPT) -o brain $(ML_SOURCES)

# Inicialització de fitxers de dades
data:
	echo "" > data.csv

# Neteja
clean:
	rm -f hands brain *.o *.cmx *.cmi

.PHONY: all clean data
