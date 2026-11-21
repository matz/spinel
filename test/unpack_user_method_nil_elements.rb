# A user-defined unpack method returns its own Array, and a nil element
# of the returned Array reads as nil. The nil padding rule belongs to
# String#unpack.
class K
  def unpack(f)
    a = [1]
    a[2] = 3
    a
  end
end
K.new.unpack("C*").each { |v| p v }
p K.new.unpack("C*").map { |v| v.nil? }

def unpack(f)
  a = [1]
  a[2] = 3
  a
end
unpack("C*").each { |v| p v }
p "\x01".b.unpack("C2").map { |v| v.nil? }
