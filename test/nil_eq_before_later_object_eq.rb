# nil's == is Object's until the program defines one: a top-level compare
# before the def is the builtin's, though the def comes later in the file.
class V
  def ==(o) = o.equal?(self)
end
x = ARGV[0] ? V.new : nil
p nil == x
p [nil == x, nil == x]
p((nil == x) && (nil == x))
class Object
  def ==(o) = :objeq
end
p x.nil?
