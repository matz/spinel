# A boxed value that may be an Array or an Enumerator: an Enumerator has
# none of Array's last, join, at, flatten, sample or values_at, so the call
# raises NoMethodError, as in CRuby. Before, the Enumerator answered with
# the members of the Array behind it.
def lst(r)
  p r.last(2)
rescue NoMethodError => e
  puts e.message
end
lst([3, 1, 2, 5])
lst((1..4))
lst([3, 1, 2, 5].each)
lst([3, 1, 2].each_slice(2))
lst(Enumerator.new { |y| y << 1; y << 2 })

def lst1(r) = (r.last rescue $!.message)
p lst1([3, 1])
p lst1([3, 1].map)

def jn(r) = (r.join("-") rescue $!.message)
p jn([1, 2])
p jn([1, 2].each)

def at0(r) = (r.at(0) rescue $!.message)
p at0([1, 2])
p at0([1, 2].each_with_index)

def fl(r) = (r.flatten rescue $!.message)
p fl([[1], 2])
p fl([1, 2].each)

def sm(r) = (r.sample.class rescue $!.message)
p sm([1])
p sm([1].each)

def va(r) = (r.values_at(0) rescue $!.message)
p va([1])
p va([1].each)
