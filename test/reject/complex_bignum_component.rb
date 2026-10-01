# A Complex holds its components as machine floats: an Integer past 64 bits
# would read back as a Float, so the construction is refused.
def big
  2**64
end
p Complex(big, 1).real
