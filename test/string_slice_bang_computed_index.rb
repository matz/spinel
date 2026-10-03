# Arithmetic indexes can be boxed Integers under --int-overflow=promote.
# String#slice! must select its Integer arm in value and statement positions.
s = +"abc"
p s.slice!("zz".size - 2)
p s

i = "zz".size
s = +"abc"
p s.slice!(i - 1)
p s
s = +"abc"
s.slice!(i - 1)
p s

# Negative indexes, the end boundary and indexes outside either end.
s = +"abc"
p s.slice!(i - 3)
p s
s = +"abc"
p s.slice!(i + 1)
p s
p s.slice!(i + 2)
p s
p s.slice!(i - 6)
p s
s.slice!(i + 1)
s.slice!(i - 6)
p s

# Character indexes still count multibyte characters, not bytes.
s = +"aéb"
p s.slice!(i - 1)
p s
s = +"aéb"
s.slice!(i - 1)
p s

# The length and Range overloads already unbox their computed bounds.
s = +"abcd"
p s.slice!(i - 1, 2)
p s
s = +"abcd"
s.slice!(i - 1, i + 0)
p s
s = +"abcd"
p s.slice!((i - 1)..(i + 0))
p s
s = +"abcd"
s.slice!((i - 1)...(i + 1))
p s

# Neighbouring index methods accept the same arithmetic expressions.
s = +"abcd"
p s[i - 1]
p s[i - 1, 2]
p s[(i - 1)..(i + 0)]
p s.byteslice(i - 1)
p s.byteslice(i - 1, 2)
p s.byteslice((i - 1)..(i + 0))
p s.insert(i - 1, "X")
p s

a = [10, 20, 30]
p a[i - 1]
p a.delete_at(i - 1)
p a
p a.insert(i - 1, 99)
p a

# Polymorphic arguments retain the String and Range overloads in both positions.
[1, "bc", "missing", (1..2), (1...3), (-3..-2), (4..), (9..), (..1)].each do |index|
  s = +"abcd"
  p s.slice!(index)
  p s
  s = +"abcd"
  s.slice!(index)
  p s
end

# Argument evaluation happens once, including when the selected arm raises.
[1, "bc", (1..2), nil].each do |index|
  calls = 0
  s = +"abcd"
  begin
    p s.slice!(begin calls += 1; index end)
  rescue TypeError
    puts "TypeError"
  end
  p calls
  p s
end
