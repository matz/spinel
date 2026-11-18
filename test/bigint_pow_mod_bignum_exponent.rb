# spinel: int64
# Integer#pow(exp, mod) on a Bignum receiver with an exponent that is a
# Bignum, or one known only at run time: the exponent is not converted to a
# word (that raised RangeError), and one that is not an Integer raises
# TypeError as in CRuby.
B = 18446744073709551616   # 2**64

def t(label)
  puts "#{label}: #{yield}"
rescue => ex
  puts "#{label}: #{ex.class}: #{ex.message}"
end

def pick(v) = v
pick("not a number")       # pick returns a value of any class
def maybe_int(k) = k > 0 ? k : nil

t("bignum exponent, word modulus") { (B + 3).pow(B + 5, 1_000_000_007) }
t("bignum exponent, bignum modulus") { (B + 3).pow(B * B + 1, B + 13) }
t("base -1 mod m") { (B + 12).pow(B, B + 13) }
t("negative modulus") { (B + 3).pow(B + 1, -(B + 13)) }
t("negative base") { (-(B + 3)).pow(B + 1, B + 13) }
t("modulus one") { (B + 3).pow(B, 1) }
t("exponent from a boxed value") { (B + 3).pow(pick(B + 5), 97) }
t("word exponent from a boxed value") { (B + 3).pow(pick(5), 97) }
t("word exponent") { (B + 3).pow(5, 1_000_000_007) }
t("negative bignum exponent") { (B + 3).pow(-B, 97) }
t("zero modulus") { (B + 3).pow(B, 0) }
t("float exponent") { (B + 3).pow(2.0, 97) }
t("string exponent") { (B + 3).pow(pick("2"), 97) }
t("rational exponent") { (B + 3).pow(Rational(1, 2), 97) }
t("nil exponent") { (B + 3).pow(pick(nil), 97) }
t("exponent that may be nil, a value") { (B + 3).pow(maybe_int(5), 97) }
t("exponent that may be nil, nil") { (B + 3).pow(maybe_int(0), 97) }
