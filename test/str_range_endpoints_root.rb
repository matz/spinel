# A String range is a by-value struct carrying two GC strings, so the struct's
# own address is not a root the collector can follow and its endpoints were
# marked by nothing: not the local's root, not an ivar scan, and a class whose
# only heap-carrying ivar was one got no scan function at all. A range whose
# endpoints nothing else names read back collected memory (#4353).
def mk(i)
  ("a#{i}".."z#{i}")
end

def churn
  k = 0
  while k < 60
    j = "q" * 128
    k += 1 if j.length > 0
  end
end

bad = 0
i = 0
while i < 200
  r = mk(i)
  churn
  bad += 1 unless r.first == "a#{i}" && r.last == "z#{i}"
  i += 1
end
puts "local: #{bad}"

class Holder
  attr_reader :r
  def initialize(i)
    @r = ("a#{i}".."z#{i}")
  end
end

bad2 = 0
j = 0
while j < 200
  h = Holder.new(j)
  churn
  bad2 += 1 unless h.r.first == "a#{j}" && h.r.last == "z#{j}"
  j += 1
end
puts "ivar: #{bad2}"

# through a parameter, and the range still answers its own operations
def span(r)
  churn
  [r.first, r.last, r.to_a.length]
end

bad3 = 0
k = 0
while k < 200
  got = span(("a#{k}".."e#{k}"))
  bad3 += 1 unless got[0] == "a#{k}" && got[1] == "e#{k}"
  k += 1
end
puts "param: #{bad3}"
