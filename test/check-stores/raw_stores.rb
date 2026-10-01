# A store the --check-stores self-check found in the C emitted for this
# program: it wrote a value into a C slot of another C type with no
# conversion. #6972 made this fill convert, so no known store is raw now;
# check-stores-test keeps the program as a smoke test of the flag (it
# compiles, and the flag changes the C only by its comments) until a new
# raw example replaces it.
a = [1, 2]
a.fill { |i| "x" }
p a
