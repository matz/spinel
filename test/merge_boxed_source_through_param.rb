# merge!/update into a hash parameter of a hash that is itself a boxed
# parameter, its callers passing hashes of different kinds: each call's
# hash for it is checked against the receiver's, and a receiver that
# cannot hold its keys or values widens. One that can, or a hash whose
# keys or values are boxed, is left to the store.
def assign(obj, source)
  obj.merge!(source)
  obj
end
p assign({ "a" => 1 }, { "b" => 2 })
p assign({ "c" => "x" }, { "d" => [1] })
x = { "q" => :s }
p assign(x, { "e" => 1 })
p x

def only_source(obj, source)
  obj.merge!(source)
  obj
end
p only_source({ "c" => "x" }, { "b" => 2 })
p only_source({ "c" => "x" }, { "d" => [1] })

def upd(obj, source) = obj.update(source)
p upd({ 1 => 2 }, { 3 => 4 })
p upd({ 1 => 2 }, { "k" => 1.5 })

def two(obj, a, b) = obj.update(a, b)
p two({ "a" => 1 }, { "b" => 2 }, { "c" => 3 })
p two({ "a" => 1 }, { "b" => 2 }, { :c => [3] })

# the receiver a getter answers, and the source boxed at the call: a hash
# that fits, left to the store
class Holder
  attr_reader :h

  def initialize = @h = { "a" => 1 }
  def opts = @opts ||= { "o" => 1 }
end
def into(obj, source)
  obj.merge!(source)
  nil
end
hd = Holder.new
s = [{ "b" => 2 }, 5][0]
into(hd.h, s)
into(hd.opts, s)
p hd.h
p hd.opts

# a source of boxed values (one helper builds them for several kinds) is
# left to the store too, though another call's hash does not fit
def entry(v) = { "n" => v }
label = entry("x")
into(hd.h, entry(2))
into({ 1 => 2 }, { 3 => 4 })
p hd.h
p label

# its keys are known, though: a String key into an Integer-keyed receiver
# widens it; and a nil value fits any receiver's values
def keyed(obj, source) = (obj.merge!(source); obj)
p keyed({ 1 => 2 }, { 3 => 4 })
p keyed({ 1 => 2 }, entry(5))
into(hd.h, { "c" => nil })
p hd.h

# an empty [] or {} in the literal is still an Array or a Hash: a receiver
# of Integer values widens for it, one of boxed values holds it as it is,
# a getter's included
p assign({ "a" => 1 }, { "k" => [] })
p assign({ "a" => 1 }, { "k" => {} })
p assign({ "a" => [1] }, { "k" => [] })
class Lists
  attr_reader :h

  def initialize = @h = { "a" => [1] }
end
ls = Lists.new
into(ls.h, { "k" => [] })
p ls.h
