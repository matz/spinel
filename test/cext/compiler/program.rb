# frozen_string_literal: true
class CextRubyOwner
  class Failure < StandardError
  end
end
require_relative 'sample'
class CextBox
  attr_accessor :ruby_value
end
box = CextBox.new
raise 'external Ruby superclass was lost' unless CextRubyFailure.superclass == CextRubyOwner::Failure
class CextReplyBuffer
  def initialize(value)
    @value = value
  end
  def read
    @value
  end
end
def cext_frozen_reply(buffer)
  value = buffer.read
  value.freeze
  value
end
reply = 'reply'.dup
box.store(cext_frozen_reply(CextReplyBuffer.new(reply)))
GC.start
raise 'C boundary lost returned String identity' unless box.get.equal?(reply)
raise 'C boundary lost returned String freeze' unless box.get.frozen?
sentinel = box.sentinel
raise 'C plain Object allocation' unless sentinel.class == Object
GC.start
raise 'C registered Object identity' unless sentinel.equal?(box.sentinel)
begin
  box.external_error
  raise 'C external class reference failed'
rescue CextRubyOwner::Failure => error
  raise 'C external error message' unless error.message == 'external Ruby error'
end
raise 'C array operations' unless box.array_operations == 9
raise 'C integer conversion' unless box.number_operations(-7) == -7
raise 'C float conversion' unless box.number_operations(1.75) == 1
raise 'C large integer' unless box.large_integer == 9223372036854775807
raise 'C floating number' unless box.floating_number == 1.25
native_hash = box.hash_operations
raise 'C hash string key' unless native_hash['answer'] == 9
raise 'C hash integer key' unless native_hash[3] == 'three'
raise 'C hash overwrite' unless native_hash.size == 2
raise 'C string type check' unless box.check_string('hello') == 'hello'
raise 'C UTF-8 response' unless box.response_string("żółw").encoding == Encoding::UTF_8
raise 'C binary response' unless box.response_string("\xff".b).encoding == Encoding::ASCII_8BIT
raise 'C response embedded NUL' unless box.response_string("a\x00b").bytesize == 3
raise 'C temporary buffer across GC' unless box.temporary_buffer == 31
raise 'C dynamic fdset and timeout' unless box.fdset_operations
raise 'C response invalid UTF-8 after NUL' unless box.response_string("a\x00\xff".b).encoding == Encoding::ASCII_8BIT
begin
  box.c_string("a\x00b")
  raise 'C string accepted embedded NUL'
rescue ArgumentError
end
begin
  box.check_string(7)
  raise 'C string type check accepted integer'
rescue TypeError
end
begin
  box.number_operations(2147483648)
  raise 'C numeric overflow accepted'
rescue RangeError
end
box.store(42)
box.ruby_value = 7
p box.get
p box.ruby_value
p CextBox.count(1, 2, 3)
p box.echo('yielded') { |value| value + ' back' }
s = +'ruby'
t = s
box.decorate(s)
p [s, t]
a = [1, 2]
b = a
box.push(a)
p [a, b]
p CextBox::ONE.equal?(CextBox::TWO)
p CextBox::ONE.equal?(box.constant)
p CextBox::ONE.encoding.name
100.times do
  begin
    box.explode
  rescue RuntimeError => e
    raise 'bad exception' unless e.message == 'from C'
  end
end
box.store('alive')
box.collect
p box.get
p box.echo(12) { |value| value + 3 }

class CextChild < CextBox
  def initialize(value)
    self.ruby_value = value
  end
end
child = CextChild.new(27)
child.store(99)
p [child.get, child.ruby_value]
p CextChild.allocate.get
h = {1 => 2}
box.store(h)
h['key'] = 'value'
p box.get

p box.respond_to?(:value)
p box.value
p box.method(:store).arity
p CextBox.count
p box.packed(1, 2, 3, 4)
strings = [+'s']
box.decorate(strings[0])
p strings
nested = [[1, 2]]
box.nested(nested)
p nested
begin
  box.decorate('frozen')
rescue FrozenError
  puts 'frozen'
end

p box.echo(1) { break 73 }
def return_through_c(box)
  box.echo('value') { return 81 }
  0
end
p return_through_c(box)
p box.echo(5) { |n| box.echo(n + 1) { |v| v + 2 } }
f1 = Fiber.new do
  box.echo(10) { |n| Fiber.yield(n); n + 1 }
end
f2 = Fiber.new do
  box.echo(20) { |n| Fiber.yield(n); n + 2 }
end
p [f1.resume, f2.resume, f1.resume, f2.resume]

p CextBox.count(CextBox.new, CextBox.new)
copy = child.dup
p [copy.equal?(child), copy.get, copy.ruby_value]
box.store(child)
copy2 = box.get.dup
p [copy2.equal?(child), copy2.get, copy2.ruby_value]
inner = [3, 4]
outer = []
outer_alias = outer
outer_alias.push(inner)
box.nested(outer)
p inner

p child.store(5).class == CextChild
p child.get

box.number = 24
p box.number
p CextHelpers.from_module
p CextBox::SYM
class CextCustomError
  attr_accessor :detail
end
begin
  box.custom_error
rescue CextCustomError => error
  error.detail = 61
  p [error.message, error.detail]
end
p box.method(:store).parameters
p box.method(:packed).parameters
p box.method(:store).source_location
method = box.method(:store)
method.call(14.5)
p box.get
untouched = [2, 3]
p untouched
p box.method(:store).source_location.nil?
# a boxed receiver (a C method's object or a Ruby class's) holds the same: a C
# method has no Ruby position, whichever way the Method is made
class CextPlain
  def store(value) = value
end
boxed = [box, CextPlain.new][ARGV.size]
p boxed.method(:store).source_location
p [CextPlain.new, box][ARGV.size].method(:store).source_location.class
# ... and one reached through a module a builtin class includes
class Integer
  include CextHelpers
end
via_module = [3, box][ARGV.size]
p via_module.method(:from_module).source_location

left = CextLeft::Box.new
right = CextRight::Box.new
left.store(17)
right.store(18)
p [left.get, right.get, left.class.name, right.class.name]
parent = CextParent.new
parent.level = 21
data_child = CextDataChild.new
data_child.level = 22
data_child.store(23)
p [parent.level, data_child.level, data_child.get]

source = [1, 2]
p box.push(source.map { |n| n + 1 })
p source
p box.push(source.reverse)
p box.packed
p box.decorate_entry('abc'.chars)
strings_source = [+'a', +'b']
string_alias = strings_source[1]
p box.decorate_entry(strings_source.reverse)
p [strings_source, string_alias]

p box.buffer
captured = nil
p box.push(Array.new(2, 4).tap { |ary| captured = ary })
p captured
begin
  box.push(Array.new(1, 7).freeze)
rescue FrozenError
  puts 'frozen array'
end

# Review regressions: internal names still dispatch to ordinary Ruby methods.
class CextBox
  def __cext_init(n) = n + 10
  def __cext_constant_value(a, b) = a + b
  def __cext_share_new_array(a) = 'ordinary share'
  def __cext_invoke(a, b, c, d, e) = a + b + c + d + e
end
def pseudo_receiver(box)
  puts 'receiver evaluated'
  box
end
p pseudo_receiver(box).__cext_init(3)
p pseudo_receiver(box).__cext_constant_value(4, 5)
p pseudo_receiver(box).__cext_share_new_array([1])
p pseudo_receiver(box).__cext_invoke(1, 2, 3, 4, 5)

# The local-store walk must grow when a fresh producer adds AST nodes.
def widen_fresh_local(box)
  values = (1..3).map { |n| n }
  alias_values = values
  box.push(values)
  p alias_values
end
widen_fresh_local(box)

# A data object and nil share one optional typed receiver.
def copy_optional_box(value)
  p value.dup.nil?
  p value.clone.nil?
  p value.clone(freeze: true).nil?
  begin
    p value.clone(freeze: false).nil?
  rescue ArgumentError => error
    p error.message
  end
end
copy_optional_box(box)
copy_optional_box(nil)

begin
  box.store
rescue ArgumentError => error
  p error.message
end
begin
  box.store(1, 2)
rescue ArgumentError => error
  p error.message
end

# Ruby ivars are insufficient to serialize native data, even through an upcast.
def dump_parent(value)
  Marshal.dump(value)
end
parent_for_marshal = CextParent.new
parent_for_marshal.level = 19
p Marshal.load(dump_parent(parent_for_marshal)).level
begin
  dump_parent(CextDataChild.new)
rescue TypeError => error
  p error.message
end
begin
  Marshal.dump(box)
rescue TypeError => error
  p error.message
end
