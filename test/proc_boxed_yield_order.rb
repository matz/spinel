class Yielder
  def take(*values, &block)
    [values, block.call]
  end

  def yield(*values, **kw, &block)
    [values, kw, block&.call]
  end
end

def block_value
  puts "block"
  proc { "block result" }
end

def choose(values, index)
  puts "receiver"
  values[index]
end

def arguments
  puts "positionals"
  [1, 2]
end

def keywords
  puts "keywords"
  {marker: 3}
end

callback = -> (*values, **kw, &block) { [values, kw, block&.call] }
list = [callback, Yielder.new]
list.length.times do |index|
  p choose(list, index).yield(*arguments, **keywords, &block_value)
  p choose(list, index).yield
  p choose(list, index).yield(1, marker: 2) { "literal result" }
end

p choose(list, 1).take { :zero }
p choose(list, 1).take(9) { :one }

curry = ->(first, second, &block) { [first, second, block&.call] }.curry
partial = curry.call("first")
curries = [partial, Yielder.new]
p choose(curries, 0).yield("second") { "fixed block" }
p choose(curries, 0).yield(*["second"], &block_value)
p choose(curries, 0).call(*["second"]) { "call block" }
p choose(curries, 0).yield("second", &nil)
zero = ->(&block) { block&.call }.curry
p choose([zero, Yielder.new], 0).yield { "zero block" }

first = ->(value, &block) { [value, block&.call] }
second = ->(value, &block) { [value, block&.call] }
wrapped = first.curry.to_proc.curry(1)
p choose([wrapped, Yielder.new], 0).yield(1) { "wrapped" }
right = (first >> second).curry(1)
left = (second << first).curry(1)
p choose([right, Yielder.new], 0).yield(2) { "right" }
p choose([left, Yielder.new], 0).yield(*[3]) { "left" }
p (first >> second).call(4) { "ordinary call" }

def keep_proc(&block)
  block
end

saved = first.curry
captured = keep_proc(&saved).curry(1)
p choose([captured, Yielder.new], 0).yield(5) { "captured" }
two_arguments = ->(x, y, &block) { [x, y, block&.call] }.curry
p keep_proc(&two_arguments).call(6, 7) { "two arguments" }

class LaterKeeper
  def keep(&block)
    block
  end
end

def later_keeper
  LaterKeeper.new
end

def keep_through_call(value)
  later_keeper.keep(&value)
end

def check_later_receiver
  value = ->(x, y, &block) { [x, y, block&.call] }.curry
  p keep_through_call(value).call(8, 9) { "later receiver" }
end

check_later_receiver

class AnotherKeeper
  def keep(&block)
    block
  end
end

def make_kept_curry
  puts "make curry"
  ->(x, y, &block) { [x, y, block&.call] }.curry
end

[LaterKeeper.new, AnotherKeeper.new].each do |keeper|
  kept = keeper.keep(&make_kept_curry)
  p kept.call(10, 11) { "boxed receiver" }
end
