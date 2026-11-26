# a send's arm that the class-value split rewrites (a read of
# File::PATH_SEPARATOR makes File a value a boxed receiver can be) keeps the
# marks the send's dispatch finds it by (Minitest's assert_predicate)
SEP = File::PATH_SEPARATOR

module A
  def assert(test, msg = nil) = test
  def assert_empty(obj, msg = nil) = assert_predicate(obj, :empty?, msg)
  def assert_predicate(o1, op, msg = nil) = assert(o1.__send__(op), msg)
end

class T
  include A
  def initialize(name) = @name = name
  def run = send(@name)
end

def pred(o, op) = o.__send__(op)
p pred([], :empty?), pred([1], :empty?), pred(2, :even?), pred("", :empty?)
t = T.new("x")
p t.assert_empty([]), t.assert_predicate(2, :even?), t.assert_predicate(3, :even?)
