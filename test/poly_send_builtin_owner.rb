# spinel: int64
# A send on a value of one of several classes, beside a top-level def of the
# method's name, runs the method of the value's own builtin class where it has
# one (a top-level def is Object's, which stands after every class below it),
# and the top-level def for the rest. The builtin-op rows say which builtin
# classes have the method (StopIteration#result, an exception's message,
# IO#write), so the value is not sent to the top-level def when the tables
# that count a class's methods leave the method out; a method of Object's own
# (itself) stays the top-level def's for every value. A program class under a
# builtin and a builtin the program reopens answer by their own methods too.

def result = :top_result
[StopIteration.new, 1].each { |o| p o.send(:result) }
[StopIteration.new, 1].each { |o| p o.__send__(:result) }
[StopIteration.new, 1].each { |o| p((o.public_send(:result) rescue :private)) }

def message = :top_message
sym = :message
[ArgumentError.new("arg"), RuntimeError.new("rt"), 7, "s"].each { |o| p o.send(sym) }

class Failure < StandardError
  def message = "failure"
end
[Failure.new, KeyError.new("key"), Object.new].each { |o| p o.send(:message) }

def write(*) = :top_write
[$stdout, 1, "s"].each { |o| p o.send(:write, "w\n") }

class MyArray < Array; end
def sum = :top_sum
[MyArray.new([1, 2]), [3, 4], 1].each { |o| p o.send(:sum) }

class Dynamic
  def method_missing(name, *args) = "missing #{name}"
  def respond_to_missing?(name, include_private = false) = true
end
[Dynamic.new, StopIteration.new, 1].each { |o| p o.send(:result) }

def shout = :top_shout
class Integer
  def shout = "int #{self}"
end
class StopIteration
  def extra = :stop_extra
end
def extra = :top_extra
class Plain; end
[1, "s", Plain.new, 2.5].each { |o| p o.send(:shout) }
[StopIteration.new, 1, Plain.new].each { |o| p o.send(:extra) }

def itself = :top_itself
[1, "s", [1], :a].each { |o| p o.send(:itself) }

require "stringio"
def pos(*) = :top_pos
def readline(*) = :top_readline
sio = StringIO.new(+"a\nb\n")
[sio, 1].each { |o| p o.send(:pos) }
[sio, 1].each { |o| p o.send(:readline) }
[sio, $stdout, 1].each { |o| p o.send(:write, "z\n") }
p sio.string

# A void builtin (IO#puts, #print) answers nil through the dispatch.
def puts(*) = :top_puts
def print(*) = :top_print
[$stdout, 1].each { |o| p o.send(:puts, "u") }
[$stdout, 1].each { |o| p o.send(:print, "v\n") }

# A method of a reopened Object answers every value, an Integer's own ahead
# of it.
class Object
  def ometh = :object_ometh
end
class Integer
  def ometh = :int_ometh
end
[1, "s"].each { |o| p o.send(:ometh) }

# A class of the program that defines puts answers its own value beside an IO,
# whose puts answers nil.
class Logger2
  def puts(*a) = 42
end
[Logger2.new, $stdout].each { |o| p o.send(:puts, "x") }

# An Integer and a Float are Numerics: a method the program adds to Numeric
# answers them.
class Numeric
  def bigness = self.class
end
def bigness = :top
[3, 2.5, "s"].each { |o| p o.send(:bigness) }
