# Array#pack on an Array that mixes Strings and Integers reads each String's
# whole byte length, as CRuby does: a String carrying a NUL keeps every byte
# instead of stopping at the first one (#7249). aws-eventstream packs its
# message prelude this way.

lead = [0, 1].pack("C*")
mid = "ab" + [0].pack("C") + "cd"
tail = "ab" + [0].pack("C")

# a*, A* and Z* with a NUL at the start, in the middle and at the end
p [lead, 7].pack("a*N").unpack1("H*")
p [mid, 7].pack("a*N").unpack1("H*")
p [tail, 7].pack("a*N").unpack1("H*")
p [mid, 7].pack("A*N").unpack1("H*")
p [mid, 7].pack("Z*N").unpack1("H*")

# a counted width truncates or pads past the NUL, not at it
p [mid, 7].pack("a3N").unpack1("H*")
p [mid, 7].pack("a8N").unpack1("H*")
p [mid, 7].pack("A8N").unpack1("H*")

# a String that is itself packed bytes, as an event-stream prelude
prelude = [43, 22].pack("NN")
p [prelude, 0x27e7742d].pack("a*N").unpack1("H*")

# u and B read the whole String too
p [mid, 7].pack("uN")
p ["1" + [0].pack("C") + "1", 7].pack("B*N").unpack1("H*")

# unchanged: a NUL-free String, and an Array of Strings only
p ["abc", 7].pack("a*N").unpack1("H*")
p [mid, "x"].pack("a*a*").unpack1("H*")
