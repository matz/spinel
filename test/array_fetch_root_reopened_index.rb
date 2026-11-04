# An Integer result alone does not prove the index expression is builtin.
# The user operator may allocate and must keep the conservative fetch path.
class Integer
  def +(other)
    GC.start
    self - -other
  end
end

def fetch_reopened(words, pc)
  words.fetch(pc + 1)
end

puts fetch_reopened([11, 22, 33], 0)
puts fetch_reopened([11, 22, 33], -2)
