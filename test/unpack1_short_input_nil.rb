# unpack1 of an Integer directive past the end of its input answers nil.
# Its element was read into the Integer slot with sp_poly_to_i, so the nil
# came back as 0; it is the slot's nil now, through a literal receiver, an
# offset: and a boxed receiver alike.

p "".unpack1("C")
p "a".unpack1("C", offset: 1)
s = ["", 1][0]
p s.unpack1("C")
x = "".unpack1("n")
p x.nil?
p "ab".unpack1("C")
p "ab".unpack1("n")
