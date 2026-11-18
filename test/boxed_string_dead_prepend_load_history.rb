class BoxedStringDeadPrependPeer
  def size = 91
end

module BoxedStringDeadPrepend
  def size = super + 10
  def fresh_reader = 31
end

value = [+"abc", BoxedStringDeadPrependPeer.new][0]
p [value.size, value.respond_to?(:fresh_reader)]
class String
  if false
    prepend BoxedStringDeadPrepend
  end
end
p [value.size, value.respond_to?(:fresh_reader)]
p String.ancestors.include?(BoxedStringDeadPrepend)
class String
  def size = 8
end
p value.size
