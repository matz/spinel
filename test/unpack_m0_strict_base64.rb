# String#unpack1("m0") rejects base64 that RFC 4648 does not allow
# The strict decoder only rejected bytes outside the alphabet. It stopped at
# the first "=", accepted a length that is not a multiple of 4, and did not
# check the bits under the padding, so "Zg", "Zg=", "Zh==" and "Zg==Zg=="
# decoded to "f" where CRuby raises ArgumentError.
def decode(s)
  s.unpack1("m0").inspect
rescue ArgumentError => e
  "#{e.class}: #{e.message}"
end

# valid input
p decode(""), decode("Zg=="), decode("Zm8="), decode("Zm9v"), decode("Zm9vYmE=")
p decode("Zm8+"), decode("Zm8/"), decode("YQ=="), decode("YWI=")

# a length that is not a multiple of 4
p decode("Zg"), decode("Zg="), decode("Zm9vYg")

# "=" anywhere but the padding of the last block
p decode("Zg==Zg=="), decode("Zm=v"), decode("===="), decode("Z==="), decode("Zm9v=")

# nonzero bits under the padding
p decode("Zh=="), decode("YR=="), decode("YWJ=")

# bytes outside the alphabet, as before
p decode("Zm9v\n"), decode(" Zm9v"), decode("Zm8-"), decode("Zm8_")

# unpack, and the lenient "m", which still skips and stops
p "Zm9v".unpack("m0")
p "Zg".unpack1("m"), "Zg==Zg==".unpack1("m"), "Zh==".unpack1("m")
