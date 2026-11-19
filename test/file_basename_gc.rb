# A basename survives after the dynamically built input path goes out of scope.
# Allocate enough to collect before reading the saved names.

class Trash
  def initialize(n)
    @n = n
    @s = "padding payload " * 64    # ~1 KB heap string per Trash
  end
  attr_reader :n
end

def name_of(i)
  path = "/very/deep/parent/directory/with/many/segments/file" + i.to_s + ".rb"
  File.basename(path)
end

names = []
i = 0
while i < 50
  names << name_of(i)
  i = i + 1
end

junk = []
j = 0
while j < 5000
  junk << Trash.new(j)
  j = j + 1
end
puts junk.length     # 5000

# Each names[i] should still resolve to "fileN.rb".
ok = 1
i = 0
while i < 50
  expected = "file" + i.to_s + ".rb"
  if names[i] != expected
    ok = 0
  end
  i = i + 1
end
puts(ok == 1 ? "ok" : "corrupt")    # ok
