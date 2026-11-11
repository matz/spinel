# In a program with Struct or Data classes and no Array or Hash subclass,
# a Class value's `k[..]` builds the record class it holds as its `new`
# does, and the Array or Hash it holds as their `[]` do.
S = Struct.new(:a, :b)
D = Data.define(:x)

def pick(i)
  case i
  when 0 then S
  when 1 then D
  when 2 then Hash
  else Array
  end
end

p pick(ARGV.size)[1, 2], pick(ARGV.size)[3]
p pick(ARGV.size + 1)[7]
p pick(ARGV.size + 2)[1, 2], pick(ARGV.size + 3)[1, 2]
