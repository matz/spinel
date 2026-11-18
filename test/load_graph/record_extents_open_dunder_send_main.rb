# A __send__ of const_get reaches a constant by a computed name: the
# lambda starts where its write runs.
L = -> { p :dunder_send }
Object.__send__(:const_get, "L").()
