# spinel: gc-stress
# A String held in a boxed slot -- read from Marshal.load, a mixed Array --
# answers b, scrub, dump, encode, strip, lstrip, rstrip, chomp, chop and
# to_str with a String: no other builtin class has these methods, and the
# receiver is read as a String (NoMethodError for anything else). The
# answers were typed boxed, so everything computed from them was boxed too
# (`buff.b.getbyte(i)`, the index, the sum).

def checksum(buff)
  r_b = buff.b
  r_scrub = buff.scrub
  r_dump = buff.dump
  r_encode = buff.encode
  r_strip = buff.strip
  r_lstrip = buff.lstrip
  r_rstrip = buff.rstrip
  r_chomp = buff.chomp
  r_chop = buff.chop
  r_to_str = buff.to_str
  sum = 0
  i = 0
  while (byte = r_b.getbyte(i))
    sum += byte
    i += 1
  end
  [sum, r_scrub.size, r_dump, r_encode.size, r_strip, r_lstrip, r_rstrip,
   r_chomp, r_chop, r_to_str.size]
end

msgs = Marshal.load(Marshal.dump([" café \n", "ab\xffcd\n".b, 7]))
p checksum(msgs[0])
p checksum(msgs[1])

# b is a binary copy, scrub replaces invalid bytes, dump quotes
s = msgs[0]
bin = s.b
p bin.encoding, bin.bytesize, bin.frozen?
bin << "!"
p s, bin
p msgs[1].scrub.valid_encoding?, [s, 1][0].dump.encoding

# to_str is the String itself: a change through it is seen through the other
u = +"grow"
v = [u, 1][0].to_str
v << "n"
p u, v.equal?(u)

# anything else raises NoMethodError, as in CRuby
[7, :sym, nil].each do |x|
  %i[b scrub dump encode strip lstrip rstrip chomp chop to_str].each do |m|
    begin
      x.public_send(m)
      print m, ":answered "
    rescue NoMethodError
      print m, " "
    end
  end
  puts
end

# a class of the program with its own method of the name keeps its answer
class Blob
  def b = 42
  def strip = :stripped
end
class Note
  def b = "note-b"
  def to_str = "note"
end
[Blob.new, msgs[0]].each { |x| p x.b, x.strip }
[Note.new, msgs[0]].each { |x| p x.b + "!", x.to_str.size }

# a Symbol still answers none of them
p :sym.respond_to?(:strip), :sym.respond_to?(:b), [s, 1][0].respond_to?(:b)
