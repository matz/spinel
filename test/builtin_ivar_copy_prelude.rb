# A builtin copy's receiver stays in scope for the call's hoisted work.
# spinel: gc-minor
class Hash
  attr_accessor :label
end

hash = {}
hash.instance_variable_set(:@tag, 7)
p hash.slice(:a).merge(b: 1)

def attributes(hash)
  hash.slice(:a)
end
p [1, 2].map { |n| attributes(hash).merge(b: n) }

$events = []
def receiver(hash)
  $events << :receiver
  hash
end

def argument(value)
  $events << :argument
  { b: value }
end

hash = { a: 1 }
hash.label = :original
copy = receiver(hash).merge(argument(2))
p $events, copy, copy.label, hash.label, hash

$events.clear
copy = receiver(hash).merge(b: argument(3)[:b])
p $events, copy, copy.label

p attributes(hash).dup, attributes(hash).clone, attributes(hash).compact
p attributes(hash).merge({}), attributes(hash).merge(b: nil)
p attributes(hash).merge(b: Object.new).keys

hash.freeze
copy = receiver(hash).merge(argument(4))
p hash.frozen?, copy.frozen?, copy.label
p hash.dup.label, hash.clone.label, hash.dup.frozen?, hash.clone.frozen?

def maybe_hash(flag, hash)
  flag ? hash : nil
end
p maybe_hash(true, hash).dup.label
p maybe_hash(false, hash).dup

$events.clear
p maybe_hash(false, hash)&.merge(argument(5))
p $events

$events.clear
p(false && receiver(hash).merge(argument(6)))
p(true || receiver(hash).merge(argument(6)))
p $events

class HashChild < Hash
  def merge(other)
    [:override, other[:b]]
  end
end
p HashChild.new.merge(b: 8)
