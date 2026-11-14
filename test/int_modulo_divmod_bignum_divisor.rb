# Integer#modulo and #divmod by a Bignum: a negative receiver's modulo is
# past the word, as `%` answers it.
big = 2**64
p 7.modulo(big)
p((-7).modulo(big))
p 7.divmod(big)
p((-7).divmod(big))
x = -3
p x.modulo(big)
p x.divmod(big)
p 7.modulo(2**64)
p 7 % big
p 7.remainder(big)
p 7.div(big)
