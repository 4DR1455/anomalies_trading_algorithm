# Variables de compilació (Bones pràctiques)
CXX = g++
OCAMLOPT = ocamlopt
# Afegeixo -lcurl (per connectar a Alpaca) i -pthread (per als fils)
CXXFLAGS = -Wall -std=c++17 -pthread
LDFLAGS = -lcurl

# Objectiu principal
all: hands brain data

# Compilació del C++ (Gestor / Comunicacions)
# Ara inclou els flags necessaris per xarxa i fils
hands: hands_api.cc
	$(CXX) $(CXXFLAGS) -o hands hands_api.cc $(LDFLAGS)

# Compilació de l'OCaml (Cervell / Lògica)
# Si uses Unix module en OCaml, potser necessites unix.cmxa també
brain: brain7_2.ml
	$(OCAMLOPT) -o brain str.cmxa brain7_2.ml

# Inicialització de fitxers de dades
data:
	echo "" > data.csv

test_api: test_api.cc
	$(CXX) $(CXXFLAGS) -o test_api test_api.cc $(LDFLAGS)

# Neteja (Ja no cal esborrar la pipe)
clean:
	rm -f hands brain *.o *.cmx *.cmi

.PHONY: all clean data