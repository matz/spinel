# Plain append callees must not inherit volatile from an unrelated begin.
def plain_append(s)
  3.times { s << "p" }
end
def unrelated_begin(s)
  begin
    n = 2
  rescue ArgumentError
    n = 1
  end
  plain_append(s)
  puts n
end

# One volatile reaching site qualifies a mixed callee; propagate through
# more than one forwarding hop, and through keyword/default argument hoists.
def leaf(s)
  s << "!"
end
def relay(s)
  leaf(s)
end
def forward(s)
  relay(s)
end
def kw_leaf(s:, suffix: s.length.to_s)
  s << suffix
end
def kw_forward(s)
  kw_leaf(s: s)
end
def mixed(s)
  s << "m"
end
def two_slots(plain, guarded)
  plain << "a"
  guarded << "b"
end
def nested_alias(s)
  s << "n"
  forward(s)
  yield
end
def yield_alias(s)
  s << "y"
  yield s
end
def yield_kw_alias(s)
  s << "k"
  yield item: s
end
def yield_pair(s)
  yield 0, s
end
def block_leaf(s)
  s << "z"
end
def kw_block_leaf(s)
  s << "q"
end
class Parent
  def decorate(s)
    s << "P"
  end
end
class Child < Parent
  def decorate(s)
    super
    s << "C"
  end
end
class Explicit < Parent
  def decorate(s)
    super(s)
    s << "X"
  end
end
def guarded(s, other)
  s << "["
  begin
    s = String.new("fresh")
    yield
  ensure
    forward(s)
    kw_forward(s)
    mixed(s)
    two_slots(other, s)
    puts other
    nested_alias(s) { 0 }
    yield_alias(s) { |item| block_leaf(item) }
    yield_kw_alias(s) { |item:| kw_block_leaf(item) }
    yield_alias(s) { |optional = String.new| optional << "O" }
    yield_pair(s) { |unused = 1, post| post << "T" }
    yield_kw_alias(s) { |item:| item << "K" }
    Child.new.decorate(s)
    Explicit.new.decorate(s)
    puts s
  end
end

# Captures and bound Methods promote their Strings to shared handles.
# These owned capture cells must stay distinct from volatile borrowed slots.
def handle_capture(s)
  s << "C"
  proc { puts s }.call
end
def handle_fiber(s)
  s << "F"
  Fiber.new { puts s; Fiber.yield }.resume
end
def handle_bound(s)
  s << "B"
end
def guarded_handles(s)
  s << "["
  begin
    s = String.new("fresh")
    yield
  ensure
    handle_capture(s)
    handle_fiber(s)
    method(:handle_bound).call(s)
    puts s
  end
end

plain = String.new("plain")
unrelated_begin(plain)
puts plain
normal = String.new("normal")
mixed(normal)
puts normal
caller = String.new("caller")
other = String.new("other")
p guarded(caller, other) { break 37 }
puts caller
handle_caller = String.new("caller")
p guarded_handles(handle_caller) { break 31 }
puts handle_caller
