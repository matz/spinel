# spinel: int64
# spinel: share
# spinel: gc-stress
# Force the generic builtin entry beside user methods of the same name.
class OtherSearch
  def rpartition(*a) = :user
  def byteindex(*a) = :user
  def byterindex(*a) = :user
  def byteoffset(*a) = :user
end

def partition_box(o, a)
  /(stale)/ =~ "stale"
  p o.rpartition(*a)
  p [$~ && $~.to_a, $1, $2, $&, $`, $', $+]
rescue => e
  p e.class
end

def index_box(o, a)
  /(stale)/ =~ "stale"
  p o.byteindex(*a)
  p [$~ && $~.to_a, $1, $&, $`, $']
rescue => e
  p e.class
  p [$&, $1]
end

def rindex_box(o, a)
  /(stale)/ =~ "stale"
  p o.byterindex(*a)
  p [$~ && $~.to_a, $1, $&, $`, $']
rescue => e
  p e.class
  p [$&, $1]
end

def offset_box(o, a)
  p o.byteoffset(*a)
rescue => e
  p e.class
end

other = OtherSearch.new
["héllol", other, 7].each do |o|
  [[/(l)(z)?/], [/z/], [/()/], ["l"], [], [nil], [1], [1.5], [:l], [[]], [{}], [2**70], ["l", 1], ["l", 1, 2]].each do |a|
    partition_box(o, a)
  end
  [["l"], ["\xA9"], ["\xA9".b], [/(l)/], [/z/], ["l", 2], [/l/, 2], ["l", 3], [/l/, 3], [/l/, -20], [/l/, 20], ["", 7], [], [nil], [1], [1.5], [:l], [[]], [{}], [2**70], ["l", nil], ["l", 1.5], ["l", 2**70], ["l", 0, 1]].each do |a|
    index_box(o, a)
    rindex_box(o, a)
  end
end
m = /(?<accent>é)(?<missing>z)?/.match("héllo")
[m, other, 7].each do |o|
  [[0], [1], [2], [5], [-1], ["accent"], [:missing], [:absent], [], [nil], [1.5], [/x/], [[]], [{}], [2**70], [0, 1], [0, 1, 2]].each do |a|
    offset_box(o, a)
  end
end
# Fixed-arity boxed calls also use the checked runtime accessors.
[m, 7].each do |o|
  begin
    p o.byteoffset(5)
  rescue => e
    p e.class
  end
end
["héllo", 7].each do |o|
  begin
    p o.byteindex("l", 2)
  rescue => e
    p e.class
  end
  /(stale)/ =~ "stale"
  begin
    p o.rpartition(/(l)/)
    p [$~.to_a, $1, $&]
  rescue => e
    p e.class
  end
end
