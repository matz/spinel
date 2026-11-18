# A public_send whose method name is not a literal may be const_get: the
# lambda starts where its write runs.
L = -> { p :public_send }
n = :const_get
Object.public_send(n, :L).call
