OCAMLC=ocamlopt
SOURCES=Types.ml Config.ml Strategy.ml Comm.ml Engine.ml Main.ml
OUT=brain

all:
	$(OCAMLC) -o $(OUT) $(SOURCES)

clean:
	rm -f *.cmi *.cmx *.o $(OUT)
