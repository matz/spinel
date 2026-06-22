# Integer methods whose argument the runtime takes as an mrb_int must unbox a
# polymorphic argument (here an element of a mixed array, poly-typed but an
# integer at runtime) rather than emitting it as a raw boxed value.
mix = [6, "x"]
b = mix.first

p 255.allbits?(b)
p 255.anybits?(b)
p 256.nobits?(b)
p 100.ceildiv(b)
p 2.pow(b)
p 2.pow(b, 100)
p 255.to_s(b)
p 12.gcd(b)
p 4.lcm(b)
p 17.remainder(b)
