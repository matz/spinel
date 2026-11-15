# spinel: gc-stress
# A class of the program with its own b, strip or to_str keeps its answer
# beside a boxed String's: a value that may be either still answers boxed
# where the class's method answers something other than a String (b, strip
# here), and the String's own answer where every one is a String (to_str).

def checksum(buff)
  r_b = buff.b
  r_strip = buff.strip
  r_to_str = buff.to_str
  [r_b, r_strip, r_to_str]
end

class Blob
  def b = 42
  def strip = :stripped
  def to_str = "blob"
end

msgs = Marshal.load(Marshal.dump([" caf\u00e9 \n", Blob.new]))
msgs.each { |m| p checksum(m) }
[Blob.new, msgs[0]].each { |x| p x.b, x.strip, x.to_str.size }

class Note
  def b = "note-b"
end
[Note.new, msgs[0]].each { |x| p x.b + "!" }
